/**
 * ESP-IDF entry point for Slack Reactions E-Paper Display
 *
 * Architecture: Event-driven with FreeRTOS tasks and auto light sleep.
 * When all tasks block on their respective synchronization primitives,
 * FreeRTOS tickless idle triggers automatic light sleep. WiFi maintains
 * DTIM beacon association, waking the radio every ~300ms (DTIM3) to
 * check for buffered data — giving ~1-3mA average vs ~60mA with the
 * Arduino deep sleep polling approach.
 *
 * Task priorities (higher = more urgent):
 *   button_task (7) - GPIO ISR semaphore, immediate response
 *   power_task  (6) - Battery monitoring, sleep decisions
 *   ws_task     (5) - WebSocket event processing
 *   display_task(3) - E-paper rendering (2-4s, non-blocking to WS)
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"

#include "config_manager.h"
#include "wifi_manager.h"
#include "websocket_manager.h"
#include "display_manager.h"
#include "security_manager.h"
#include "ota_manager.h"
#include "power_manager.h"
#include "led_manager.h"
#include "audio_manager.h"
#include "resilience_manager.h"
#include "pairing_manager.h"
#include "timezone_manager.h"
#include "board.h"
#include "diagnostics.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "APP_MAIN";

/**
 * RTC memory persists across deep sleep (not across power-on reset).
 * Stores last display content and state flags so that on deep sleep wake:
 * - The device can skip display refresh if content hasn't changed
 *   (skip_refresh_on_no_message policy)
 * - Restores last content (reaction or broadcast) without needing WebSocket reconnect
 */
enum display_content_type_t : uint8_t {
    DISPLAY_CONTENT_NONE = 0,
    DISPLAY_CONTENT_REACTION = 1,
    DISPLAY_CONTENT_BROADCAST = 2,
};

RTC_DATA_ATTR static struct {
    display_content_type_t content_type;
    union {
        struct {
            char emoji[32];
            char emoji_url[128];
            char user[64];
            char channel[128];
            char message[256];
            char platform[16];  // Matches reaction_data_t.platform size
            bool is_encrypted;
        } reaction;
        struct {
            char source[64];
            char message[256];
            char platform[16];
            bool encrypted;
        } broadcast;
    };
    bool showing_connection_lost;
    bool has_shown_blank_screen;
    bool was_critical_battery;    // Previously entered critical sleep — require recovery threshold to exit
} s_rtc_state = {};

/* FreeRTOS task handles */
static TaskHandle_t ws_task_handle = NULL;
static TaskHandle_t display_task_handle = NULL;
static TaskHandle_t power_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;

/* Inter-task communication */
static QueueHandle_t display_queue = NULL;
static EventGroupHandle_t system_events = NULL;
static SemaphoreHandle_t button_semaphore = NULL;

/* Event group bits */
#define EVT_WIFI_CONNECTED      BIT0
#define EVT_WS_CONNECTED        BIT1
#define EVT_PAIRING_COMPLETE    BIT2
#define EVT_SHUTDOWN_REQUEST    BIT3
#define EVT_PAIRING_REQUEST     BIT4   /* button_task → ws_task: enter pairing mode */
#define EVT_WIFI_PORTAL_REQUEST BIT5   /* button_task → ws_task: enter WiFi provisioning */
#define EVT_WS_ERROR            BIT6   /* WS callback received server error */
#define EVT_WS_FIRMWARE         BIT7   /* WS callback received firmware_update message */
#define EVT_HEALTH_CHECK        BIT8   /* esp_timer: run resilience check (30s periodic) */
#define EVT_HEARTBEAT_DUE       BIT9   /* esp_timer: send heartbeat + check timeout (30s periodic) */
#define EVT_OTA_PERIODIC        BIT10  /* esp_timer: 24h OTA check (one-shot, re-armed) */

/* Display queue depth: buffer a few reactions while display is refreshing */
#define DISPLAY_QUEUE_DEPTH  4

/* Timeout for task shutdown checks — allows tasks to notice EVT_SHUTDOWN_REQUEST
 * instead of blocking forever on portMAX_DELAY. 30s is acceptable because
 * shutdown only occurs before deep sleep (minutes-to-hours), and button_task
 * still responds instantly to presses via GPIO ISR semaphore. */
#define TASK_SHUTDOWN_CHECK_MS  30000

/* Forward declaration — defined before app_main(), used by ws_task and power_task */
static void graceful_shutdown(void);

/* ---------- esp_timer handles for event-driven ws_task ----------
 * All callbacks run in the esp_timer task context (CONFIG_ESP_TIMER_ISR_DISPATCH
 * is not set), so xEventGroupSetBits() is safe (not the FromISR variant). */
static esp_timer_handle_t s_health_timer = NULL;
static esp_timer_handle_t s_heartbeat_timer = NULL;
static esp_timer_handle_t s_ota_timer = NULL;
static esp_timer_handle_t s_firmware_reeval_timer = NULL;

static void health_timer_cb(void *arg) {
    xEventGroupSetBits((EventGroupHandle_t)arg, EVT_HEALTH_CHECK);
}
static void heartbeat_timer_cb(void *arg) {
    xEventGroupSetBits((EventGroupHandle_t)arg, EVT_HEARTBEAT_DUE);
}
static void ota_timer_cb(void *arg) {
    xEventGroupSetBits((EventGroupHandle_t)arg, EVT_OTA_PERIODIC);
}
static void firmware_reeval_cb(void *arg) {
    xEventGroupSetBits((EventGroupHandle_t)arg, EVT_WS_FIRMWARE);
}

/** Stop all ws_task timers — call before graceful_shutdown() or break paths. */
static void stop_ws_timers(void) {
    if (s_health_timer) esp_timer_stop(s_health_timer);
    if (s_heartbeat_timer) esp_timer_stop(s_heartbeat_timer);
    if (s_ota_timer) esp_timer_stop(s_ota_timer);
    if (s_firmware_reeval_timer) esp_timer_stop(s_firmware_reeval_timer);
}

/**
 * OTA download progress callback — updates e-paper display at 20% intervals.
 * E-paper refresh is slow (~1-2s), so fewer updates = faster install.
 * Accesses file-static display_queue (same translation unit).
 */
static void ota_display_progress_cb(size_t current, size_t total)
{
    static int last_percent = 0;  /* Start at 0 to skip redundant 0% update —
                                   * "Downloading vX.Y.Z" screen already conveys this */
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    if (percent != last_percent && percent % 20 == 0) {
        last_percent = percent;
        ESP_LOGI(TAG, "OTA download: %d%%", percent);
        display_event_t evt = {};
        evt.type = DISPLAY_EVT_STATUS;
        snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Firmware Update");
        snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "%d%%", percent);
        snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "Please wait");
        xQueueSend(display_queue, &evt, pdMS_TO_TICKS(100));
    }
    if (current >= total) {
        last_percent = 0;  /* Reset for next OTA cycle */
    }
}

/**
 * OTA check + download with e-paper progress display.
 * Shows: Downloading vX.Y.Z → progress % → Update Complete / Update Failed.
 * Only updates display when an update is actually found — avoids unnecessary
 * e-paper refreshes during routine checks with no available update.
 */
