/**
 * Configuration Manager — ESP-IDF port of Arduino ConfigManager.
 *
 * Replaces:
 *   ArduinoJson  → cJSON (built into ESP-IDF)
 *   LittleFS.h   → esp_littlefs VFS mount + standard fopen/fread
 *   String       → char[] with fixed-size buffers
 *
 * Preserves:
 *   Same config.json format, same defaults, same validation logic.
 *   Config files from the Arduino version work without modification.
 */

#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_littlefs.h"  /* From joltwallet/littlefs managed component */
#include "cJSON.h"

static const char *TAG = "CONFIG";
static const char *CONFIG_PATH = "/littlefs/config.json";
static const size_t MAX_CONFIG_SIZE = 4096;

static app_config_t s_config;
static bool s_loaded = false;

/* Forward declarations */
static void set_defaults(void);
static esp_err_t load_from_json(const char *json_str);
static void auto_generate_device_id(void);

/* Helper: safe string copy into fixed buffer */
static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
    if (src) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

/* Helper: get string from cJSON object with default */
static const char *json_get_string(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return def;
}

/* Helper: get int from cJSON object with default */
static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return def;
}

/* Helper: get bool from cJSON object with default */
static bool json_get_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return def;
}

static void set_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));

    /* Device */
    safe_strcpy(s_config.device.name, sizeof(s_config.device.name), "ESP32 Device");

    /* WiFi */
    s_config.wifi.timeout_ms = 15000;
    s_config.wifi.force_high_power = false;
    s_config.wifi.escalation_threshold = 3;
    s_config.wifi.max_failed_wakes = 20;
    s_config.wifi.seed_network_count = 0;

    /* Server */
    s_config.server.port = 443;
    safe_strcpy(s_config.server.path, sizeof(s_config.server.path), "/ws-stream");
    s_config.server.use_ssl = true;
    s_config.server.reconnect_jitter_max_sec = 0;

    /* Display */
    s_config.display.rotation = 1;

    /* Security */
    safe_strcpy(s_config.security.encryption, sizeof(s_config.security.encryption), "ecdh");

    /* Power */
    s_config.power.sleep_enabled = true;
    s_config.power.sleep_duration_min = 1;

    /* Logging */
    safe_strcpy(s_config.logging.default_level, sizeof(s_config.logging.default_level), "WARN");
    s_config.logging.enable_test_commands = false;

    /* Timezone */
    s_config.timezone.sync_interval_hours = 24;
    safe_strcpy(s_config.timezone.source, sizeof(s_config.timezone.source), "server");
    s_config.timezone.update_server = true;

    /* Quiet hours */
    s_config.quiet_hours.start_hour = 23;
    s_config.quiet_hours.end_hour = 7;
    s_config.quiet_hours.sleep_multiplier = 6;

    /* Display policy */
    s_config.display_policy.skip_refresh_on_no_message = true;

    /* OTA */
    s_config.ota.enabled = true;  /* OTA enabled by default for official devices */
}

static void auto_generate_device_id(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_config.device.id, sizeof(s_config.device.id),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID auto-generated from eFuse MAC: %s", s_config.device.id);
}

