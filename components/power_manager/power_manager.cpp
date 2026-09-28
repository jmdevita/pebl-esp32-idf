/**
 * Power Manager — ESP-IDF port of Arduino battery monitoring + sleep management.
 *
 * Key changes from Arduino:
 * - FreeRTOS software timer replaces loop()-based polling
 * - board_acquire/release_wake_lock() prevents light sleep during SPI transactions
 * - Deep sleep only for: critical battery (<15%), extended WiFi loss
 * - Normal idle state: auto light sleep with WiFi DTIM maintained (~1-3mA)
 */

#include "power_manager.h"
#include "board.h"
#include "config_manager.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/usb_serial_jtag_struct.h"
#endif

static const char *TAG = "POWER";

#if CONFIG_IDF_TARGET_ESP32S3
/**
 * Detect USB host connection by checking the USB Serial/JTAG peripheral's
 * SOF (Start of Frame) counter. A USB host sends SOF packets every 1ms.
 * If the frame counter increments between two reads, a host is connected.
 *
 * This approach requires no driver init, no TinyUSB, and no VBUS sense GPIO.
 * The USB Serial/JTAG peripheral is always clocked on ESP32-S3.
 */
static bool usb_host_detected(void)
{
    uint32_t frame1 = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    vTaskDelay(pdMS_TO_TICKS(3));
    uint32_t frame2 = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    return (frame2 != frame1);
}
#endif

/* Track the last power source to detect transitions and reconfigure
 * CPU frequency only when the source actually changes. */
static power_source_t s_last_freq_source = POWER_SOURCE_UNKNOWN;

static power_source_t s_source = POWER_SOURCE_UNKNOWN;
static int s_battery_mv = 0;
/* True when the ADC reading is plausible enough to trust as a real battery
 * voltage. Always true on ESP32 (LilyGo T5 has a hardware divider). On
 * ESP32-S3 it tracks whether the reading is above BATTERY_SENSE_FLOOR_MV —
 * v1.1 of the custom PCB has no divider, so this stays false there. */
static bool s_battery_sense_available = true;

/**
 * Update s_battery_sense_available based on the latest reading.
 * On ESP32 this is a no-op (LilyGo T5 always has sense). On ESP32-S3 it
 * latches the result of comparing s_battery_mv against the sanity floor —
 * a stuck-low reading indicates no battery divider on the PCB.
 */
static void update_sense_availability(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    s_battery_sense_available = (s_battery_mv >= BATTERY_SENSE_FLOOR_MV);
#else
    s_battery_sense_available = true;
#endif
}

/**
 * Map battery voltage to percentage using LiPo discharge curve.
 * Same mapping as Arduino version for consistent UX.
 */
static uint8_t voltage_to_percent(int mv)
{
    if (mv >= BATTERY_LIPO_MAX_MV) return 100;
    if (mv >= 4100) return 90;
    if (mv >= 4000) return 80;
    if (mv >= 3900) return 70;
    if (mv >= 3800) return 60;
    if (mv >= 3700) return 50;
    if (mv >= 3600) return 40;
    if (mv >= BATTERY_LOW_MV) return 30;
    if (mv >= 3400) return 20;
    if (mv >= BATTERY_CRITICAL_MV) return 15;
    if (mv >= 3200) return 10;
    if (mv >= 3100) return 5;
    return 0;
}


esp_err_t power_manager_init(void)
{
    /* Wake lock is now managed by the board component (board_acquire/release_wake_lock)
     * to avoid circular dependency between power_manager and display_manager. */

    /* Initial battery reading */
    int raw_mv = board_adc_read_battery_mv();
    if (raw_mv < 0) {
        ESP_LOGW(TAG, "ADC read failed on init, defaulting to 0 mV");
        raw_mv = 0;
    }
    s_battery_mv = raw_mv * BATTERY_VOLTAGE_DIVIDER_RATIO;
    update_sense_availability();

    /* Determine initial power source.
     * ESP32-S3 (custom PCB): detect USB host via SOF frame counter — reliable
     * regardless of battery charge level.
     * ESP32 (LilyGo T5): infer from battery voltage threshold (no USB sense). */
#if CONFIG_IDF_TARGET_ESP32S3
    s_source = usb_host_detected() ? POWER_SOURCE_USB : POWER_SOURCE_BATTERY;
#else
    if (s_battery_mv > BATTERY_USB_THRESHOLD) {
        s_source = POWER_SOURCE_USB;
    } else if (s_battery_mv > BATTERY_NO_BATTERY_MIN && s_battery_mv < BATTERY_NO_BATTERY_MAX) {
        s_source = POWER_SOURCE_BATTERY;
    } else {
        s_source = POWER_SOURCE_UNKNOWN;
    }
#endif

    ESP_LOGI(TAG, "Power init: %d mV, %d%%, source=%s",
             s_battery_mv, voltage_to_percent(s_battery_mv),
             s_source == POWER_SOURCE_USB ? "USB" :
             s_source == POWER_SOURCE_BATTERY ? "battery" : "unknown");

    if (!s_battery_sense_available) {
        ESP_LOGI(TAG, "Battery sense unavailable (reading %d mV < %d mV floor) — "
                      "low-battery monitoring disabled.",
                 s_battery_mv, BATTERY_SENSE_FLOOR_MV);
    }

    return ESP_OK;
}

