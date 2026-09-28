/**
 * OTA Manager — ESP-IDF port using esp_https_ota + esp_ota_ops.
 *
 * Replaces:
 *   HTTPClient → esp_http_client
 *   Arduino Update.h → esp_ota_begin/write/end
 *   Boot validation → probation + self-test (see below)
 *
 * Same ECDSA P-256 signature verification using mbedTLS.
 * Dual partition scheme with atomic swap (same partitions.csv).
 *
 * Install flow:
 *   1. Check server for update → parse JSON response
 *   2. Start download; check the image header (project name, version) before
 *      anything past the header is written
 *   3. Stream the rest to the inactive OTA partition
 *   4. Compute SHA-256 of the written image, verify the ECDSA signature
 *   5. Finalize (set boot partition), record the transition in NVS, restart
 *
 * First boot of a new image (probation):
 *   The bootloader starts the new image in ESP_OTA_IMG_PENDING_VERIFY. It is
 *   NOT marked valid at boot. It is confirmed (ota_manager_confirm) once it
 *   proves it can reach the server — the property that matters most, because a
 *   device that can reach the server can always fetch a fixed release. Until
 *   then any reset (crash, watchdog, power loss, deep sleep) makes the
 *   bootloader boot the previous image, and a 10-minute self-test timeout rolls
 *   back explicitly if the device stays awake but never gets there.
 *
 * After confirmation (crash-loop guard):
 *   For the first hour (wall-clock, so deep-sleep cycles count), three
 *   panic/watchdog resets mark this image invalid so the bootloader boots the
 *   previous slot. This covers bugs that only surface after the self-test
 *   (e.g. while rendering the first reaction).
 *
 * Cross-firmware migration:
 *   If the previous slot holds a different project (the Arduino firmware),
 *   the image is confirmed at boot instead. Rolling back to a firmware that
 *   has no failure memory would only make it reinstall this one in a loop,
 *   and it may not be able to read this firmware's WiFi credentials, so the
 *   normal self-test can't run before a person provisions the device.
 *
 * Failure memory:
 *   An image that fails OTA_MAX_ATTEMPTS_PER_VERSION install attempts (rolled
 *   back because it crashed or timed out, or failed header/hash/signature
 *   checks) is not installed again, so a bad release can't cause an endless
 *   download → boot → rollback loop. Memory is keyed on version + sha256, so a
 *   corrected build published under the same version installs normally, as
 *   does any other version. Rollbacks caused by power (brownout, power loss,
 *   deep sleep) are reported but not counted — they say nothing about the
 *   release.
 */

#include "ota_manager.h"
#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_timer.h"
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

/* Install attempts allowed per version before it is skipped. 2 = one retry,
 * so a coincidental power loss or WiFi outage during a good release's first
 * boot doesn't strand the device on the old version; a genuinely bad release
 * is tried twice and then left alone until a different version is published. */
#define OTA_MAX_ATTEMPTS_PER_VERSION  2

/* How long a probationary image may run without passing the self-test before
 * it rolls itself back. A healthy boot confirms within about a minute (WiFi
 * connect + boot firmware check); 10 minutes leaves room for slow networks and
 * the optional 0-60s boot jitter. */
#define OTA_PROBATION_TIMEOUT_US      (10LL * 60 * 1000000)

/* Crash-loop guard: armed for this long after an update is confirmed, and
 * trips after this many panic/watchdog resets within that window. */
#define OTA_CRASH_GUARD_WINDOW_US     (60LL * 60 * 1000000)
#define OTA_CRASH_GUARD_MAX_CRASHES   3

/* NVS namespace "ota" keys:
 *   old_ver / new_ver  version transition of the last install (until reported)
 *   new_sha            sha256 of the installed image (until reported)
 *   outcome            OTA_OUTCOME_* for that install (until reported)
 *   why                human-readable rollback reason (until reported)
 *   fail_ver/fail_sha/fail_cnt  failure memory (see OTA_MAX_ATTEMPTS_PER_VERSION)
 *   guard_until        wall-clock deadline (time_t) while the crash-loop guard is armed */
enum { OTA_OUTCOME_NONE = 0, OTA_OUTCOME_SUCCESS = 1, OTA_OUTCOME_ROLLBACK = 2 };

