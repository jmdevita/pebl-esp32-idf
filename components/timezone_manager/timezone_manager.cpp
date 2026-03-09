/**
 * Timezone Manager — fetches timezone offset from server GeoIP or
 * ipgeolocation.io API and sets the ESP32 system clock.
 *
 * Matches Arduino fetchTimezoneFromAPI() behavior:
 * - Two sources: "server" (free, default) and "ipgeolocation" (user's API key)
 * - Sets system clock via settimeofday() so time() returns real wall-clock time
 * - Persists offset and sync state in RTC memory (survives deep sleep)
 * - Sync interval configurable (default 24h, range 1-168h)
 *
 * The system clock being set is critical for quiet hours to work —
 * power_manager's is_quiet_hours() guards against un-synced time
 * by checking if time() returns a value before 2024.
 */

#include "timezone_manager.h"
#include "config_manager.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "TIMEZONE";

/* RTC memory persists across deep sleep but resets on power-on.
 * Matches Arduino's RTC_DATA_ATTR timezone variables. */
RTC_DATA_ATTR static time_t s_last_sync_timestamp = 0;
RTC_DATA_ATTR static int32_t s_timezone_offset_seconds = 0;
RTC_DATA_ATTR static time_t s_current_time = 0;
RTC_DATA_ATTR static bool s_has_ever_synced = false;

/* HTTP response buffer — timezone API responses are small (<512 bytes) */
#define HTTP_RESPONSE_BUF_SIZE  1024

/* Timestamp validation range (same as Arduino):
 * Jan 1, 2020 to Jan 1, 2100 */
#define MIN_VALID_TIMESTAMP  1577836800
#define MAX_VALID_TIMESTAMP  4102444800

/* HTTP response accumulator for esp_http_client event handler */
typedef struct {
    char *buf;
    int len;
    int capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->len + evt->data_len < resp->capacity - 1) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Parse timezone API response (same JSON format for both sources):
 * {
 *   "date_time_unix": 1761273385.454,
 *   "timezone": "America/New_York",
 *   "timezone_offset": -5,
 *   "timezone_offset_with_dst": -4,
 *   "is_dst": true
 * }
 */
