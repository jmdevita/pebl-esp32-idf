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
    /* Read battery voltage */
    int raw_mv = board_adc_read_battery_mv();
    if (raw_mv < 0) {
        return false;
    }
    s_battery_mv = raw_mv * BATTERY_VOLTAGE_DIVIDER_RATIO;
    update_sense_availability();

    /* Update power source.
     * ESP32-S3: direct USB host detection via SOF counter.
     * ESP32: voltage threshold with hysteresis to avoid flip-flopping. */
    bool transition = false;
#if CONFIG_IDF_TARGET_ESP32S3
    power_source_t new_source = usb_host_detected() ? POWER_SOURCE_USB : POWER_SOURCE_BATTERY;
    if (new_source != s_source) {
        s_source = new_source;
        transition = true;
        ESP_LOGI(TAG, "Power source: %s (%d mV)",
                 s_source == POWER_SOURCE_USB ? "USB" : "battery", s_battery_mv);
    }
#else
    if (s_source == POWER_SOURCE_BATTERY && s_battery_mv > BATTERY_USB_HYSTERESIS) {
        s_source = POWER_SOURCE_USB;
        transition = true;
        ESP_LOGI(TAG, "Power source: USB detected (%d mV)", s_battery_mv);
    } else if (s_source == POWER_SOURCE_USB && s_battery_mv < BATTERY_USB_THRESHOLD) {
        s_source = POWER_SOURCE_BATTERY;
        transition = true;
        ESP_LOGI(TAG, "Power source: battery (%d mV, %d%%)",
                 s_battery_mv, voltage_to_percent(s_battery_mv));
    }
#endif

    /* CPU frequency and light sleep on power source transition.
     * Battery: cap at 160MHz + enable light sleep (~1-3mA average).
     * USB: full 240MHz + disable light sleep (native USB Serial/JTAG on
     *      ESP32-S3 is powered down during light sleep, causing a bus reset
     *      and chip reboot). No power saving needed on USB anyway. */
    if (s_source != s_last_freq_source) {
        bool on_battery = (s_source == POWER_SOURCE_BATTERY);
        esp_pm_config_t pm_cfg = {
            .max_freq_mhz = on_battery ? 160 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = 80,
            .light_sleep_enable = on_battery
        };
        esp_err_t pm_ret = esp_pm_configure(&pm_cfg);
        if (pm_ret == ESP_OK) {
            ESP_LOGI(TAG, "CPU freq cap set to %d MHz, light sleep %s (%s)",
                     pm_cfg.max_freq_mhz,
                     on_battery ? "enabled" : "disabled",
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

/**
 * Handle physical button press with tiered hold duration actions.
 * Same tiered behavior as the Arduino version:
 *
 *   < 3 seconds:  Short press — show status (battery, connection)
 *   3-9 seconds:  Enter pairing mode for multi-platform linking
 *   15+ seconds:  Enter WiFi provisioning portal (factory reset WiFi)
 *
 * Returns a button_result_t indicating what action the caller should take.
 * The caller (button_task in app_main) handles display rendering and
 * system events, avoiding a circular dependency between power_manager
 * and display_manager.
 */
button_result_t power_manager_handle_button(void)
{
    button_result_t result = {
        .action = BUTTON_ACTION_NONE,
        .battery_percent = power_manager_get_battery_percent(),
        .battery_mv = s_battery_mv,
        .source = s_source,
        .has_auth_token = false,
    };

    board_pins_t pins;
    board_get_pin_config(&pins);

    int64_t press_start_us = esp_timer_get_time();

    /* Measure hold duration (up to 17s max) by polling GPIO level.
     * Button is active LOW — held = gpio reads 0. */
    while (gpio_get_level((gpio_num_t)pins.button_gpio) == 0) {
        int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;
        if (held_ms > 17000) {
            break;  /* Safety limit */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int64_t press_duration_ms = (esp_timer_get_time() - press_start_us) / 1000;
    ESP_LOGI(TAG, "Button held for %lld ms", press_duration_ms);

    if (press_duration_ms >= 15000) {
        /* 15+ seconds: WiFi provisioning portal (factory reset WiFi) */
        ESP_LOGI(TAG, "Extra-long press — WiFi provisioning requested");
        result.action = BUTTON_ACTION_WIFI_PORTAL;

    } else if (press_duration_ms >= 3000) {
        /* 3-9 seconds: Enter pairing mode for multi-platform linking */
        const app_config_t *cfg = config_manager_get_config();
        result.has_auth_token = (cfg->security.auth_token[0] != '\0');

        if (result.has_auth_token) {
            ESP_LOGI(TAG, "Medium press — pairing mode requested");
            result.action = BUTTON_ACTION_ENTER_PAIRING;
        } else {
            ESP_LOGW(TAG, "Medium press but no auth token — cannot pair");
            result.action = BUTTON_ACTION_SHOW_STATUS;
        }

    } else {
        /* Short press: show status */
        ESP_LOGI(TAG, "Short press — show status");
        result.action = BUTTON_ACTION_SHOW_STATUS;
    }

    return result;
}

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
