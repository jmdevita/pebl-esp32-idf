#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start self-service pairing flow.
 * 1. Request pairing code from server
 * 2. Display QR code + pairing code on e-paper
 * 3. Poll server until user completes pairing in Slack
 * 4. Save auth_token to config, set EVT_PAIRING_COMPLETE
 *
 * Same server API as Arduino version, using esp_http_client.
 */
esp_err_t pairing_manager_start(QueueHandle_t display_queue,
                                 EventGroupHandle_t system_events);

/**
 * Check if device is currently in pairing mode.
 */
bool pairing_manager_is_active(void);

#ifdef __cplusplus
}
#endif
