#include "board.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BOARD";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static adc_channel_t s_battery_channel = (adc_channel_t)CONFIG_BOARD_ADC_BATTERY_CHANNEL;

/* Number of ADC samples to average for noise rejection (WiFi causes ADC noise) */
#define ADC_SAMPLE_COUNT   16
#define ADC_SAMPLE_DELAY_MS 5

/* PM lock prevents light sleep during critical SPI transactions (e-paper refresh).
 * Created in board_init(), used by display_manager and any other SPI consumer. */
static esp_pm_lock_handle_t s_wake_lock = NULL;

void board_get_pin_config(board_pins_t *pins)
{
    pins->button_gpio = CONFIG_BOARD_BUTTON_GPIO;
    pins->adc_battery_channel = CONFIG_BOARD_ADC_BATTERY_CHANNEL;
}

/**
 * Probe a single ADC channel: configure it, read raw value, return it.
 * Returns -1 on error.
 */
static int probe_adc_channel(adc_channel_t ch)
{
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc_handle, ch, &chan_cfg) != ESP_OK) {
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  /* Let ADC settle after channel switch */
    int raw = 0;
    if (adc_oneshot_read(adc_handle, ch, &raw) != ESP_OK) {
        return -1;
    }
    return raw;
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Initializing board HAL (button=%d, adc_ch=%d, ext0=%s)",
             CONFIG_BOARD_BUTTON_GPIO, CONFIG_BOARD_ADC_BATTERY_CHANNEL,
             CONFIG_BOARD_WAKE_USE_EXT0 ? "yes" : "no");

    /* Initialize ADC for battery monitoring */
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&adc_cfg, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Auto-detect battery ADC channel.
     * LilyGo T5 boards connect the battery voltage divider to either
     * GPIO 35 (ADC1_CH7) or GPIO 36 (ADC1_CH0) depending on revision.
     * Probe both and pick whichever has a valid reading (100-3900 raw).
     * A floating/unconnected pin reads near 0 or near 4095. */
#if CONFIG_IDF_TARGET_ESP32
    int raw_ch0 = probe_adc_channel(ADC_CHANNEL_0);  /* GPIO 36 */
    int raw_ch7 = probe_adc_channel(ADC_CHANNEL_7);  /* GPIO 35 */

    bool ch0_valid = (raw_ch0 >= 100 && raw_ch0 < 4000);
    bool ch7_valid = (raw_ch7 >= 100 && raw_ch7 < 4000);

    if (ch7_valid && !ch0_valid) {
        s_battery_channel = ADC_CHANNEL_7;
    } else if (ch0_valid && !ch7_valid) {
        s_battery_channel = ADC_CHANNEL_0;
    } else if (ch0_valid && ch7_valid) {
        /* Both valid — pick the higher reading (more likely the actual battery) */
        s_battery_channel = (raw_ch7 > raw_ch0) ? ADC_CHANNEL_7 : ADC_CHANNEL_0;
    } else {
        /* Neither valid — fall back to Kconfig default */
        s_battery_channel = (adc_channel_t)CONFIG_BOARD_ADC_BATTERY_CHANNEL;
    }

    ESP_LOGI(TAG, "Battery ADC: ch0(GPIO36)=%d, ch7(GPIO35)=%d → using channel %d",
             raw_ch0, raw_ch7, s_battery_channel);
#else
    s_battery_channel = (adc_channel_t)CONFIG_BOARD_ADC_BATTERY_CHANNEL;
#endif

    /* Configure the selected channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(adc_handle, s_battery_channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize ADC calibration — scheme differs by chip */
#if CONFIG_IDF_TARGET_ESP32
    /* ESP32: line fitting calibration using eFuse Vref */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle);
#elif CONFIG_IDF_TARGET_ESP32S3
    /* ESP32-S3: curve fitting calibration (higher accuracy) */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_battery_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle);
#endif

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration init failed: %s (readings will be uncalibrated)",
                 esp_err_to_name(ret));
        /* Non-fatal: uncalibrated readings still work, just less accurate */
    }

    /* Create PM lock to prevent light sleep during SPI transactions */
    ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "spi_active", &s_wake_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create PM lock: %s (light sleep may interrupt SPI)",
                 esp_err_to_name(ret));
    }

    return ESP_OK;
}

int board_adc_read_battery_mv(void)
{
    /* Multi-sample averaging for noise rejection.
     * WiFi radio activity causes significant ADC noise on ESP32;
     * averaging 16 samples reduces jitter to ~±20mV. */
    int total = 0;
    int valid_count = 0;

    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t ret = adc_oneshot_read(adc_handle, s_battery_channel, &raw);
        if (ret == ESP_OK) {
            total += raw;
            valid_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_DELAY_MS));
    }

    if (valid_count == 0) {
        ESP_LOGE(TAG, "ADC read failed: no valid samples");
        return -1;
    }

    int raw_avg = total / valid_count;

    if (adc_cali_handle != NULL) {
        int voltage_mv = 0;
        esp_err_t ret = adc_cali_raw_to_voltage(adc_cali_handle, raw_avg, &voltage_mv);
        if (ret == ESP_OK) {
            return voltage_mv;
        }
        ESP_LOGW(TAG, "ADC calibration conversion failed, returning raw");
    }

    /* Fallback: approximate conversion without calibration.
     * ADC_ATTEN_DB_12 range is ~0-2450mV, 12-bit resolution. */
    return (raw_avg * 2450) / 4095;
}

esp_err_t board_acquire_wake_lock(void)
{
    if (s_wake_lock) {
        return esp_pm_lock_acquire(s_wake_lock);
    }
    return ESP_OK;
}

esp_err_t board_release_wake_lock(void)
{
    if (s_wake_lock) {
        return esp_pm_lock_release(s_wake_lock);
    }
    return ESP_OK;
}

void board_configure_wake_sources(int button_gpio, uint64_t sleep_duration_us)
{
    /* Timer wake for scheduled wake-up (quiet hours, periodic check) */
    if (sleep_duration_us > 0) {
        esp_sleep_enable_timer_wakeup(sleep_duration_us);
    }

    /* Button wake from deep sleep */
#if CONFIG_BOARD_WAKE_USE_EXT0
    /* ESP32: ext0 wake on single GPIO, trigger on LOW level */
    esp_sleep_enable_ext0_wakeup((gpio_num_t)button_gpio, 0);
#else
    /* ESP32-S3: ext1 wake on GPIO bitmask, trigger when all selected GPIOs are LOW */
    esp_sleep_enable_ext1_wakeup(1ULL << button_gpio, ESP_EXT1_WAKEUP_ALL_LOW);
#endif

    ESP_LOGI(TAG, "Wake sources configured: timer=%llu us, button=GPIO %d (%s)",
             sleep_duration_us, button_gpio,
             CONFIG_BOARD_WAKE_USE_EXT0 ? "ext0" : "ext1");
}