static std::atomic<bool> s_pending_verify{false};
static esp_timer_handle_t s_probation_timer = NULL;
static esp_timer_handle_t s_crash_guard_timer = NULL;

/* Crash counter for the crash-loop guard. RTC_NOINIT memory survives panic and
 * watchdog resets (what it counts) but not power loss; the magic value tells a
 * real count apart from power-on garbage. */
#define CRASH_GUARD_MAGIC 0x0A7AC0DEu
RTC_NOINIT_ATTR static uint32_t s_crash_guard_magic;
RTC_NOINIT_ATTR static uint32_t s_crash_guard_count;

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
 * Version equality on the numeric MAJOR.MINOR.PATCH triple. The deploy script
 * publishes the base version ("2.1.0") even when the image was built from a
 * suffixed version.txt ("2.1.0-beta.1"), so exact string comparison between
 * the server's version and the image's embedded version would misfire.
 */
static bool versions_equal(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) return false;
    return semver_compare_numeric(a, b) == 0;
}

/**
 * Verify firmware image hash and signature.
 *
 * Reads the OTA partition that was just written, computes SHA-256,
 * compares against expected hash, then verifies ECDSA signature.
 */
static bool verify_firmware(const firmware_info_t *info, char *why, size_t why_len)
{
    /* Signature is MANDATORY — fail closed. The server always ECDSA-signs
     * (P-256) firmware, so a response with no signature means either a
     * misconfigured/compromised server or a tampered download_url followed
     * verbatim. Installing it would defeat the whole hardcoded-key protection,
     * so refuse rather than "skip verification" (ESP-4). The SHA-256 hash below
     * remains an optional integrity pre-check; the signature is the trust root. */
    if (info->signature[0] == '\0') {
        ESP_LOGE(TAG, "Firmware response carries no signature — refusing to install (fail closed)");
        snprintf(why, why_len, "no signature in firmware response");
        return false;
    }

    /* Get the next OTA partition (the one we just wrote to) */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "Cannot find OTA update partition");
        snprintf(why, why_len, "no OTA update partition");
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
            snprintf(why, why_len, "partition read failed: %s", esp_err_to_name(err));
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
            snprintf(why, why_len, "sha256 mismatch");
            return false;
        }
        ESP_LOGI(TAG, "SHA-256 hash verified: %s", hash_hex);
    }

    /* Verify ECDSA signature over the hash */
    if (info->signature[0] != '\0') {
        if (!verify_ecdsa_signature(hash, info->signature)) {
            ESP_LOGE(TAG, "Firmware signature invalid — may be tampered");
            snprintf(why, why_len, "signature invalid");
            return false;
        }
    }

    return true;
}

/* ---------- NVS helpers (namespace "ota") ---------- */

static bool ota_nvs_get_str(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t len = size;
    return nvs_get_str(h, key, buf, &len) == ESP_OK;
}

/**
 * Does the stored failure record match this image? Same version, and the same
 * sha256 when both sides know it (a rollback recorded by an older firmware or
 * a header-check failure may not carry a hash).
 */
static bool failure_record_matches(nvs_handle_t h, const char *version, const char *sha)
{
    char prev_ver[32] = {0};
    char prev_sha[65] = {0};
    if (!ota_nvs_get_str(h, "fail_ver", prev_ver, sizeof(prev_ver)) ||
        !versions_equal(prev_ver, version)) {
        return false;
    }
    ota_nvs_get_str(h, "fail_sha", prev_sha, sizeof(prev_sha));
    if (prev_sha[0] && sha && sha[0] && strcmp(prev_sha, sha) != 0) {
        return false;  /* same version, different build */
    }
    return true;
}

/**
 * Count one failed install attempt of the image (version, sha): same image as
 * the stored record → increment, otherwise start a new record at 1.
 */
static void record_failed_attempt(const char *version, const char *sha)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t cnt = 0;
    if (failure_record_matches(h, version, sha)) {
        nvs_get_u8(h, "fail_cnt", &cnt);
    } else {
        nvs_set_str(h, "fail_ver", version);
        nvs_erase_key(h, "fail_sha");
    }
    if (sha && sha[0]) nvs_set_str(h, "fail_sha", sha);
    if (cnt < UINT8_MAX) cnt++;
    nvs_set_u8(h, "fail_cnt", cnt);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "Firmware %s: failed install attempt %u of %d allowed",
             version, cnt, OTA_MAX_ATTEMPTS_PER_VERSION);
}

