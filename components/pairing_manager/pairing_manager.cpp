/**
 * Pairing Manager — ESP-IDF port of self-service device pairing.
 *
 * Uses esp_http_client streaming API (open/read/close) instead of
 * perform(), since we need to read response bodies.
 */

#include "pairing_manager.h"
#include "config_manager.h"
#include "display_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "PAIRING";
static bool s_active = false;

/* 10 minutes: matches server pairing code TTL and Arduino client's USB timeout.
 * Prevents device from polling indefinitely if user abandons setup. */
#define PAIRING_TIMEOUT_MS  (10 * 60 * 1000)

/* EVT_PAIRING_COMPLETE bit — must match app_main.c definition */
#define EVT_PAIRING_COMPLETE  BIT2

/**
 * Perform HTTP request and read response body into heap-allocated buffer.
 * Caller must free() the returned buffer. Returns NULL on failure.
 */
static char *http_request(const char *url, esp_http_client_method_t method,
                           const char *post_data, int *out_status)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return NULL;
    }

    if (post_data) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_open(client, post_data ? strlen(post_data) : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (post_data) {
        esp_http_client_write(client, post_data, strlen(post_data));
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (out_status) {
        *out_status = esp_http_client_get_status_code(client);
    }

    if (content_len <= 0 || content_len > 4096) {
        /* Try reading with a fixed buffer for chunked transfers */
        content_len = 1024;
    }

    char *response = (char *)malloc(content_len + 1);
    if (response == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    /* Loop reads until the body is complete. A single esp_http_client_read()
     * can return a short read for chunked / unknown-length responses (it returns
     * whatever is currently buffered), which previously truncated the JSON and
     * made the initial /api/pairing/request hard-fail. Mirrors the read loop in
     * ota_manager's check-for-update. The fixed buffer bound is preserved:
     * we never read past `content_len` bytes. */
    int total_read = 0;
    while (total_read < content_len) {
        int r = esp_http_client_read(client, response + total_read,
                                     content_len - total_read);
        if (r < 0) {
            /* Transport error mid-read — discard the partial body */
            total_read = -1;
            break;
        }
        if (r == 0) {
            /* No more data: connection closed or full body already received */
            break;
        }
        total_read += r;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read <= 0) {
        free(response);
        return NULL;
    }
    response[total_read] = '\0';
    return response;
}

esp_err_t pairing_manager_start(QueueHandle_t display_queue,
                                 EventGroupHandle_t system_events)
{
    s_active = true;
    const app_config_t *cfg = config_manager_get_config();

    ESP_LOGI(TAG, "Starting self-service pairing for device %s", cfg->device.id);

    /* Step 1: Request pairing code from server
     * POST /api/pairing/request?device_id={id}&device_type=esp32_eink
     * Returns: { session_id, pairing_code, expires_in } */
    char url[512];
    snprintf(url, sizeof(url), "https://%s/api/pairing/request?device_id=%s&device_type=esp32_eink",
             cfg->server.host, cfg->device.id);

    int status = 0;
    char *response = http_request(url, HTTP_METHOD_POST, NULL, &status);
    if (response == NULL || (status != 200 && status != 201)) {
        ESP_LOGE(TAG, "Pairing request failed (status %d)", status);
        free(response);
        s_active = false;
        return ESP_FAIL;
    }

    /* Parse session_id and pairing_code from server response */
    cJSON *root = cJSON_Parse(response);
    free(response);

    if (!root) {
        s_active = false;
        return ESP_FAIL;
    }

    const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *pairing_code = cJSON_GetObjectItemCaseSensitive(root, "pairing_code");

    if (!cJSON_IsString(session_id) || !cJSON_IsString(pairing_code)) {
        ESP_LOGE(TAG, "Missing session_id or pairing_code in response");
        cJSON_Delete(root);
        s_active = false;
        return ESP_FAIL;
    }

    /* Save session_id for polling */
    char saved_session_id[64];
    strncpy(saved_session_id, session_id->valuestring, sizeof(saved_session_id) - 1);
    saved_session_id[sizeof(saved_session_id) - 1] = '\0';

    /* Step 2: Display pairing code on e-paper.
     * render_pairing_code() encodes a fixed https://pebl.ink/connect URL in the
     * QR itself and shows the pairing code human-readable, so only the code is
     * passed through the event. (The display layer does not read
     * data.pairing.url — that field is left unset intentionally.) */
    display_event_t evt = {
        .type = DISPLAY_EVT_PAIRING_QR,
    };
    strncpy(evt.data.pairing.code, pairing_code->valuestring, sizeof(evt.data.pairing.code) - 1);
    xQueueSend(display_queue, &evt, portMAX_DELAY);

    ESP_LOGI(TAG, "Pairing code: %s (session: %.8s...)", pairing_code->valuestring, session_id->valuestring);
    cJSON_Delete(root);

    /* Step 3: Poll server until pairing is complete or timeout
     * GET /api/pairing/status/{session_id}
     * Returns: { status: "pending"|"claimed"|"expired", auth_token (when claimed) } */
    snprintf(url, sizeof(url), "https://%s/api/pairing/status/%s",
             cfg->server.host, saved_session_id);

    int64_t poll_start_us = esp_timer_get_time();

    while (s_active) {
        vTaskDelay(pdMS_TO_TICKS(5000));  /* Poll every 5 seconds */

        /* Check timeout — reboot so device retries pairing with a fresh code */
        int64_t elapsed_ms = (esp_timer_get_time() - poll_start_us) / 1000;
        if (elapsed_ms > PAIRING_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Pairing timed out after %d minutes — rebooting",
                     PAIRING_TIMEOUT_MS / 60000);
            s_active = false;
            vTaskDelay(pdMS_TO_TICKS(100));  /* Allow log to flush */
            esp_restart();
        }

        status = 0;
        response = http_request(url, HTTP_METHOD_GET, NULL, &status);
        if (response == NULL || status != 200) {
            free(response);
            continue;
        }

        root = cJSON_Parse(response);
        free(response);

        if (!root) {
            continue;
        }

        const cJSON *pair_status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const char *status_str = cJSON_IsString(pair_status) ? pair_status->valuestring : "unknown";

        if (strcmp(status_str, "claimed") == 0) {
            /* Pairing complete — extract and save auth token */
            const cJSON *auth_token = cJSON_GetObjectItemCaseSensitive(root, "auth_token");
            if (cJSON_IsString(auth_token) && auth_token->valuestring[0] != '\0') {
                app_config_t *mut_cfg = config_manager_get_mutable_config();
                strncpy(mut_cfg->security.auth_token, auth_token->valuestring,
                        sizeof(mut_cfg->security.auth_token) - 1);
                config_manager_save();

                ESP_LOGI(TAG, "Pairing complete — auth token saved");
                cJSON_Delete(root);
                s_active = false;

                xEventGroupSetBits(system_events, EVT_PAIRING_COMPLETE);
                return ESP_OK;
            }
        } else if (strcmp(status_str, "expired") == 0) {
            /* Code expired on server — reboot to request a fresh one */
            ESP_LOGW(TAG, "Pairing code expired — rebooting for fresh code");
            cJSON_Delete(root);
            s_active = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        /* "pending" or "rate_limited" — keep polling */
        cJSON_Delete(root);
    }

    s_active = false;
    return ESP_FAIL;
}

bool pairing_manager_is_active(void)
{
    return s_active;
}
