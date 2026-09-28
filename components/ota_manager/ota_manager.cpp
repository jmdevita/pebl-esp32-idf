/**
 * OTA Manager — ESP-IDF port using esp_https_ota + esp_ota_ops.
 *
 * Replaces:
 *   HTTPClient → esp_http_client
 *   Arduino Update.h → esp_ota_begin/write/end
 *   Boot validation → esp_ota_mark_app_valid_cancel_rollback()
 *
 * Same ECDSA P-256 signature verification using mbedTLS.
 * Dual partition scheme with atomic swap (same partitions.csv).
 *
 * Verification flow:
 *   1. Check server for update → parse JSON response
 *   2. Download firmware via esp_https_ota (streaming to OTA partition)
 *   3. Compute SHA-256 hash of downloaded image
 *   4. Verify ECDSA signature over hash using hardcoded public key
 *   5. Finalize OTA (set boot partition to new image)
 */

#include "ota_manager.h"
#include "config_manager.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "cJSON.h"

#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/error.h"

static const char *TAG = "OTA";
static ota_status_t s_status = OTA_STATUS_IDLE;

/* Maximum response body size for firmware check endpoint */
#define MAX_CHECK_RESPONSE_SIZE  2048

/**
 * ECDSA P-256 public key for firmware signature verification.
 * Same key as the Arduino client — generated using scripts/generate_ota_keys.sh.
 * Fingerprint: 882517373e7bac2b9f552445d3f8269f15a35998a4c7f52866cd56810620d752
 */
static const char *FIRMWARE_PUBLIC_KEY =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEpaUoIStuizMGtDR9hJ7SeY8gX9m4\n"
    "2frDuv7haRz+O67nPYZ/VxMc5R/q4spN3bk315CnIOChSx18gUFoYQ7HFQ==\n"
    "-----END PUBLIC KEY-----\n";

/**
 * Verify ECDSA P-256 signature over a SHA-256 hash.
 *
 * Steps:
 *   1. Base64-decode the signature string (ASN.1 DER encoded)
 *   2. Parse the hardcoded ECDSA public key (PEM format)
 *   3. Verify signature using mbedtls_pk_verify(MBEDTLS_MD_SHA256)
 *
 * Returns true if signature is valid.
 */
static bool verify_ecdsa_signature(const uint8_t *hash, const char *signature_b64)
{
    /* Decode base64 signature — ECDSA P-256 DER is typically 70-72 bytes */
    uint8_t sig_bytes[128];
    size_t sig_len = 0;
    int ret = mbedtls_base64_decode(sig_bytes, sizeof(sig_bytes), &sig_len,
                                     (const unsigned char *)signature_b64,
                                     strlen(signature_b64));
    if (ret != 0) {
        ESP_LOGE(TAG, "Signature base64 decode failed: -0x%04x", -ret);
        return false;
    }

    /* Parse public key */
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    ret = mbedtls_pk_parse_public_key(&pk,
                                       (const unsigned char *)FIRMWARE_PUBLIC_KEY,
                                       strlen(FIRMWARE_PUBLIC_KEY) + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Public key parse failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    /* Verify key type is EC */
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY &&
        mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECDSA) {
        ESP_LOGE(TAG, "Public key is not an EC key (type=%d)",
                 mbedtls_pk_get_type(&pk));
        mbedtls_pk_free(&pk);
        return false;
    }

    /* Verify ECDSA signature over SHA-256 hash */
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig_bytes, sig_len);
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Signature verification failed: %s (-0x%04x)", err_buf, -ret);
        return false;
    }

    ESP_LOGI(TAG, "ECDSA signature verified successfully");
    return true;
}

/**
 * Parse the numeric MAJOR.MINOR.PATCH triple from a version string.
 * Tolerates an optional leading 'v' and any pre-release/build suffix
 * (e.g. "v2.1.0-rc1" → {2,1,0}); missing components default to 0.
 * Suffixes are deliberately ignored for the downgrade comparison — RC/beta
 * ordering is subtle and getting it wrong could block a legitimate update, so
 * the guard keys only on the well-defined numeric triple.
 */
static void parse_semver_numeric(const char *v, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!v) return;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

/**
 * Compare two version strings by numeric MAJOR.MINOR.PATCH only.
 * Returns <0 if a<b, 0 if equal, >0 if a>b.
 */
static int semver_compare_numeric(const char *a, const char *b)
{
    int va[3], vb[3];
    parse_semver_numeric(a, va);
    parse_semver_numeric(b, vb);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] - vb[i];
    }
    return 0;
}

