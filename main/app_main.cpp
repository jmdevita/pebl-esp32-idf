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
#include "wifi_credential_manager.h"
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

/* Brownout-backoff counter (RTC-retained so it survives the reset it's tracking).
 *
 * On a low/aged battery (or any marginal supply), WiFi power-up pulls a current
 * spike that sags the rail below the brownout threshold and hard-resets the chip
 * mid-connect. That reset lands BEFORE wifi_manager's failure counter increments,
 * so the normal WiFi-fallback safety never engages — the device tight-loops on
 * cold boot (visible as the splash re-flashing until the cell finally dies).
 * The custom PCB has no battery sense (power_manager_has_valid_battery_reading()
 * is false there), so we can't see the low voltage coming; the only signal we get
 * is the brownout reset reason after the fact. We use it to back off instead of
 * immediately re-attempting WiFi.
 *
 * Best-effort: a deep enough sag can wipe RTC RAM, resetting this to 0. That still
 * yields a 30s backoff — escalation is lost but the destructive loop is still
 * broken, which is the point. */
RTC_DATA_ATTR static uint32_t s_brownout_backoff_count;

/* Whether the "LOW BATTERY / Charge, then toggle switch" explanation screen has
 * been drawn for the CURRENT brownout episode. E-paper retains its image with zero
 * power, so a device that dies of battery exhaustion otherwise leaves whatever it
 * last drew — usually the boot splash — and looks "stuck" rather than dead (users
 * flip switches and hold reset buttons on a device with an empty cell). Drawn once
 * per episode at backoff rung >= 3 (see the brownout breaker), cleared wherever the
 * brownout counter clears. Latched AFTER the render completes so a brownout
 * mid-draw retries on the next rung. Best-effort like the counter: a deep sag can
 * wipe RTC RAM — worst case the screen draws again (cosmetically redundant,
 * energetically minor). A full power cut (switch toggle) zeroes RTC RAM, which is
 * exactly the reset-to-normal the screen's instructions ask the user to perform. */
RTC_DATA_ATTR static bool s_drew_dead_battery_screen;

/* Escalating deep-sleep backoff after a WiFi connect fails on an UNATTENDED
 * deep-sleep wake (stored credentials present, no brownout). RTC-retained so the
 * escalation survives the sleep it's tracking. Prevents a transient router outage
 * at wake time from parking the device in the captive portal (~80-100mA) until
 * the cell dies — see the WiFi-failed branch in app_main (ESP-1). Cleared on any
 * successful WiFi association. */
RTC_DATA_ATTR static uint32_t s_wifi_fail_backoff_count;

/* Escalating awake-pairing attempt counter (RTC-retained). An unlinked device on
 * battery would otherwise cycle boot → WS connect → NOT_LINKED → ~10 min awake
 * pairing → restart → repeat forever, because a per-boot local counter resets on
 * every esp_restart(). Persisting it lets the inter-attempt deep sleep lengthen as
 * attempts accumulate, so an abandoned unlinked device settles into long naps
 * instead of burning the cell awake (ESP-6). Cleared on successful pairing. */
RTC_DATA_ATTR static uint32_t s_pairing_attempt_count;

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
/* BIT1 reserved (was EVT_WS_CONNECTED — never set or waited, removed as dead code) */
#define EVT_PAIRING_COMPLETE    BIT2
#define EVT_SHUTDOWN_REQUEST    BIT3
#define EVT_PAIRING_REQUEST     BIT4   /* button_task → ws_task: enter pairing mode */
#define EVT_WIFI_PORTAL_REQUEST BIT5   /* button_task → ws_task: enter WiFi provisioning */
#define EVT_WS_ERROR            BIT6   /* WS callback received server error */
#define EVT_WS_FIRMWARE         BIT7   /* WS callback received firmware_update message */
#define EVT_HEALTH_CHECK        BIT8   /* esp_timer: run resilience check (30s periodic) */
#define EVT_HEARTBEAT_DUE       BIT9   /* esp_timer: send heartbeat + check timeout (30s periodic) */
#define EVT_OTA_PERIODIC        BIT10  /* esp_timer: 24h OTA check (one-shot, re-armed) */
#define EVT_DISPLAY_IDLE        BIT11  /* display_task is blocked on queue (no render in flight) */

/* Display queue depth: buffer a few reactions while display is refreshing */
#define DISPLAY_QUEUE_DEPTH  4

/* Timeout for task shutdown checks — allows tasks to notice EVT_SHUTDOWN_REQUEST
 * instead of blocking forever on portMAX_DELAY. 30s is acceptable because
 * shutdown only occurs before deep sleep (minutes-to-hours), and button_task
 * still responds instantly to presses via GPIO ISR semaphore. */
#define TASK_SHUTDOWN_CHECK_MS  30000

/* Forward declaration — defined before app_main(), used by ws_task and power_task */
static void graceful_shutdown(void);
/* Forward declaration — defined below app_main's helpers, used by ws_task */
static uint64_t pairing_backoff_us(uint32_t count);