bool power_manager_check(EventGroupHandle_t system_events)
{
    int64_t now = esp_timer_get_time();
    bool transition = false;

    /* Full battery ADC sampling (16 samples × 5ms ≈ 80ms) is comparatively
     * expensive and battery voltage drifts slowly (~1mV/min under light load),
     * so read it at most every 5 minutes even though power_task now loops more
     * frequently to catch USB transitions quickly (ESP-M7). */
    static int64_t s_last_battery_read_us = 0;
    bool do_battery_read = (s_last_battery_read_us == 0) ||
                           (now - s_last_battery_read_us >= 5LL * 60 * 1000000);
    if (do_battery_read) {
        int raw_mv = board_adc_read_battery_mv();
        if (raw_mv >= 0) {
            s_battery_mv = raw_mv * BATTERY_VOLTAGE_DIVIDER_RATIO;
            update_sense_availability();
        }
        s_last_battery_read_us = now;
    }

    /* Update power source.
     * ESP32-S3: direct USB host detection via SOF counter — cheap (~3ms), so run
     *   it every call so an unplug/replug is noticed within the power_task loop
     *   interval (~30s) instead of only on the 5-minute battery cadence. This is
     *   what lets light sleep re-enable soon after unplug on the S3 (ESP-M7).
     * ESP32: voltage threshold with hysteresis — only meaningful right after a
     *   battery read refreshed s_battery_mv. */
#if CONFIG_IDF_TARGET_ESP32S3
    power_source_t new_source = usb_host_detected() ? POWER_SOURCE_USB : POWER_SOURCE_BATTERY;
    if (new_source != s_source) {
        s_source = new_source;
        transition = true;
        ESP_LOGI(TAG, "Power source: %s (%d mV)",
                 s_source == POWER_SOURCE_USB ? "USB" : "battery", s_battery_mv);
    }
#else
    if (do_battery_read) {
        if (s_source == POWER_SOURCE_UNKNOWN) {
            /* Resolve an indeterminate source the same way init does. The
             * hysteresis rules below only transition between BATTERY and USB, so
             * without this an ESP32 that booted into UNKNOWN would stay UNKNOWN
             * forever (ESP-8). */
            if (s_battery_mv > BATTERY_USB_THRESHOLD) {
                s_source = POWER_SOURCE_USB;
                transition = true;
            } else if (s_battery_mv > BATTERY_NO_BATTERY_MIN && s_battery_mv < BATTERY_NO_BATTERY_MAX) {
                s_source = POWER_SOURCE_BATTERY;
                transition = true;
            }
            if (transition) {
                ESP_LOGI(TAG, "Power source resolved: %s (%d mV)",
                         s_source == POWER_SOURCE_USB ? "USB" : "battery", s_battery_mv);
            }
        } else if (s_source == POWER_SOURCE_BATTERY && s_battery_mv > BATTERY_USB_HYSTERESIS) {
            s_source = POWER_SOURCE_USB;
            transition = true;
            ESP_LOGI(TAG, "Power source: USB detected (%d mV)", s_battery_mv);
        } else if (s_source == POWER_SOURCE_USB && s_battery_mv < BATTERY_USB_THRESHOLD) {
            s_source = POWER_SOURCE_BATTERY;
            transition = true;
            ESP_LOGI(TAG, "Power source: battery (%d mV, %d%%)",
                     s_battery_mv, voltage_to_percent(s_battery_mv));
        }
    }
#endif

    /* CPU frequency and light sleep on power source transition.
     * Battery: cap at 160MHz + enable light sleep (~1-3mA average). */
    if (s_source != s_last_freq_source) {
        bool on_battery = (s_source == POWER_SOURCE_BATTERY);
#if CONFIG_IDF_TARGET_ESP32S3
        /* S3 native USB-Serial/JTAG is powered down during light sleep, causing a
         * bus reset and chip reboot — so disable light sleep while on USB. */
        bool light_sleep = on_battery;
#else
        /* ESP32 / LilyGo T5 uses an external UART bridge (CP210x) that survives
         * light sleep, so keep light sleep enabled regardless of the detected
         * source. Otherwise a fully-charged cell resting at ~4.15V is misread as
         * USB and light sleep stays disabled (~40-60mA) for hours until the cell
         * sags below the threshold — which the disabled sleep only hastens (ESP-8). */
        bool light_sleep = true;
#endif
        esp_pm_config_t pm_cfg = {
            .max_freq_mhz = on_battery ? 160 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = 80,
            .light_sleep_enable = light_sleep
        };
        esp_err_t pm_ret = esp_pm_configure(&pm_cfg);
        if (pm_ret == ESP_OK) {
            ESP_LOGI(TAG, "CPU freq cap set to %d MHz, light sleep %s (%s)",
                     pm_cfg.max_freq_mhz,
                     light_sleep ? "enabled" : "disabled",
                     on_battery ? "battery" : "USB");
        }
        s_last_freq_source = s_source;
    }

    /* Critical battery detection moved to power_task (app_main.cpp) so it can
     * show the "LOW BATTERY / PLEASE CHARGE" screen before entering deep sleep.
     * power_manager_check() no longer sleeps directly — the caller is responsible
     * for checking power_manager_is_critical_battery() and acting on it. */

    ESP_LOGD(TAG, "Battery: %d mV (%d%%), source: %s",
             s_battery_mv, voltage_to_percent(s_battery_mv),
             s_source == POWER_SOURCE_USB ? "USB" : "battery");

    return transition;
}