static void ota_check_with_display(void)
{
    firmware_info_t info;
    esp_err_t ret = ota_manager_check_for_update(&info);
    if (ret != ESP_OK) {
        return;  /* No update available or check failed */
    }

    /* Update found — show download screen */
    display_event_t evt = {};
    evt.type = DISPLAY_EVT_STATUS;
    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Firmware Update");
    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Downloading v%s", info.version);
    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "Please wait");
    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));

    /* Download and install with progress display */
    ret = ota_manager_download_and_install(&info, ota_display_progress_cb);
    if (ret == ESP_OK) {
        evt = {};
        evt.type = DISPLAY_EVT_STATUS;
        snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Update Complete");
        snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "v%s installed", info.version);
        snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "Rebooting...");
        xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(3000));  /* Let display render before restart */
        ota_manager_finalize_and_restart(info.version);
        /* Does not return */
    } else {
        evt = {};
        evt.type = DISPLAY_EVT_STATUS;
        snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Update Failed");
        xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
    }
}

/**
 * Deterministic jitter from device ID using FNV-1a hash.
 * Same device always gets the same delay — predictable for debugging,
 * uniformly distributed across a fleet.
 * Returns 0 if max_ms is 0 (jitter disabled).
 */
static uint32_t device_id_jitter_ms(const char *device_id, uint32_t max_ms)
{
    if (max_ms == 0) return 0;
    uint32_t hash = 2166136261u;  /* FNV-1a offset basis */
    for (const char *p = device_id; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;  /* FNV-1a prime */
    }
    return hash % max_ms;
}

/**
 * WebSocket task: processes incoming WebSocket events (reactions, heartbeats, commands).
 * Blocks on event group when no data available — allows light sleep.
 */
