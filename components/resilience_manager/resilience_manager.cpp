/**
 * Resilience Manager — connection health tracking and escalation.
 *
 * Port from: ResilienceManager class (esp32_arduino_client)
 *
 * Key differences from Arduino version:
 * - No message queue: FreeRTOS xQueue handles buffering between WS and display tasks
 * - Uses esp_timer_get_time() (microseconds) instead of millis()
 * - Escalation actions are returned to caller instead of executing directly,
 *   keeping this component free of WiFi/system dependencies
 * - Thread-safe: all state accessed only from ws_task (single writer)
 *
 * Escalation ladder:
 *   1. esp_websocket_client auto-reconnect (built-in, fixed 15s reconnect_timeout_ms)
 *   2. After 10 failed reconnects → WIFI_RECONNECT (caller forces WiFi cycle, ~7-10 min)
 *   3. After 5 WiFi reconnects   → RESILIENCE_ACTION_REBOOT (caller enters 60-min deep sleep)
 *   4. After 30min downtime      → DEEP_SLEEP (caller enters 60-min deep sleep)
 */

#include "resilience_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "RESILIENCE";

/* Escalation thresholds */
#define HEARTBEAT_TIMEOUT_MS       90000   /* 90s: 3x the 30s heartbeat interval, avoids false positives from network jitter */
#define WIFI_RECONNECT_THRESHOLD   10      /* WS reconnects before forcing WiFi cycle (~10-15 min of WS backoff) */
#define REBOOT_THRESHOLD           5       /* WiFi reconnects before deep sleep as last resort */
#define DEEP_SLEEP_DOWNTIME_MS     (30 * 60 * 1000)  /* 30 min total downtime → deep sleep */
#define HEALTH_LOG_INTERVAL_US     (60 * 1000000LL)   /* Log health status every 60s */
#define HEALTH_CHECK_INTERVAL_US   (30 * 1000000LL)   /* Rate-limit check_health to every 30s */

static struct {
    bool is_healthy;
    int64_t last_heartbeat_us;
    uint32_t missed_heartbeats;
    uint32_t reconnect_attempts;     /* WS reconnects since last healthy */
    uint32_t total_downtime_ms;
    int64_t last_downtime_start_us;
    uint32_t messages_delivered;
    uint32_t wifi_reconnect_count;   /* WiFi reconnects since last healthy */
    int64_t init_time_us;            /* For uptime calculation */
    int64_t last_health_log_us;      /* Rate limit for periodic status logging */
    int64_t last_check_us;           /* Rate limit for check_health */
} s_state;

void resilience_manager_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.is_healthy = false;
    s_state.init_time_us = esp_timer_get_time();
    s_state.last_check_us = s_state.init_time_us;
    s_state.last_health_log_us = s_state.init_time_us;

    ESP_LOGI(TAG, "Initialized (ws_reconnect_threshold=%d, reboot_threshold=%d, "
             "deep_sleep_after=%ds)",
             WIFI_RECONNECT_THRESHOLD, REBOOT_THRESHOLD,
             DEEP_SLEEP_DOWNTIME_MS / 1000);
}

void resilience_manager_record_heartbeat(void)
{
    int64_t now = esp_timer_get_time();
    int64_t elapsed_ms = 0;

    if (s_state.last_heartbeat_us > 0) {
        elapsed_ms = (now - s_state.last_heartbeat_us) / 1000;
    }

    s_state.last_heartbeat_us = now;
    s_state.missed_heartbeats = 0;

    if (!s_state.is_healthy) {
        resilience_manager_mark_connection_restored();
    }

    ESP_LOGD(TAG, "Heartbeat (elapsed=%lld ms)", elapsed_ms);
}

resilience_action_t resilience_manager_check_health(void)
{
    int64_t now = esp_timer_get_time();

    /* Rate limit: only check every 30 seconds (HEALTH_CHECK_INTERVAL_US) to
     * avoid busy-loop overhead — matches the 30s health-check timer cadence. */
    if (now - s_state.last_check_us < HEALTH_CHECK_INTERVAL_US) {
        return RESILIENCE_ACTION_NONE;
    }
    s_state.last_check_us = now;

    /* Track missed heartbeats for metrics (informational only —
     * actual disconnect detection is handled by WebSocket events) */
    if (s_state.last_heartbeat_us > 0) {
        int64_t since_hb_ms = (now - s_state.last_heartbeat_us) / 1000;
        if (since_hb_ms > HEARTBEAT_TIMEOUT_MS) {
            s_state.missed_heartbeats = (uint32_t)(since_hb_ms / (HEARTBEAT_TIMEOUT_MS / 2));
        }
    }

    /* Periodic health status log (every 60s at DEBUG level) */
    if (now - s_state.last_health_log_us > HEALTH_LOG_INTERVAL_US) {
        s_state.last_health_log_us = now;
        uint32_t downtime = resilience_manager_get_total_downtime_ms();
        int64_t uptime_total = (now - s_state.init_time_us) / 1000;
        float uptime_pct = uptime_total > 0
            ? (float)(uptime_total - downtime) / (float)uptime_total * 100.0f
            : 100.0f;

        ESP_LOGD(TAG, "Health: %s | reconnects=%lu | wifi_reconnects=%lu | "
                 "downtime=%lu ms | delivered=%lu | uptime=%.1f%%",
                 s_state.is_healthy ? "OK" : "DOWN",
                 s_state.reconnect_attempts,
                 s_state.wifi_reconnect_count,
                 downtime,
                 s_state.messages_delivered,
                 uptime_pct);
    }

    /* No escalation needed if connection is healthy */
    if (s_state.is_healthy) {
        return RESILIENCE_ACTION_NONE;
    }

    /* Escalation ladder — check from most severe to least */

    /* Extended downtime → deep sleep to conserve battery */
    uint32_t total_down = resilience_manager_get_total_downtime_ms();
    if (total_down > DEEP_SLEEP_DOWNTIME_MS) {
        ESP_LOGW(TAG, "Extended downtime (%lu ms) — recommending deep sleep", total_down);
        return RESILIENCE_ACTION_DEEP_SLEEP;
    }

    /* Multiple WiFi reconnects failed → deep sleep as last resort.
     * Caller handles RESILIENCE_ACTION_REBOOT with deep sleep (not esp_restart)
     * to avoid cold-boot WiFi failure triggering captive portal. */
    if (s_state.wifi_reconnect_count >= REBOOT_THRESHOLD) {
        ESP_LOGW(TAG, "WiFi reconnect threshold reached (%lu/%d) — recommending deep sleep",
                 s_state.wifi_reconnect_count, REBOOT_THRESHOLD);
        return RESILIENCE_ACTION_REBOOT;
    }

    /* Multiple WS reconnects failed → try forcing a WiFi cycle */
    if (s_state.reconnect_attempts >= WIFI_RECONNECT_THRESHOLD) {
        ESP_LOGW(TAG, "WS reconnect threshold reached (%lu/%d) — recommending WiFi reconnect",
                 s_state.reconnect_attempts, WIFI_RECONNECT_THRESHOLD);
        return RESILIENCE_ACTION_WIFI_RECONNECT;
    }

    /* Below thresholds — let esp_websocket_client auto-reconnect handle it */
    return RESILIENCE_ACTION_NONE;
}