/**
 * Block until display_task has drained the queue and the in-flight render
 * (if any) has completed. Required before host deep sleep so we don't
 * interrupt an SPI refresh mid-transfer (leaves the panel in an
 * indeterminate state — symptom: black screen after sleep).
 *
 * Returns true if idle within `timeout_ms`, false on timeout.
 */
static bool wait_for_display_idle(uint32_t timeout_ms)
{
    if (!display_queue || !system_events) return true;
    const TickType_t poll = pdMS_TO_TICKS(100);
    /* Compare by elapsed subtraction rather than an absolute deadline: unsigned
     * tick subtraction stays correct across the ~49.7-day tick counter wrap,
     * whereas `tick < deadline` breaks when the deadline wraps past 0. */
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - start) < limit) {
        EventBits_t bits = xEventGroupGetBits(system_events);
        if ((bits & EVT_DISPLAY_IDLE) && uxQueueMessagesWaiting(display_queue) == 0) {
            return true;
        }
        vTaskDelay(poll);
    }
    return false;
}

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
 * OTA download progress callback — switches the e-paper to an "Installing"
 * frame when the download completes. Percentage updates are intentionally
 * not rendered: e-paper 4-gray refresh (~3.5s) is slower than the OTA's
 * progress cadence (~1.5s), so any percent frame paints stale and the queue
 * backs up — a queued "60%" was painting after install on ESP32-S3.
 * Serial log still emits 20% checkpoints for debugging.
 * Accesses file-static display_queue (same translation unit).
 */
