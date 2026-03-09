#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum lengths for string fields */
#define CONFIG_MAX_STRING_LEN      128
#define CONFIG_MAX_SSID_LEN        33   /* 32 chars + null (WiFi spec) */
#define CONFIG_MAX_PASSWORD_LEN    65   /* 64 chars + null (WPA2 spec) */
#define CONFIG_MAX_SEED_NETWORKS   5
#define CONFIG_MAX_API_KEY_LEN     257  /* 256 chars + null */

/**
 * WiFi seed network entry (pre-configured, pinned — never evicted by LRU).
 */
typedef struct {
    char ssid[CONFIG_MAX_SSID_LEN];
    char password[CONFIG_MAX_PASSWORD_LEN];
} wifi_seed_network_t;

/**
 * Application configuration — mirrors the Arduino AppConfig struct.
 * Loaded from /littlefs/config.json with defaults applied for missing fields.
 * Same JSON format as the Arduino version for config.json compatibility.
 */
typedef struct {
    struct {
        char id[CONFIG_MAX_STRING_LEN];
        char name[CONFIG_MAX_STRING_LEN];
        char display_variant[CONFIG_MAX_STRING_LEN];
    } device;

    struct {
        uint32_t timeout_ms;
        bool force_high_power;
        uint8_t escalation_threshold;
        uint8_t max_failed_wakes;
        wifi_seed_network_t seed_networks[CONFIG_MAX_SEED_NETWORKS];
        uint8_t seed_network_count;
    } wifi;

    struct {
        char host[CONFIG_MAX_STRING_LEN];
        uint16_t port;
        char path[CONFIG_MAX_STRING_LEN];
        bool use_ssl;
        uint16_t reconnect_jitter_max_sec;  /* 0 = disabled (B2C default), set 30-60 for fleet deployments */
    } server;

    struct {
        uint8_t rotation;
    } display;

    struct {
        char auth_token[CONFIG_MAX_STRING_LEN];
        char encryption[16];  /* "ecdh" or "none" */
    } security;

    struct {
        bool sleep_enabled;
        uint8_t sleep_duration_min;
    } power;

    struct {
        char default_level[8];  /* "ERROR", "WARN", "INFO", "DEBUG", "TEST" */
        bool enable_test_commands;
    } logging;

    struct {
        uint8_t sync_interval_hours;
        char source[16];  /* "server" or "ipgeolocation" */
        char ipgeolocation_api_key[CONFIG_MAX_API_KEY_LEN];
        bool update_server;
    } timezone;

    struct {
        uint8_t start_hour;
        uint8_t end_hour;
        uint8_t sleep_multiplier;
    } quiet_hours;

    struct {
        bool skip_refresh_on_no_message;
    } display_policy;

    struct {
        bool enabled;  /* Enable/disable OTA firmware updates (default: true) */
    } ota;
} app_config_t;

/**
 * Initialize LittleFS and load configuration from /config.json.
 * Sets defaults for any missing fields. Auto-generates device ID from eFuse MAC if empty.
 */
esp_err_t config_manager_init(void);

/**
 * Get read-only pointer to the current configuration.
 * Valid after config_manager_init() returns ESP_OK.
 */
const app_config_t *config_manager_get_config(void);

/**
 * Get mutable pointer to configuration (for pairing flow and test commands).
 */
app_config_t *config_manager_get_mutable_config(void);

/**
 * Save current configuration to /config.json.
 */
esp_err_t config_manager_save(void);

/**
 * Reset configuration to defaults (does not save to flash).
 */
void config_manager_reset(void);

/**
 * Serialize current config to JSON string.
 * Caller must free() the returned buffer.
 * Returns NULL on error.
 */
char *config_manager_to_json(void);

#ifdef __cplusplus
}
#endif