/** Number of failed install attempts recorded for this image (0 if none). */
static uint8_t failed_attempts_for(const char *version, const char *sha)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t cnt = 0;
    if (failure_record_matches(h, version, sha)) {
        nvs_get_u8(h, "fail_cnt", &cnt);
    }
    nvs_close(h);
    return cnt;
}

/**
 * POST an install result to /api/firmware/stats. The server logs it to
 * update_logs as "update_<status>" (e.g. update_success, update_rollback,
 * update_failed) with the error text, which is how a bad release becomes
 * visible without physical access to devices. Returns true on HTTP 200.
 */
static bool post_update_stats(const char *old_ver, const char *new_ver,
                              const char *status, const char *error)
{
    const app_config_t *cfg = config_manager_get_config();
    char url[256];
    snprintf(url, sizeof(url), "https://%s/api/firmware/stats", cfg->server.host);

    /* cJSON escapes the free-text error string */
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "device_id", cfg->device.id);
    if (old_ver && old_ver[0]) cJSON_AddStringToObject(root, "old_version", old_ver);
    cJSON_AddStringToObject(root, "new_version", new_ver);
    cJSON_AddStringToObject(root, "status", status);
    if (error && error[0]) cJSON_AddStringToObject(root, "error", error);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        cJSON_free(body);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(body);

    if (err != ESP_OK || http_status != 200) {
        ESP_LOGW(TAG, "Update stats report (%s) failed: %s, HTTP %d",
                 status, esp_err_to_name(err), http_status);
        return false;
    }
    return true;
}

/* ---------- Probation, self-test and crash-loop guard ---------- */

/**
 * Self-test timeout: the image has been running for OTA_PROBATION_TIMEOUT_US
 * without reaching a confirmation checkpoint — it booted, but can't reach the
 * server (or never got far enough to try). Roll back explicitly rather than
 * leave a device that can't fetch a fix running indefinitely. Runs in the
 * esp_timer task.
 */