static void ota_display_progress_cb(size_t current, size_t total)
{
    static int last_logged_percent = -1;
    static bool install_frame_shown = false;

    /* Reset per-OTA state when a new download begins (current==0) so a
     * subsequent OTA attempt in the same boot session re-fires the frame. */
    if (current == 0) {
        last_logged_percent = -1;
        install_frame_shown = false;
    }

    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    if (percent != last_logged_percent && percent % 20 == 0) {
        last_logged_percent = percent;
        ESP_LOGI(TAG, "OTA download: %d%%", percent);
    }

    /* Switch display to "Installing" stage at 100%. The frame paints during
     * the post-download verify+install (~1s) and the pre-reboot vTaskDelay
     * in ota_check_with_display, which together exceed the e-paper refresh. */
    if (current >= total && total > 0 && !install_frame_shown) {
        install_frame_shown = true;
        display_event_t evt = {};
        evt.type = DISPLAY_EVT_STATUS;
        snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Firmware Update");
        snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Installing");
        snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "Restarting...");
        xQueueSend(display_queue, &evt, pdMS_TO_TICKS(100));
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

    /* Download and install with progress display. The progress callback
     * queues an "Installing / Restarting..." frame when download hits 100%;
     * the pre-reboot delay below gives that frame time to paint fully. */
    ret = ota_manager_download_and_install(&info, ota_display_progress_cb);
    if (ret == ESP_OK) {
        /* 4s covers e-paper 4-gray FAST refresh (~3.5s) plus a small margin.
         * The "Installing / Restarting..." frame was queued at 100% download
         * and is painting during this delay. */
        vTaskDelay(pdMS_TO_TICKS(4000));
        ota_manager_finalize_and_restart(&info);
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

    /* DEVICE_NOT_LINKED attempts are tracked in RTC (s_pairing_attempt_count) so the
     * count survives the esp_restart() that ends each pairing cycle — a per-boot
     * local would reset every restart and never escalate (ESP-6). After MAX awake
     * attempts we switch to escalating deep-sleep naps instead of staying awake. */
    const uint8_t MAX_DEVICE_NOT_LINKED_ATTEMPTS = 2;

    /* Track DEVICE_NOT_REGISTERED retries (3 attempts with 30s delays, then deep sleep) */
    uint8_t registration_error_count = 0;
    const uint8_t MAX_REGISTRATION_RETRIES = 3;

    /* Event-driven main loop: ws_task blocks until an event bit fires.
     * Timer-driven events: health check (30s), heartbeat (30s), OTA (24h).
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

        /* Self-test checkpoint for a probationary image whose boot firmware check
         * didn't get through (server briefly down, or OTA disabled in config):
         * registering on the WebSocket proves the same thing. */
        if (ota_manager_is_pending_verify() && websocket_manager_is_connected()) {
            ota_manager_confirm("websocket registered");
            /* Report the success now: the boot-time report ran while still on
             * probation and skipped, and a light-sleep device may not reboot
             * for weeks. */
            ota_manager_check_on_boot();
        }

        /* --- Server errors (immediate, from WS callback) --- */
        if (bits & EVT_WS_ERROR) {
            /* The server answered (not linked / trial expired / auth), so the
             * firmware works; several branches below restart or deep-sleep, which
             * would otherwise roll back a probationary image. */
            ota_manager_confirm("server responded on websocket");
            ws_error_info_t error_info;
            ws_error_code_t ws_error = websocket_manager_get_pending_error(&error_info);

            if (ws_error == WS_ERROR_DEVICE_NOT_LINKED) {
                s_pairing_attempt_count++;

                if (s_pairing_attempt_count > MAX_DEVICE_NOT_LINKED_ATTEMPTS) {
                    /* Stop cycling boot→WS→~10min awake pairing→restart forever.
                     * Deep-sleep with an escalating nap so an abandoned unlinked
                     * device rests; the user can button-wake to pair anytime (ESP-6). */
                    uint64_t nap_us = pairing_backoff_us(s_pairing_attempt_count);
                    ESP_LOGW(TAG, "Device still not linked after %lu attempts — deep sleeping %llu min",
                             (unsigned long)s_pairing_attempt_count, nap_us / 60000000ULL);
                    display_event_t evt = {};
                    evt.type = DISPLAY_EVT_STATUS;
                    snprintf(evt.data.status.line1, sizeof(evt.data.status.line1), "Pairing needed");
                    snprintf(evt.data.status.line2, sizeof(evt.data.status.line2), "Hold button 3-9 sec");
                    snprintf(evt.data.status.line3, sizeof(evt.data.status.line3), "to re-enter pairing");
                    xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    stop_ws_timers();
                    graceful_shutdown();
                    power_manager_deep_sleep(nap_us);
                    break;
                }

                ESP_LOGI(TAG, "Device not linked — entering pairing mode");
                esp_timer_stop(s_health_timer);
                esp_timer_stop(s_heartbeat_timer);
                websocket_manager_stop();

                xEventGroupClearBits(system_events, EVT_PAIRING_COMPLETE);
                esp_err_t pair_ret = pairing_manager_start(display_queue, system_events);
                if (pair_ret != ESP_OK) {
                    /* Pairing request failed (server/DNS down) without setting
                     * EVT_PAIRING_COMPLETE. Waiting on portMAX_DELAY here would hang
                     * forever with WiFi associated (~80mA) — nap and retry instead,
                     * matching the button-pairing path's return check (ESP-5). */
                    uint64_t nap_us = pairing_backoff_us(s_pairing_attempt_count);
                    ESP_LOGW(TAG, "Pairing request failed — deep sleeping %llu min before retry",
                             nap_us / 60000000ULL);
                    stop_ws_timers();
                    graceful_shutdown();
                    power_manager_deep_sleep(nap_us);
                    break;
                }

                /* pairing_manager_start() returns ESP_OK only once the code is
                 * claimed (it already set EVT_PAIRING_COMPLETE), so this wait
                 * returns immediately. Pairing timeout/expiry restart internally. */
                xEventGroupWaitBits(system_events, EVT_PAIRING_COMPLETE,
                                    pdFALSE, pdTRUE, portMAX_DELAY);

                s_pairing_attempt_count = 0;
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

                ESP_LOGI(TAG, "Trial expired — waiting for QR render before deep sleep");
                /* Block until the QR (and any prior in-flight render) is
                 * fully on the panel. A 4-gray full refresh after a B&W
                 * pre-clear can take ~10s on UC8151D, so allow generous
                 * headroom — the wait returns as soon as truly idle. */
                if (!wait_for_display_idle(30000)) {
                    ESP_LOGW(TAG, "Display did not go idle within 30s — sleeping anyway");
                }
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
            /* User-started flow that ends in a restart — must not roll back. */
            ota_manager_confirm("user started pairing");
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
            /* User-started flow that ends in a restart — must not roll back. */
            ota_manager_confirm("user started WiFi setup");
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
            if (wifi_manager_start_portal(system_events, EVT_WIFI_CONNECTED) != ESP_OK) {
                ESP_LOGE(TAG, "Portal failed to start — restarting");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                break;
            }
            /* Bounded wait: a portal left unattended must not beacon the SoftAP
             * (~80-100mA) forever. 15 min is generous for a user actively
             * provisioning; on timeout, restart back into normal operation
             * (which retries stored networks) rather than drain the cell. */
            {
                EventBits_t pbits = xEventGroupWaitBits(system_events, EVT_WIFI_CONNECTED,
                    pdFALSE, pdTRUE, pdMS_TO_TICKS(15 * 60 * 1000));
                wifi_manager_stop_portal();
                if (!(pbits & EVT_WIFI_CONNECTED)) {
                    ESP_LOGW(TAG, "WiFi provisioning timed out — restarting");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                }
            }

            ESP_LOGI(TAG, "WiFi provisioning complete — restarting");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            break;
        }

        /* --- Connection health check (30s timer) --- */
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
            /* The pending flags stay set until an install attempt starts, so a
             * deferred optional update is still pending when the 60s re-evaluation
             * timer fires. */
            if (websocket_manager_has_pending_firmware_required()) {
                ESP_LOGI(TAG, "Required firmware update — installing now");
                websocket_manager_clear_pending_firmware();
                ota_check_with_display();
            } else if (websocket_manager_has_pending_firmware_optional()) {
                int64_t last_reaction = websocket_manager_get_last_reaction_time();
                int64_t idle_ms = last_reaction > 0 ? (esp_timer_get_time() - last_reaction) / 1000 : INT64_MAX;
                if (idle_ms > 5 * 60 * 1000) {
                    ESP_LOGI(TAG, "Optional firmware update — device idle for %lld min",
                             idle_ms / 60000);
                    websocket_manager_clear_pending_firmware();
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
 * Download an emoji PNG from a URL, run entirely in display_task context.
 * Returns a heap-allocated buffer (caller/display_manager frees it), or NULL on
 * failure. Used both for deep-sleep display restore AND for the normal reaction
 * render path: the download was deliberately moved out of the WebSocket event
 * handler (which would stall all WS RX and open a second TLS session) into
 * display_task, which already does TLS work at its 8KB stack (ESP-M6).
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

    /* Reject a short read against a known Content-Length: a truncated PNG renders
     * as garbage and wastes a display queue slot + heap. (content_length was set
     * to the 64KB cap when the length was unknown, so skip the check in that case.) */
    if (total_read < content_length && content_length != 64 * 1024) {
        ESP_LOGW(TAG, "Emoji download incomplete: %d/%d bytes", total_read, content_length);
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

    /* The "Connection Lost" (and boot) dot animation wakes this task every ~800ms
     * for a ~300ms partial refresh, holding the CPU busy ~40% of the time and
     * defeating light sleep for the whole disconnected period (up to 30 min). On
     * battery, freeze the animation after this many cycles — the last frame stays
     * on the e-paper (zero power) and tickless idle can take over. On USB there's
     * no battery cost, so animate indefinitely (ESP-M2). */
    const int MAX_BATTERY_DOT_CYCLES = 30;   /* ~24s of animation before settling */
    int dot_anim_cycles = 0;

    while (!(xEventGroupGetBits(system_events) & EVT_SHUTDOWN_REQUEST)) {
        /* Shorter timeout during dot animation (boot/disconnected) for updates
         * (~800ms per frame). Otherwise use the normal shutdown-check interval.
         * Once the animation is frozen on battery, revert to the long interval so
         * the task stops waking. */
        bool animating = display_manager_is_dot_animating();
        bool on_battery = (power_manager_get_source() == POWER_SOURCE_BATTERY);
        bool anim_paused = animating && on_battery && dot_anim_cycles >= MAX_BATTERY_DOT_CYCLES;
        TickType_t timeout = (animating && !anim_paused)
            ? pdMS_TO_TICKS(800)
            : pdMS_TO_TICKS(TASK_SHUTDOWN_CHECK_MS);

        /* Signal idle while blocked on the queue. Cleared as soon as an event
         * is received (before the render starts), so wait_for_display_idle()
         * never sees a transient idle window between two queued events. */
        xEventGroupSetBits(system_events, EVT_DISPLAY_IDLE);

        if (xQueueReceive(display_queue, &evt, timeout) == pdTRUE) {
            xEventGroupClearBits(system_events, EVT_DISPLAY_IDLE);
            /* A real event ends the disconnected animation — reset the freeze counter. */
            dot_anim_cycles = 0;
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

            /* Fetch the emoji PNG here in display_task rather than in the WS
             * event handler (ESP-M6). The WS handler only recorded emoji_url; a
             * reaction restored from RTC already carries its image. Download only
             * when a URL is present but no image has been fetched yet. */
            if (evt.type == DISPLAY_EVT_REACTION &&
                evt.data.reaction.emoji_png_data == NULL &&
                evt.data.reaction.emoji_url[0] != '\0') {
                size_t png_size = 0;
                uint8_t *png_data = download_emoji_for_restore(evt.data.reaction.emoji_url, &png_size);
                if (png_data) {
                    evt.data.reaction.emoji_png_data = png_data;
                    evt.data.reaction.emoji_png_size = png_size;
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
        } else if (display_manager_is_dot_animating()) {
            /* Queue timed out during the dot animation. */
            if (on_battery && dot_anim_cycles >= MAX_BATTERY_DOT_CYCLES) {
                /* Frozen on battery — skip the SPI refresh so the CPU can light
                 * sleep; the last dot frame remains on the e-paper (ESP-M2). */
            } else {
                /* Advance the animation. Clear EVT_DISPLAY_IDLE around the partial
                 * SPI refresh and restore it after: without this the queue-timeout
                 * branch leaves the idle bit set, so a concurrent shutdown's
                 * wait_for_display_idle() could pass and cut power mid-transfer
                 * (ESP-M3). */
                xEventGroupClearBits(system_events, EVT_DISPLAY_IDLE);
                display_manager_animate_dots();
                xEventGroupSetBits(system_events, EVT_DISPLAY_IDLE);
                dot_anim_cycles++;
            }
        }
    }

    vTaskDelete(NULL);
}

/**
 * Power task: monitors battery voltage, manages sleep transitions.
 * Loops every 30s (source detection); the battery ADC burst inside
 * power_manager_check() is internally rate-limited to every 5 minutes.
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
            ESP_LOGW(TAG, "Critical battery (%d mV) — graceful shutdown before indefinite deep sleep",
                     power_manager_get_battery_mv());
            s_rtc_state.was_critical_battery = true;
            display_event_t evt = {};
            evt.type = DISPLAY_EVT_LOW_BATTERY;
            xQueueSend(display_queue, &evt, pdMS_TO_TICKS(1000));
            /* Route through the same graceful path the trial-expired branch uses
             * rather than deep-sleeping directly after a fixed 5s. Wait for the
             * warning (and any in-flight render) to fully paint, then
             * graceful_shutdown() drains the queue and hibernates the panel. A
             * 4-gray refresh can run ~10s worst case, so a fixed delay could cut
             * power mid-SPI — corrupting the frame and leaving the panel
             * un-hibernated (higher standby draw) while parked (ESP-3). */
            if (!wait_for_display_idle(30000)) {
                ESP_LOGW(TAG, "Display did not go idle within 30s — sleeping anyway");
            }
            stop_ws_timers();
            graceful_shutdown();
            power_manager_deep_sleep(0);  /* Indefinite — button wake only, prevents drain to death */
            /* Does not return */
        }

        /* Block for 30 seconds or until shutdown requested.
         * The expensive 16-sample battery ADC burst is rate-limited to every
         * 5 minutes inside power_manager_check() (voltage drifts slowly), but the
         * loop itself runs every 30s so a USB unplug/replug is detected within
         * ~30s instead of up to 5 minutes — otherwise light sleep stays disabled
         * for minutes after unplug on the ESP32-S3 (ESP-M7). The per-loop source
         * check is cheap (SOF counter, ~3ms on S3); the ADC burst is not repeated. */
        xEventGroupWaitBits(system_events, EVT_SHUTDOWN_REQUEST,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    }

    vTaskDelete(NULL);
}

/**
 * Button task: handles physical button presses via GPIO ISR.
 * Blocks on semaphore with finite timeout — allows checking shutdown flag.
 *
 * The hold-duration state machine is implemented inline here (rather than in
 * power_manager) because it drives mid-hold display feedback and must re-arm the
 * level-triggered GPIO after release. It translates hold durations into system
 * events: 3-9s → pairing request, 15s+ → WiFi provisioning request.
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
 *
 * ESP32-S3 with native USB Serial/JTAG: light sleep powers down the CPU,
 * which kills the USB controller and causes a bus reset → chip reboot.
 * When USB is connected, skip light sleep (no power saving needed on USB).
 * Battery operation still gets full light sleep benefits (~1-3mA).
 */
static void enable_light_sleep(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    /* S3 native USB-Serial/JTAG is powered down in light sleep → bus reset →
     * reboot. Disable light sleep only while on USB (no power saving needed there). */
    bool on_usb = (power_manager_get_source() == POWER_SOURCE_USB);
    bool light_sleep = !on_usb;
#else
    /* ESP32 / LilyGo T5 uses an external UART bridge (CP210x) that survives light
     * sleep, so keep it enabled regardless of source. Voltage-based source
     * detection can misread a full cell (~4.15V) as USB, which would otherwise
     * disable light sleep for hours (~40-60mA) until the cell sags (ESP-8). */
    bool on_usb = false;
    bool light_sleep = true;
#endif

    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = light_sleep
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    if (light_sleep) {
        ESP_LOGI(TAG, "Auto light sleep enabled with WiFi DTIM power save");
    } else {
        ESP_LOGI(TAG, "USB connected — light sleep disabled (WiFi power save still active)");
    }
    (void)on_usb;
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

    /* Drain any in-flight display render before hibernating the panel.
     * Hibernating mid-refresh would corrupt the frame buffer (symptom:
     * black screen after wake). Caller paths typically already wait via
     * wait_for_display_idle(), so this is normally a no-op. */
    wait_for_display_idle(15000);

    /* Park the e-paper controller in its own deep sleep before the host MCU
     * powers down. Lowers panel standby current and matches GxEPD2's
     * hibernate() pattern. */
    display_manager_hibernate();

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

/* Escalating deep-sleep backoff after a brownout reset, in microseconds.
 * Each successive brownout lengthens the rest period: a healthy cell charging on
 * USB recovers after the first 30s nap, while a failed/absent cell parks at
 * 10-minute cycles — minimal self-discharge — instead of a tight reboot loop.
 * Indexed by the (1-based) brownout count, clamped to the last rung. */
static uint64_t brownout_backoff_us(uint32_t count)
{
    static const uint64_t ladder_s[] = { 30, 60, 120, 300, 600 };
    const size_t n = sizeof(ladder_s) / sizeof(ladder_s[0]);
    uint32_t idx = (count == 0) ? 0 : (count - 1);
    if (idx >= n) idx = n - 1;
    return ladder_s[idx] * 1000000ULL;
}

/* Escalating deep-sleep nap between pairing attempts for an unlinked device, in
 * microseconds, indexed by the (1-based) attempt count and clamped to the last
 * rung. Short at first (the user is likely nearby just after setup), lengthening
 * to 30-minute naps so an abandoned, never-linked device rests instead of burning
 * the cell in back-to-back ~10-minute awake pairing cycles (ESP-6). */
static uint64_t pairing_backoff_us(uint32_t count)
{
    static const uint64_t ladder_min[] = { 5, 15, 30 };
    const size_t n = sizeof(ladder_min) / sizeof(ladder_min[0]);
    uint32_t idx = (count == 0) ? 0 : (count - 1);
    if (idx >= n) idx = n - 1;
    return ladder_min[idx] * 60ULL * 1000000ULL;
}

/* Escalating deep-sleep nap after a WiFi connect fails on an UNATTENDED wake of an
 * already-provisioned device, in microseconds, indexed by the (1-based) failure
 * count and clamped to the last rung. A transient router/AP outage recovers within
 * a minute or two; a longer outage settles into 30-minute naps. Never opens the
 * captive portal on these wakes — the SoftAP beacons at ~80-100mA and would drain
 * the cell to death while nobody is watching (ESP-1). */
static uint64_t wifi_fail_backoff_us(uint32_t count)
{
    static const uint64_t ladder_min[] = { 1, 5, 15, 30 };
    const size_t n = sizeof(ladder_min) / sizeof(ladder_min[0]);
    uint32_t idx = (count == 0) ? 0 : (count - 1);
    if (idx >= n) idx = n - 1;
    return ladder_min[idx] * 60ULL * 1000000ULL;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Slack Reactions E-Paper Display (ESP-IDF) ===");
    ESP_LOGI(TAG, "Boot reason: wake_cause=%d reset_reason=%d "
                  "(esp_reset_reason_t: %d=POWERON %d=SW %d=DEEPSLEEP %d=BROWNOUT)",
             esp_sleep_get_wakeup_cause(), esp_reset_reason(),
             ESP_RST_POWERON, ESP_RST_SW, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT);

    /* 1. Initialize NVS and event loop */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. Load configuration from LittleFS */
    ESP_ERROR_CHECK(config_manager_init());

    /* 2a. One-time self-heal for early ESP32-S3 (custom PCB) units flashed
     * with display_variant pinned to a LilyGo string by the original flash.sh.
     * The OTA system routes by display_variant; serving an ESP32 (Xtensa-LX6)
     * binary to ESP32-S3 (Xtensa-LX7) hardware bricks the device on reboot.
     * Rewrite to the correct string and persist. After flash.sh is fixed,
     * new units arrive with the right string and this becomes a no-op. */
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_DISPLAY_GDEY0213B74
    {
        app_config_t *mut_cfg = config_manager_get_mutable_config();
        /* Skip the self-heal flash write when this boot followed a brownout: the
         * rail is known to be sagging, and a LittleFS write on a marginal supply
         * risks corrupting config. The correction is idempotent and retries on the
         * next healthy boot (ESP-B2). */
        if (esp_reset_reason() != ESP_RST_BROWNOUT &&
            strncmp(mut_cfg->device.display_variant, "custom_pcb_", 11) != 0) {
            ESP_LOGW(TAG,
                     "Self-heal: rewriting display_variant '%s' -> 'custom_pcb_gdey_4g'",
                     mut_cfg->device.display_variant);
            strncpy(mut_cfg->device.display_variant, "custom_pcb_gdey_4g",
                    sizeof(mut_cfg->device.display_variant) - 1);
            mut_cfg->device.display_variant[sizeof(mut_cfg->device.display_variant) - 1] = '\0';
            esp_err_t save_err = config_manager_save();
            if (save_err != ESP_OK) {
                /* In-memory config is corrected; persist failed. Next boot
                 * retries the correction. Don't block boot — log and proceed. */
                ESP_LOGE(TAG, "Self-heal save failed: %s", esp_err_to_name(save_err));
            } else {
                ESP_LOGI(TAG, "Self-heal: display_variant persisted to LittleFS");
            }
        }
    }
#endif

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

    /* 3b. OTA boot bookkeeping — before anything that can deep-sleep or restart.
     * On the first boot after an update the image stays on probation (NOT marked
     * valid) until it reaches the server: the boot firmware check (step 15b) or
     * WebSocket registration confirms it. Until then any reset — crash, watchdog,
     * power loss, deep sleep — boots the previous firmware. Also detects a rolled-back
     * update and runs the post-update crash-loop guard. See ota_manager.cpp. */
    ota_manager_boot_init();

    /* 4. Check wake reason for deep sleep recovery */
    esp_sleep_wakeup_cause_t wake_reason = esp_sleep_get_wakeup_cause();
    bool is_deep_sleep_wake = (wake_reason == ESP_SLEEP_WAKEUP_TIMER ||
                               wake_reason == ESP_SLEEP_WAKEUP_EXT0 ||
                               wake_reason == ESP_SLEEP_WAKEUP_EXT1);

    /* 4a. Brownout-backoff circuit breaker.
     * If the last reset was a brownout, the rail sagged — on this hardware that's
     * almost always WiFi power-up on a low/failing battery (see s_brownout_backoff_count).
     * Charging straight back into wifi_manager_connect() just sags and resets again.
     * Instead, deep-sleep with escalating backoff so the cell can relax or charge.
     * Runs after board_init (step 3) so wake sources are configured; runs BEFORE
     * the normal display init so we don't spend an e-paper full-refresh (itself a
     * current spike) on the very boot we're trying to survive — the panel retains
     * its last image with zero power. Exception: at rung >= 3 we deliberately spend
     * ONE refresh on a last-gasp explanation screen (below). A manual power-cycle
     * clears the backoff. */
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_BROWNOUT) {
        s_brownout_backoff_count++;
        uint64_t backoff_us = brownout_backoff_us(s_brownout_backoff_count);
        ESP_LOGW(TAG, "Brownout reset (#%lu) — backing off %llu s before retrying WiFi "
                      "(low/failing battery or marginal supply)",
                 (unsigned long)s_brownout_backoff_count, backoff_us / 1000000ULL);

        /* Last-gasp explanation screen. Three consecutive brownouts through the
         * escalating ladder is a genuine power problem, not a transient — rungs 1-2
         * also occur on healthy cells recovering on charge or in cold weather, and
         * drawing there would flash a scary warning at devices that recover 30s
         * later. Draw "LOW BATTERY / Charge, then toggle switch" once, so the
         * retained e-paper image explains WHY the device is dark instead of showing
         * a stale splash that makes a dead device look merely stuck. Current
         * budget: a mono refresh peaks ~20-40mA vs the ~300mA WiFi spike that keeps
         * causing these brownouts, and we draw before any WiFi init — survivable
         * even on a sagging cell. If the refresh itself browns out, the flag is
         * still unset (latched only after render returns) and the next rung
         * retries. display_manager_init(true) skips the splash render, so the only
         * pixels spent are our own screen. This path matters most on hardware
         * without battery sense (custom PCB v1.1), where the <15% critical-battery
         * screen never triggers and brownout is the first observable symptom. */
        if (s_brownout_backoff_count >= 3 && !s_drew_dead_battery_screen) {
            if (display_manager_init(true /* preserve = no splash */) == ESP_OK) {
                display_event_t bat_evt = {};
                bat_evt.type = DISPLAY_EVT_LOW_BATTERY;
                display_manager_render(&bat_evt);
                s_drew_dead_battery_screen = true;
                /* The warning replaced whatever content was on screen. Mark it so
                 * the DISPLAY_EVT_CONNECTED handler restores the last reaction (or
                 * shows the connected screen) once the device recovers, instead of
                 * leaving a healthy device parked on "LOW BATTERY". */
                s_rtc_state.showing_connection_lost = true;
                ESP_LOGW(TAG, "Dead-battery explanation screen drawn (brownout rung %lu)",
                         (unsigned long)s_brownout_backoff_count);
            }
        }

        /* Does not return — wakes on the timer (retry) or the button (user). */
        power_manager_deep_sleep(backoff_us);
    } else if (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_EXT) {
        /* Unambiguous fresh/external reset (battery inserted, EN pin) — the user
         * intervened, so start the ladder over.
         *
         * Deliberately NOT clearing on ESP_RST_SW: on the ESP32-S3 a brownout can
         * be MISREPORTED as a software reset (esp-idf issue #17718 — the raw reset
         * code is 0x3 and RTC context is lost when the sag is deep). Treating SW as
         * "manual reset" here would wrongly clear the backoff mid-loop and defeat
         * the breaker. A genuine SW reset (e.g. OTA reboot) is harmless to leave the
         * counter on — it's cleared on the next successful WiFi connect (step 12). */
        s_brownout_backoff_count = 0;
        s_drew_dead_battery_screen = false;
    }
    /* Any other reason (ESP_RST_SW, ESP_RST_WDT — both possible brownout aliases on
     * S3 — plus ESP_RST_DEEPSLEEP from our own backoff wake and OTA reboots) leaves
     * the counter untouched so escalation persists across the recovery cycle and
     * resets only on a confirmed success (step 12) or unambiguous power-on. */

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
    if (is_deep_sleep_wake && power_manager_get_source() == POWER_SOURCE_BATTERY
        && power_manager_has_valid_battery_reading()) {
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
                /* The warning replaced the on-screen content. Mark it so the
                 * DISPLAY_EVT_CONNECTED handler restores the last reaction (or shows
                 * the connected screen) after the battery recovers, instead of
                 * leaving a recharged device parked on "LOW BATTERY". */
                s_rtc_state.showing_connection_lost = true;
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
        /* WiFi connection failed. Deciding whether to open the captive portal:
         * the portal beacons a SoftAP at ~80-100mA with no way to light-sleep, so
         * opening it on an UNATTENDED deep-sleep wake of an already-provisioned
         * device would let a transient router/AP outage drain the cell to death
         * (ESP-1). Only open it when a human is plausibly present — a cold boot
         * (someone just powered/plugged in) or a device with no stored credentials
         * (nothing to retry). Otherwise nap with escalating backoff and retry. */
        uint8_t cred_count = 0;
        (void)wifi_credential_manager_get_all(&cred_count);
        bool have_credentials = (cred_count > 0);

        if (is_deep_sleep_wake && have_credentials) {
            if (s_wifi_fail_backoff_count < UINT32_MAX) s_wifi_fail_backoff_count++;
            uint64_t nap_us = wifi_fail_backoff_us(s_wifi_fail_backoff_count);
            ESP_LOGW(TAG,
                     "WiFi unreachable on unattended wake (attempt %lu) — napping %llu min "
                     "instead of draining the cell in the captive portal",
                     (unsigned long)s_wifi_fail_backoff_count, nap_us / 60000000ULL);
            power_manager_deep_sleep(nap_us);  /* does not return */
        }

        /* No confirm here even with no stored networks: the previous firmware had
         * WiFi moments ago (it downloaded this image), so a probationary image that
         * can't connect — or can't read its credentials — points at the update, and
         * the 10-minute self-test timeout rolls it back. The one case where that's
         * wrong, an image installed by the Arduino firmware, is confirmed in
         * ota_manager_boot_init() instead. */
        ESP_LOGW(TAG, "WiFi connection failed, starting captive portal");

        /* Show WiFi provisioning screen with QR code and instructions */
        display_event_t portal_evt = {};
        portal_evt.type = DISPLAY_EVT_WIFI_PROVISION;
        strncpy(portal_evt.data.wifi_provision.ssid, "pebl-setup",
                sizeof(portal_evt.data.wifi_provision.ssid) - 1);
        strncpy(portal_evt.data.wifi_provision.ip, "192.168.4.1",
                sizeof(portal_evt.data.wifi_provision.ip) - 1);
        xQueueSend(display_queue, &portal_evt, pdMS_TO_TICKS(1000));

        if (wifi_manager_start_portal(system_events, EVT_WIFI_CONNECTED) != ESP_OK) {
            ESP_LOGE(TAG, "Captive portal failed to start — napping 15 min before retry");
            power_manager_deep_sleep(15ULL * 60 * 1000000);  /* does not return */
        }

        /* Portal runs until credentials are provided and WiFi connects, but not
         * forever: even during an attended cold boot the SoftAP drains the battery,
         * so cap the wait. If nobody provisions within the window, nap and re-show
         * the portal on the next wake (a never-provisioned device still lands here
         * because have_credentials stays false). */
        #define PORTAL_PROVISION_TIMEOUT_MS (30UL * 60 * 1000)
        EventBits_t portal_bits = xEventGroupWaitBits(
            system_events, EVT_WIFI_CONNECTED,
            pdFALSE, pdTRUE, pdMS_TO_TICKS(PORTAL_PROVISION_TIMEOUT_MS));

        /* Shut down the captive portal (HTTP server, DNS, SoftAP) — either we
         * connected, or we timed out. Frees ~15KB RAM and stops DNS hijacking. */
        wifi_manager_stop_portal();

        if (!(portal_bits & EVT_WIFI_CONNECTED)) {
            ESP_LOGW(TAG, "Captive portal timed out with no connection — napping 15 min");
            power_manager_deep_sleep(15ULL * 60 * 1000000);  /* does not return */
        }
    }

    /* Survived to here with WiFi up (direct connect or via the portal). Clear both
     * the brownout ladder and the WiFi-fail ladder so a future failure starts fresh
     * at the shortest rung (ESP-B1 — previously cleared only on the direct branch).
     * Also re-arm the dead-battery explanation screen for the next episode. */
    s_brownout_backoff_count = 0;
    s_wifi_fail_backoff_count = 0;
    s_drew_dead_battery_screen = false;

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
        esp_err_t pair_ret = pairing_manager_start(display_queue, system_events);
        if (pair_ret != ESP_OK) {
            /* Pairing request failed (server/DNS down) without setting
             * EVT_PAIRING_COMPLETE. Waiting on portMAX_DELAY here would hang the
             * boot forever with WiFi associated (~80mA) — and unlike the
             * DEVICE_NOT_LINKED path, power_task/ws_task aren't started yet, so
             * nothing would ever escalate it. Nap with escalating backoff and
             * retry on the next boot instead (ESP-5). */
            if (s_pairing_attempt_count < UINT32_MAX) s_pairing_attempt_count++;
            uint64_t nap_us = pairing_backoff_us(s_pairing_attempt_count);
            ESP_LOGW(TAG, "Pairing request failed at boot — deep sleeping %llu min before retry",
                     nap_us / 60000000ULL);
            graceful_shutdown();
            power_manager_deep_sleep(nap_us);  /* does not return */
        }
        xEventGroupWaitBits(system_events, EVT_PAIRING_COMPLETE,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        s_pairing_attempt_count = 0;

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

    /* 15. Check server for available firmware updates.
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
    ota_check_with_display();  // If update found: downloads, verifies, restarts.
                               // A 200 from the server also confirms a probationary image.

    /* 15b. Report the previous update's outcome (success or rollback). After the
     * check so a probationary image has been confirmed first; if the check installed
     * a newer version, finalize already flushed this report before restarting. */
    ota_manager_check_on_boot();

    /* 16. Enable auto light sleep with WiFi DTIM power save */
    enable_light_sleep();

    /* 17. Start remaining tasks — ws_task and power_task need WiFi connected */
    /* ws_task runs inline TLS work — captive-portal-free pairing (esp_http_client)
     * and OTA (esp_https_ota) — whose mbedTLS handshake temporaries alone peak at
     * 4-6KB of caller stack. 6144 overflowed on deep cert chains; 12288 leaves
     * headroom (ESP-7). */
    xTaskCreate(ws_task,      "ws_task",      12288, NULL, 5, &ws_task_handle);
    xTaskCreate(power_task,   "power_task",   4096, NULL, 6, &power_task_handle);

    ESP_LOGI(TAG, "All tasks started — entering event-driven mode");
    /* app_main returns, FreeRTOS scheduler continues running tasks.
     * When all tasks block, tickless idle triggers auto light sleep. */
}
