#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Server error codes received via WebSocket {"type":"error","code":"..."}.
 * Set atomically by the WS event handler, consumed by ws_task in app_main.
 */
typedef enum {
    WS_ERROR_NONE,
    WS_ERROR_DEVICE_NOT_LINKED,      /* Device unpaired — re-enter pairing mode */
    WS_ERROR_TRIAL_EXPIRED,          /* Trial ended — show purchase QR, deep sleep */
    WS_ERROR_DEVICE_NOT_REGISTERED,  /* Credentials invalid — retry then deep sleep */
    WS_ERROR_AUTH_FAILED,            /* Auth token invalid — retry then deep sleep */
} ws_error_code_t;

/**
 * Error details populated by the event handler alongside ws_error_code_t.
 * Only valid when websocket_manager_get_pending_error() != WS_ERROR_NONE.
 */
typedef struct {
    char purchase_url[256];  /* TRIAL_EXPIRED: URL for purchase QR code */
    char device_id[32];      /* Device ID from server response */
} ws_error_info_t;

/**
 * Check and consume any pending server error.
 * Returns the error code and clears it atomically (one-shot).
 * If info is non-NULL, copies error details into it.
 */
ws_error_code_t websocket_manager_get_pending_error(ws_error_info_t *info);

/**
 * Register the system event group for wake-on-event signaling.
 * Call before websocket_manager_start(). The WS event handler will set
 * the specified bits when server errors or firmware updates arrive,
 * allowing ws_task to wake immediately instead of polling.
 */
void websocket_manager_set_event_group(EventGroupHandle_t events,
                                        EventBits_t error_bit,
                                        EventBits_t firmware_bit);

/**
 * Start WebSocket connection to server.
 * Uses esp_websocket_client which runs its own internal task for I/O.
 * Events are dispatched to the ws_event_handler callback.
 *
 * display_queue: FreeRTOS queue for display_event_t items.
 *   Reactions are parsed in the WS event handler and pushed directly
 *   to this queue (FreeRTOS queues are thread-safe).
 */
esp_err_t websocket_manager_start(QueueHandle_t display_queue);

/**
 * Periodic health check, called from ws_task.
 * Handles heartbeat timeout detection and sends client heartbeats.
 * Reaction dispatch happens inline in the WS event handler, not here.
 */
void websocket_manager_process(EventGroupHandle_t system_events);

/**
 * Stop WebSocket connection and clean up.
 */
void websocket_manager_stop(void);

/**
 * Check if WebSocket is currently connected.
 */
bool websocket_manager_is_connected(void);

/**
 * Send a text message over the WebSocket.
 * Thread-safe (esp_websocket_client handles locking).
 */
esp_err_t websocket_manager_send(const char *data, int len);

/**
 * Check pending firmware update flags (set by a server firmware_update push).
 * Returns true if a required or optional firmware update was signaled.
 * Does not clear the flag — call websocket_manager_clear_pending_firmware()
 * when an install attempt starts, so a deferred update survives re-evaluation.
 */
bool websocket_manager_has_pending_firmware_required(void);
bool websocket_manager_has_pending_firmware_optional(void);

/**
 * Clear both pending firmware flags. Call immediately before an install attempt.
 */
void websocket_manager_clear_pending_firmware(void);

/**
 * Get timestamp (in microseconds) of the last reaction delivered.
 * Returns 0 if no reactions have been received.
 */
int64_t websocket_manager_get_last_reaction_time(void);

#ifdef __cplusplus
}
#endif
