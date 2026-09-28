/**
 * Security Manager — ESP-IDF port of Arduino SecurityManager.
 *
 * Easiest port: already uses mbedTLS directly. Same crypto operations.
 *
 * Replaces:
 *   Arduino Preferences → native NVS (nvs_open/get_blob/set_blob)
 *   HTTPClient          → esp_http_client (for public key upload)
 *   String              → char buffers
 *
 * Same crypto:
 *   ECDH P-256 keypair (32-byte private scalar)
 *   HKDF-SHA256 key derivation
 *   AES-256-GCM authenticated decryption
 */

#include "security_manager.h"
#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "SECURITY";
static const char *NVS_NAMESPACE = "security";
static const char *NVS_KEY_PRIVATE = "ecdh_priv";
static const char *NVS_KEY_UPLOADED = "key_uploaded";

static uint8_t s_private_key[32];
static bool s_initialized = false;
static bool s_key_uploaded = false;
static char s_public_key_pem[512];

/* Shared DRBG for all crypto operations */
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;
static bool s_rng_initialized = false;

static bool init_rng(void)
{
    if (s_rng_initialized) {
        return true;
    }
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);
    const char *pers = "pebl_ecdh";
    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy,
                                     (const uint8_t *)pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "DRBG seed failed: -0x%04x", -ret);
        return false;
    }
    s_rng_initialized = true;
    return true;
}

static bool load_key_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return false;
    }

    size_t key_size = sizeof(s_private_key);
    ret = nvs_get_blob(handle, NVS_KEY_PRIVATE, s_private_key, &key_size);

    uint8_t uploaded = 0;
    size_t uploaded_size = sizeof(uploaded);
    if (nvs_get_blob(handle, NVS_KEY_UPLOADED, &uploaded, &uploaded_size) == ESP_OK) {
        s_key_uploaded = (uploaded != 0);
    }

    nvs_close(handle);
    return (ret == ESP_OK && key_size == 32);
}

static bool generate_and_save_key(void)
{
    if (!init_rng()) {
        return false;
    }

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);

    int ret = mbedtls_ecp_gen_keypair(&grp, &d, &Q,
                                       mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Key generation failed: -0x%04x", -ret);
        mbedtls_ecp_group_free(&grp);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_point_free(&Q);
        return false;
    }

    /* Export private key as 32-byte big-endian scalar */
    ret = mbedtls_mpi_write_binary(&d, s_private_key, 32);

    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);

    if (ret != 0) {
        ESP_LOGE(TAG, "Private key export failed: -0x%04x", -ret);
        return false;
    }

    /* Save to NVS — verify each step since a failed persist means the key
     * exists only in RAM and will be lost on reboot, breaking server pairing */
    nvs_handle_t handle;
    esp_err_t nvs_ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(nvs_ret));
        return false;
    }

    nvs_ret = nvs_set_blob(handle, NVS_KEY_PRIVATE, s_private_key, 32);
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write private key failed: %s", esp_err_to_name(nvs_ret));
        nvs_close(handle);
        return false;
    }

    uint8_t uploaded = 0;
    nvs_set_blob(handle, NVS_KEY_UPLOADED, &uploaded, sizeof(uploaded));
    nvs_ret = nvs_commit(handle);
    nvs_close(handle);

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(nvs_ret));
        return false;
    }

    s_key_uploaded = false;
    ESP_LOGI(TAG, "New ECDH P-256 keypair generated and saved to NVS");
    return true;
}

/**
 * Derive PEM-encoded SubjectPublicKeyInfo from stored private key.
 * Uses mbedtls_pk to get proper PEM output matching server expectations.
 */
