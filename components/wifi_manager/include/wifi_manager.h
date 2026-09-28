#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect to WiFi using stored credentials (NVS + seed networks).
 * Scans available networks, matches against stored credentials,
 * connects to strongest match. Implements adaptive TX power escalation.
 *
 * Returns ESP_OK on successful connection, ESP_FAIL otherwise.
 */
esp_err_t wifi_manager_connect(void);

/**
 * Returns true if any WiFi credentials are stored (config seed networks or NVS).
 * Loads the credential store on first call (idempotent). Callers use this to
 * decide whether a failed connect should open the captive portal (attended,
 * provisioning needed) or simply deep-sleep and retry later.
 */
bool wifi_manager_has_credentials(void);

/**
 * Start captive portal for WiFi provisioning.
 * Creates SoftAP "pebl-setup" with branded HTML portal and starts the WiFi
 * driver so the AP beacons. Portal runs until user submits credentials and
 * WiFi connects.
 *
 * system_events: event group from app_main (sets wifi_connected_bit on success)
 * wifi_connected_bit: the bit to set in system_events when WiFi connects
 *
 * Returns ESP_OK if the portal started (AP beaconing, HTTP+DNS up), or an error
 * if the WiFi driver could not be brought up. Callers must not block waiting for
 * a connection if this returns non-OK.
 */
esp_err_t wifi_manager_start_portal(EventGroupHandle_t system_events, EventBits_t wifi_connected_bit);

/**
 * Stop the captive portal (HTTP server, DNS server, SoftAP).
 * Call after WiFi connects via the portal to free resources.
 */
void wifi_manager_stop_portal(void);

/**
 * Disconnect and clean up WiFi resources.
 */
void wifi_manager_disconnect(void);

/**
 * Force a WiFi reconnect cycle (disconnect + reconnect).
 * Used by resilience manager when WebSocket reconnects fail repeatedly.
 * Returns ESP_OK on successful reconnection.
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * Check if WiFi is currently connected.
 */
bool wifi_manager_is_connected(void);

/**
 * Get current RSSI (signal strength).
 * Returns 0 if not connected.
 */
int8_t wifi_manager_get_rssi(void);

/**
 * Check if WiFi is in fallback mode (all TX power levels exhausted).
 * When true, WiFi is skipped entirely to conserve battery.
 * Recovers automatically after 6 wake cycles (~6 hours at 60-min sleep).
 */
bool wifi_manager_is_fallback_mode(void);

#ifdef __cplusplus
}
#endif
