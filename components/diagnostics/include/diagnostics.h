#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Network diagnostics result — populated by run_network_diagnostics().
 * Each check runs sequentially; on first failure, subsequent checks
 * are skipped and auth_hint explains the likely cause.
 */
typedef struct {
    bool wifi_ok;
    char ssid[33];
    int8_t rssi;
    char ip_addr[16];
    bool dns_ok;
    bool https_ok;
    bool ws_ok;
    bool auth_ok;
    char auth_hint[48];         /* Human-readable hint for first failure */
    char device_id[32];
    char firmware_version[16];
    uint32_t free_heap;
    int8_t battery_pct;         /* -1 if unavailable */
} diag_result_t;

/**
 * Run all network diagnostic checks sequentially.
 * Requires WiFi to be connected before calling.
 * Checks: WiFi info -> DNS -> HTTPS /health -> WebSocket + Auth.
 * Stops at first failure with descriptive hint.
 */
diag_result_t run_network_diagnostics(void);

#ifdef __cplusplus
}
#endif