static void probation_rollback_task(void *arg)
{
    ESP_LOGE(TAG, "Self-test did not pass within %lld min — rolling back to previous firmware",
             OTA_PROBATION_TIMEOUT_US / 60000000LL);
    /* Leave the reason for the previous firmware's detect_rollback(): a reset
     * with this marker set is a genuine self-test failure and counts against
     * the image, unlike a brownout or power loss during probation. */
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "why", "self-test timeout: server not reached within 10 min of first boot");
        nvs_commit(h);
        nvs_close(h);
    }
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only returns on failure (no valid previous image to boot). Keep running:
     * with nothing to fall back to, this image is the best available. */
    ESP_LOGE(TAG, "Rollback not possible (%s) — continuing on this firmware",
             esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void probation_timeout_cb(void *arg)
{
    if (!s_pending_verify.exchange(false)) return;  /* confirmed meanwhile */

    /* The esp_timer task's stack (3.5 KB) is too small to safely rewrite otadata
     * and run the restart shutdown handlers (WiFi stop etc.), so hand off. */
    if (xTaskCreate(probation_rollback_task, "ota_rollback", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start rollback task — the next reset will roll back instead");
        s_pending_verify.store(true);
    }
}

/** Crash-loop guard window elapsed without tripping — disarm it. */
static void crash_guard_disarm(nvs_handle_t h)
{
    s_crash_guard_count = 0;
    nvs_erase_key(h, "guard_until");
    nvs_commit(h);
}

/** Awake for the whole guard window without tripping — disarm it. */
static void crash_guard_expired_cb(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        crash_guard_disarm(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Firmware stable for %lld min after update — crash-loop guard disarmed",
             OTA_CRASH_GUARD_WINDOW_US / 60000000LL);
}

static void start_one_shot(esp_timer_handle_t *timer, esp_timer_cb_t cb,
                           const char *name, int64_t timeout_us)
{
    if (!*timer) {
        esp_timer_create_args_t args = {};
        args.callback = cb;
        args.name = name;
        if (esp_timer_create(&args, timer) != ESP_OK) return;
    }
    esp_timer_stop(*timer);  /* ESP_ERR_INVALID_STATE if not running — harmless */
    esp_timer_start_once(*timer, timeout_us);
}

/**
 * Crash-loop guard, run early on every boot of a confirmed image. Counts
 * panic / watchdog resets while the guard is armed (first hour after an
 * update) and, on the third, marks this image invalid and reboots — the
 * bootloader then boots the previous slot, which stays in its VALID state (no
 * second probation) while this one can never be selected again.
 *
 * The window is a wall-clock deadline stored in NVS, not an awake-time timer:
 * a battery device may never stay awake for an hour, and the guard must not
 * remain armed for months and then count three unrelated crashes.
 *
 * ESP_RST_WDT (RTC watchdog) is deliberately not counted: on the ESP32-S3 a
 * brownout can be misreported as it (and as ESP_RST_SW), and a low battery
 * must not be mistaken for bad firmware.
 */
static void crash_guard_check(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    if (s_crash_guard_magic != CRASH_GUARD_MAGIC || rr == ESP_RST_POWERON) {
        s_crash_guard_magic = CRASH_GUARD_MAGIC;
        s_crash_guard_count = 0;
    }

    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    int64_t guard_until = 0;
    if (nvs_get_i64(h, "guard_until", &guard_until) != ESP_OK) {
        s_crash_guard_count = 0;
        nvs_close(h);
        return;  /* not armed */
    }
    if ((int64_t)time(NULL) >= guard_until) {
        crash_guard_disarm(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Crash-loop guard window elapsed — disarmed");
        return;
    }

    bool crash = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT);
    if (crash) {
        s_crash_guard_count++;
        ESP_LOGW(TAG, "Crash reset %lu of %d since update (reason %d)",
                 (unsigned long)s_crash_guard_count, OTA_CRASH_GUARD_MAX_CRASHES, rr);
    }

    if (s_crash_guard_count < OTA_CRASH_GUARD_MAX_CRASHES) {
        nvs_close(h);
        /* Still armed. The awake-time timer disarms early on a device that
         * stays up the whole window (USB power) so its logs say so. */
        start_one_shot(&s_crash_guard_timer, crash_guard_expired_cb, "ota_guard",
                       OTA_CRASH_GUARD_WINDOW_US);
        return;
    }

    /* Tripped. Record the transition as a rollback so the previous firmware
     * reports it and skips this version (failure memory), then invalidate. */
    const char *running_ver = esp_app_get_description()->version;
    crash_guard_disarm(h);
    if (!esp_ota_check_rollback_is_possible()) {
        nvs_close(h);
        ESP_LOGE(TAG, "Crash loop after update, but no valid previous firmware — staying on %s",
                 running_ver);
        return;
    }
    const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t prev_desc = {};
    if (!prev || esp_ota_get_partition_description(prev, &prev_desc) != ESP_OK) {
        prev_desc.version[0] = '\0';
    }
    char new_sha[65] = {0};
    ota_nvs_get_str(h, "new_sha", new_sha, sizeof(new_sha));
    nvs_set_str(h, "old_ver", prev_desc.version);
    nvs_set_str(h, "new_ver", running_ver);
    nvs_set_u8(h, "outcome", OTA_OUTCOME_ROLLBACK);
    nvs_set_str(h, "why", "crash loop after update (3 crash resets within 1 h)");
    nvs_commit(h);
    nvs_close(h);
    record_failed_attempt(running_ver, new_sha);

    ESP_LOGE(TAG, "Crash loop after update — invalidating %s, booting previous firmware %s",
             running_ver, prev_desc.version);
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only returns if the other slot holds no valid image. */
    ESP_LOGE(TAG, "Rollback not possible (%s) — staying on %s", esp_err_to_name(err), running_ver);
}

/**
 * True when the other OTA slot holds a bootable image from a different
 * project, i.e. this image was installed by the Arduino firmware
 * (cross-firmware migration). See the header comment.
 */
static bool previous_image_is_foreign(void)
{
    const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t prev_desc;
    if (!prev || esp_ota_get_partition_description(prev, &prev_desc) != ESP_OK) {
        return false;
    }
    return strncmp(prev_desc.project_name, esp_app_get_description()->project_name,
                   sizeof(prev_desc.project_name)) != 0;
}

/**
 * Determine whether the last install booted. new_ver in NVS is the image we
 * installed; if a different version is running, the bootloader rolled it back
 * (reset before the self-test passed, or explicit self-test timeout). Recorded
 * once — outcome stays set until the report is acknowledged.
 */
static void detect_rollback(const char *running_ver)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;

    char new_ver[32] = {0};
    char new_sha[65] = {0};
    char why[128] = {0};
    uint8_t outcome = OTA_OUTCOME_NONE;
    bool have_install = ota_nvs_get_str(h, "new_ver", new_ver, sizeof(new_ver));
    nvs_get_u8(h, "outcome", &outcome);

    if (!have_install || outcome != OTA_OUTCOME_NONE || versions_equal(new_ver, running_ver)) {
        nvs_close(h);
        return;
    }
    ota_nvs_get_str(h, "new_sha", new_sha, sizeof(new_sha));

    /* Only count it against the image when the image itself failed: its own
     * self-test timeout left a `why` marker, or the reset that ended it was a
     * crash. The bootloader picks the slot without resetting again, so this
     * boot's reset reason is the one that ended the probationary image. A
     * brownout, power loss or deep sleep is reported but not counted — a low
     * battery must not use up a good release's attempts (ESP_RST_SW without
     * the marker is also excluded: on the ESP32-S3 a brownout can be
     * misreported as SW). */
    esp_reset_reason_t rr = esp_reset_reason();
    bool self_test_timeout = ota_nvs_get_str(h, "why", why, sizeof(why)) && why[0];
    bool crashed = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT);
    bool counts = self_test_timeout || crashed;
    if (!self_test_timeout) {
        snprintf(why, sizeof(why), "rolled back on first boot before confirmation (reset reason %d%s)",
                 rr, crashed ? ", crash" : ", not counted as a firmware failure");
        nvs_set_str(h, "why", why);
    }

    nvs_set_u8(h, "outcome", OTA_OUTCOME_ROLLBACK);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGE(TAG, "Firmware %s did not pass its first boot — running %s again (%s)",
             new_ver, running_ver, why);
    if (counts) record_failed_attempt(new_ver, new_sha);
}

