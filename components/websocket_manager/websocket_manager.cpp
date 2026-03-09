/**
 * WebSocket Manager — ESP-IDF port using esp_websocket_client.
 *
 * Key difference from Arduino version:
 * - No more webSocket.loop() polling in Arduino loop()
 * - esp_websocket_client runs its own internal FreeRTOS task
 * - Events arrive via callback, enabling the CPU to sleep between events
 * - This is the critical enabler for auto light sleep with WiFi
 *
 * Same JSON protocol as Arduino version:
 * - Registration: {"type":"register","device_id":"...","display_variant":"..."}
 * - Heartbeat: {"type":"heartbeat","timestamp":...}
 * - Reaction: {"type":"reaction","data":{...}} (may be encrypted)
 * - ACK: {"type":"ack","message_id":"..."}
 */

#include "websocket_manager.h"
#include "config_manager.h"
#include "security_manager.h"
#include "resilience_manager.h"
#include "display_manager.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "WS";

/* Connection timing — matches server protocol (not user-configurable).
 * Server sends heartbeats every 30s. Timeout uses 3x interval (90s) to
 * tolerate network jitter and light-sleep wake delays — same 2x-buffer
 * strategy as the Arduino client (which disconnects at 2× its 30s timeout). */
#define HEARTBEAT_INTERVAL_MS   15000
#define HEARTBEAT_TIMEOUT_MS    90000
#define WS_INITIAL_RECONNECT_MS 15000
#define WS_MAX_RECONNECT_MS     60000
#define WS_PING_INTERVAL_SEC    20

static esp_websocket_client_handle_t s_client = NULL;

/* Accessed from both the WS client's internal task (event handler) and ws_task,
 * so use atomics to prevent data races on multi-core ESP32 */
static atomic_bool s_connected = false;
static atomic_bool s_registered = false;
static atomic_int_least64_t s_last_heartbeat_us = 0;
static atomic_int_least64_t s_last_reaction_us = 0;  /* Timestamp of last delivered reaction */
static uint32_t s_disconnect_count = 0;  /* Consecutive disconnects — reset on CONNECTED */

/* Server error signaling — set by event handler, consumed by ws_task.
 * Atomic int because ws_error_code_t may not have atomic support directly. */
static atomic_int s_pending_error = WS_ERROR_NONE;
static ws_error_info_t s_error_info = {};

/* Display queue handle — set by websocket_manager_start(), used by event handler
 * to push reaction events directly (FreeRTOS queues are thread-safe) */
static QueueHandle_t s_display_queue = NULL;

/* Accumulate fragmented WebSocket frames into a complete message.
 * Only accessed from the WS client's internal task (event handler),
 * so no synchronization needed. */
static char *s_rx_buffer = NULL;
static size_t s_rx_buffer_len = 0;
static size_t s_rx_buffer_capacity = 0;

/* Static URI buffer — must outlive esp_websocket_client_init() since the
 * client may store the pointer rather than copying the string */
static char s_uri[768];

/* Maximum emoji PNG size to download (prevent OOM on malicious URLs) */
#define MAX_EMOJI_PNG_SIZE  (64 * 1024)

/**
 * Copy a JSON string field to a fixed-size destination buffer.
 * Truncates if source exceeds dest_size-1.
 */
static void copy_json_string(char *dest, size_t dest_size, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dest, item->valuestring, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else {
        dest[0] = '\0';
    }
}

/**
 * Download emoji PNG from URL via HTTP GET.
 * Returns heap-allocated buffer with PNG data, or NULL on failure.
 * Caller must free the returned buffer.
 */
