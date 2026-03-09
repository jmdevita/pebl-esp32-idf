/**
 * Network Diagnostics — sequential connectivity checks for IT troubleshooting.
 *
 * Runs WiFi info, DNS resolution, HTTPS /health, and WebSocket+Auth checks.
 * Results are displayed on e-paper so non-technical users can photograph
 * the screen and share it with IT support.
 *
 * Each check depends on the previous one succeeding. On first failure,
 * auth_hint is set with a human-readable explanation and remaining checks
 * are skipped (left as false).
 */

#include "diagnostics.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "power_manager.h"
#include "board.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"

static const char *TAG = "DIAG";

/* Event bits for WebSocket diagnostics handshake */
#define DIAG_WS_CONNECTED_BIT   BIT0
#define DIAG_WS_DATA_BIT        BIT1
#define DIAG_WS_ERROR_BIT       BIT2

/* Context passed to the diagnostic WebSocket event handler */
typedef struct {
    EventGroupHandle_t event_group;
    bool auth_ok;
    char error_hint[48];
} diag_ws_ctx_t;

static void diag_ws_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    diag_ws_ctx_t *ctx = (diag_ws_ctx_t *)arg;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        xEventGroupSetBits(ctx->event_group, DIAG_WS_CONNECTED_BIT);
        break;

    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
        if (data->op_code == 0x01 && data->data_len > 0) {
            /* Check for "registered" in the response — indicates auth success.
             * Simple substring search avoids pulling in cJSON for diagnostics. */
            char buf[256];
            size_t copy_len = data->data_len < sizeof(buf) - 1 ? data->data_len : sizeof(buf) - 1;
            memcpy(buf, data->data_ptr, copy_len);
            buf[copy_len] = '\0';

            if (strstr(buf, "\"registered\"")) {
                ctx->auth_ok = true;
            } else if (strstr(buf, "\"error\"")) {
                /* Extract error info for hint */
                if (strstr(buf, "invalid_token") || strstr(buf, "unauthorized")) {
                    strncpy(ctx->error_hint, "Auth token invalid/expired", sizeof(ctx->error_hint) - 1);
                } else if (strstr(buf, "trial_expired")) {
                    strncpy(ctx->error_hint, "Trial expired", sizeof(ctx->error_hint) - 1);
                } else {
                    strncpy(ctx->error_hint, "Server rejected auth", sizeof(ctx->error_hint) - 1);
                }
            }
            xEventGroupSetBits(ctx->event_group, DIAG_WS_DATA_BIT);
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_DISCONNECTED:
        xEventGroupSetBits(ctx->event_group, DIAG_WS_ERROR_BIT);
        break;
    }
}

/**
 * Check 4-5: WebSocket connect + server auth registration.
 * Creates a short-lived esp_websocket_client (separate from the main one),
 * sends a register message, and waits for the "registered" response.
 */
static void check_ws_and_auth(diag_result_t *result)
{
    const app_config_t *cfg = config_manager_get_config();

    /* Build WebSocket URI */
    char uri[576];
    snprintf(uri, sizeof(uri), "%s://%s:%d%s?device_id=%s&display_variant=%s",
             cfg->server.use_ssl ? "wss" : "ws",
             cfg->server.host, cfg->server.port, cfg->server.path,
             cfg->device.id, cfg->device.display_variant);

    diag_ws_ctx_t ctx = {};
    ctx.event_group = xEventGroupCreate();
    if (!ctx.event_group) {
        strncpy(result->auth_hint, "Out of memory", sizeof(result->auth_hint) - 1);
        return;
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = uri;
    ws_cfg.task_stack = 6144;
    ws_cfg.buffer_size = 512;
    ws_cfg.network_timeout_ms = 5000;
    ws_cfg.disable_auto_reconnect = true;  /* Single attempt only for diagnostics */
    if (cfg->server.use_ssl) {
        ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        strncpy(result->auth_hint, "WS client init failed", sizeof(result->auth_hint) - 1);
        vEventGroupDelete(ctx.event_group);
        return;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                   diag_ws_event_handler, &ctx);

    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        strncpy(result->auth_hint, "Firewall may block WSS :443", sizeof(result->auth_hint) - 1);
        esp_websocket_client_destroy(client);
        vEventGroupDelete(ctx.event_group);
        return;
    }

    /* Wait for WebSocket CONNECTED event (5s timeout) */
    EventBits_t bits = xEventGroupWaitBits(ctx.event_group,
        DIAG_WS_CONNECTED_BIT | DIAG_WS_ERROR_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));

    if (!(bits & DIAG_WS_CONNECTED_BIT)) {
        strncpy(result->auth_hint, "Firewall may block WSS :443", sizeof(result->auth_hint) - 1);
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        vEventGroupDelete(ctx.event_group);
        return;
    }

    /* WebSocket connected — check 4 passes */
    result->ws_ok = true;

    /* Send registration message (same format as normal boot) */
    char reg_msg[256];
    snprintf(reg_msg, sizeof(reg_msg),
             "{\"type\":\"register\","
             "\"device_type\":\"esp32_eink\","
             "\"auth_token\":\"%s\"}",
             cfg->security.auth_token);
    esp_websocket_client_send_text(client, reg_msg, strlen(reg_msg), pdMS_TO_TICKS(3000));

    /* Wait for server response (5s timeout) */
    bits = xEventGroupWaitBits(ctx.event_group,
        DIAG_WS_DATA_BIT | DIAG_WS_ERROR_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));

    if (bits & DIAG_WS_DATA_BIT) {
        if (ctx.auth_ok) {
            result->auth_ok = true;
        } else if (ctx.error_hint[0] != '\0') {
            strncpy(result->auth_hint, ctx.error_hint, sizeof(result->auth_hint) - 1);
        } else {
            strncpy(result->auth_hint, "Unexpected server response", sizeof(result->auth_hint) - 1);
        }
    } else {
        strncpy(result->auth_hint, "Auth response timeout", sizeof(result->auth_hint) - 1);
    }

    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    vEventGroupDelete(ctx.event_group);
}