void ota_manager_boot_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Running firmware: %s (partition: %s)", app_desc->version, running->label);

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        /* Probation: the crash-loop guard is the bootloader's job until the
         * self-test passes (any reset now rolls back). */
        s_pending_verify.store(true);
        if (previous_image_is_foreign()) {
            ota_manager_confirm("installed by a different firmware — no useful rollback target");
            return;
        }
        start_one_shot(&s_probation_timer, probation_timeout_cb, "ota_probation",
                       OTA_PROBATION_TIMEOUT_US);
        ESP_LOGW(TAG, "First boot after update — on probation until the server is reached "
                      "(rolls back after %lld min otherwise)",
                 OTA_PROBATION_TIMEOUT_US / 60000000LL);
        return;
    }

    detect_rollback(app_desc->version);
    crash_guard_check();
}

bool ota_manager_is_pending_verify(void)
{
    return s_pending_verify.load();
}

void ota_manager_confirm(const char *reason)
{
    if (!s_pending_verify.exchange(false)) return;  /* not on probation / already confirmed */

    if (s_probation_timer) esp_timer_stop(s_probation_timer);
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    const char *running_ver = esp_app_get_description()->version;
    if (err != ESP_OK) {
        /* Leave the image on probation so a later checkpoint retries; the
         * timer stays stopped so a transient flash error can't force a rollback. */
        ESP_LOGE(TAG, "Failed to confirm firmware %s: %s — will retry at the next checkpoint",
                 running_ver, esp_err_to_name(err));
        s_pending_verify.store(true);
        return;
    }
    ESP_LOGI(TAG, "Self-test passed (%s) — firmware %s confirmed", reason, running_ver);

    /* Record success for the report, clear this version's failure memory, and
     * arm the crash-loop guard for the next hour. */
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        char new_ver[32] = {0};
        if (ota_nvs_get_str(h, "new_ver", new_ver, sizeof(new_ver)) &&
            versions_equal(new_ver, running_ver)) {
            nvs_set_u8(h, "outcome", OTA_OUTCOME_SUCCESS);
        }
        char new_sha[65] = {0};
        ota_nvs_get_str(h, "new_sha", new_sha, sizeof(new_sha));
        if (failure_record_matches(h, running_ver, new_sha)) {
            nvs_erase_key(h, "fail_ver");
            nvs_erase_key(h, "fail_sha");
            nvs_erase_key(h, "fail_cnt");
        }
        /* time() is at least the build-time floor set early in boot; on the
         * normal path (confirmed after WiFi) it has been synced. If it is only
         * the floor, the deadline is reached once the clock syncs — 1 h after
         * the build date is long past — so the guard disarms on the next boot
         * rather than staying armed. */
        nvs_set_i64(h, "guard_until",
                    (int64_t)time(NULL) + OTA_CRASH_GUARD_WINDOW_US / 1000000LL);
        nvs_commit(h);
        nvs_close(h);
    }
    s_crash_guard_magic = CRASH_GUARD_MAGIC;
    s_crash_guard_count = 0;
    start_one_shot(&s_crash_guard_timer, crash_guard_expired_cb, "ota_guard",
                   OTA_CRASH_GUARD_WINDOW_US);
}