static void ws_task(void *arg)
{
    ESP_LOGI(TAG, "ws_task started");

    /* Wait for WiFi + pairing before connecting WebSocket */
    xEventGroupWaitBits(system_events,
                        EVT_WIFI_CONNECTED | EVT_PAIRING_COMPLETE,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    /* Spread initial WebSocket connections over configured window to avoid
     * thundering herd after mass power-on or AP reboot. */
    {
        const app_config_t *cfg = config_manager_get_config();
        uint32_t jitter_max_ms = (uint32_t)cfg->server.reconnect_jitter_max_sec * 1000;
        uint32_t jitter = device_id_jitter_ms(cfg->device.id, jitter_max_ms);
        if (jitter > 0) {
            ESP_LOGI(TAG, "Reconnect jitter: %lu ms", (unsigned long)jitter);
            vTaskDelay(pdMS_TO_TICKS(jitter));
        }
    }

    websocket_manager_start(display_queue);

    /* Register event group so WS callbacks can wake ws_task immediately */
    websocket_manager_set_event_group(system_events, EVT_WS_ERROR, EVT_WS_FIRMWARE);

    /* Create periodic timers for event-driven architecture.
     * All callbacks run in esp_timer task context (not ISR), so
     * xEventGroupSetBits() is safe. See CONFIG_ESP_TIMER_ISR_DISPATCH. */
    {
        esp_timer_create_args_t args = {};

        args.callback = health_timer_cb; args.arg = system_events; args.name = "health";
        args.skip_unhandled_events = true;  /* Event bits are level-triggered; re-setting is a no-op */
        esp_timer_create(&args, &s_health_timer);
        esp_timer_start_periodic(s_health_timer, 30 * 1000000);  /* 30 seconds */

        args.callback = heartbeat_timer_cb; args.arg = system_events; args.name = "heartbeat";
        args.skip_unhandled_events = true;
        esp_timer_create(&args, &s_heartbeat_timer);
        esp_timer_start_periodic(s_heartbeat_timer, 30 * 1000000);  /* 30 seconds — matches server interval */

        args.callback = ota_timer_cb; args.arg = system_events; args.name = "ota_24h";
        esp_timer_create(&args, &s_ota_timer);
        esp_timer_start_once(s_ota_timer, 24LL * 60 * 60 * 1000000);  /* 24 hours */

        args.callback = firmware_reeval_cb; args.arg = system_events; args.name = "fw_reeval";
        esp_timer_create(&args, &s_firmware_reeval_timer);
        /* One-shot, started only when optional firmware is deferred */
    }

    /* Track DEVICE_NOT_LINKED attempts to prevent infinite pairing loop on battery.
     * After MAX attempts, show help screen and deep sleep so user can intervene. */
    uint8_t device_not_linked_count = 0;
    const uint8_t MAX_DEVICE_NOT_LINKED_ATTEMPTS = 2;

    /* Track DEVICE_NOT_REGISTERED retries (3 attempts with 30s delays, then deep sleep) */
    uint8_t registration_error_count = 0;
    const uint8_t MAX_REGISTRATION_RETRIES = 3;

    /* Event-driven main loop: ws_task blocks until an event bit fires.
     * Timer-driven events: health check (5s), heartbeat (15s), OTA (24h).
     * Callback-driven events: server errors, firmware updates (from WS handler).
     * Button-driven events: pairing, WiFi portal (from button_task ISR).
     *
     * pdTRUE clears matched bits on return. EVT_PAIRING_COMPLETE and
     * EVT_WIFI_CONNECTED are NOT in WAKE_BITS, so they are unaffected. */
    const EventBits_t WAKE_BITS =
        EVT_SHUTDOWN_REQUEST | EVT_PAIRING_REQUEST | EVT_WIFI_PORTAL_REQUEST |
        EVT_WS_ERROR | EVT_WS_FIRMWARE |
        EVT_HEALTH_CHECK | EVT_HEARTBEAT_DUE | EVT_OTA_PERIODIC;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(system_events,
            WAKE_BITS, pdTRUE, pdFALSE, portMAX_DELAY);

        /* --- Shutdown (highest priority) --- */
        if (bits & EVT_SHUTDOWN_REQUEST) {
            break;
        }

        /* --- Heartbeat send + timeout check (30s timer) --- */
        if (bits & EVT_HEARTBEAT_DUE) {
            websocket_manager_process(system_events);
        }

        /* --- Server errors (immediate, from WS callback) --- */
        if (bits & EVT_WS_ERROR) {
            ws_error_info_t error_info;
            ws_error_code_t ws_error = websocket_manager_get_pending_error(&error_info);

            if (ws_error == WS_ERROR_DEVICE_NOT_LINKED) {
                device_not_linked_count++;

                if (device_not_linked_count > MAX_DEVICE_NOT_LINKED_ATTEMPTS) {
                    ESP_LOGW(TAG, "Too many DEVICE_NOT_LINKED errors — sleeping");
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Pairing needed");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Hold button 3-9 sec");
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "to re-enter pairing");
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    stop_ws_timers();
                    graceful_shutdown();
                    power_manager_deep_sleep(15ULL * 60 * 1000000);  /* 15 minutes */
                    break;
                }

                ESP_LOGI(TAG, "Device not linked — entering pairing mode");
                esp_timer_stop(s_health_timer);
                esp_timer_stop(s_heartbeat_timer);
                websocket_manager_stop();

                xEventGroupClearBits(system_events, EVT_PAIRING_COMPLETE);
                pairing_manager_start(display_queue, system_events);
                xEventGroupWaitBits(system_events, EVT_PAIRING_COMPLETE,
                                    pdFALSE, pdTRUE, portMAX_DELAY);

                security_manager_reset_key_uploaded();

                ESP_LOGI(TAG, "Pairing complete — restarting");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                break;

            } else if (ws_error == WS_ERROR_TRIAL_EXPIRED) {
                const app_config_t *trial_cfg = config_manager_get_config();

                if (error_info.purchase_url[0] != '\0') {
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_PURCHASE_QR;
                    const char *id = error_info.device_id[0] ? error_info.device_id : trial_cfg->device.id;
                    snprintf(evt.data.purchase.url, sizeof(evt.data.purchase.url),
                             "%.90s%cdevice_id=%.24s",
                             error_info.purchase_url,
                             strchr(error_info.purchase_url, '?') ? '&' : '?',
                             id);
                    strncpy(evt.data.purchase.device_id, id, sizeof(evt.data.purchase.device_id) - 1);
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                } else {
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Trial Expired");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Purchase key at:");
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "See purchase info");
                    const char *id = error_info.device_id[0] ? error_info.device_id : trial_cfg->device.id;
                    snprintf(evt.data.status.line4, sizeof(evt.data.status.line4), "ID: %.59s", id);
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                }

                ESP_LOGI(TAG, "Trial expired — entering 60-minute deep sleep");
                vTaskDelay(pdMS_TO_TICKS(2000));
                stop_ws_timers();
                graceful_shutdown();
                power_manager_deep_sleep(60ULL * 60 * 1000000);  /* 60 minutes */
                break;

            } else if (ws_error == WS_ERROR_DEVICE_NOT_REGISTERED) {
                const app_config_t *reg_cfg = config_manager_get_config();
                registration_error_count++;
                const char *id = error_info.device_id[0] ? error_info.device_id : reg_cfg->device.id;

                if (registration_error_count < MAX_REGISTRATION_RETRIES) {
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Connecting...");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2),
                             "Attempt %d of %d", registration_error_count, MAX_REGISTRATION_RETRIES - 1);
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "ID: %.59s", id);
                    snprintf(evt.data.status.line4, sizeof(evt.data.status.line4), "Checking registration");
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                    vTaskDelay(pdMS_TO_TICKS(30000));  /* 30 seconds between retries */
                } else {
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Setup Required");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "ID: %.59s", id);
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "Complete registration");
                    snprintf(evt.data.status.line4, sizeof(evt.data.status.line4), "then RESTART device");
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));

                    ESP_LOGI(TAG, "Registration failed — entering 10-minute deep sleep");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    stop_ws_timers();
                    graceful_shutdown();
                    power_manager_deep_sleep(10ULL * 60 * 1000000);  /* 10 minutes */
                    break;
                }

            } else if (ws_error == WS_ERROR_AUTH_FAILED) {
                registration_error_count++;

                if (registration_error_count < MAX_REGISTRATION_RETRIES) {
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Auth Failed");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2),
                             "Attempt %d of %d", registration_error_count, MAX_REGISTRATION_RETRIES - 1);
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "Check auth_token");
                    snprintf(evt.data.status.line4, sizeof(evt.data.status.line4), "in config.json");
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                    vTaskDelay(pdMS_TO_TICKS(30000));  /* 30 seconds between retries */
                } else {
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Auth Failed");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Check auth_token");
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "in config.json");
                    snprintf(evt.data.status.line4, sizeof(evt.data.status.line4), "then RESTART device");
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));

                    ESP_LOGI(TAG, "Auth failed — entering 10-minute deep sleep");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    stop_ws_timers();
                    graceful_shutdown();
                    power_manager_deep_sleep(10ULL * 60 * 1000000);  /* 10 minutes */
                    break;
                }
            }
        }

        /* --- Button-triggered pairing (immediate, from button_task ISR) ---
         * Runs here (ws_task, 6KB stack) instead of button_task (4KB) because
         * pairing_manager does TLS HTTP requests requiring ~10-16KB via mbedtls. */
        if (bits & EVT_PAIRING_REQUEST) {
            ESP_LOGI(TAG, "Button-triggered pairing mode");
            esp_timer_stop(s_health_timer);
            esp_timer_stop(s_heartbeat_timer);
            websocket_manager_stop();

            xEventGroupClearBits(system_events, EVT_PAIRING_COMPLETE);
            esp_err_t pair_ret = pairing_manager_start(display_queue, system_events);
            if (pair_ret == ESP_OK) {
                security_manager_reset_key_uploaded();
                ESP_LOGI(TAG, "Pairing complete — restarting to reconnect");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                break;
            } else {
                ESP_LOGW(TAG, "Pairing failed or cancelled");
                display_event_t evt = {};
                evt.type = DISPLAY_EVT_STATUS;
                snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Pairing failed");
                snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Try again");
                xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                websocket_manager_start(display_queue);
                esp_timer_start_periodic(s_health_timer, 30 * 1000000);
                esp_timer_start_periodic(s_heartbeat_timer, 30 * 1000000);
            }
        }

        /* --- Button-triggered WiFi provisioning (immediate, from button_task ISR) ---
         * Stops WebSocket, disconnects WiFi, starts captive portal inline.
         * Matches Arduino: startProvisioning(true) runs inline, no reboot. */
        if (bits & EVT_WIFI_PORTAL_REQUEST) {
            ESP_LOGI(TAG, "Button-triggered WiFi provisioning");
            esp_timer_stop(s_health_timer);
            esp_timer_stop(s_heartbeat_timer);
            websocket_manager_stop();

            display_event_t portal_evt = {};
            portal_evt.type = DISPLAY_EVT_WIFI_PROVISION;
            strncpy(portal_evt.data.wifi_provision.ssid, "pebl-setup",
                    sizeof(portal_evt.data.wifi_provision.ssid) - 1);
            strncpy(portal_evt.data.wifi_provision.ip, "192.168.4.1",
                    sizeof(portal_evt.data.wifi_provision.ip) - 1);
            xQueueSend(display_queue, &portal_evt, pdMS_TO_TICKS(1000));

            wifi_manager_disconnect();

            xEventGroupClearBits(system_events, EVT_WIFI_CONNECTED);
            wifi_manager_start_portal(system_events, EVT_WIFI_CONNECTED);
            xEventGroupWaitBits(system_events, EVT_WIFI_CONNECTED,
                                pdFALSE, pdTRUE, portMAX_DELAY);
            wifi_manager_stop_portal();

            ESP_LOGI(TAG, "WiFi provisioning complete — restarting");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            break;
        }

        /* --- Connection health check (5s timer) --- */
        if (bits & EVT_HEALTH_CHECK) {
            resilience_action_t action = resilience_manager_check_health();
            switch (action) {
            case RESILIENCE_ACTION_WIFI_RECONNECT:
                ESP_LOGW(TAG, "Resilience: forcing WiFi reconnect");
                resilience_manager_mark_wifi_reconnect();
                websocket_manager_stop();
                wifi_manager_reconnect();
                {
                    const app_config_t *rcfg = config_manager_get_config();
                    uint32_t jitter_max_ms = (uint32_t)rcfg->server.reconnect_jitter_max_sec * 1000;
                    uint32_t jitter = device_id_jitter_ms(rcfg->device.id, jitter_max_ms);
                    if (jitter > 0) {
                        ESP_LOGI(TAG, "Reconnect jitter: %lu ms", (unsigned long)jitter);
                        vTaskDelay(pdMS_TO_TICKS(jitter));
                    }
                }
                websocket_manager_start(display_queue);
                break;

            case RESILIENCE_ACTION_REBOOT:
                ESP_LOGW(TAG, "Resilience: entering deep sleep instead of rebooting");
                stop_ws_timers();
                graceful_shutdown();
                power_manager_deep_sleep(60ULL * 60 * 1000000);
                break;

            case RESILIENCE_ACTION_DEEP_SLEEP:
                ESP_LOGW(TAG, "Resilience: entering deep sleep after extended downtime");
                stop_ws_timers();
                graceful_shutdown();
                power_manager_deep_sleep(60ULL * 60 * 1000000);
                break;

            case RESILIENCE_ACTION_NONE:
            default:
                break;
            }
        }

        /* --- Firmware updates (immediate, from WS callback) --- */
        if (bits & EVT_WS_FIRMWARE) {
            if (websocket_manager_has_pending_firmware_required()) {
                ESP_LOGI(TAG, "Required firmware update — installing now");
                ota_check_with_display();
            } else if (websocket_manager_has_pending_firmware_optional()) {
                int64_t last_reaction = websocket_manager_get_last_reaction_time();
                int64_t idle_ms = last_reaction > 0 ? (esp_timer_get_time() - last_reaction) / 1000 : INT64_MAX;
                if (idle_ms > 5 * 60 * 1000) {
                    ESP_LOGI(TAG, "Optional firmware update — device idle for %lld min",
                             idle_ms / 60000);
                    ota_check_with_display();
                } else {
                    ESP_LOGI(TAG, "Optional firmware update deferred — device active (%lld s idle)",
                             idle_ms / 1000);
                    /* Re-evaluate in 60 seconds. If still not idle, reschedules.
                     * Stop first in case a previous reeval timer is still pending. */
                    esp_timer_stop(s_firmware_reeval_timer);
                    esp_timer_start_once(s_firmware_reeval_timer, 60 * 1000000);
                }
            }
        }

        /* --- Periodic OTA check (24h timer, one-shot re-armed) --- */
        if (bits & EVT_OTA_PERIODIC) {
            if (!websocket_manager_has_pending_firmware_optional()) {
                ESP_LOGI(TAG, "24h periodic OTA check");
                ota_check_with_display();
            }
            /* Re-arm for next 24h */
            esp_timer_start_once(s_ota_timer, 24LL * 60 * 60 * 1000000);
        }
    }

    stop_ws_timers();
    esp_timer_delete(s_health_timer);
    esp_timer_delete(s_heartbeat_timer);
    esp_timer_delete(s_ota_timer);
    esp_timer_delete(s_firmware_reeval_timer);

    websocket_manager_stop();
    vTaskDelete(NULL);
}