static esp_err_t parse_timezone_response(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse timezone JSON");
        return ESP_FAIL;
    }

    /* Extract unix timestamp */
    cJSON *ts_item = cJSON_GetObjectItemCaseSensitive(root, "date_time_unix");
    if (!ts_item || !cJSON_IsNumber(ts_item)) {
        ESP_LOGE(TAG, "Missing or invalid date_time_unix");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    time_t timestamp = (time_t)ts_item->valuedouble;

    /* Validate timestamp range */
    if (timestamp < MIN_VALID_TIMESTAMP || timestamp > MAX_VALID_TIMESTAMP) {
        ESP_LOGE(TAG, "Timestamp out of valid range: %ld", (long)timestamp);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Extract timezone offset with DST (hours → seconds).
     * Use timezone_offset_with_dst which already includes DST adjustment,
     * matching Arduino behavior. */
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(root, "timezone_offset_with_dst");
    if (!offset_item || !cJSON_IsNumber(offset_item)) {
        ESP_LOGE(TAG, "Missing or invalid timezone_offset_with_dst");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    int32_t offset_seconds = (int32_t)(offset_item->valuedouble * 3600);

    /* Optional: log timezone name and DST status */
    const cJSON *tz_name = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    const cJSON *is_dst = cJSON_GetObjectItemCaseSensitive(root, "is_dst");
    ESP_LOGI(TAG, "Timezone: %s, offset: %+ldh (DST: %s)",
             (tz_name && cJSON_IsString(tz_name)) ? tz_name->valuestring : "Unknown",
             (long)(offset_seconds / 3600),
             (is_dst && cJSON_IsTrue(is_dst)) ? "yes" : "no");

    /* Set ESP32 system clock to UTC time.
     * This allows the hardware RTC to track time during deep sleep. */
    struct timeval tv = {
        .tv_sec = timestamp,
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);

    /* Update RTC-persisted state */
    s_current_time = timestamp;
    s_timezone_offset_seconds = offset_seconds;
    s_last_sync_timestamp = timestamp;
    s_has_ever_synced = true;

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Fetch timezone from the pebl server's GeoIP endpoint.
 * Uses the same auth headers as WebSocket (X-Device-ID, X-Auth-Token).
 */
static esp_err_t fetch_from_server(void)
{
    const app_config_t *cfg = config_manager_get_config();

    /* Build URL: {protocol}://{host}[:{port}]/api/timezone/lookup[?update_db=true] */
    char url[256];
    const char *protocol = cfg->server.use_ssl ? "https" : "http";
    bool non_default_port = (cfg->server.use_ssl && cfg->server.port != 443) ||
                            (!cfg->server.use_ssl && cfg->server.port != 80);

    if (non_default_port) {
        snprintf(url, sizeof(url), "%s://%s:%d/api/timezone/lookup%s",
                 protocol, cfg->server.host, cfg->server.port,
                 cfg->timezone.update_server ? "?update_db=true" : "");
    } else {
        snprintf(url, sizeof(url), "%s://%s/api/timezone/lookup%s",
                 protocol, cfg->server.host,
                 cfg->timezone.update_server ? "?update_db=true" : "");
    }

    ESP_LOGI(TAG, "Fetching timezone from server: %s", url);

    char response_buf[HTTP_RESPONSE_BUF_SIZE] = {};
    http_response_t resp = {
        .buf = response_buf,
        .len = 0,
        .capacity = HTTP_RESPONSE_BUF_SIZE
    };

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.timeout_ms = 10000;
    http_cfg.event_handler = http_event_handler;
    http_cfg.user_data = &resp;
    /* Use ESP-IDF's built-in CA certificate bundle for SSL verification.
     * The bundle includes Cloudflare/Let's Encrypt CAs, so this works
     * with Cloudflare tunnels unlike Arduino's setInsecure() workaround. */
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.transport_type = cfg->server.use_ssl ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    /* Auth headers — same two-factor auth as WebSocket connection */
    esp_http_client_set_header(client, "X-Device-ID", cfg->device.id);
    esp_http_client_set_header(client, "X-Auth-Token", cfg->security.auth_token);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "Server returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    return parse_timezone_response(response_buf);
}

/**
 * Fetch timezone from ipgeolocation.io API.
 * Uses the user's API key (free tier: 1000 requests/day).
 */
static esp_err_t fetch_from_ipgeolocation(void)
{
    const app_config_t *cfg = config_manager_get_config();

    if (cfg->timezone.ipgeolocation_api_key[0] == '\0') {
        ESP_LOGE(TAG, "IPGeolocation source selected but no API key configured");
        return ESP_FAIL;
    }

    char url[384];
    snprintf(url, sizeof(url), "https://api.ipgeolocation.io/timezone?apiKey=%s",
             cfg->timezone.ipgeolocation_api_key);

    ESP_LOGI(TAG, "Fetching timezone from ipgeolocation.io");

    char response_buf[HTTP_RESPONSE_BUF_SIZE] = {};
    http_response_t resp = {
        .buf = response_buf,
        .len = 0,
        .capacity = HTTP_RESPONSE_BUF_SIZE
    };

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.timeout_ms = 10000;
    http_cfg.event_handler = http_event_handler;
    http_cfg.user_data = &resp;
    /* Use ESP-IDF's built-in CA certificate bundle for SSL verification.
     * Requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y in sdkconfig (default on). */
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "IPGeolocation returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    return parse_timezone_response(response_buf);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

esp_err_t timezone_manager_init(void)
{
    ESP_LOGI(TAG, "Timezone manager init (synced=%s, offset=%+ldh)",
             s_has_ever_synced ? "yes" : "no",
             (long)(s_timezone_offset_seconds / 3600));

    /* If RTC memory has a previous sync, restore the system clock.
     * The hardware RTC keeps ticking during deep sleep, but settimeofday()
     * is lost. On deep sleep wake, gettimeofday() returns a time that
     * has advanced from the last settimeofday() by the sleep duration,
     * so we just need to verify it's still valid. */
    if (s_has_ever_synced && s_current_time > MIN_VALID_TIMESTAMP) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > MIN_VALID_TIMESTAMP) {
            ESP_LOGI(TAG, "System clock valid from RTC (epoch %ld)", (long)tv.tv_sec);
        }
    }

    return ESP_OK;
}

esp_err_t timezone_manager_fetch(void)
{
    const app_config_t *cfg = config_manager_get_config();

    /* Dispatch to configured source (matching Arduino fetchTimezoneFromAPI) */
    if (strcmp(cfg->timezone.source, "ipgeolocation") == 0) {
        return fetch_from_ipgeolocation();
    }

    /* Default to server source (also handles invalid source values) */
    return fetch_from_server();
}

void timezone_manager_sync_if_needed(bool is_cold_boot)
{
    const app_config_t *cfg = config_manager_get_config();

    bool should_sync = false;

    if (is_cold_boot) {
        ESP_LOGI(TAG, "Cold boot — timezone sync needed");
        should_sync = true;
    } else if (!s_has_ever_synced || s_current_time == 0) {
        ESP_LOGI(TAG, "Never synced — timezone sync needed");
        should_sync = true;
    } else {
        /* Check if sync interval has elapsed.
         * Use current system time (which advances during deep sleep via hardware RTC)
         * minus the last sync timestamp. */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t elapsed = tv.tv_sec - s_last_sync_timestamp;
        time_t interval = (time_t)cfg->timezone.sync_interval_hours * 3600;

        if (elapsed >= interval) {
            ESP_LOGI(TAG, "Sync interval elapsed (%ldh since last sync) — syncing",
                     (long)(elapsed / 3600));
            should_sync = true;
        } else {
            ESP_LOGD(TAG, "Next sync in %ldh",
                     (long)((interval - elapsed) / 3600));
        }
    }

    if (should_sync) {
        esp_err_t ret = timezone_manager_fetch();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Timezone fetch failed — will retry on next wake/interval");
        }
    }
}

int32_t timezone_manager_get_offset_seconds(void)
{
    return s_timezone_offset_seconds;
}

bool timezone_manager_has_synced(void)
{
    return s_has_ever_synced;
}
