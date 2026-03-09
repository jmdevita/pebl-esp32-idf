#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Board pin configuration.
 * Populated from Kconfig values so the rest of the codebase
 * never uses #ifdef CONFIG_IDF_TARGET_xxx directly.
 */
typedef struct {
    int button_gpio;
    int adc_battery_channel;
} board_pins_t;

/**
 * Initialize board-specific hardware (ADC calibration, etc).
 * Call once during boot after NVS init.
 */
esp_err_t board_init(void);

/**
 * Get pin configuration for this board variant.
 */
void board_get_pin_config(board_pins_t *pins);

/**
 * Read battery voltage in millivolts.
 * Uses chip-appropriate ADC calibration:
 *   ESP32:    line fitting (eFuse Vref)
 *   ESP32-S3: curve fitting (more accurate)
 *
 * Returns raw ADC millivolt reading before voltage divider compensation.
 * Caller applies the board's voltage divider ratio.
 */
int board_adc_read_battery_mv(void);

/**
 * Acquire PM lock to prevent light sleep during critical SPI operations
 * (e.g., e-paper display refresh). The lock is created once during board_init().
 */
esp_err_t board_acquire_wake_lock(void);

/**
 * Release PM lock to allow light sleep to resume.
 */
esp_err_t board_release_wake_lock(void);

/**
 * Configure deep sleep wake sources for this board.
 *   ESP32:    ext0 (single GPIO, level-triggered)
 *   ESP32-S3: ext1 (bitmask, all-low trigger)
 * Also configures timer wake for quiet hours / scheduled wake.
 */
void board_configure_wake_sources(int button_gpio, uint64_t sleep_duration_us);

#ifdef __cplusplus
}
#endif
