#pragma once

/**
 * Resilience Manager — connection health tracking and escalation.
 *
 * Monitors WebSocket heartbeats, tracks connection quality metrics,
 * and decides escalation actions when connectivity degrades:
 *
 *   Escalation ladder (each step tried after repeated failures):
 *   1. Wait for esp_websocket_client auto-reconnect (built-in backoff)
 *   2. After 10 WS disconnects: Force WiFi reconnect (disconnect + reconnect STA)
 *   3. After 5 WiFi reconnects: Enter 60-min deep sleep (safer than reboot — avoids
 *      cold-boot WiFi failure triggering captive portal on an otherwise working device)
 *   4. After 30 min total downtime: Enter 60-min deep sleep
 *
 * Also provides uptime percentage and health status for diagnostics.
 *
 * Port from: ResilienceManager class (esp32_arduino_client)
 * Key difference: no message queue — FreeRTOS xQueue between WebSocket
 * and display tasks handles message buffering natively.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Escalation action returned by resilience_manager_check_health().
 * Caller (ws_task) is responsible for executing the action.
 */
typedef enum {
    RESILIENCE_ACTION_NONE,          /* Everything healthy, no action needed */
    RESILIENCE_ACTION_WIFI_RECONNECT,/* Force WiFi disconnect + reconnect */
    RESILIENCE_ACTION_REBOOT,        /* Last resort: caller enters deep sleep (not esp_restart, to avoid captive portal) */
    RESILIENCE_ACTION_DEEP_SLEEP,    /* Enter deep sleep (extended downtime) */
} resilience_action_t;

/**
 * Connection health metrics (read-only snapshot).
 */
typedef struct {
    bool is_healthy;
    int64_t last_heartbeat_us;
    uint32_t missed_heartbeats;
    uint32_t reconnect_attempts;
    uint32_t total_downtime_ms;
    int64_t last_downtime_start_us;
    uint32_t messages_delivered;
    uint32_t wifi_reconnect_count;
    uint32_t reboot_count;
    float uptime_percent;
} connection_health_t;

/**
 * Initialize resilience manager.
 */
void resilience_manager_init(void);

/**
 * Record a heartbeat from the server.
 * Call this when a heartbeat message is received on the WebSocket.
 */
void resilience_manager_record_heartbeat(void);

/**
 * Periodic health check — call from ws_task every ~5 seconds.
 *
 * Evaluates connection state and returns an escalation action if needed:
 * - 10+ WS disconnects with no success   → WIFI_RECONNECT
 * - 5+ WiFi reconnects with no success   → RESILIENCE_ACTION_REBOOT (→ deep sleep)
 * - 30+ minutes total downtime           → DEEP_SLEEP
 *
 * Also logs health status every 60 seconds at DEBUG level.
 */
resilience_action_t resilience_manager_check_health(void);

/**
 * Mark connection as lost (WebSocket disconnected).
 */
void resilience_manager_mark_connection_lost(void);

/**
 * Mark connection as restored (WebSocket registered successfully).
 */
void resilience_manager_mark_connection_restored(void);

/**
 * Mark that a WiFi reconnect was attempted (for escalation tracking).
 */
void resilience_manager_mark_wifi_reconnect(void);

/**
 * Check if connection is healthy.
 */
bool resilience_manager_is_healthy(void);

/**
 * Get number of reconnect attempts since last stable connection.
 */
uint32_t resilience_manager_get_reconnect_attempts(void);

/**
 * Get total downtime in milliseconds.
 */
uint32_t resilience_manager_get_total_downtime_ms(void);

/**
 * Track a successfully delivered message (for health metrics).
 */
void resilience_manager_mark_message_delivered(void);

/**
 * Get total messages delivered since init.
 */
uint32_t resilience_manager_get_messages_delivered(void);

/**
 * Get a snapshot of all health metrics.
 */
connection_health_t resilience_manager_get_health(void);

/**
 * Reset all metrics.
 */
void resilience_manager_reset_metrics(void);

#ifdef __cplusplus
}
#endif
