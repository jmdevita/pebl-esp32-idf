#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the WS2812B notification LED.
 * No-op on platforms where CONFIG_LED_ENABLED is not set.
 */
esp_err_t led_manager_init(void);

/**
 * Set LED to a notification color and start/reset the auto-off timer.
 * Each call resets the timer to CONFIG_LED_TIMEOUT_MINUTES from now.
 * No-op on platforms where CONFIG_LED_ENABLED is not set.
 */
void led_manager_notify(uint8_t r, uint8_t g, uint8_t b);

/**
 * Turn off the LED immediately.
 * No-op on platforms where CONFIG_LED_ENABLED is not set.
 */
void led_manager_clear(void);

#ifdef __cplusplus
}
#endif