/**
 * Verify firmware image hash and signature.
 *
 * Reads the OTA partition that was just written, computes SHA-256,
 * compares against expected hash, then verifies ECDSA signature.
 */
static bool verify_firmware(const firmware_info_t *info)
{
    /* Signature is MANDATORY — fail closed. The server always ECDSA-signs
     * (P-256) firmware, so a response with no signature means either a
     * misconfigured/compromised server or a tampered download_url followed
     * verbatim. Installing it would defeat the whole hardcoded-key protection,
     * so refuse rather than "skip verification" (ESP-4). The SHA-256 hash below
     * remains an optional integrity pre-check; the signature is the trust root. */
    if (info->signature[0] == '\0') {
        ESP_LOGE(TAG, "Firmware response carries no signature — refusing to install (fail closed)");
        return false;
    }

    /* Get the next OTA partition (the one we just wrote to) */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "Cannot find OTA update partition");
        return false;
    }

    /* Compute SHA-256 over the written firmware image */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);  /* 0 = SHA-256 (not SHA-224) */

    uint8_t buf[512];
    size_t remaining = info->size;
    size_t offset = 0;

    while (remaining > 0) {
        size_t to_read = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        esp_err_t err = esp_partition_read(update_partition, offset, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition read failed at offset %u: %s",
                     (unsigned)offset, esp_err_to_name(err));
            mbedtls_sha256_free(&sha_ctx);
            return false;
        }
        mbedtls_sha256_update(&sha_ctx, buf, to_read);
        offset += to_read;
        remaining -= to_read;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    /* Convert hash to hex string for comparison */
    char hash_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hash_hex + (i * 2), "%02x", hash[i]);
    }
    hash_hex[64] = '\0';

    /* Verify SHA-256 hash matches expected value */
    if (info->sha256_hash[0] != '\0') {
        if (strcmp(hash_hex, info->sha256_hash) != 0) {
            ESP_LOGE(TAG, "Hash mismatch: expected %s, got %s",
                     info->sha256_hash, hash_hex);
            return false;
        }
        ESP_LOGI(TAG, "SHA-256 hash verified: %s", hash_hex);
    }

    /* Verify ECDSA signature over the hash */
    if (info->signature[0] != '\0') {
        if (!verify_ecdsa_signature(hash, info->signature)) {
            ESP_LOGE(TAG, "Firmware signature invalid — may be tampered");
            return false;
        }
    }

    return true;
}

void ota_manager_mark_valid_on_boot(void)
{
    /* Mark firmware valid before WiFi to prevent OTA rollback if the device
     * enters the captive portal (no credentials on first boot after cross-firmware
     * migration). Mirrors Arduino's checkBootValidation() timing — if NVS, config,
     * and board init succeed, the firmware is functional. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA — marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Running firmware: %s (partition: %s)",
             app_desc->version, running->label);
}

void ota_manager_check_on_boot(void)
{
    /* Firmware was already marked valid early in boot by ota_manager_mark_valid_on_boot().
     * This call only handles deferred server reporting (requires WiFi to be connected). */
    ota_manager_report_boot_status();
}