static esp_err_t load_from_json(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error near: %.20s", err ? err : "unknown");
        return ESP_ERR_INVALID_ARG;
    }

    /* Device section.
     * The empty-ID check is deliberately NOT done here — it runs
     * unconditionally after all sections are parsed (see below) so that a
     * config.json lacking a "device" object at all still gets a valid ID. */
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(device)) {
        const char *id = json_get_string(device, "id", "");
        safe_strcpy(s_config.device.id, sizeof(s_config.device.id), id);

        safe_strcpy(s_config.device.name, sizeof(s_config.device.name),
                     json_get_string(device, "name", s_config.device.name));
        safe_strcpy(s_config.device.display_variant, sizeof(s_config.device.display_variant),
                     json_get_string(device, "display_variant", s_config.device.display_variant));
    }

    /* WiFi section */
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        s_config.wifi.timeout_ms = json_get_int(wifi, "timeout_ms", s_config.wifi.timeout_ms);
        s_config.wifi.force_high_power = json_get_bool(wifi, "force_high_power", s_config.wifi.force_high_power);
        s_config.wifi.escalation_threshold = json_get_int(wifi, "escalation_threshold", s_config.wifi.escalation_threshold);
        s_config.wifi.max_failed_wakes = json_get_int(wifi, "max_failed_wakes", s_config.wifi.max_failed_wakes);

        /* Parse seed_networks array */
        const cJSON *seeds = cJSON_GetObjectItemCaseSensitive(wifi, "seed_networks");
        if (cJSON_IsArray(seeds)) {
            s_config.wifi.seed_network_count = 0;
            const cJSON *seed = NULL;
            cJSON_ArrayForEach(seed, seeds) {
                if (s_config.wifi.seed_network_count >= CONFIG_MAX_SEED_NETWORKS) {
                    ESP_LOGW(TAG, "Max %d seed networks, ignoring extras", CONFIG_MAX_SEED_NETWORKS);
                    break;
                }
                const char *ssid = json_get_string(seed, "ssid", "");
                const char *password = json_get_string(seed, "password", "");

                if (ssid[0] == '\0') {
                    ESP_LOGW(TAG, "Skipping seed network with empty SSID");
                    continue;
                }
                if (strlen(ssid) > 32) {
                    ESP_LOGW(TAG, "Skipping seed network — SSID too long (%zu, max 32)", strlen(ssid));
                    continue;
                }
                if (strlen(password) > 64) {
                    ESP_LOGW(TAG, "Skipping seed network — password too long (%zu, max 64)", strlen(password));
                    continue;
                }

                wifi_seed_network_t *net = &s_config.wifi.seed_networks[s_config.wifi.seed_network_count];
                safe_strcpy(net->ssid, sizeof(net->ssid), ssid);
                safe_strcpy(net->password, sizeof(net->password), password);
                s_config.wifi.seed_network_count++;
                ESP_LOGD(TAG, "Loaded seed network: %s", ssid);
            }
            if (s_config.wifi.seed_network_count > 0) {
                ESP_LOGI(TAG, "Seed networks loaded: %d", s_config.wifi.seed_network_count);
            }
        }
    }

    /* Server section */
    const cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "server");
    if (cJSON_IsObject(server)) {
        safe_strcpy(s_config.server.host, sizeof(s_config.server.host),
                     json_get_string(server, "host", s_config.server.host));
        s_config.server.port = json_get_int(server, "port", s_config.server.port);
        safe_strcpy(s_config.server.path, sizeof(s_config.server.path),
                     json_get_string(server, "path", s_config.server.path));
        s_config.server.use_ssl = json_get_bool(server, "use_ssl", s_config.server.use_ssl);
        s_config.server.reconnect_jitter_max_sec = (uint16_t)json_get_int(server, "reconnect_jitter_max_sec", s_config.server.reconnect_jitter_max_sec);
    }

    /* Display section */
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (cJSON_IsObject(display)) {
        s_config.display.rotation = json_get_int(display, "rotation", s_config.display.rotation);
    }

    /* Security section */
    const cJSON *security = cJSON_GetObjectItemCaseSensitive(root, "security");
    if (cJSON_IsObject(security)) {
        safe_strcpy(s_config.security.auth_token, sizeof(s_config.security.auth_token),
                     json_get_string(security, "auth_token", s_config.security.auth_token));

        /* Support both new "encryption" field and legacy "use_aes" migration */
        const cJSON *encryption = cJSON_GetObjectItemCaseSensitive(security, "encryption");
        if (cJSON_IsString(encryption)) {
            safe_strcpy(s_config.security.encryption, sizeof(s_config.security.encryption),
                         encryption->valuestring);
        } else {
            const cJSON *use_aes = cJSON_GetObjectItemCaseSensitive(security, "use_aes");
            if (cJSON_IsBool(use_aes)) {
                safe_strcpy(s_config.security.encryption, sizeof(s_config.security.encryption),
                             cJSON_IsTrue(use_aes) ? "ecdh" : "none");
                ESP_LOGI(TAG, "Migrated use_aes to encryption=%s", s_config.security.encryption);
            }
        }
    }

    /* Power section */
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
    if (cJSON_IsObject(power)) {
        s_config.power.sleep_enabled = json_get_bool(power, "sleep_enabled", s_config.power.sleep_enabled);
        s_config.power.sleep_duration_min = json_get_int(power, "sleep_duration_min", s_config.power.sleep_duration_min);
        /* These fields are parsed for Arduino config.json compatibility but are
         * NOT yet enforced by the IDF firmware — sleep timing is governed by
         * power_manager's own light-sleep/DTIM logic, which ignores them. Warn
         * so a user who sets them isn't misled into expecting a behavior change. */
        ESP_LOGW(TAG, "power.sleep_enabled/sleep_duration_min are parsed but NOT "
                      "enforced by the IDF firmware (managed by power_manager)");
    }

    /* Logging section */
    const cJSON *logging = cJSON_GetObjectItemCaseSensitive(root, "logging");
    if (cJSON_IsObject(logging)) {
        safe_strcpy(s_config.logging.default_level, sizeof(s_config.logging.default_level),
                     json_get_string(logging, "default_level", s_config.logging.default_level));
        s_config.logging.enable_test_commands = json_get_bool(logging, "enable_test_commands", s_config.logging.enable_test_commands);
    }

    /* Timezone section */
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (cJSON_IsObject(tz)) {
        s_config.timezone.sync_interval_hours = json_get_int(tz, "sync_interval_hours", s_config.timezone.sync_interval_hours);
        safe_strcpy(s_config.timezone.source, sizeof(s_config.timezone.source),
                     json_get_string(tz, "source", s_config.timezone.source));
        safe_strcpy(s_config.timezone.ipgeolocation_api_key, sizeof(s_config.timezone.ipgeolocation_api_key),
                     json_get_string(tz, "ipgeolocation_api_key", s_config.timezone.ipgeolocation_api_key));
        s_config.timezone.update_server = json_get_bool(tz, "update_server", s_config.timezone.update_server);
    }

    /* Quiet hours section */
    const cJSON *qh = cJSON_GetObjectItemCaseSensitive(root, "quiet_hours");
    if (cJSON_IsObject(qh)) {
        s_config.quiet_hours.start_hour = json_get_int(qh, "start_hour", s_config.quiet_hours.start_hour);
        s_config.quiet_hours.end_hour = json_get_int(qh, "end_hour", s_config.quiet_hours.end_hour);
        s_config.quiet_hours.sleep_multiplier = json_get_int(qh, "sleep_multiplier", s_config.quiet_hours.sleep_multiplier);
        /* Parsed for Arduino config.json compatibility, but quiet-hours sleep
         * behavior is NOT yet implemented in the IDF firmware — no consumer
         * reads these fields. Warn so the setting isn't silently ignored. */
        ESP_LOGW(TAG, "quiet_hours is configured but NOT yet enforced by the IDF "
                      "firmware — quiet-hours behavior is not implemented");
    }

    /* Display policy section */
    const cJSON *dp = cJSON_GetObjectItemCaseSensitive(root, "display_policy");
    if (cJSON_IsObject(dp)) {
        s_config.display_policy.skip_refresh_on_no_message = json_get_bool(dp, "skip_refresh_on_no_message", s_config.display_policy.skip_refresh_on_no_message);
    }

    /* OTA section */
    const cJSON *ota = cJSON_GetObjectItemCaseSensitive(root, "ota");
    if (cJSON_IsObject(ota)) {
        s_config.ota.enabled = json_get_bool(ota, "enabled", s_config.ota.enabled);
    }

    cJSON_Delete(root);

    /* Ensure a device ID always exists, even when config.json omits the
     * "device" section entirely (or leaves id empty / at the placeholder).
     * This runs unconditionally after section parsing — an empty device.id
     * would otherwise flow into the pairing/OTA/WebSocket URLs and produce
     * malformed requests. */
    if (s_config.device.id[0] == '\0' ||
        strcmp(s_config.device.id, "esp32-default") == 0) {
        auto_generate_device_id();
    }

    /* Validation */
    if (s_config.device.display_variant[0] == '\0') {
        ESP_LOGW(TAG, "display_variant not set — OTA updates will be disabled");
    }

    /* Clamp sync_interval_hours to 1-168 range */
    if (s_config.timezone.sync_interval_hours < 1) {
        ESP_LOGW(TAG, "Sync interval %d too low, clamping to 1", s_config.timezone.sync_interval_hours);
        s_config.timezone.sync_interval_hours = 1;
    } else if (s_config.timezone.sync_interval_hours > 168) {
        ESP_LOGW(TAG, "Sync interval %d too high, clamping to 168", s_config.timezone.sync_interval_hours);
        s_config.timezone.sync_interval_hours = 168;
    }

    /* Validate timezone source */
    if (strcmp(s_config.timezone.source, "server") != 0 &&
        strcmp(s_config.timezone.source, "ipgeolocation") != 0) {
        ESP_LOGW(TAG, "Invalid timezone source '%s', defaulting to 'server'", s_config.timezone.source);
        safe_strcpy(s_config.timezone.source, sizeof(s_config.timezone.source), "server");
    }

    /* Warn if ipgeolocation selected but no API key */
    if (strcmp(s_config.timezone.source, "ipgeolocation") == 0 &&
        s_config.timezone.ipgeolocation_api_key[0] == '\0') {
        ESP_LOGW(TAG, "source='ipgeolocation' but no API key set — timezone sync will fail");
    }

    s_loaded = true;
    ESP_LOGI(TAG, "Configuration loaded: device_id=%s server=%s:%d variant=%s",
             s_config.device.id, s_config.server.host, s_config.server.port,
             s_config.device.display_variant);

    return ESP_OK;
}

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing configuration manager");

    /* Mount LittleFS partition */
    esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path = "/littlefs",
        .partition_label = "spiffs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&lfs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: total=%zu used=%zu free=%zu", total, used, total - used);

    /* Set defaults first */
    set_defaults();

    /* Try to load config.json */
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "Config file not found, using defaults");
        auto_generate_device_id();
        s_loaded = true;
        return ESP_OK;
    }

    /* Read file content */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || (size_t)file_size > MAX_CONFIG_SIZE) {
        /* Fall back to defaults rather than returning a fatal error: the caller
         * wraps init in ESP_ERROR_CHECK, so a fatal return would abort → boot
         * loop. A user who hand-edits config.json past 4 KB (or truncates it)
         * must not brick the device — same graceful path as a parse failure. */
        ESP_LOGW(TAG, "Config file invalid size: %ld (max %zu) — using defaults",
                 file_size, MAX_CONFIG_SIZE);
        fclose(f);
        auto_generate_device_id();
        s_loaded = true;
        return ESP_OK;
    }

    char *buf = (char *)malloc(file_size + 1);
    if (buf == NULL) {
        /* Fall back to defaults instead of a fatal ESP_ERR_NO_MEM return (which
         * ESP_ERROR_CHECK in the caller would turn into an abort/boot loop). */
        ESP_LOGW(TAG, "Failed to allocate %ld bytes for config — using defaults", file_size);
        fclose(f);
        auto_generate_device_id();
        s_loaded = true;
        return ESP_OK;
    }

    size_t read_bytes = fread(buf, 1, file_size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    ret = load_from_json(buf);
    free(buf);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Config parse failed, using defaults");
        set_defaults();
        auto_generate_device_id();
        s_loaded = true;
    }

    return ESP_OK;
}