void resilience_manager_mark_connection_lost(void)
{
    if (s_state.is_healthy) {
        s_state.is_healthy = false;
        s_state.last_downtime_start_us = esp_timer_get_time();
        s_state.reconnect_attempts++;
        ESP_LOGW(TAG, "Connection lost (reconnect attempt #%lu, wifi_reconnects=%lu)",
                 s_state.reconnect_attempts, s_state.wifi_reconnect_count);
    } else {
        /* Already marked unhealthy — just increment reconnect counter.
         * This handles repeated WEBSOCKET_EVENT_DISCONNECTED during backoff. */
        s_state.reconnect_attempts++;
    }
}

void resilience_manager_mark_connection_restored(void)
{
    if (!s_state.is_healthy) {
        s_state.is_healthy = true;
        if (s_state.last_downtime_start_us > 0) {
            uint32_t downtime_ms =
                (uint32_t)((esp_timer_get_time() - s_state.last_downtime_start_us) / 1000);
            s_state.total_downtime_ms += downtime_ms;
            ESP_LOGI(TAG, "Connection restored (downtime=%lu ms, total=%lu ms, "
                     "reconnects=%lu, wifi_reconnects=%lu)",
                     downtime_ms, s_state.total_downtime_ms,
                     s_state.reconnect_attempts, s_state.wifi_reconnect_count);
        }
        /* Reset escalation counters on successful recovery */
        s_state.reconnect_attempts = 0;
        s_state.wifi_reconnect_count = 0;
    }
}

void resilience_manager_mark_wifi_reconnect(void)
{
    s_state.wifi_reconnect_count++;
    /* Reset WS reconnect counter since we're trying a fresh WiFi connection */
    s_state.reconnect_attempts = 0;
    ESP_LOGI(TAG, "WiFi reconnect #%lu", s_state.wifi_reconnect_count);
}

bool resilience_manager_is_healthy(void)
{
    return s_state.is_healthy;
}

uint32_t resilience_manager_get_reconnect_attempts(void)
{
    return s_state.reconnect_attempts;
}

uint32_t resilience_manager_get_total_downtime_ms(void)
{
    uint32_t total = s_state.total_downtime_ms;
    if (!s_state.is_healthy && s_state.last_downtime_start_us > 0) {
        total += (uint32_t)((esp_timer_get_time() - s_state.last_downtime_start_us) / 1000);
    }
    return total;
}

void resilience_manager_mark_message_delivered(void)
{
    s_state.messages_delivered++;
}

uint32_t resilience_manager_get_messages_delivered(void)
{
    return s_state.messages_delivered;
}

connection_health_t resilience_manager_get_health(void)
{
    int64_t now = esp_timer_get_time();
    uint32_t downtime = resilience_manager_get_total_downtime_ms();
    int64_t uptime_total = (now - s_state.init_time_us) / 1000;

    connection_health_t h = {
        .is_healthy = s_state.is_healthy,
        .last_heartbeat_us = s_state.last_heartbeat_us,
        .missed_heartbeats = s_state.missed_heartbeats,
        .reconnect_attempts = s_state.reconnect_attempts,
        .total_downtime_ms = downtime,
        .last_downtime_start_us = s_state.last_downtime_start_us,
        .messages_delivered = s_state.messages_delivered,
        .wifi_reconnect_count = s_state.wifi_reconnect_count,
        .reboot_count = 0,
        .uptime_percent = uptime_total > 0
            ? (float)(uptime_total - downtime) / (float)uptime_total * 100.0f
            : 100.0f,
    };
    return h;
}

void resilience_manager_reset_metrics(void)
{
    s_state.missed_heartbeats = 0;
    s_state.reconnect_attempts = 0;
    s_state.wifi_reconnect_count = 0;
    s_state.total_downtime_ms = 0;
    s_state.last_downtime_start_us = 0;
    s_state.messages_delivered = 0;
    ESP_LOGI(TAG, "Metrics reset");
}