diag_result_t run_network_diagnostics(void)
{
    diag_result_t result = {};
    result.battery_pct = -1;

    const app_config_t *cfg = config_manager_get_config();

    /* Populate device info (shown regardless of check results) */
    strncpy(result.device_id, cfg->device.id, sizeof(result.device_id) - 1);
    strncpy(result.firmware_version, APP_VERSION, sizeof(result.firmware_version) - 1);
    result.free_heap = esp_get_free_heap_size();

    int mv = power_manager_get_battery_mv();
    if (mv >= BATTERY_NO_BATTERY_MIN) {
        result.battery_pct = (int8_t)power_manager_get_battery_percent();
    }

    /* Check 1: WiFi info (already connected — just read state) */
    ESP_LOGI(TAG, "Check 1: WiFi info");
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        result.wifi_ok = true;
        memcpy(result.ssid, ap_info.ssid, sizeof(result.ssid) - 1);
        result.ssid[sizeof(result.ssid) - 1] = '\0';
        result.rssi = ap_info.rssi;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(result.ip_addr, sizeof(result.ip_addr), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    if (!result.wifi_ok) {
        strncpy(result.auth_hint, "WiFi not connected", sizeof(result.auth_hint) - 1);
        return result;
    }

    /* Check 2: DNS resolution of server hostname */
    ESP_LOGI(TAG, "Check 2: DNS resolution (%s)", cfg->server.host);
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addr_result = NULL;

    int dns_ret = getaddrinfo(cfg->server.host, NULL, &hints, &addr_result);
    if (dns_ret == 0 && addr_result != NULL) {
        result.dns_ok = true;
        freeaddrinfo(addr_result);
    } else {
        strncpy(result.auth_hint, "DNS failed - check network", sizeof(result.auth_hint) - 1);
        if (addr_result) freeaddrinfo(addr_result);
        return result;
    }

    /* Check 3: HTTPS GET /health */
    ESP_LOGI(TAG, "Check 3: HTTPS /health");
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
    http_cfg.timeout_ms = 5000;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t http_client = esp_http_client_init(&http_cfg);
    if (http_client) {
        esp_err_t err = esp_http_client_perform(http_client);
        int status = esp_http_client_get_status_code(http_client);
        esp_http_client_cleanup(http_client);

        if (err == ESP_OK && status == 200) {
            result.https_ok = true;
        } else if (err != ESP_OK) {
            strncpy(result.auth_hint, "TLS/HTTPS blocked by proxy", sizeof(result.auth_hint) - 1);
            return result;
        } else {
            snprintf(result.auth_hint, sizeof(result.auth_hint),
                     "Server returned HTTP %d", status);
            return result;
        }
    } else {
        strncpy(result.auth_hint, "HTTP client init failed", sizeof(result.auth_hint) - 1);
        return result;
    }

    /* Check 4-5: WebSocket + Auth */
    ESP_LOGI(TAG, "Check 4-5: WebSocket + Auth");
    check_ws_and_auth(&result);

    return result;
}