/**
 * Download emoji PNG from a cached URL for display restore after reconnect.
 * Returns heap-allocated buffer (caller must free), or NULL on failure.
 * Kept simple — websocket_manager has a similar function for the normal path,
 * but exposing it would create an unnecessary cross-component dependency.
 */
static uint8_t *download_emoji_for_restore(const char *url, size_t *out_size)
{
    if (!url || url[0] == '\0') {
        return NULL;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > 64 * 1024) {
        if (content_length < 0) {
            content_length = 64 * 1024;
        } else {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return NULL;
        }
    }

    uint8_t *buf = (uint8_t *)malloc(content_length);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int n = esp_http_client_read(client, (char *)buf + total_read,
                                     content_length - total_read);
        if (n <= 0) break;
        total_read += n;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) {
        free(buf);
        return NULL;
    }

    *out_size = total_read;
    return buf;
}

/**
 * Restore last displayed content from RTC memory.
 * For reactions, re-downloads emoji via cached URL. For broadcasts, constructs
 * a display event directly (no image needed). Returns true if restored.
 */
static bool restore_last_display(void)
{
    switch (s_rtc_state.content_type) {
    case DISPLAY_CONTENT_REACTION: {
        ESP_LOGI(TAG, "Restoring last reaction from RTC");
        display_event_t restore_evt = {};
        restore_evt.type = DISPLAY_EVT_REACTION;
        restore_evt.data.reaction.is_encrypted = s_rtc_state.reaction.is_encrypted;
        strncpy(restore_evt.data.reaction.user, s_rtc_state.reaction.user, sizeof(restore_evt.data.reaction.user) - 1);
        strncpy(restore_evt.data.reaction.channel, s_rtc_state.reaction.channel, sizeof(restore_evt.data.reaction.channel) - 1);
        strncpy(restore_evt.data.reaction.message_preview, s_rtc_state.reaction.message, sizeof(restore_evt.data.reaction.message_preview) - 1);
        strncpy(restore_evt.data.reaction.emoji_name, s_rtc_state.reaction.emoji, sizeof(restore_evt.data.reaction.emoji_name) - 1);
        strncpy(restore_evt.data.reaction.platform, s_rtc_state.reaction.platform, sizeof(restore_evt.data.reaction.platform) - 1);

        size_t png_size = 0;
        uint8_t *png_data = download_emoji_for_restore(s_rtc_state.reaction.emoji_url, &png_size);
        restore_evt.data.reaction.emoji_png_data = png_data;
        restore_evt.data.reaction.emoji_png_size = png_size;

        display_manager_render(&restore_evt);
        return true;
    }
    case DISPLAY_CONTENT_BROADCAST: {
        ESP_LOGI(TAG, "Restoring last broadcast from RTC");
        display_event_t restore_evt = {};
        restore_evt.type = DISPLAY_EVT_BROADCAST;
        strncpy(restore_evt.data.broadcast.source, s_rtc_state.broadcast.source, sizeof(restore_evt.data.broadcast.source) - 1);
        strncpy(restore_evt.data.broadcast.message, s_rtc_state.broadcast.message, sizeof(restore_evt.data.broadcast.message) - 1);
        strncpy(restore_evt.data.broadcast.platform, s_rtc_state.broadcast.platform, sizeof(restore_evt.data.broadcast.platform) - 1);
        restore_evt.data.broadcast.encrypted = s_rtc_state.broadcast.encrypted;

        display_manager_render(&restore_evt);
        return true;
    }
    default:
        return false;
    }
}

/**
 * Display task: renders reactions on e-paper display.
 * Blocks on xQueueReceive with finite timeout — allows checking shutdown flag.
 * Takes 2-4s per refresh but does not block WebSocket reception
 * because esp_websocket_client runs its own internal task.
 */