static bool derive_public_key_pem(void)
{
    if (!init_rng()) {
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        ESP_LOGE(TAG, "PK setup failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    /* Load private key using v3 API (struct members grp, d, Q are private in mbedTLS v3) */
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(pk);
    ret = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, ec, s_private_key, 32);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load private key into keypair: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    /* Derive public point Q = d * G using the v3 keypair API */
    ret = mbedtls_ecp_keypair_calc_public(ec, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Public key derivation failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    /* Write as PEM (SubjectPublicKeyInfo format, matching server expectations) */
    uint8_t pem_buf[512];
    memset(pem_buf, 0, sizeof(pem_buf));
    ret = mbedtls_pk_write_pubkey_pem(&pk, pem_buf, sizeof(pem_buf));
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        ESP_LOGE(TAG, "PEM encoding failed: -0x%04x", -ret);
        return false;
    }

    /* mbedtls_pk_write_pubkey_pem writes from the END of the buffer */
    strncpy(s_public_key_pem, (char *)pem_buf, sizeof(s_public_key_pem) - 1);
    s_public_key_pem[sizeof(s_public_key_pem) - 1] = '\0';

    return true;
}

/**
 * HKDF-SHA256: extract-then-expand.
 * info = "aes-key", output = 32 bytes (one HMAC block).
 * Matches Arduino SecurityManager::hkdfSHA256().
 */
static bool hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                         const uint8_t *salt, size_t salt_len,
                         uint8_t *okm, size_t okm_len)
{
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return false;
    }

    /* Use 32 zero bytes as default salt per RFC 5869 */
    uint8_t default_salt[32] = {0};
    const uint8_t *effective_salt = (salt && salt_len > 0) ? salt : default_salt;
    size_t effective_salt_len = (salt && salt_len > 0) ? salt_len : 32;

    /* Extract: PRK = HMAC-SHA256(salt, IKM) */
    uint8_t prk[32];
    int ret = mbedtls_md_hmac(md_info, effective_salt, effective_salt_len,
                               ikm, ikm_len, prk);
    if (ret != 0) {
        return false;
    }

    /* Expand: T(1) = HMAC-SHA256(PRK, info || 0x01) */
    const char *info = "aes-key";
    size_t info_len = strlen(info);
    uint8_t expand_input[info_len + 1];
    memcpy(expand_input, info, info_len);
    expand_input[info_len] = 0x01;

    ret = mbedtls_md_hmac(md_info, prk, 32, expand_input, info_len + 1, okm);

    /* Zero PRK from stack */
    memset(prk, 0, sizeof(prk));

    return (ret == 0);
}

esp_err_t security_manager_init(void)
{
    const app_config_t *cfg = config_manager_get_config();

    if (strcmp(cfg->security.encryption, "none") == 0) {
        ESP_LOGI(TAG, "Encryption disabled in config");
        s_initialized = false;
        return ESP_OK;
    }

    if (load_key_from_nvs()) {
        ESP_LOGI(TAG, "ECDH keypair loaded from NVS (uploaded=%s)",
                 s_key_uploaded ? "yes" : "no");
    } else {
        ESP_LOGI(TAG, "No keypair in NVS — generating new ECDH P-256 keypair");
        if (!generate_and_save_key()) {
            ESP_LOGE(TAG, "Key generation failed");
            return ESP_FAIL;
        }
    }

    if (!derive_public_key_pem()) {
        ESP_LOGE(TAG, "Failed to derive public key PEM");
        return ESP_FAIL;
    }

    s_initialized = true;
    return ESP_OK;
}

void security_manager_cleanup(void)
{
    memset(s_private_key, 0, sizeof(s_private_key));
    s_initialized = false;
    if (s_rng_initialized) {
        mbedtls_ctr_drbg_free(&s_ctr_drbg);
        mbedtls_entropy_free(&s_entropy);
        s_rng_initialized = false;
    }
}

