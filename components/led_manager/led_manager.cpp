/**
 * LED Manager — WS2812B notification LED for the custom PCB v1.1.
 *
 * Uses the ESP-IDF led_strip component (RMT-based) to drive a single
 * WS2812B-2020 on GPIO18. Each reaction notification sets a platform-
 * colored LED that auto-clears after a configurable timeout (default 15 min).
 *
 * RMT transactions are microsecond-duration, so this does not interfere
 * with auto light sleep when idle.
 *
 * Entire component compiles to no-op stubs when CONFIG_LED_ENABLED is not set
 * (i.e., on ESP32 / LilyGo T5 builds).
 */

#include "led_manager.h"

#if CONFIG_LED_ENABLED

#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "LED";

static led_strip_handle_t s_strip = NULL;
static TimerHandle_t s_timeout_timer = NULL;

/**
 * Timer callback: clear the LED when the notification timeout expires.
 * Since this is a one-shot timer, it won't re-fire — no need to stop it.
 * Avoids calling xTimerStop on itself which is undefined in some FreeRTOS ports.
 */
static void timeout_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_strip) {
        led_strip_clear(s_strip);  /* clear() zeros buffer + refreshes via RMT */
    }
    ESP_LOGI(TAG, "Notification LED auto-off after %d min", CONFIG_LED_TIMEOUT_MINUTES);
}

esp_err_t led_manager_init(void)
{
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = CONFIG_LED_GPIO;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;  /* 10 MHz for WS2812 timing */

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start with LED off (clear() zeros buffer + refreshes via RMT) */
    led_strip_clear(s_strip);

    /* Create auto-off timer (dormant until first notification) */
    s_timeout_timer = xTimerCreate(
        "led_timeout",
        pdMS_TO_TICKS((uint32_t)CONFIG_LED_TIMEOUT_MINUTES * 60 * 1000),
        pdFALSE,   /* One-shot timer */
        NULL,
        timeout_callback
    );

    if (!s_timeout_timer) {
        ESP_LOGE(TAG, "Failed to create timeout timer");
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED manager initialized (GPIO %d, brightness %d/255, timeout %d min)",
             CONFIG_LED_GPIO, CONFIG_LED_BRIGHTNESS, CONFIG_LED_TIMEOUT_MINUTES);
    return ESP_OK;
}

void led_manager_notify(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;

    /* Scale color by brightness factor */
    uint8_t scaled_r = (uint8_t)((uint16_t)r * CONFIG_LED_BRIGHTNESS / 255);
    uint8_t scaled_g = (uint8_t)((uint16_t)g * CONFIG_LED_BRIGHTNESS / 255);
    uint8_t scaled_b = (uint8_t)((uint16_t)b * CONFIG_LED_BRIGHTNESS / 255);

    led_strip_set_pixel(s_strip, 0, scaled_r, scaled_g, scaled_b);
    led_strip_refresh(s_strip);

    /* Reset the auto-off timer (extends timeout from now) */
    if (s_timeout_timer) {
        xTimerReset(s_timeout_timer, 0);
    }

    ESP_LOGI(TAG, "Notification LED set to (%d,%d,%d) scaled from (%d,%d,%d)",
             scaled_r, scaled_g, scaled_b, r, g, b);
}

void led_manager_clear(void)
{
    if (!s_strip) return;

    led_strip_clear(s_strip);  /* clear() zeros buffer + refreshes via RMT */

    /* Stop the timer if it's running */
    if (s_timeout_timer) {
        xTimerStop(s_timeout_timer, 0);
    }
}

#else /* !CONFIG_LED_ENABLED */

esp_err_t led_manager_init(void) { return ESP_OK; }
void led_manager_notify(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void led_manager_clear(void) {}

#endif /* CONFIG_LED_ENABLED */