static void display_task(void *arg)
{
    ESP_LOGI(TAG, "display_task started");
    display_event_t evt;

    while (!(xEventGroupGetBits(system_events) & EVT_SHUTDOWN_REQUEST)) {
        /* Use finite timeout so we periodically check for shutdown */
        if (xQueueReceive(display_queue, &evt, pdMS_TO_TICKS(TASK_SHUTDOWN_CHECK_MS)) == pdTRUE) {
            /* Three-way logic for CONNECTED events to suppress redundant e-paper refreshes.
             * On light sleep reconnects (~5-10 min), the screen content hasn't changed,
             * so refreshing wastes 2-4s of battery and causes visible flicker.
             *
             * Case 1: "Connection Lost" screen is showing and we have a cached reaction
             *         → Restore the reaction (re-downloads emoji from cached URL)
             * Case 2: First-ever connection (no reaction yet, no blank screen shown)
             *         → Show the "Waiting for Reactions" connected screen once
             * Case 3: Normal reconnect with valid content already on screen
             *         → Skip refresh entirely (preserve current display) */
            /* Power source changed (USB↔battery) — partial refresh the top
             * status bar only (~300ms). No full re-render or emoji re-download. */
            if (evt.type == DISPLAY_EVT_POWER_CHANGE) {
                display_manager_render(&evt);
                continue;
            }

            if (evt.type == DISPLAY_EVT_CONNECTED) {
                if (s_rtc_state.showing_connection_lost && s_rtc_state.content_type != DISPLAY_CONTENT_NONE) {
                    /* "Connection Lost" screen replaced the last content — restore it.
                     * For reactions, re-downloads emoji PNG from cached URL.
                     * For broadcasts, constructs display event directly (no image needed). */
                    ESP_LOGI(TAG, "Reconnected — restoring last display content");
                    restore_last_display();
                    s_rtc_state.showing_connection_lost = false;
                    continue;
                } else if (s_rtc_state.showing_connection_lost || !s_rtc_state.has_shown_blank_screen) {
                    /* "Connection Lost" showing but no reaction to restore, or
                     * first-ever connection. Show connected screen. */
                    if (s_rtc_state.showing_connection_lost) {
                        ESP_LOGI(TAG, "Reconnected — clearing connection lost screen");
                    } else {
                        ESP_LOGI(TAG, "First connection — showing connected screen");
                    }
                    s_rtc_state.has_shown_blank_screen = true;
                    s_rtc_state.showing_connection_lost = false;
                    display_manager_render(&evt);
                    continue;
                } else {
                    /* Normal reconnect — reaction or connected screen is still valid
                     * on the e-paper. Skip refresh to save battery. */
                    ESP_LOGI(TAG, "Reconnected — preserving display (no refresh)");
                    continue;
                }
            }

            display_manager_render(&evt);

            /* Persist display content to RTC memory for deep sleep recovery.
             * On next deep sleep wake, skip_refresh_on_no_message can avoid
             * a full display refresh if the same content is still showing. */
            if (evt.type == DISPLAY_EVT_REACTION) {
                s_rtc_state.content_type = DISPLAY_CONTENT_REACTION;
                strncpy(s_rtc_state.reaction.emoji, evt.data.reaction.emoji_name, sizeof(s_rtc_state.reaction.emoji) - 1);
                s_rtc_state.reaction.emoji[sizeof(s_rtc_state.reaction.emoji) - 1] = '\0';
                strncpy(s_rtc_state.reaction.emoji_url, evt.data.reaction.emoji_url, sizeof(s_rtc_state.reaction.emoji_url) - 1);
                s_rtc_state.reaction.emoji_url[sizeof(s_rtc_state.reaction.emoji_url) - 1] = '\0';
                strncpy(s_rtc_state.reaction.user, evt.data.reaction.user, sizeof(s_rtc_state.reaction.user) - 1);
                s_rtc_state.reaction.user[sizeof(s_rtc_state.reaction.user) - 1] = '\0';
                strncpy(s_rtc_state.reaction.channel, evt.data.reaction.channel, sizeof(s_rtc_state.reaction.channel) - 1);
                s_rtc_state.reaction.channel[sizeof(s_rtc_state.reaction.channel) - 1] = '\0';
                strncpy(s_rtc_state.reaction.message, evt.data.reaction.message_preview, sizeof(s_rtc_state.reaction.message) - 1);
                s_rtc_state.reaction.message[sizeof(s_rtc_state.reaction.message) - 1] = '\0';
                strncpy(s_rtc_state.reaction.platform, evt.data.reaction.platform, sizeof(s_rtc_state.reaction.platform) - 1);
                s_rtc_state.reaction.platform[sizeof(s_rtc_state.reaction.platform) - 1] = '\0';
                s_rtc_state.reaction.is_encrypted = evt.data.reaction.is_encrypted;
                s_rtc_state.showing_connection_lost = false;
            } else if (evt.type == DISPLAY_EVT_BROADCAST) {
                s_rtc_state.content_type = DISPLAY_CONTENT_BROADCAST;
                strncpy(s_rtc_state.broadcast.source, evt.data.broadcast.source, sizeof(s_rtc_state.broadcast.source) - 1);
                s_rtc_state.broadcast.source[sizeof(s_rtc_state.broadcast.source) - 1] = '\0';
                strncpy(s_rtc_state.broadcast.message, evt.data.broadcast.message, sizeof(s_rtc_state.broadcast.message) - 1);
                s_rtc_state.broadcast.message[sizeof(s_rtc_state.broadcast.message) - 1] = '\0';
                strncpy(s_rtc_state.broadcast.platform, evt.data.broadcast.platform, sizeof(s_rtc_state.broadcast.platform) - 1);
                s_rtc_state.broadcast.platform[sizeof(s_rtc_state.broadcast.platform) - 1] = '\0';
                s_rtc_state.broadcast.encrypted = evt.data.broadcast.encrypted;
                s_rtc_state.showing_connection_lost = false;
            } else if (evt.type == DISPLAY_EVT_DISCONNECTED) {
                s_rtc_state.showing_connection_lost = true;
            }
        }
    }

    vTaskDelete(NULL);
}

/**
 * Power task: monitors battery voltage, manages sleep transitions.
 * Runs on a 5-minute interval.
 *
 * Low battery (<30%): Top bar shows "LOW BATTERY" automatically via
 * draw_power_status_text() — no full-screen warning needed.
 *
 * Critical battery (<15%): Shows full-screen "LOW BATTERY / PLEASE CHARGE",
 * waits for display to render, then enters deep sleep for 30 minutes.
 *
 * Extended WiFi loss (30+ min): Handled by power_manager_check() via
 * resilience_manager.
 */
static void power_task(void *arg)
{
    ESP_LOGI(TAG, "power_task started");

    while (!(xEventGroupGetBits(system_events) & EVT_SHUTDOWN_REQUEST)) {
        bool power_transition = power_manager_check(system_events);

        /* Power source changed (USB↔battery) — partial refresh the top status bar
         * to update battery indicator and power text (~300ms vs 2s full refresh). */
        if (power_transition) {
            ESP_LOGI(TAG, "Power source changed — requesting display update");
            display_event_t evt = {};
            evt.type = DISPLAY_EVT_POWER_CHANGE;
            evt.data.power_change.show_lock = (s_rtc_state.content_type == DISPLAY_CONTENT_REACTION
                                                && s_rtc_state.reaction.is_encrypted);
            xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
        }

        /* Critical battery: show warning screen, then deep sleep.
         * power_manager_check() updates voltage/source but no longer sleeps
         * directly, so we can show the screen first. */
        if (power_manager_is_critical_battery()) {
            ESP_LOGW(TAG, "Critical battery (%d mV) — showing warning before indefinite deep sleep",
                     power_manager_get_battery_mv());
            s_rtc_state.was_critical_battery = true;
            display_event_t evt = {};
            evt.type = DISPLAY_EVT_LOW_BATTERY;
            xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(5000));  /* Let display_task render before sleep */
            power_manager_deep_sleep(0);  /* Indefinite — button wake only, prevents drain to death */
            /* Does not return */
        }

        /* Block for 5 minutes or until shutdown requested.
         * Battery voltage changes slowly (~1mV/min under light load), so
         * checking every 5 minutes is sufficient for USB/battery detection
         * and critical battery shutdown. The cell's own protection circuit
         * is the real safety net against over-discharge.
         *
         * Previous 10-second interval caused unnecessary wakes: 16 ADC
         * samples × 5ms delay each = 80ms of active time, 8,640 times/day.
         * At 5-minute intervals this drops to 288 times/day. */
        xEventGroupWaitBits(system_events, EVT_SHUTDOWN_REQUEST,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(300000));
    }

    vTaskDelete(NULL);
}

/**
 * Button task: handles physical button presses via GPIO ISR.
 * Blocks on semaphore with finite timeout — allows checking shutdown flag.
 *
 * power_manager_handle_button() returns an action (not void) to avoid
 * a circular dependency between power_manager and display_manager.
 * This task translates button actions into display events and system actions.
 */
