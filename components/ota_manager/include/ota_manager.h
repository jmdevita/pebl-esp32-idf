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
 * Early-boot OTA bookkeeping. Call once, right after NVS/config/board init and
 * before anything that can deep-sleep or restart.
 *
 *  - Detects whether this image is on probation (first boot after an OTA,
 *    ESP_OTA_IMG_PENDING_VERIFY). A probationary image is NOT marked valid here:
 *    it must pass the self-test (ota_manager_confirm) or the bootloader rolls it
 *    back on the next reset. A 10-minute timer rolls it back explicitly if the
 *    self-test never passes while the device stays awake.
 *  - Detects that the previous OTA was rolled back (the image we installed is
 *    not the one running) and remembers that version as failed, so it is not
 *    reinstalled in a loop.
 *  - Crash-loop guard: during the first hour after an OTA, three crash resets
 *    (panic / watchdog) invalidate this image so the previous slot boots.
 *  - An image installed by a different firmware (Arduino → IDF migration) is
 *    confirmed immediately: rolling back to it would only loop.
 */
void ota_manager_boot_init(void);

/**
 * Self-test passed: mark the running image valid and cancel rollback.
 * No-op unless the image is on probation. Safe to call from any task, any
 * number of times. `reason` is logged.
 *
 * Checkpoints that call this: a 200 from the firmware-check endpoint (the
 * device can reach the server and fetch a fix), WebSocket registration or a
 * server error message, and user-initiated flows (button pairing / WiFi setup)
 * where the device must survive a restart to finish what the user started.
 */
void ota_manager_confirm(const char *reason);

/**
 * True while the running image is on probation (not yet confirmed).
 */
bool ota_manager_is_pending_verify(void);

/**
 * Report the last OTA outcome (success or rollback) stored in NVS to the
 * server, once network is up. Skips while the image is still on probation —
 * success is only reported once confirmed. Entries are kept for retry until
 * the server acknowledges.
 */
void ota_manager_check_on_boot(void);

/**
 * Check server for available firmware update.
 * Returns ESP_OK if update available and info populated. A 200 response also
 * confirms a probationary image (see ota_manager_confirm). An image (version +
 * sha256) that has already failed OTA_MAX_ATTEMPTS_PER_VERSION times is skipped.
 */
esp_err_t ota_manager_check_for_update(firmware_info_t *info);

/**
 * Download and install firmware update.
 * Uses esp_https_ota for atomic partition swap. Before writing past the image
 * header, checks the image's embedded project name and version match this
 * firmware and the advertised version. Verifies SHA-256 hash and ECDSA
 * signature before finalizing. Failures are reported to the server; integrity
 * failures (header/hash/signature) also count toward the per-version attempt
 * limit.
 */
esp_err_t ota_manager_download_and_install(const firmware_info_t *info,
                                            ota_progress_cb_t progress_cb);

/**
 * Get current OTA status.
 */
ota_status_t ota_manager_get_status(void);

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
void ota_manager_finalize_and_restart(const firmware_info_t *info);

#ifdef __cplusplus
}
#endif