esp_err_t ota_manager_check_for_update(firmware_info_t *info)
{
    s_status = OTA_STATUS_CHECKING;

    const app_config_t *cfg = config_manager_get_config();

    /* Check if OTA updates are enabled in config (allows users to opt out) */
    if (!cfg->ota.enabled) {
        ESP_LOGI(TAG, "OTA updates disabled via config.json (ota.enabled = false)");
        s_status = OTA_STATUS_IDLE;
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();

    /* Build check URL — matches Arduino client format:
     * GET /api/firmware/version?device_id=X&display_variant=Y&current_version=Z */
    char url[512];
    snprintf(url, sizeof(url),
             "https://%s/api/firmware/version?device_id=%s&display_variant=%s&current_version=%s",
             cfg->server.host, cfg->device.id,
             cfg->device.display_variant, app_desc->version);

    ESP_LOGI(TAG, "Checking for firmware update: %s", url);

    /* Use streaming HTTP API to read response */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        s_status = OTA_STATUS_IDLE;
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        s_status = OTA_STATUS_IDLE;
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGW(TAG, "Firmware check returned HTTP %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_status = OTA_STATUS_IDLE;
        return ESP_FAIL;
    }

    /* Read response body */
    int buf_size = (content_length > 0 && content_length < MAX_CHECK_RESPONSE_SIZE)
                   ? content_length + 1
                   : MAX_CHECK_RESPONSE_SIZE;
    char *body = (char *)malloc(buf_size);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_status = OTA_STATUS_IDLE;
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < buf_size - 1) {
        int read = esp_http_client_read(client, body + total_read, buf_size - 1 - total_read);
        if (read <= 0) break;
        total_read += read;
    }
    body[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Parse JSON response.
     * Server returns one of three statuses:
     *   - "no_firmware_configured": admin hasn't uploaded firmware
     *   - "up_to_date": device is on latest version
     *   - "update_available": new firmware ready to download */
    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse firmware check response");
        s_status = OTA_STATUS_IDLE;
        return ESP_FAIL;
    }

    const cJSON *update_available = cJSON_GetObjectItemCaseSensitive(root, "update_available");
    const cJSON *status_str = cJSON_GetObjectItemCaseSensitive(root, "status");

    if (!cJSON_IsTrue(update_available)) {
        /* No update needed */
        if (cJSON_IsString(status_str)) {
            if (strcmp(status_str->valuestring, "no_firmware_configured") == 0) {
                ESP_LOGI(TAG, "No firmware configured on server");
            } else if (strcmp(status_str->valuestring, "up_to_date") == 0) {
                const cJSON *latest = cJSON_GetObjectItemCaseSensitive(root, "latest_version");
                ESP_LOGI(TAG, "Firmware up to date: %s",
                         cJSON_IsString(latest) ? latest->valuestring : "unknown");
            }
        }
        cJSON_Delete(root);
        s_status = OTA_STATUS_IDLE;
        return ESP_ERR_NOT_FOUND;
    }

    /* Update available — extract firmware metadata */
    memset(info, 0, sizeof(*info));

    const cJSON *j;

    j = cJSON_GetObjectItemCaseSensitive(root, "latest_version");
    if (cJSON_IsString(j)) {
        strncpy(info->version, j->valuestring, sizeof(info->version) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "download_url");
    if (cJSON_IsString(j)) {
        strncpy(info->download_url, j->valuestring, sizeof(info->download_url) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    if (cJSON_IsString(j)) {
        strncpy(info->sha256_hash, j->valuestring, sizeof(info->sha256_hash) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "signature");
    if (cJSON_IsString(j)) {
        strncpy(info->signature, j->valuestring, sizeof(info->signature) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (cJSON_IsNumber(j)) {
        info->size = (uint32_t)j->valuedouble;
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "required");
    if (cJSON_IsBool(j)) {
        info->required = cJSON_IsTrue(j);
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "changelog");
    if (cJSON_IsString(j)) {
        strncpy(info->changelog, j->valuestring, sizeof(info->changelog) - 1);
    }

    cJSON_Delete(root);

    /* Monotonic-version (anti-downgrade) guard: refuse a target whose numeric
     * MAJOR.MINOR.PATCH is older than what's running. A signed-but-older build
     * advertised by a misconfigured or hostile server would otherwise install
     * and could reintroduce a patched vulnerability. Equal versions never reach
     * here (server reports up_to_date). Only the numeric triple is compared;
     * see parse_semver_numeric() for the RC/beta-suffix rationale. */
    if (semver_compare_numeric(info->version, app_desc->version) < 0) {
        ESP_LOGE(TAG, "Refusing firmware downgrade: offered %s < running %s",
                 info->version, app_desc->version);
        s_status = OTA_STATUS_IDLE;
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "Update available: %s (%u bytes, required=%d)",
             info->version, info->size, info->required);

    s_status = OTA_STATUS_IDLE;
    return ESP_OK;
}

esp_err_t ota_manager_download_and_install(const firmware_info_t *info,
                                            ota_progress_cb_t progress_cb)
{
    s_status = OTA_STATUS_DOWNLOADING;
    ESP_LOGI(TAG, "Downloading firmware %s (%u bytes)", info->version, info->size);

    /* Build full download URL. The server returns a relative path
     * (e.g., "/api/firmware/download"), so prepend scheme + host.
     * Also append device_id and display_variant for server-side tracking. */
    const app_config_t *cfg = config_manager_get_config();
    char download_url[768];
    if (strncmp(info->download_url, "http", 4) == 0) {
        /* Already a full URL */
        snprintf(download_url, sizeof(download_url), "%s%cdevice_id=%s&display_variant=%s",
                 info->download_url,
                 strchr(info->download_url, '?') ? '&' : '?',
                 cfg->device.id, cfg->device.display_variant);
    } else {
        /* Relative path — prepend server host */
        snprintf(download_url, sizeof(download_url),
                 "https://%s%s%cdevice_id=%s&display_variant=%s",
                 cfg->server.host, info->download_url,
                 strchr(info->download_url, '?') ? '&' : '?',
                 cfg->device.id, cfg->device.display_variant);
    }

    esp_http_client_config_t http_cfg = {
        .url = download_url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        s_status = OTA_STATUS_FAILED;
        return ret;
    }

    /* Download in chunks with progress reporting */
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        if (progress_cb) {
            int image_size = esp_https_ota_get_image_size(ota_handle);
            int read_size = esp_https_ota_get_image_len_read(ota_handle);
            progress_cb(read_size, image_size);
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        s_status = OTA_STATUS_FAILED;
        return ret;
    }

    /* Verify firmware integrity before finalizing.
     * Read the written partition, compute SHA-256, verify ECDSA signature. */
    s_status = OTA_STATUS_VERIFYING;
    if (!verify_firmware(info)) {
        ESP_LOGE(TAG, "Firmware verification failed — aborting OTA");
        esp_https_ota_abort(ota_handle);
        s_status = OTA_STATUS_FAILED;
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Finalize OTA: set boot partition to the new image */
    s_status = OTA_STATUS_INSTALLING;
    ret = esp_https_ota_finish(ota_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful — will boot new firmware on restart");
        s_status = OTA_STATUS_SUCCESS;
    } else {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        s_status = OTA_STATUS_FAILED;
    }

    return ret;
}

/**
 * Store old and new firmware versions in NVS before OTA finalize.
 * On next boot, ota_manager_report_boot_status() reads these and
 * sends a success report to the server.
 */
static void nvs_store_ota_versions(const char *old_ver, const char *new_ver)
{
    nvs_handle_t handle;
    if (nvs_open("ota", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "old_ver", old_ver);
        nvs_set_str(handle, "new_ver", new_ver);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void ota_manager_report_boot_status(void)
{
    nvs_handle_t handle;
    if (nvs_open("ota", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    char old_ver[32] = {0};
    char new_ver[32] = {0};
    size_t len;

    len = sizeof(old_ver);
    if (nvs_get_str(handle, "old_ver", old_ver, &len) != ESP_OK) {
        nvs_close(handle);
        return;  /* No pending OTA report */
    }
    len = sizeof(new_ver);
    if (nvs_get_str(handle, "new_ver", new_ver, &len) != ESP_OK) {
        nvs_close(handle);
        return;
    }

    ESP_LOGI(TAG, "Reporting OTA success: %s → %s", old_ver, new_ver);

    const app_config_t *cfg = config_manager_get_config();
    char url[256];
    snprintf(url, sizeof(url), "https://%s/api/firmware/stats", cfg->server.host);

    /* Build JSON body */
    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\",\"old_version\":\"%s\","
        "\"new_version\":\"%s\",\"status\":\"success\"}",
        cfg->device.id, old_ver, new_ver);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        nvs_close(handle);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        /* Server acknowledged — clear NVS entries */
        nvs_erase_key(handle, "old_ver");
        nvs_erase_key(handle, "new_ver");
        nvs_commit(handle);
        ESP_LOGI(TAG, "OTA boot status reported successfully");
    } else {
        /* Leave NVS entries for retry on next boot */
        ESP_LOGW(TAG, "OTA boot status report failed (HTTP %d) — will retry", status);
    }

    nvs_close(handle);
}

esp_err_t ota_manager_check_and_update(void)
{
    firmware_info_t info;
    esp_err_t ret = ota_manager_check_for_update(&info);
    if (ret != ESP_OK) {
        return ret;  /* No update available or check failed */
    }

    ret = ota_manager_download_and_install(&info, NULL);
    if (ret == ESP_OK) {
        ota_manager_finalize_and_restart(info.version);
        /* Does not return */
    }

    return ret;
}

void ota_manager_finalize_and_restart(const char *new_version)
{
    /* Store version info in NVS only after successful download+verify,
     * so on next boot we can report success to the server.
     * Must happen before esp_restart() but after the OTA is finalized. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    nvs_store_ota_versions(app_desc->version, new_version);
    ESP_LOGI(TAG, "OTA update applied — restarting");
    vTaskDelay(pdMS_TO_TICKS(100));  /* Allow log to flush */
    esp_restart();
    /* Does not return */
}

ota_status_t ota_manager_get_status(void)
{
    return s_status;
}

esp_err_t ota_manager_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}