static void button_task(void *arg)
{
    ESP_LOGI(TAG, "button_task started");

    while (!(xEventGroupGetBits(system_events) & EVT_SHUTDOWN_REQUEST)) {
        if (xSemaphoreTake(button_semaphore, pdMS_TO_TICKS(TASK_SHUTDOWN_CHECK_MS)) == pdTRUE) {
            /* Drain any extra semaphore gives from switch bounce or noise
             * that accumulated between the ISR firing and now. */
            while (xSemaphoreTake(button_semaphore, 0) == pdTRUE) {}

            /* ESP32 errata 3.11: GPIO 36/39 can produce false interrupts when
             * the ADC (used by WiFi, battery monitoring) switches input channels.
             * Verify the button is actually pressed (LOW) before processing.
             * A real press holds LOW for at least a few ms; a glitch is gone
             * by the time we get here. */
            board_pins_t pins;
            board_get_pin_config(&pins);
            if (gpio_get_level((gpio_num_t)pins.button_gpio) != 0) {
                ESP_LOGD(TAG, "Ignoring spurious button ISR (GPIO already HIGH)");
                gpio_intr_enable((gpio_num_t)pins.button_gpio);
                continue;
            }

            /* Measure hold duration with mid-hold feedback screen.
             * At 3s, show instructions so user knows what release vs continued
             * hold will do — matching Arduino handleButtonHold() behavior. */
            int64_t press_start_us = esp_timer_get_time();
            bool showed_feedback = false;

            while (gpio_get_level((gpio_num_t)pins.button_gpio) == 0) {
                int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;
                if (held_ms > 17000) break;  /* Safety limit */

                /* At 3s mark: show feedback so user knows what actions are available */
                if (!showed_feedback && held_ms >= 3000) {
                    showed_feedback = true;
                    display_event_t fb_evt = {};
                    fb_evt.type = DISPLAY_EVT_STATUS;
                    snprintf(fb_evt.data.status.line1, sizeof(fb_evt.data.status.line1),
                             "Release: Add Platform");
                    snprintf(fb_evt.data.status.line2, sizeof(fb_evt.data.status.line2),
                             "Keep holding 15s:");
                    snprintf(fb_evt.data.status.line3, sizeof(fb_evt.data.status.line3),
                             "WiFi Setup");
                    xQueueSend(display_queue, &fb_evt, pdMS_TO_TICKS(1000));
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }

            int64_t press_duration_ms = (esp_timer_get_time() - press_start_us) / 1000;
            ESP_LOGI(TAG, "Button held for %lld ms", press_duration_ms);

            if (press_duration_ms >= 15000) {
                /* 15+ seconds: Signal ws_task to start WiFi provisioning portal.
                 * Portal involves HTTP server + DNS which need more stack than button_task (4KB).
                 * Matches Arduino: startProvisioning() runs inline, no reboot. */
                ESP_LOGI(TAG, "Button: requesting WiFi provisioning");
                xEventGroupSetBits(system_events, EVT_WIFI_PORTAL_REQUEST);

            } else if (press_duration_ms >= 3000) {
                /* 3-9 seconds: Signal ws_task to enter pairing mode.
                 * Pairing involves TLS HTTP requests (mbedtls) which need ~10-16KB stack.
                 * button_task only has 4KB, so we delegate to ws_task (6KB+) via event bit. */
                ESP_LOGI(TAG, "Button: requesting pairing mode");
                xEventGroupSetBits(system_events, EVT_PAIRING_REQUEST);

            } else {
                /* Short press — no action */
                ESP_LOGD(TAG, "Short press — ignoring (battery: %d%%, %d mV)",
                         power_manager_get_battery_percent(), power_manager_get_battery_mv());
            }

            /* Wait for button release before re-enabling the interrupt.
             * With level-triggered GPIO (required for light sleep wake),
             * re-enabling while the pin is LOW would immediately re-fire. */
            while (gpio_get_level((gpio_num_t)pins.button_gpio) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(50));  /* Debounce after release */
            gpio_intr_enable((gpio_num_t)pins.button_gpio);
        }
    }

    vTaskDelete(NULL);
}

/**
 * GPIO ISR handler for button press.
 * Gives semaphore to wake button_task, then disables the interrupt.
 *
 * gpio_wakeup_enable() sets the interrupt type to LOW_LEVEL (required for
 * light sleep wake). Level-triggered interrupts re-fire continuously while
 * the button is held, starving the CPU. To prevent this, the ISR disables
 * the interrupt immediately after signaling. button_task re-enables it
 * after the button is released.
 *
 * Direct register write (GPIO.pin[n].int_ena = 0) is used because
 * gpio_intr_disable() is not IRAM-safe.
 */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    GPIO.pin[gpio_num].int_ena = 0;  /* Disable interrupt — ISR-safe register write */

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(button_semaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * Initialize NVS flash storage.
 * Erases and re-initializes if NVS partition is corrupt or version mismatch.
 */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing and re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/**
 * Configure automatic light sleep with WiFi power save.
 * Called after WiFi connects. When all FreeRTOS tasks block,
 * the CPU enters light sleep automatically. WiFi radio wakes on
 * DTIM beacons to check for buffered frames.
 */
static void enable_light_sleep(void)
{
    /* ESP-IDF v5.x unified PM config type (replaces target-specific types) */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_LOGI(TAG, "Auto light sleep enabled with WiFi DTIM power save");
}

/**
 * Configure button GPIO interrupt for wake and runtime press detection.
 */
static void init_button_gpio(void)
{
    board_pins_t pins;
    board_get_pin_config(&pins);

    /* GPIO 34-39 on ESP32 are input-only and lack internal pull-ups.
     * The LilyGo T5 has an external pull-up on GPIO 39 (button pin),
     * so we skip the internal pull-up for those pins to avoid the error. */
    bool has_internal_pullup = (pins.button_gpio < 34);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pins.button_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = has_internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);

    /* gpio_install_isr_service returns ESP_ERR_INVALID_STATE if already installed
     * (e.g., by SPI driver for e-paper). That's fine — just use the existing one. */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }

    /* Enable GPIO as light sleep wake source. Auto light sleep (enabled later
     * in boot) puts the CPU to sleep when all tasks block. Without this,
     * the button GPIO interrupt fires but can't wake the CPU from light sleep.
     * Light sleep wakeup requires level trigger (not edge), so we use LOW_LEVEL
     * since the button is active-low. The NEGEDGE ISR below handles runtime
     * interrupt processing; this just ensures the CPU wakes up first. */
    gpio_wakeup_enable((gpio_num_t)pins.button_gpio, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    /* Install ISR handler AFTER all GPIO configuration is complete.
     * gpio_wakeup_enable() modifies GPIO registers inside a critical section —
     * if the ISR is already installed, a pending interrupt can fire when the
     * critical section exits, causing a nested interrupt crash. */
    gpio_isr_handler_add((gpio_num_t)pins.button_gpio, button_isr_handler,
                         (void*)(uintptr_t)pins.button_gpio);
}

/**
 * Structured shutdown sequence before entering deep sleep.
 * Cleanly tears down network connections and display to avoid:
 * - WebSocket half-open connections on the server (timeout-based cleanup)
 * - Corrupted e-paper refresh if sleep interrupts SPI transaction
 * - Stale WiFi association on the access point
 */
static void graceful_shutdown(void)
{
    ESP_LOGI(TAG, "Graceful shutdown — stopping services");

    if (system_events) {
        xEventGroupSetBits(system_events, EVT_SHUTDOWN_REQUEST);
    }

    websocket_manager_stop();
    wifi_manager_disconnect();

    /* Brief delay for log flush before deep sleep halts UART */
    vTaskDelay(pdMS_TO_TICKS(100));
}

/**
 * Network diagnostics mode — triggered by holding button 3-15s during cold boot.
 * Runs before FreeRTOS tasks start, so all calls are synchronous.
 * Display is already initialized (step 5), WiFi is not yet connected.
 *
 * Flow: show "Running Diagnostics..." → connect WiFi → run checks → show results
 *       → wait for button press → return to normal boot.
 */
static void run_boot_diagnostics(void)
{
    ESP_LOGI(TAG, "=== Network Diagnostics Mode ===");

    /* Show "Running Diagnostics..." on display (direct render, no task needed) */
    display_event_t diag_status = {};
    diag_status.type = DISPLAY_EVT_STATUS;
    strncpy(diag_status.data.status.line1, "Running", sizeof(diag_status.data.status.line1) - 1);
    strncpy(diag_status.data.status.line2, "Diagnostics...", sizeof(diag_status.data.status.line2) - 1);
    diag_status.data.status.show_lock = false;
    display_manager_render(&diag_status);

    /* Connect WiFi (needed for all network checks) */
    esp_err_t wifi_ret = wifi_manager_connect();

    display_event_t result_evt = {};
    result_evt.type = DISPLAY_EVT_DIAGNOSTICS;

    if (wifi_ret != ESP_OK) {
        /* WiFi failed — populate minimal result showing the failure */
        diag_result_t *r = &result_evt.data.diagnostics.result;
        memset(r, 0, sizeof(*r));
        r->battery_pct = -1;
        const app_config_t *cfg = config_manager_get_config();
        strncpy(r->device_id, cfg->device.id, sizeof(r->device_id) - 1);
        strncpy(r->firmware_version, APP_VERSION, sizeof(r->firmware_version) - 1);
        r->free_heap = esp_get_free_heap_size();
        strncpy(r->auth_hint, "WiFi connection failed", sizeof(r->auth_hint) - 1);
        display_manager_render(&result_evt);
    } else {
        /* Run all diagnostic checks */
        result_evt.data.diagnostics.result = run_network_diagnostics();
        display_manager_render(&result_evt);
    }

    /* Wait for button press to dismiss (poll GPIO, no ISR needed) */
    board_pins_t pins;
    board_get_pin_config(&pins);
    ESP_LOGI(TAG, "Diagnostics complete — press button to continue boot");

    /* Wait for button release first (user may still be holding from boot) */
    while (gpio_get_level((gpio_num_t)pins.button_gpio) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* Wait for next button press */
    while (gpio_get_level((gpio_num_t)pins.button_gpio) != 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* Debounce */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Diagnostics dismissed — continuing normal boot");
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Slack Reactions E-Paper Display (ESP-IDF) ===");
    ESP_LOGI(TAG, "Boot reason: %d", esp_sleep_get_wakeup_cause());

    /* 1. Initialize NVS and event loop */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. Load configuration from LittleFS */
    ESP_ERROR_CHECK(config_manager_init());

    /* 3. Initialize board HAL (pin config, ADC calibration) */
    ESP_ERROR_CHECK(board_init());

    /* 3a. Set clock floor to firmware build date so the clock is never at 1970.
     * On cold boot the ESP32 starts at epoch 0 — this ensures timestamps in logs
     * and on the display are "at least in the right year" before WiFi/timezone sync. */
    {
        struct tm build_tm = {};
        strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &build_tm);
        time_t build_epoch = mktime(&build_tm);
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        if (now_tv.tv_sec < build_epoch) {
            struct timeval tv = { .tv_sec = build_epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Clock floor set to build time: %s %s", __DATE__, __TIME__);
        }
    }

    /* 3b. Mark firmware valid immediately after basic init succeeds.
     * Called before WiFi to prevent OTA rollback if the device enters the captive
     * portal (no credentials on first boot after cross-firmware OTA migration).
     * Mirrors Arduino's checkBootValidation() timing — if we can init NVS, config,
     * and board, the firmware is functional. Network reporting is deferred to step 15. */
    ota_manager_mark_valid_on_boot();

    /* 4. Check wake reason for deep sleep recovery */
    esp_sleep_wakeup_cause_t wake_reason = esp_sleep_get_wakeup_cause();
    bool is_deep_sleep_wake = (wake_reason == ESP_SLEEP_WAKEUP_TIMER ||
                               wake_reason == ESP_SLEEP_WAKEUP_EXT0 ||
                               wake_reason == ESP_SLEEP_WAKEUP_EXT1);

    /* 5. Initialize display (show splash on cold boot, preserve on wake) */
    bool skip_display_refresh = false;
    if (is_deep_sleep_wake) {
        const app_config_t *skip_cfg = config_manager_get_config();
        /* skip_refresh_on_no_message: if the e-paper still shows the last reaction
         * and no power state changed, skip the refresh entirely. E-paper retains
         * its image without power, so the content is still visible. */
        if (skip_cfg->display_policy.skip_refresh_on_no_message &&
            s_rtc_state.content_type != DISPLAY_CONTENT_NONE &&
            !s_rtc_state.showing_connection_lost) {
            ESP_LOGI(TAG, "Preserving display — will update on new message only");
            skip_display_refresh = true;
        }
    }
    ESP_ERROR_CHECK(display_manager_init(is_deep_sleep_wake || skip_display_refresh));

    /* 5b. Play startup chime on cold boot (no-op on ESP32 / LilyGo T5) */
    if (!is_deep_sleep_wake) {
        audio_manager_play_startup();
    }

    /* 6. Initialize security (load or generate ECDH keypair) */
    ESP_ERROR_CHECK(security_manager_init());

    /* 7. Create synchronization primitives */
    system_events = xEventGroupCreate();
    display_queue = xQueueCreate(DISPLAY_QUEUE_DEPTH, sizeof(display_event_t));
    button_semaphore = xSemaphoreCreateBinary();
    configASSERT(system_events && display_queue && button_semaphore);

    /* 8. Initialize resilience manager (message queue, health tracking) */
    resilience_manager_init();

    /* 9. Initialize power manager (battery ADC, thresholds) */
    ESP_ERROR_CHECK(power_manager_init());

    /* 9b. Initialize notification LED (no-op on ESP32 / LilyGo T5) */
    ESP_ERROR_CHECK(led_manager_init());

    /* 9a. Critical battery check on wake BEFORE WiFi connection.
     * WiFi is the most power-hungry operation — skip it if battery is critically low.
     *
     * Hysteresis: If we previously entered critical sleep (was_critical_battery),
     * require battery to reach BATTERY_RECOVERY_MV (~20%) before allowing boot.
     * This prevents oscillation where voltage sag under WiFi load drops below 15%,
     * device sleeps, voltage recovers to 16%, device boots, WiFi drops it again. */
    if (is_deep_sleep_wake && power_manager_get_source() == POWER_SOURCE_BATTERY) {
        int threshold = s_rtc_state.was_critical_battery
            ? BATTERY_RECOVERY_MV    // Previously critical: require ~20% to exit
            : BATTERY_CRITICAL_MV;   // Normal wake: 15% entry threshold
        int mv = power_manager_get_battery_mv();
        if (mv > 0 && mv < threshold) {
            ESP_LOGW(TAG, "Critical battery on wake (%d mV, threshold %d mV) — sleeping",
                     mv, threshold);
            if (!s_rtc_state.was_critical_battery) {
                // First time entering critical state — show warning screen
                display_event_t bat_evt = {};
                bat_evt.type = DISPLAY_EVT_LOW_BATTERY;
                display_manager_render(&bat_evt);
                vTaskDelay(pdMS_TO_TICKS(5000));  // Let e-paper finish refresh
                s_rtc_state.was_critical_battery = true;
            }
            // Already showing low battery screen from prior cycle — skip redundant refresh
            power_manager_deep_sleep(0);      // Indefinite — button wake only
            // Does not return
        }
        // Battery recovered above threshold — clear critical flag and proceed
        if (s_rtc_state.was_critical_battery) {
            ESP_LOGI(TAG, "Battery recovered (%d mV >= %d mV) — clearing critical flag", mv, threshold);
            s_rtc_state.was_critical_battery = false;
        }
    }

    /* 9b. Network diagnostics (cold boot only, before button_task starts).
     * Hold button 3-15s during power-on to enter diagnostics mode.
     * Shows WiFi/DNS/HTTPS/WS/Auth results on e-paper for IT troubleshooting.
     * >= 15s falls through to normal boot (captive portal at step 12).
     * < 3s falls through to normal boot (accidental bump). */
    if (!is_deep_sleep_wake) {
        board_pins_t diag_pins;
        board_get_pin_config(&diag_pins);
        if (gpio_get_level((gpio_num_t)diag_pins.button_gpio) == 0) {
            uint32_t held_ms = 0;
            while (gpio_get_level((gpio_num_t)diag_pins.button_gpio) == 0 && held_ms < 17000) {
                vTaskDelay(pdMS_TO_TICKS(50));
                held_ms += 50;
            }
            if (held_ms >= 3000 && held_ms < 15000) {
                run_boot_diagnostics();
            }
        }
    }

    /* 10. Configure button GPIO interrupt */
    init_button_gpio();

    /* 11. Start display and button tasks early — they must be running before
     * WiFi/pairing because those steps send display events (captive portal
     * instructions, QR codes) that need a consumer on the display queue. */
    xTaskCreate(display_task, "display_task", 8192, NULL, 3, &display_task_handle);
    xTaskCreate(button_task,  "button_task",  4096, NULL, 7, &button_task_handle);

    /* 12. Connect WiFi (multi-network scan or captive portal).
     * Skip if already connected (diagnostics mode connects WiFi at step 9b). */
    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi already connected (from diagnostics) — skipping");
        xEventGroupSetBits(system_events, EVT_WIFI_CONNECTED);
    } else if (wifi_manager_connect() == ESP_OK) {
        xEventGroupSetBits(system_events, EVT_WIFI_CONNECTED);
    } else {
        ESP_LOGW(TAG, "WiFi connection failed, starting captive portal");

        /* Show WiFi provisioning screen with QR code and instructions */
        display_event_t portal_evt = {};
        portal_evt.type = DISPLAY_EVT_WIFI_PROVISION;
        strncpy(portal_evt.data.wifi_provision.ssid, "pebl-setup",
                sizeof(portal_evt.data.wifi_provision.ssid) - 1);
        strncpy(portal_evt.data.wifi_provision.ip, "192.168.4.1",
                sizeof(portal_evt.data.wifi_provision.ip) - 1);
        xQueueSend(display_queue, &portal_evt, pdMS_TO_TICKS(1000));

        wifi_manager_start_portal(system_events, EVT_WIFI_CONNECTED);
        /* Portal runs until credentials are provided and WiFi connects.
         * Portal save handler sets EVT_WIFI_CONNECTED on system_events. */
        xEventGroupWaitBits(system_events, EVT_WIFI_CONNECTED,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* Shut down the captive portal (HTTP server, DNS, SoftAP) now that
         * WiFi is connected. Frees ~15KB RAM and stops DNS hijacking. */
        wifi_manager_stop_portal();
    }

    /* 12b. Sync timezone (sets system clock for quiet hours).
     * Must happen after WiFi connects. On cold boot, always syncs.
     * On deep sleep wake, syncs only if interval has elapsed. */
    timezone_manager_init();
    {
        bool is_cold_boot = (wake_reason == ESP_SLEEP_WAKEUP_UNDEFINED);
        timezone_manager_sync_if_needed(is_cold_boot);
    }

    /* 12c. Show "Connecting to Server..." boot status after WiFi connects */
    {
        const app_config_t *boot_cfg = config_manager_get_config();
        display_event_t boot_evt = {};
        boot_evt.type = DISPLAY_EVT_BOOT_STATUS;
        strncpy(boot_evt.data.boot.device_name, boot_cfg->device.name,
                sizeof(boot_evt.data.boot.device_name) - 1);
        xQueueSend(display_queue, &boot_evt, pdMS_TO_TICKS(1000));
    }

    /* 13. Handle pairing if needed */
    const app_config_t *cfg = config_manager_get_config();
    if (cfg->security.auth_token[0] == '\0') {
        ESP_LOGI(TAG, "No auth token — starting self-service pairing");
        pairing_manager_start(display_queue, system_events);
        xEventGroupWaitBits(system_events, EVT_PAIRING_COMPLETE,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* New pairing creates a fresh user record on the server without a key.
         * Reset the upload flag so we re-upload even if NVS had uploaded=yes
         * from a previous pairing session. */
        security_manager_reset_key_uploaded();

        /* Re-read config since pairing writes a new auth_token */
        cfg = config_manager_get_config();
    } else {
        xEventGroupSetBits(system_events, EVT_PAIRING_COMPLETE);
    }

    /* 14. Upload ECDH public key if not yet uploaded */
    if (!security_manager_is_key_uploaded()) {
        esp_err_t upload_ret = security_manager_upload_public_key(
            cfg->server.host, cfg->security.auth_token, cfg->device.id);
        if (upload_ret != ESP_OK) {
            ESP_LOGW(TAG, "ECDH key upload failed — reactions will not be encrypted");
        }
    }

    /* 15. Check for OTA updates (non-blocking, skip if recently checked) */
    ota_manager_check_on_boot();

    /* 15b. Check server for available firmware updates.
     * Random 0-60s jitter avoids thundering herd when many devices power on together
     * (e.g., after a power outage in a fleet deployment).
     * Only applied when fleet jitter is enabled (reconnect_jitter_max_sec > 0);
     * single B2C devices skip the delay. */
    if (cfg->server.reconnect_jitter_max_sec > 0) {
        uint32_t ota_jitter_ms = esp_random() % 60000;
        ESP_LOGI(TAG, "Boot OTA check in %lu ms (jitter)", (unsigned long)ota_jitter_ms);
        vTaskDelay(pdMS_TO_TICKS(ota_jitter_ms));
    }
    ESP_LOGI(TAG, "Checking for firmware updates on boot");
    ota_check_with_display();  // If update found: downloads, verifies, restarts

    /* 16. Enable auto light sleep with WiFi DTIM power save */
    enable_light_sleep();

    /* 17. Start remaining tasks — ws_task and power_task need WiFi connected */
    xTaskCreate(ws_task,      "ws_task",      6144, NULL, 5, &ws_task_handle);
    xTaskCreate(power_task,   "power_task",   4096, NULL, 6, &power_task_handle);

    ESP_LOGI(TAG, "All tasks started — entering event-driven mode");
    /* app_main returns, FreeRTOS scheduler continues running tasks.
     * When all tasks block, tickless idle triggers auto light sleep. */
}