void ota_manager_check_on_boot(void)
{
    /* Success is only reported once the self-test has passed; a probationary
     * image may still roll back, in which case the previous firmware reports
     * the rollback instead. */
    if (s_pending_verify.load()) {
        ESP_LOGI(TAG, "Firmware on probation — install report deferred until confirmed");
        return;
    }

    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;

    char old_ver[32] = {0};
    char new_ver[32] = {0};
    char why[128] = {0};
    uint8_t outcome = OTA_OUTCOME_NONE;
    if (!ota_nvs_get_str(h, "new_ver", new_ver, sizeof(new_ver))) {
        nvs_close(h);
        return;  /* nothing to report */
    }
    ota_nvs_get_str(h, "old_ver", old_ver, sizeof(old_ver));
    ota_nvs_get_str(h, "why", why, sizeof(why));
    nvs_get_u8(h, "outcome", &outcome);

    /* OUTCOME_NONE with the installed version running means it was confirmed
     * by a firmware that predates the outcome key — treat as success. */
    bool rollback = (outcome == OTA_OUTCOME_ROLLBACK);
    const char *status = rollback ? "rollback" : "success";

    ESP_LOGI(TAG, "Reporting OTA %s: %s → %s", status, old_ver, new_ver);
    if (post_update_stats(old_ver, new_ver, status, rollback ? why : NULL)) {
        nvs_erase_key(h, "old_ver");
        nvs_erase_key(h, "new_ver");
        nvs_erase_key(h, "new_sha");
        nvs_erase_key(h, "outcome");
        nvs_erase_key(h, "why");
        nvs_commit(h);
    }
    /* On failure the entries stay for retry on the next boot. */
    nvs_close(h);
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

    /* Self-test checkpoint: this firmware reached the server over TLS and got a
     * valid firmware-check answer, so it can fetch a fixed release if it turns
     * out to be bad. */
    ota_manager_confirm("firmware server reachable");

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

    /* Failure memory: don't reinstall a version that already failed its allowed
     * attempts (rolled back or failed integrity checks). Reported as "no update"
     * so callers behave as if up to date until a different version is published. */
    uint8_t failed = failed_attempts_for(info->version, info->sha256_hash);
    if (failed >= OTA_MAX_ATTEMPTS_PER_VERSION) {
        ESP_LOGW(TAG, "Skipping firmware %s: failed %u install attempts",
                 info->version, failed);
        s_status = OTA_STATUS_IDLE;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Update available: %s (%u bytes, required=%d)",
             info->version, info->size, info->required);

    s_status = OTA_STATUS_IDLE;
    return ESP_OK;
}

/**
 * Common failure exit for ota_manager_download_and_install(): abort the OTA
 * handle (if still open), report the failure to the server, and — for
 * integrity failures (wrong image, hash, signature, image validation) — count
 * an attempt against this version. Transport failures (timeouts, dropped
 * connections) are not counted: they say nothing about the release.
 */