static uint8_t *download_emoji_png(const char *url, size_t *out_size)
{
    if (!url || url[0] == '\0') {
        return NULL;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return NULL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > MAX_EMOJI_PNG_SIZE) {
        /* If content_length is unknown (-1), try reading up to max */
        if (content_length < 0) {
            content_length = MAX_EMOJI_PNG_SIZE;
        } else {
            ESP_LOGW(TAG, "Emoji too large: %d bytes", content_length);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return NULL;
        }
    }

    uint8_t *buf = (uint8_t *)malloc(content_length);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read = esp_http_client_read(client, (char *)buf + total_read,
                                         content_length - total_read);
        if (read <= 0) {
            break;
        }
        total_read += read;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) {
        free(buf);
        return NULL;
    }

    /* Partial reads produce corrupt PNGs that waste display queue slots
     * and heap memory. Free and return NULL so caller skips the emoji. */
    if (total_read < content_length && content_length != MAX_EMOJI_PNG_SIZE) {
        ESP_LOGW(TAG, "Emoji download incomplete: %d/%d bytes", total_read, content_length);
        free(buf);
        return NULL;
    }

    *out_size = total_read;
    return buf;
}

/**
 * Send ACK for a received message.
 * Server uses this to confirm delivery and avoid re-sending.
 */
static void send_ack(const char *message_id)
{
    if (!message_id || !s_client) {
        return;
    }
    char ack[256];
    int len = snprintf(ack, sizeof(ack), "{\"type\":\"ack\",\"id\":\"%s\"}", message_id);
    esp_websocket_client_send_text(s_client, ack, len, pdMS_TO_TICKS(5000));
    ESP_LOGD(TAG, "ACK sent for %s", message_id);
}

/**
 * Process a reaction message JSON and dispatch to display queue.
 * Handles both encrypted (ECDH envelope) and plaintext reactions.
 *
 * Plaintext reaction JSON:
 *   {"type":"reaction","message_id":"...","emoji":"smile","emoji_url":"https://...",
 *    "user":"alice","channel":"general","message":"hello world",
 *    "platform":"slack","encrypted":false}
 *
 * Encrypted reaction: same but with "ephemeral_public_key", "iv", "tag", "salt" fields.
 * The encrypted payload decrypts to the same JSON fields.
 */
/**
 * Process a reaction message and dispatch to display queue.
 * Called with already-decrypted JSON for encrypted messages (decryption
 * happens at the dispatch level in ws_event_handler), or with the raw
 * JSON for unencrypted messages.
 *
 * @param source  JSON object with reaction fields (user, channel, emoji, etc.)
 * @param was_encrypted  true if the message was decrypted from an ECDH envelope
 */