char *security_manager_decrypt(const char *envelope_json)
{
    if (!s_initialized || !init_rng()) {
        ESP_LOGE(TAG, "Decryption called but security not initialized");
        return NULL;
    }

    /* Parse the ECDH envelope JSON */
    cJSON *root = cJSON_Parse(envelope_json);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse envelope JSON");
        return NULL;
    }

    const cJSON *encrypted_b64 = cJSON_GetObjectItemCaseSensitive(root, "encrypted");
    const cJSON *ephem_pem = cJSON_GetObjectItemCaseSensitive(root, "ephemeral_public_key");
    const cJSON *iv_b64 = cJSON_GetObjectItemCaseSensitive(root, "iv");
    const cJSON *tag_b64 = cJSON_GetObjectItemCaseSensitive(root, "tag");
    const cJSON *salt_b64 = cJSON_GetObjectItemCaseSensitive(root, "salt");

    if (!cJSON_IsString(encrypted_b64) || !cJSON_IsString(ephem_pem) ||
        !cJSON_IsString(iv_b64) || !cJSON_IsString(tag_b64)) {
        ESP_LOGE(TAG, "Missing required fields in envelope");
        cJSON_Delete(root);
        return NULL;
    }

    char *result = NULL;

    /* 1. Parse server's ephemeral public key from PEM */
    mbedtls_pk_context ephPk;
    mbedtls_pk_init(&ephPk);

    const char *pem_str = ephem_pem->valuestring;
    int ret = mbedtls_pk_parse_public_key(&ephPk, (const uint8_t *)pem_str,
                                           strlen(pem_str) + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse ephemeral public key: -0x%04x", -ret);
        goto cleanup;
    }

    if (mbedtls_pk_get_type(&ephPk) != MBEDTLS_PK_ECKEY) {
        ESP_LOGE(TAG, "Ephemeral key is not EC type");
        goto cleanup;
    }

    /* 2. ECDH shared secret: shared_z = x-coordinate of (d * ephemeral_Q) */
    {
        /*
         * mbedTLS v3 compatibility: keypair struct members (grp, d, Q) are
         * private. Use mbedtls_ecp_export() to extract the ephemeral public
         * point Q from the parsed PK context.
         */
        mbedtls_ecp_keypair *ephEc = mbedtls_pk_ec(ephPk);

        mbedtls_ecp_group grp;
        mbedtls_ecp_point ephQ;
        mbedtls_mpi d, shared_z;

        mbedtls_ecp_group_init(&grp);
        mbedtls_ecp_point_init(&ephQ);
        mbedtls_mpi_init(&d);
        mbedtls_mpi_init(&shared_z);

        /* Export the ephemeral public point from the parsed keypair */
        ret = mbedtls_ecp_export(ephEc, &grp, NULL, &ephQ);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to export ephemeral public key: -0x%04x", -ret);
            mbedtls_ecp_group_free(&grp);
            mbedtls_ecp_point_free(&ephQ);
            mbedtls_mpi_free(&d);
            mbedtls_mpi_free(&shared_z);
            goto cleanup;
        }

        /* Reject an ephemeral key that isn't on our expected P-256 curve. Our
         * private scalar is P-256; running ECDH against a point on a different
         * curve is meaningless and would otherwise surface as a confusing GCM
         * auth failure rather than a clear "wrong curve" error. */
        if (grp.id != MBEDTLS_ECP_DP_SECP256R1) {
            ESP_LOGE(TAG, "Ephemeral key not on SECP256R1 (curve id=%d)", (int)grp.id);
            mbedtls_ecp_group_free(&grp);
            mbedtls_ecp_point_free(&ephQ);
            mbedtls_mpi_free(&d);
            mbedtls_mpi_free(&shared_z);
            goto cleanup;
        }

        /* Load our private key scalar and compute shared secret */
        mbedtls_mpi_read_binary(&d, s_private_key, 32);

        ret = mbedtls_ecdh_compute_shared(&grp, &shared_z, &ephQ, &d,
                                           mbedtls_ctr_drbg_random, &s_ctr_drbg);

        /* Serialize the shared X-coordinate to a fixed 32-byte big-endian buffer.
         * Capture the write return: mbedtls_mpi_write_binary fails if the value
         * somehow needs more than 32 bytes, which must not be treated as a valid
         * secret. */
        uint8_t shared_secret[32];
        if (ret == 0) {
            ret = mbedtls_mpi_write_binary(&shared_z, shared_secret, 32);
        }

        mbedtls_mpi_free(&d);
        mbedtls_mpi_free(&shared_z);
        mbedtls_ecp_point_free(&ephQ);
        mbedtls_ecp_group_free(&grp);

        if (ret != 0) {
            ESP_LOGE(TAG, "ECDH shared secret failed: -0x%04x", -ret);
            goto cleanup;
        }

        /* 3. Base64-decode salt, IV, tag, ciphertext.
         * Every decode return is checked: a truncated/invalid field would
         * otherwise leave a short buffer and surface downstream as a misleading
         * GCM auth failure (or use the wrong salt in HKDF), instead of a clear
         * "bad field" error. IV and tag also have exact expected lengths. */
        uint8_t salt[32];
        size_t salt_len = 0;
        if (cJSON_IsString(salt_b64)) {
            ret = mbedtls_base64_decode(salt, sizeof(salt), &salt_len,
                                        (const uint8_t *)salt_b64->valuestring,
                                        strlen(salt_b64->valuestring));
            if (ret != 0) {
                ESP_LOGE(TAG, "salt base64 decode failed: -0x%04x", -ret);
                memset(shared_secret, 0, sizeof(shared_secret));
                goto cleanup;
            }
        }

        uint8_t iv[12];
        size_t iv_len = 0;
        ret = mbedtls_base64_decode(iv, sizeof(iv), &iv_len,
                                    (const uint8_t *)iv_b64->valuestring,
                                    strlen(iv_b64->valuestring));
        if (ret != 0 || iv_len != 12) {
            ESP_LOGE(TAG, "Invalid GCM IV (decode=-0x%04x, len=%zu, expected 12)", -ret, iv_len);
            memset(shared_secret, 0, sizeof(shared_secret));
            goto cleanup;
        }

        uint8_t tag[16];
        size_t tag_len = 0;
        ret = mbedtls_base64_decode(tag, sizeof(tag), &tag_len,
                                    (const uint8_t *)tag_b64->valuestring,
                                    strlen(tag_b64->valuestring));
        if (ret != 0 || tag_len != 16) {
            ESP_LOGE(TAG, "Invalid GCM tag (decode=-0x%04x, len=%zu, expected 16)", -ret, tag_len);
            memset(shared_secret, 0, sizeof(shared_secret));
            goto cleanup;
        }

        /* Ciphertext can be large — heap allocate */
        size_t ct_b64_len = strlen(encrypted_b64->valuestring);
        size_t ct_max_len = (ct_b64_len * 3) / 4 + 4;
        uint8_t *ciphertext = (uint8_t *)malloc(ct_max_len);
        if (!ciphertext) {
            memset(shared_secret, 0, sizeof(shared_secret));
            goto cleanup;
        }
        size_t ct_len = 0;
        ret = mbedtls_base64_decode(ciphertext, ct_max_len, &ct_len,
                                    (const uint8_t *)encrypted_b64->valuestring,
                                    ct_b64_len);
        if (ret != 0) {
            ESP_LOGE(TAG, "ciphertext base64 decode failed: -0x%04x", -ret);
            memset(shared_secret, 0, sizeof(shared_secret));
            free(ciphertext);
            goto cleanup;
        }

        /* 4. HKDF-SHA256: derive AES key from shared secret + salt */
        uint8_t aes_key[32];
        if (!hkdf_sha256(shared_secret, 32, salt, salt_len, aes_key, 32)) {
            ESP_LOGE(TAG, "HKDF key derivation failed");
            memset(shared_secret, 0, sizeof(shared_secret));
            free(ciphertext);
            goto cleanup;
        }
        memset(shared_secret, 0, sizeof(shared_secret));

        /* 5. AES-256-GCM authenticated decryption */
        uint8_t *plaintext = (uint8_t *)malloc(ct_len + 1);
        if (!plaintext) {
            free(ciphertext);
            goto cleanup;
        }

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, aes_key, 256);
        memset(aes_key, 0, sizeof(aes_key));

        if (ret == 0) {
            ret = mbedtls_gcm_auth_decrypt(&gcm, ct_len,
                                            iv, iv_len,
                                            NULL, 0,
                                            tag, 16,
                                            ciphertext, plaintext);
        }
        mbedtls_gcm_free(&gcm);
        free(ciphertext);

        if (ret != 0) {
            ESP_LOGE(TAG, "AES-GCM decryption failed: -0x%04x%s", -ret,
                     ret == MBEDTLS_ERR_GCM_AUTH_FAILED ? " (auth tag mismatch)" : "");
            free(plaintext);
            goto cleanup;
        }

        plaintext[ct_len] = '\0';
        result = (char *)plaintext;
        ESP_LOGD(TAG, "Decrypted %zu bytes", ct_len);
    }

