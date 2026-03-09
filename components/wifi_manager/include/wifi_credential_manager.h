#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CRED_MAX_NETWORKS  5
#define WIFI_CRED_MAX_SSID      33
#define WIFI_CRED_MAX_PASS      65

/**
 * Stored WiFi credential with LRU timestamp.
 */
typedef struct {
    char ssid[WIFI_CRED_MAX_SSID];
    char password[WIFI_CRED_MAX_PASS];
    uint32_t last_used;   /* Unix timestamp for LRU ordering */
    bool is_pinned;       /* true = from config.json seed_networks, never evicted */
} wifi_credential_t;

/**
 * Initialize credential manager: load from NVS, merge seed networks from config.
 */
esp_err_t wifi_credential_manager_init(void);

/**
 * Add or update a network credential.
 * Evicts oldest unpinned network if at capacity.
 */
esp_err_t wifi_credential_manager_add(const char *ssid, const char *password);

/**
 * Update LRU timestamp for a network after successful connection.
 */
void wifi_credential_manager_update_last_used(const char *ssid);

/**
 * Get all stored credentials.
 * Returns pointer to internal array and sets count.
 */
const wifi_credential_t *wifi_credential_manager_get_all(uint8_t *count);

/**
 * Clear all NVS-stored credentials (preserves seed networks).
 */
void wifi_credential_manager_clear_nvs(void);

#ifdef __cplusplus
}
#endif