const app_config_t *config_manager_get_config(void)
{
    return &s_config;
}

app_config_t *config_manager_get_mutable_config(void)
{
    return &s_config;
}

esp_err_t config_manager_save(void)
{
    char *json = config_manager_to_json();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json);

    /* Atomic write: write to a temp file, then rename() over the live config.
     * fopen(CONFIG_PATH, "w") truncates in place, so a power loss mid-write
     * (pairing saves the auth token while on battery) would leave a corrupt,
     * half-written config.json → next boot falls back to defaults → auth_token
     * is lost → silent unpair. rename() is atomic on LittleFS, so an
     * interrupted save leaves either the old file fully intact or the new file
     * fully written — never a truncated hybrid. */
    const char *tmp_path = "/littlefs/config.json.tmp";

    FILE *f = fopen(tmp_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open temp config file for writing");
        free(json);
        return ESP_FAIL;
    }

    size_t written = fwrite(json, 1, json_len, f);
    /* Capture the close result too: a buffered filesystem may only surface a
     * write error (e.g. no free space) at fclose() time. */
    int close_ret = fclose(f);
    free(json);

    /* Require the full payload to land. A short write means the FS is full or
     * failing — abort WITHOUT clobbering the existing good config. */
    if (written != json_len || close_ret != 0) {
        ESP_LOGE(TAG, "Config short write (%zu/%zu bytes, close=%d) — keeping previous config",
                 written, json_len, close_ret);
        remove(tmp_path);
        return ESP_FAIL;
    }

    if (rename(tmp_path, CONFIG_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename temp config into place — keeping previous config");
        remove(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Configuration saved (%zu bytes)", written);
    return ESP_OK;
}

void config_manager_reset(void)
{
    ESP_LOGW(TAG, "Resetting configuration to defaults");
    set_defaults();
    auto_generate_device_id();
    s_loaded = true;
}

char *config_manager_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* Device */
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "id", s_config.device.id);
    cJSON_AddStringToObject(device, "name", s_config.device.name);
    cJSON_AddStringToObject(device, "display_variant", s_config.device.display_variant);

    /* WiFi */
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddNumberToObject(wifi, "timeout_ms", s_config.wifi.timeout_ms);
    cJSON_AddBoolToObject(wifi, "force_high_power", s_config.wifi.force_high_power);
    cJSON_AddNumberToObject(wifi, "escalation_threshold", s_config.wifi.escalation_threshold);
    cJSON_AddNumberToObject(wifi, "max_failed_wakes", s_config.wifi.max_failed_wakes);
    if (s_config.wifi.seed_network_count > 0) {
        cJSON *seeds = cJSON_AddArrayToObject(wifi, "seed_networks");
        for (int i = 0; i < s_config.wifi.seed_network_count; i++) {
            cJSON *seed = cJSON_CreateObject();
            cJSON_AddStringToObject(seed, "ssid", s_config.wifi.seed_networks[i].ssid);
            cJSON_AddStringToObject(seed, "password", s_config.wifi.seed_networks[i].password);
            cJSON_AddItemToArray(seeds, seed);
        }
    }

    /* Server */
    cJSON *server = cJSON_AddObjectToObject(root, "server");
    cJSON_AddStringToObject(server, "host", s_config.server.host);
    cJSON_AddNumberToObject(server, "port", s_config.server.port);
    cJSON_AddStringToObject(server, "path", s_config.server.path);
    cJSON_AddBoolToObject(server, "use_ssl", s_config.server.use_ssl);
    cJSON_AddNumberToObject(server, "reconnect_jitter_max_sec", s_config.server.reconnect_jitter_max_sec);

    /* Display */
    cJSON *display = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(display, "rotation", s_config.display.rotation);

    /* Security */
    cJSON *security = cJSON_AddObjectToObject(root, "security");
    cJSON_AddStringToObject(security, "auth_token", s_config.security.auth_token);
    cJSON_AddStringToObject(security, "encryption", s_config.security.encryption);

    /* Power */
    cJSON *power = cJSON_AddObjectToObject(root, "power");
    cJSON_AddBoolToObject(power, "sleep_enabled", s_config.power.sleep_enabled);
    cJSON_AddNumberToObject(power, "sleep_duration_min", s_config.power.sleep_duration_min);

    /* Logging */
    cJSON *logging = cJSON_AddObjectToObject(root, "logging");
    cJSON_AddStringToObject(logging, "default_level", s_config.logging.default_level);
    cJSON_AddBoolToObject(logging, "enable_test_commands", s_config.logging.enable_test_commands);

    /* Timezone */
    cJSON *tz = cJSON_AddObjectToObject(root, "timezone");
    cJSON_AddNumberToObject(tz, "sync_interval_hours", s_config.timezone.sync_interval_hours);
    cJSON_AddStringToObject(tz, "source", s_config.timezone.source);
    cJSON_AddStringToObject(tz, "ipgeolocation_api_key", s_config.timezone.ipgeolocation_api_key);
    cJSON_AddBoolToObject(tz, "update_server", s_config.timezone.update_server);

    /* Quiet hours */
    cJSON *qh = cJSON_AddObjectToObject(root, "quiet_hours");
    cJSON_AddNumberToObject(qh, "start_hour", s_config.quiet_hours.start_hour);
    cJSON_AddNumberToObject(qh, "end_hour", s_config.quiet_hours.end_hour);
    cJSON_AddNumberToObject(qh, "sleep_multiplier", s_config.quiet_hours.sleep_multiplier);

    /* Display policy */
    cJSON *dp = cJSON_AddObjectToObject(root, "display_policy");
    cJSON_AddBoolToObject(dp, "skip_refresh_on_no_message", s_config.display_policy.skip_refresh_on_no_message);

    /* OTA */
    cJSON *ota_obj = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddBoolToObject(ota_obj, "enabled", s_config.ota.enabled);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    return json_str;
}