static void handle_reaction_message(const cJSON *source, bool was_encrypted)
{
    if (!s_display_queue) {
        return;
    }

    const cJSON *message_id = cJSON_GetObjectItemCaseSensitive(source, "message_id");

    /* Build display event from reaction fields */
    display_event_t evt = {};
    evt.type = DISPLAY_EVT_REACTION;
    evt.data.reaction.emoji_png_data = NULL;
    evt.data.reaction.emoji_png_size = 0;
    evt.data.reaction.is_encrypted = was_encrypted;

    copy_json_string(evt.data.reaction.user, sizeof(evt.data.reaction.user),
                     source, "user");
    copy_json_string(evt.data.reaction.channel, sizeof(evt.data.reaction.channel),
                     source, "channel");
    copy_json_string(evt.data.reaction.message_preview, sizeof(evt.data.reaction.message_preview),
                     source, "message");
    copy_json_string(evt.data.reaction.emoji_name, sizeof(evt.data.reaction.emoji_name),
                     source, "emoji");
    copy_json_string(evt.data.reaction.timestamp, sizeof(evt.data.reaction.timestamp),
                     source, "timestamp");
    copy_json_string(evt.data.reaction.platform, sizeof(evt.data.reaction.platform),
                     source, "platform");

    ESP_LOGI(TAG, "Reaction from %s in %s%s",
             evt.data.reaction.user, evt.data.reaction.channel,
             evt.data.reaction.is_encrypted ? " [encrypted]" : "");

    /* Cache emoji URL in event for RTC persistence (enables re-download on restore).
     * Download the PNG immediately for the current render. */
    const cJSON *emoji_url = cJSON_GetObjectItemCaseSensitive(source, "emoji_url");
    if (cJSON_IsString(emoji_url) && emoji_url->valuestring[0] != '\0') {
        strncpy(evt.data.reaction.emoji_url, emoji_url->valuestring,
                sizeof(evt.data.reaction.emoji_url) - 1);
        evt.data.reaction.emoji_url[sizeof(evt.data.reaction.emoji_url) - 1] = '\0';

        size_t png_size = 0;
        uint8_t *png_data = download_emoji_png(emoji_url->valuestring, &png_size);
        if (png_data) {
            evt.data.reaction.emoji_png_data = png_data;
            evt.data.reaction.emoji_png_size = png_size;
        }
    }

    /* Push to display queue (non-blocking: if queue full, drop oldest) */
    if (xQueueSend(s_display_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropping reaction");
        free(evt.data.reaction.emoji_png_data);
    }

    /* Send ACK to server */
    if (cJSON_IsString(message_id)) {
        send_ack(message_id->valuestring);
    }

    /* Track successful delivery in resilience manager */
    resilience_manager_mark_message_delivered();
    atomic_store(&s_last_reaction_us, esp_timer_get_time());
}

/* Firmware update flags — set by WS event handler, consumed by ws_task.
 * Required updates install immediately; optional updates wait for 5 min idle. */
static atomic_bool s_pending_firmware_required = false;
static atomic_bool s_pending_firmware_optional = false;

/**
 * Process a firmware update notification from WebSocket.
 * Server pushes: {"type":"firmware_update","version":"...","required":true/false}
 * Required updates are installed immediately. Optional updates are deferred
 * until the device has been idle (no reactions) for 5 minutes.
 */
static void handle_firmware_message(const cJSON *root)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *required = cJSON_GetObjectItemCaseSensitive(root, "required");
    bool is_required = cJSON_IsTrue(required);

    if (cJSON_IsString(version)) {
        ESP_LOGI(TAG, "Firmware update available: %s (%s)",
                 version->valuestring, is_required ? "required" : "optional");
    }

    if (is_required) {
        atomic_store(&s_pending_firmware_required, true);
    } else {
        atomic_store(&s_pending_firmware_optional, true);
    }
}

/**
 * Process a server error message.
 * Server sends: {"type":"error","code":"DEVICE_NOT_LINKED","message":"..."}
 * Stores the error code atomically for ws_task to consume and act on.
 * The event handler cannot take blocking actions (pairing, deep sleep) directly
 * because it runs on the WS client's internal task.
 */
static void handle_error_message(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsString(code)) {
        ESP_LOGW(TAG, "Server error with no code field");
        return;
    }

    const char *error_code = code->valuestring;
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    ESP_LOGE(TAG, "Server error: code=%s message=\"%s\"",
             error_code,
             cJSON_IsString(message) ? message->valuestring : "(none)");

    if (strcmp(error_code, "DEVICE_NOT_LINKED") == 0) {
        atomic_store(&s_pending_error, WS_ERROR_DEVICE_NOT_LINKED);

    } else if (strcmp(error_code, "TRIAL_EXPIRED") == 0) {
        /* Extract purchase URL and device ID for the purchase QR screen */
        copy_json_string(s_error_info.purchase_url, sizeof(s_error_info.purchase_url),
                         root, "purchase_url");
        copy_json_string(s_error_info.device_id, sizeof(s_error_info.device_id),
                         root, "device_id");
        atomic_store(&s_pending_error, WS_ERROR_TRIAL_EXPIRED);

    } else if (strcmp(error_code, "DEVICE_NOT_REGISTERED") == 0) {
        copy_json_string(s_error_info.device_id, sizeof(s_error_info.device_id),
                         root, "device_id");
        atomic_store(&s_pending_error, WS_ERROR_DEVICE_NOT_REGISTERED);

    } else if (strcmp(error_code, "INVALID_AUTH_TOKEN") == 0 ||
               strcmp(error_code, "AUTH_FAILED") == 0) {
        copy_json_string(s_error_info.device_id, sizeof(s_error_info.device_id),
                         root, "device_id");
        atomic_store(&s_pending_error, WS_ERROR_AUTH_FAILED);
    }
}

