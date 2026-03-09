#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize timezone manager.
 * Must be called after config_manager_init() and WiFi connection.
 */
esp_err_t timezone_manager_init(void);

/**
 * Fetch timezone from configured source (server GeoIP or ipgeolocation.io)
 * and set the system clock via settimeofday().
 *
 * Returns ESP_OK on success, ESP_FAIL on HTTP or parse error.
 * On failure, existing time (if any) is preserved.
 */
esp_err_t timezone_manager_fetch(void);

/**
 * Check if a timezone sync is needed based on:
 * - Cold boot (always sync)
 * - Never synced before
 * - Sync interval elapsed since last sync
 *
 * If sync is needed, calls timezone_manager_fetch() automatically.
 */
void timezone_manager_sync_if_needed(bool is_cold_boot);

/**
 * Get current timezone offset in seconds (includes DST).
 * Returns 0 if never synced.
 */
int32_t timezone_manager_get_offset_seconds(void);

/**
 * Check if timezone has been successfully synced at least once.
 */
bool timezone_manager_has_synced(void);

#ifdef __cplusplus
}
#endif
