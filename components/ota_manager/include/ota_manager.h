#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA update status.
 */
typedef enum {
    OTA_STATUS_IDLE,
    OTA_STATUS_CHECKING,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_VERIFYING,
    OTA_STATUS_INSTALLING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED,
    OTA_STATUS_ROLLBACK,
} ota_status_t;

/**
 * Firmware update metadata from server.
 */
typedef struct {
    char version[32];
    char download_url[256];
    char sha256_hash[65];
    char signature[256];
    uint32_t size;
    bool required;
    char changelog[256];
} firmware_info_t;

/**
 * Progress callback for OTA downloads.
 */
typedef void (*ota_progress_cb_t)(size_t current, size_t total);

/**
 * Mark current firmware as valid immediately after basic init (before WiFi).
 * Must be called early in boot to prevent OTA rollback if WiFi blocks in
 * captive portal. Mirrors Arduino's checkBootValidation() timing in setup().
 * Only marks valid if partition state is ESP_OTA_IMG_PENDING_VERIFY.
 */
void ota_manager_mark_valid_on_boot(void);

/**
 * Check for OTA update on boot.
 * Non-blocking: requires WiFi — only handles deferred server reporting.
 * Firmware was already marked valid early in boot by ota_manager_mark_valid_on_boot().
 */
void ota_manager_check_on_boot(void);

/**
 * Check server for available firmware update.
 * Returns ESP_OK if update available and info populated.
 */
esp_err_t ota_manager_check_for_update(firmware_info_t *info);

/**
 * Download and install firmware update.
 * Uses esp_https_ota for atomic partition swap.
 * Verifies SHA-256 hash and ECDSA signature before finalizing.
 */
esp_err_t ota_manager_download_and_install(const firmware_info_t *info,
                                            ota_progress_cb_t progress_cb);

/**
 * Get current OTA status.
 */
ota_status_t ota_manager_get_status(void);

/**
 * Mark current boot as valid (prevents automatic rollback).
 * Call after successful boot validation.
 */
esp_err_t ota_manager_mark_valid(void);

/**
 * Full OTA cycle: check for update, download, verify, install, restart.
 * Used for both required (immediate) and optional (idle) firmware updates.
 * Returns ESP_OK on successful update (device will restart).
 */
esp_err_t ota_manager_check_and_update(void);

/**
 * Store OTA version transition in NVS and restart into new firmware.
 * Call after successful ota_manager_download_and_install() when managing
 * the OTA flow manually (e.g., to show display progress between stages).
 * Does not return — calls esp_restart().
 */
void ota_manager_finalize_and_restart(const char *new_version);

/**
 * Report OTA boot validation result to server.
 * Sends old_version → new_version status stored in NVS during update.
 * Called automatically by ota_manager_check_on_boot() after validation.
 */
void ota_manager_report_boot_status(void);

#ifdef __cplusplus
}
#endif