ws_error_code_t websocket_manager_get_pending_error(ws_error_info_t *info)
{
    ws_error_code_t err = (ws_error_code_t)atomic_exchange(&s_pending_error, WS_ERROR_NONE);
    if (err != WS_ERROR_NONE && info) {
        *info = s_error_info;
    }
    return err;
}

/**
 * Determine disconnect reason for display subtitle.
 * WiFi down → "Check WiFi", server unreachable → "Server Unavailable",
 * server OK but WS rejected → "Reconnecting..." (transient, e.g. duplicate lock).
 */
static void get_disconnect_reason(char *buf, size_t buf_size)
{
    if (!wifi_manager_is_connected()) {
        strncpy(buf, "Check WiFi", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }

    /* Build /health URL from config */
    const app_config_t *cfg = config_manager_get_config();
    char url[192];
    if (cfg->server.use_ssl) {
        if (cfg->server.port != 443) {
            snprintf(url, sizeof(url), "https://%s:%d/health", cfg->server.host, cfg->server.port);
        } else {
            snprintf(url, sizeof(url), "https://%s/health", cfg->server.host);
        }
    } else {
        if (cfg->server.port != 80) {
            snprintf(url, sizeof(url), "http://%s:%d/health", cfg->server.host, cfg->server.port);
        } else {
            snprintf(url, sizeof(url), "http://%s/health", cfg->server.host);
        }
    }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.timeout_ms = 3000;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        strncpy(buf, "Server Unavailable", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        strncpy(buf, "Reconnecting...", buf_size - 1);
    } else {
        strncpy(buf, "Server Unavailable", buf_size - 1);
    }
    buf[buf_size - 1] = '\0';
}

static void ws_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        atomic_store(&s_connected, true);
        atomic_store(&s_registered, false);
        atomic_store(&s_last_heartbeat_us, esp_timer_get_time());
        s_disconnect_count = 0;

        /* Notify display task of reconnection. display_task applies three-way
         * logic to decide whether to actually refresh the e-paper — skipping
         * redundant refreshes on normal reconnects to save battery. */
        if (s_display_queue) {
            display_event_t conn_evt = {};
            conn_evt.type = DISPLAY_EVT_CONNECTED;
            conn_evt.data.connected.show_lock = security_manager_is_key_uploaded();
            xQueueSend(s_display_queue, &conn_evt, pdMS_TO_TICKS(100));
        }

        /* Send registration message — matches Arduino format.
         * Server requires: type=register + auth_token.
         * device_type is logged server-side for diagnostics. */
        {
            const app_config_t *cfg = config_manager_get_config();
            char reg_msg[256];
            snprintf(reg_msg, sizeof(reg_msg),
                     "{\"type\":\"register\","
                     "\"device_type\":\"esp32_eink\","
                     "\"auth_token\":\"%s\"}",
                     cfg->security.auth_token);
            esp_websocket_client_send_text(s_client, reg_msg, strlen(reg_msg), portMAX_DELAY);
            ESP_LOGI(TAG, "Registration sent for device %s", cfg->device.id);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 || data->op_code == 0x00) {
            /* Text frame (0x01) or continuation frame (0x00) */
            /* Accumulate fragmented frames */
            size_t needed = s_rx_buffer_len + data->data_len + 1;
            if (needed > s_rx_buffer_capacity) {
                size_t new_cap = needed + 256;
                char *new_buf = (char *)realloc(s_rx_buffer, new_cap);
                if (new_buf == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate RX buffer (%zu bytes)", new_cap);
                    s_rx_buffer_len = 0;
                    break;
                }
                s_rx_buffer = new_buf;
                s_rx_buffer_capacity = new_cap;
            }
            memcpy(s_rx_buffer + s_rx_buffer_len, data->data_ptr, data->data_len);
            s_rx_buffer_len += data->data_len;
            s_rx_buffer[s_rx_buffer_len] = '\0';

            /* Process only when we have the complete message
             * (payload_offset + data_len >= payload_len means final fragment) */
            if (data->payload_offset + data->data_len >= data->payload_len) {
                ESP_LOGD(TAG, "RX complete (%zu bytes)", s_rx_buffer_len);

                cJSON *root = cJSON_Parse(s_rx_buffer);
                if (root) {
                    /* Server sends ECDH-encrypted messages as a raw envelope
                     * without a "type" field: {encrypted, ephemeral_public_key, iv, tag, salt}.
                     * Detect this by checking for ephemeral_public_key, decrypt first,
                     * then dispatch based on the decrypted content's type. */
                    const cJSON *ephemeral_key = cJSON_GetObjectItemCaseSensitive(root, "ephemeral_public_key");
                    cJSON *decrypted_root = NULL;
                    const cJSON *dispatch_root = root;

                    if (cJSON_IsString(ephemeral_key) && security_manager_is_enabled()) {
                        char *envelope_str = cJSON_PrintUnformatted(root);
                        if (envelope_str) {
                            char *plaintext = security_manager_decrypt(envelope_str);
                            free(envelope_str);
                            if (plaintext) {
                                decrypted_root = cJSON_Parse(plaintext);
                                free(plaintext);
                                if (decrypted_root) {
                                    dispatch_root = decrypted_root;
                                    ESP_LOGD(TAG, "ECDH envelope decrypted");
                                } else {
                                    ESP_LOGE(TAG, "Failed to parse decrypted JSON");
                                }
                            } else {
                                ESP_LOGE(TAG, "ECDH decryption failed");
                            }
                        }
                    }

                    const cJSON *type = cJSON_GetObjectItemCaseSensitive(dispatch_root, "type");
                    if (cJSON_IsString(type)) {
                        if (strcmp(type->valuestring, "heartbeat") == 0) {
                            atomic_store(&s_last_heartbeat_us, esp_timer_get_time());
                            resilience_manager_record_heartbeat();
                            ESP_LOGD(TAG, "Heartbeat received");
                        } else if (strcmp(type->valuestring, "registered") == 0) {
                            atomic_store(&s_registered, true);
                            resilience_manager_mark_connection_restored();
                            ESP_LOGI(TAG, "Registration confirmed by server");
                        } else if (strcmp(type->valuestring, "reaction") == 0) {
                            /* For encrypted reactions, pass the decrypted root
                             * which already has plaintext fields (user, channel, etc.).
                             * For unencrypted reactions, pass root as-is. */
                            handle_reaction_message(dispatch_root, decrypted_root != NULL);
                        } else if (strcmp(type->valuestring, "firmware_update") == 0) {
                            handle_firmware_message(dispatch_root);
                        } else if (strcmp(type->valuestring, "error") == 0) {
                            handle_error_message(dispatch_root);
                        }
                    }

                    if (decrypted_root) {
                        cJSON_Delete(decrypted_root);
                    }
                    cJSON_Delete(root);
                }

                s_rx_buffer_len = 0;
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        atomic_store(&s_connected, false);
        atomic_store(&s_registered, false);
        resilience_manager_mark_connection_lost();
        s_disconnect_count++;

        /* Show disconnected screen once after 10 consecutive failures.
         * Avoids repeated e-paper refreshes that drain battery and are visually disruptive.
         * Matches Arduino client behavior (metrics.failedConnections == 10). */
        if (s_display_queue && s_disconnect_count == 10) {
            display_event_t disc_evt = {};
            disc_evt.type = DISPLAY_EVT_DISCONNECTED;
            get_disconnect_reason(disc_evt.data.disconnected.subtitle,
                                  sizeof(disc_evt.data.disconnected.subtitle));
            xQueueSend(s_display_queue, &disc_evt, pdMS_TO_TICKS(100));
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;
    }
}

esp_err_t websocket_manager_start(QueueHandle_t display_queue)
{
    if (!display_queue) {
        ESP_LOGE(TAG, "display_queue is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    s_display_queue = display_queue;
    const app_config_t *cfg = config_manager_get_config();

    /* Build WebSocket URI into static buffer (must outlive the client) */
    snprintf(s_uri, sizeof(s_uri), "%s://%s:%d%s?device_id=%s&display_variant=%s",
             cfg->server.use_ssl ? "wss" : "ws",
             cfg->server.host, cfg->server.port, cfg->server.path,
             cfg->device.id, cfg->device.display_variant);

    ESP_LOGI(TAG, "Connecting to %s", s_uri);

    esp_websocket_client_config_t ws_cfg = {
        .uri = s_uri,
        .task_stack = 12288,    /* Default 4096 is too small — event handler does JSON parsing,
                                 * ECDH decryption, and emoji PNG download via esp_http_client
                                 * with TLS (handshake temporaries consume ~3-4KB stack).
                                 * 12KB provides safe headroom for the full call chain. */
        .buffer_size = 2048,
        .reconnect_timeout_ms = WS_INITIAL_RECONNECT_MS,
        .network_timeout_ms = 10000,
        .ping_interval_sec = WS_PING_INTERVAL_SEC,
    };

    if (cfg->server.use_ssl) {
        ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_websocket_client_init(&ws_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    esp_err_t ret = esp_websocket_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
    }
    return ret;
}

void websocket_manager_process(EventGroupHandle_t system_events)
{
    /* Check heartbeat timeout — server sends heartbeats every 30s,
     * if we haven't received one in 90s the connection is likely dead */
    bool connected = atomic_load(&s_connected);
    int64_t last_hb = atomic_load(&s_last_heartbeat_us);

    if (connected && last_hb > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - last_hb) / 1000;
        if (elapsed_ms > HEARTBEAT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Heartbeat timeout (%lld ms) — connection may be dead", elapsed_ms);
            resilience_manager_mark_connection_lost();
        }
    }

    /* Send periodic heartbeat to server */
    if (connected && atomic_load(&s_registered)) {
        static int64_t s_last_sent_heartbeat_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_sent_heartbeat_us > (int64_t)HEARTBEAT_INTERVAL_MS * 1000) {
            char hb[128];
            int len = snprintf(hb, sizeof(hb),
                               "{\"type\":\"heartbeat\",\"timestamp\":%lld}",
                               now / 1000000);
            websocket_manager_send(hb, len);
            s_last_sent_heartbeat_us = now;
        }
    }
}

void websocket_manager_stop(void)
{
    if (s_client) {
        /* Stop first (drains pending events), then destroy.
         * Set atomics before destroy so any in-flight event handler
         * sees disconnected state and skips buffer writes. */
        esp_websocket_client_stop(s_client);
        atomic_store(&s_connected, false);
        atomic_store(&s_registered, false);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    } else {
        atomic_store(&s_connected, false);
        atomic_store(&s_registered, false);
    }

    free(s_rx_buffer);
    s_rx_buffer = NULL;
    s_rx_buffer_len = 0;
    s_rx_buffer_capacity = 0;
}

bool websocket_manager_is_connected(void)
{
    return atomic_load(&s_connected) && atomic_load(&s_registered);
}

esp_err_t websocket_manager_send(const char *data, int len)
{
    if (!s_client || !atomic_load(&s_connected)) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(s_client, data, len, pdMS_TO_TICKS(5000));
    return (sent >= 0) ? ESP_OK : ESP_FAIL;
}

bool websocket_manager_has_pending_firmware_required(void)
{
    return atomic_exchange(&s_pending_firmware_required, false);
}

bool websocket_manager_has_pending_firmware_optional(void)
{
    return atomic_exchange(&s_pending_firmware_optional, false);
}

int64_t websocket_manager_get_last_reaction_time(void)
{
    return atomic_load(&s_last_reaction_us);
}