bool power_manager_is_critical_battery(void)
{
    /* Without a working battery divider we cannot tell low from full — never
     * trigger the critical-battery shutdown path on those boards (custom PCB v1.1). */
    if (!s_battery_sense_available) {
        return false;
    }
    return s_source == POWER_SOURCE_BATTERY && s_battery_mv < BATTERY_CRITICAL_MV;
}

bool power_manager_has_valid_battery_reading(void)
{
    return s_battery_sense_available;
}

/* power_manager_handle_button() removed: button_task in app_main.cpp implements
 * the hold-duration state machine inline (it needs to drive mid-hold display
 * feedback and level-triggered GPIO re-arming), so this parallel copy was dead
 * code that could only drift out of sync. */

int power_manager_get_battery_mv(void)
{
    return s_battery_mv;
}

uint8_t power_manager_get_battery_percent(void)
{
    return voltage_to_percent(s_battery_mv);
}

power_source_t power_manager_get_source(void)
{
    return s_source;
}

void power_manager_deep_sleep(uint64_t duration_us)
{
    /* WiFi fallback mode override: use 60-minute intervals so the device
     * periodically retries WiFi rather than sleeping for potentially longer
     * quiet-hours or resilience-triggered durations. */
    if (wifi_manager_is_fallback_mode()) {
        const uint64_t fallback_us = 60ULL * 60 * 1000000;  /* 60 minutes */
        if (duration_us > fallback_us) {
            ESP_LOGI(TAG, "WiFi fallback — capping sleep to 60 min (was %llu min)",
                     duration_us / 60000000ULL);
            duration_us = fallback_us;
        }
    }

    /* Clean up WiFi before deep sleep to avoid stale AP associations.
     * WebSocket cleanup is handled by the caller (graceful_shutdown in
     * app_main.cpp) for most paths. This ensures WiFi is always stopped
     * even for paths that call deep_sleep directly (quiet hours, critical battery). */
    wifi_manager_disconnect();

    board_pins_t pins;
    board_get_pin_config(&pins);
    board_configure_wake_sources(pins.button_gpio, duration_us);

    ESP_LOGI(TAG, "Entering deep sleep for %llu ms", duration_us / 1000);
    esp_deep_sleep_start();
    /* Does not return */
}

esp_err_t power_manager_acquire_wake_lock(void)
{
    return board_acquire_wake_lock();
}

esp_err_t power_manager_release_wake_lock(void)
{
    return board_release_wake_lock();
}