cleanup:
    mbedtls_pk_free(&ephPk);
    cJSON_Delete(root);
    return result;
}

const char *security_manager_get_public_key_pem(void)
{
    return s_public_key_pem;
}

esp_err_t security_manager_upload_public_key(const char *server_host,
                                              const char *auth_token,
                                              const char *device_id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Server endpoint: POST /upload?name={device_id}&key_type=ECDH-P256
     * Auth: Authorization: Bearer {auth_token} header — keeps the token out of
     * URL-shaped log sinks (Cloudflare analytics, access logs, APM URL tags).
     * Body: multipart/form-data with a "file" field containing PEM public key.
     * The server still accepts ?token= as a fallback for legacy clients. */
    char url[256];
    snprintf(url, sizeof(url), "https://%s/upload?name=%s&key_type=ECDH-P256",
             server_host, device_id);

    /* Build multipart form-data body */
    const char *boundary = "----ESP32ECDHKeyUpload";
    char body[1024];
    snprintf(body, sizeof(body),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"file\"; filename=\"device_key.pem\"\r\n"
             "Content-Type: application/x-pem-file\r\n\r\n"
             "%s\r\n"
             "--%s--\r\n",
             boundary, s_public_key_pem, boundary);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);

    char content_type[80];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_token);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK && (status == 200 || status == 409)) {
        /* 409 = KEY_EXISTS: server already has a key for this device.
         * Treat as success — either same key (reboot) or different key (NVS erased).
         * If different key, decryption will fail but that's a separate recovery flow. */
        s_key_uploaded = true;

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            uint8_t uploaded = 1;
            nvs_set_blob(handle, NVS_KEY_UPLOADED, &uploaded, sizeof(uploaded));
            nvs_commit(handle);
            nvs_close(handle);
        }

        ESP_LOGI(TAG, "Public key uploaded to server");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Key upload failed: http_err=%s status=%d", esp_err_to_name(ret), status);
    return ESP_FAIL;
}

bool security_manager_is_key_uploaded(void)
{
    return s_key_uploaded;
}

void security_manager_reset_key_uploaded(void)
{
    s_key_uploaded = false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        uint8_t uploaded = 0;
        nvs_set_blob(handle, NVS_KEY_UPLOADED, &uploaded, sizeof(uploaded));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool security_manager_is_enabled(void)
{
    return s_initialized;
}