static esp_err_t fail_install(esp_https_ota_handle_t handle, const firmware_info_t *info,
                              esp_err_t err, bool integrity, const char *why)
{
    ESP_LOGE(TAG, "Firmware %s install failed: %s", info->version, why);
    if (handle) esp_https_ota_abort(handle);
    if (integrity) record_failed_attempt(info->version, info->sha256_hash);
    post_update_stats(esp_app_get_description()->version, info->version, "failed", why);
    s_status = OTA_STATUS_FAILED;
    return err;
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

    char why[96];
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        snprintf(why, sizeof(why), "download start failed: %s", esp_err_to_name(ret));
        return fail_install(NULL, info, ret, false, why);
    }

    /* Image header check, before anything past the header is written to flash.
     * esp_https_ota already rejects images built for a different chip; this
     * also rejects a signed image that isn't this firmware (e.g. an Arduino
     * build published under a shared variant name — different project name) or
     * isn't the version the server advertised (publish mix-up). */
    esp_app_desc_t new_desc;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_desc);
    if (ret != ESP_OK) {
        snprintf(why, sizeof(why), "image header unreadable: %s", esp_err_to_name(ret));
        return fail_install(ota_handle, info, ret, false, why);
    }
    const esp_app_desc_t *running_desc = esp_app_get_description();
    if (strncmp(new_desc.project_name, running_desc->project_name,
                sizeof(new_desc.project_name)) != 0) {
        snprintf(why, sizeof(why), "image is '%.32s', expected '%.32s'",
                 new_desc.project_name, running_desc->project_name);
        return fail_install(ota_handle, info, ESP_ERR_INVALID_RESPONSE, true, why);
    }
    if (!versions_equal(new_desc.version, info->version)) {
        snprintf(why, sizeof(why), "image version %.32s, advertised %.31s",
                 new_desc.version, info->version);
        return fail_install(ota_handle, info, ESP_ERR_INVALID_RESPONSE, true, why);
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
        snprintf(why, sizeof(why), "download failed: %s", esp_err_to_name(ret));
        return fail_install(ota_handle, info, ret, false, why);
    }
    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        snprintf(why, sizeof(why), "download incomplete");
        return fail_install(ota_handle, info, ESP_FAIL, false, why);
    }

    /* Verify firmware integrity before finalizing.
     * Read the written partition, compute SHA-256, verify ECDSA signature. */
    s_status = OTA_STATUS_VERIFYING;
    if (!verify_firmware(info, why, sizeof(why))) {
        return fail_install(ota_handle, info, ESP_ERR_INVALID_RESPONSE, true, why);
    }

    /* Finalize OTA: validate the image and set the boot partition to it.
     * esp_https_ota_finish frees the handle whether or not it succeeds. */
    s_status = OTA_STATUS_INSTALLING;
    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        /* Only a rejected image counts against the release; a flash write error
         * while switching the boot partition is a device problem. */
        bool image_bad = (ret == ESP_ERR_OTA_VALIDATE_FAILED || ret == ESP_ERR_IMAGE_INVALID);
        snprintf(why, sizeof(why), "%s: %s",
                 image_bad ? "image validation failed" : "finalize failed", esp_err_to_name(ret));
        return fail_install(NULL, info, ret, image_bad, why);
    }

    ESP_LOGI(TAG, "OTA update successful — will boot new firmware on restart");
    s_status = OTA_STATUS_SUCCESS;
    return ESP_OK;
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
        ota_manager_finalize_and_restart(&info);
        /* Does not return */
    }

    return ret;
}

void ota_manager_finalize_and_restart(const firmware_info_t *info)
{
    /* Flush the previous install's report first — the entries below overwrite
     * it (e.g. a rollback being reported just before a newer version installs). */
    ota_manager_check_on_boot();

    /* Record the transition only after a successful download + verify. On the
     * next boot, detect_rollback() compares new_ver with the running version,
     * and the report goes out once the outcome is known. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "old_ver", app_desc->version);
        nvs_set_str(h, "new_ver", info->version);
        nvs_set_str(h, "new_sha", info->sha256_hash);
        nvs_erase_key(h, "outcome");
        nvs_erase_key(h, "why");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "OTA update applied — restarting");
    vTaskDelay(pdMS_TO_TICKS(100));  /* Allow log to flush */
    esp_restart();
    /* Does not return */
}

ota_status_t ota_manager_get_status(void)
{
    return s_status;
}
