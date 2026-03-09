/**
 * WiFi Credential Manager — ESP-IDF port using native NVS.
 *
 * Replaces Arduino Preferences library with nvs_open/get_blob/set_blob.
 * Same LRU eviction logic: max 5 networks, pinned seeds never evicted.
 */

#include "wifi_credential_manager.h"
#include "config_manager.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "WIFI_CRED";
static const char *NVS_NAMESPACE = "wifi_creds";

static wifi_credential_t s_credentials[WIFI_CRED_MAX_NETWORKS];
static uint8_t s_count = 0;
static bool s_initialized = false;

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Collect non-pinned credentials to persist */
    wifi_credential_t save_creds[WIFI_CRED_MAX_NETWORKS];
    uint8_t save_count = 0;
    for (int i = 0; i < s_count; i++) {
        if (!s_credentials[i].is_pinned) {
            save_creds[save_count++] = s_credentials[i];
        }
    }

    /* Erase all existing keys before rewriting — ensures stale entries are removed
     * if count shrinks (e.g., after LRU eviction removes a credential, the old
     * ssid_N key would persist without this erase). */
    ret = nvs_erase_all(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    /* Write individual keys — format matches Arduino Preferences for cross-firmware
     * compatibility: count + ssid_N + pass_N + used_N per credential. */
    ret = nvs_set_u8(handle, "count", save_count);
    for (int i = 0; i < save_count && ret == ESP_OK; i++) {
        char key[12];
        snprintf(key, sizeof(key), "ssid_%d", i);
        ret = nvs_set_str(handle, key, save_creds[i].ssid);
        if (ret != ESP_OK) break;
        snprintf(key, sizeof(key), "pass_%d", i);
        ret = nvs_set_str(handle, key, save_creds[i].password);
        if (ret != ESP_OK) break;
        snprintf(key, sizeof(key), "used_%d", i);
        ret = nvs_set_u32(handle, key, save_creds[i].last_used);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

esp_err_t wifi_credential_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(s_credentials, 0, sizeof(s_credentials));
    s_count = 0;

    /* Load seed networks from config (pinned) */
    const app_config_t *cfg = config_manager_get_config();
    for (int i = 0; i < cfg->wifi.seed_network_count && s_count < WIFI_CRED_MAX_NETWORKS; i++) {
        strncpy(s_credentials[s_count].ssid, cfg->wifi.seed_networks[i].ssid,
                WIFI_CRED_MAX_SSID - 1);
        strncpy(s_credentials[s_count].password, cfg->wifi.seed_networks[i].password,
                WIFI_CRED_MAX_PASS - 1);
        s_credentials[s_count].is_pinned = true;
        s_credentials[s_count].last_used = 0;
        s_count++;
    }

    /* Load NVS credentials and merge (avoiding duplicates with seeds) */
    wifi_credential_t nvs_creds[WIFI_CRED_MAX_NETWORKS];
    uint8_t nvs_count = 0;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t count = 0;
        if (nvs_get_u8(handle, "count", &count) == ESP_OK && count > 0) {
            if (count > WIFI_CRED_MAX_NETWORKS) count = WIFI_CRED_MAX_NETWORKS;
            for (int i = 0; i < count; i++) {
                char key[12];
                char ssid[WIFI_CRED_MAX_SSID] = {0};
                char pass[WIFI_CRED_MAX_PASS] = {0};
                uint32_t last_used = 0;
                size_t len;

                snprintf(key, sizeof(key), "ssid_%d", i); len = sizeof(ssid);
                nvs_get_str(handle, key, ssid, &len);
                snprintf(key, sizeof(key), "pass_%d", i); len = sizeof(pass);
                nvs_get_str(handle, key, pass, &len);
                snprintf(key, sizeof(key), "used_%d", i);
                nvs_get_u32(handle, key, &last_used);

                if (ssid[0] != '\0') {
                    strncpy(nvs_creds[nvs_count].ssid,     ssid, WIFI_CRED_MAX_SSID - 1);
                    strncpy(nvs_creds[nvs_count].password, pass, WIFI_CRED_MAX_PASS - 1);
                    nvs_creds[nvs_count].last_used = last_used;
                    nvs_creds[nvs_count].is_pinned = false;
                    nvs_count++;
                }
            }
        }
        nvs_close(handle);
    }

    /* Merge NVS credentials, skip duplicates of seed SSIDs */
    for (int i = 0; i < nvs_count && s_count < WIFI_CRED_MAX_NETWORKS; i++) {
        bool is_duplicate = false;
        for (int j = 0; j < s_count; j++) {
            if (strcmp(nvs_creds[i].ssid, s_credentials[j].ssid) == 0) {
                is_duplicate = true;
                break;
            }
        }
        if (!is_duplicate) {
            s_credentials[s_count] = nvs_creds[i];
            s_credentials[s_count].is_pinned = false;
            s_count++;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Credential manager initialized: %d networks total", s_count);
    return ESP_OK;
}

esp_err_t wifi_credential_manager_add(const char *ssid, const char *password)
{
    /* Check if SSID already exists */
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_credentials[i].ssid, ssid) == 0) {
            /* Update password */
            strncpy(s_credentials[i].password, password, WIFI_CRED_MAX_PASS - 1);
            s_credentials[i].last_used = (uint32_t)time(NULL);
            return save_to_nvs();
        }
    }

    /* If at capacity, evict oldest unpinned */
    if (s_count >= WIFI_CRED_MAX_NETWORKS) {
        int oldest_idx = -1;
        uint32_t oldest_time = UINT32_MAX;
        for (int i = 0; i < s_count; i++) {
            if (!s_credentials[i].is_pinned && s_credentials[i].last_used < oldest_time) {
                oldest_time = s_credentials[i].last_used;
                oldest_idx = i;
            }
        }
        if (oldest_idx < 0) {
            ESP_LOGW(TAG, "All %d networks are pinned, cannot add", s_count);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Evicting oldest unpinned network: %s", s_credentials[oldest_idx].ssid);
        /* Shift remaining entries down */
        for (int i = oldest_idx; i < s_count - 1; i++) {
            s_credentials[i] = s_credentials[i + 1];
        }
        s_count--;
    }

    /* Add new credential */
    strncpy(s_credentials[s_count].ssid, ssid, WIFI_CRED_MAX_SSID - 1);
    strncpy(s_credentials[s_count].password, password, WIFI_CRED_MAX_PASS - 1);
    s_credentials[s_count].last_used = (uint32_t)time(NULL);
    s_credentials[s_count].is_pinned = false;
    s_count++;

    return save_to_nvs();
}

void wifi_credential_manager_update_last_used(const char *ssid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_credentials[i].ssid, ssid) == 0) {
            s_credentials[i].last_used = (uint32_t)time(NULL);
            save_to_nvs();
            return;
        }
    }
}

const wifi_credential_t *wifi_credential_manager_get_all(uint8_t *count)
{
    if (count) {
        *count = s_count;
    }
    return s_credentials;
}

void wifi_credential_manager_clear_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    /* Remove non-pinned from runtime */
    uint8_t new_count = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_credentials[i].is_pinned) {
            s_credentials[new_count++] = s_credentials[i];
        }
    }
    s_count = new_count;
    ESP_LOGI(TAG, "NVS cleared, %d pinned networks remain", s_count);
}
