#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Battery voltage thresholds (millivolts after voltage divider compensation).
 * Same thresholds as the Arduino version for consistent behavior.
 */
#define BATTERY_CRITICAL_MV     3300    /* <15% — trigger deep sleep */
#define BATTERY_RECOVERY_MV     3400    /* ~20% — must reach this to exit critical state (hysteresis) */
#define BATTERY_LOW_MV          3500    /* <30% — warning */
#define BATTERY_USB_THRESHOLD   4050    /* Detect USB power (charging) */
#define BATTERY_USB_HYSTERESIS  4150    /* Hysteresis for USB→battery transition */
#define BATTERY_LIPO_MAX_MV     4200    /* Fully charged */
#define BATTERY_NO_BATTERY_MIN  2500    /* Below = no battery detected */
#define BATTERY_NO_BATTERY_MAX  5500    /* Above = no battery detected */

/**
 * Voltage divider ratio for LilyGo T5 battery measurement.
 * The ADC reads through a resistor divider, so actual voltage = ADC reading * ratio.
 */
#define BATTERY_VOLTAGE_DIVIDER_RATIO  2

typedef enum {
    POWER_SOURCE_BATTERY,
    POWER_SOURCE_USB,
    POWER_SOURCE_UNKNOWN,
} power_source_t;

/**
 * Button press action returned by power_manager_handle_button().
 * The caller (button_task in app_main) is responsible for executing the
 * action, which may include display rendering. This avoids a circular
 * dependency between power_manager and display_manager.
 */
typedef enum {
    BUTTON_ACTION_NONE,           /* No action (press too short or debounce) */
    BUTTON_ACTION_SHOW_STATUS,    /* Short press: show battery/connection status */
    BUTTON_ACTION_ENTER_PAIRING,  /* Medium press (3-9s): re-enter pairing mode */
    BUTTON_ACTION_WIFI_PORTAL,    /* Long press (15s+): factory reset WiFi */
} button_action_t;

/**
 * Status info populated by power_manager_handle_button() for display.
 */
typedef struct {
    button_action_t action;
    uint8_t battery_percent;
    int battery_mv;
    power_source_t source;
    bool has_auth_token;  /* true if device is paired (has auth token) */
} button_result_t;

/**
 * Initialize power manager: configure ADC, set initial thresholds.
 */
esp_err_t power_manager_init(void);

/**
 * Periodic power check (called from power_task every 5 minutes).
 * Reads battery voltage, determines power source, adjusts CPU frequency.
 * Returns true if power source changed (USB↔battery transition).
 * Caller should check power_manager_is_critical_battery() afterward and
 * handle deep sleep (with display warning) if true.
 */
bool power_manager_check(EventGroupHandle_t system_events);

/**
 * Handle physical button press (called from button_task).
 * Measures hold duration and returns an action for the caller to execute.
 * The caller handles display rendering to avoid circular dependencies.
 */
button_result_t power_manager_handle_button(void);

/**
 * Get current battery voltage in millivolts.
 */
int power_manager_get_battery_mv(void);

/**
 * Get current battery percentage (0-100).
 */
uint8_t power_manager_get_battery_percent(void);

/**
 * Get current power source.
 */
power_source_t power_manager_get_source(void);

/**
 * Check if battery is at critical level (below BATTERY_CRITICAL_MV).
 * Caller should show a warning screen and then call power_manager_deep_sleep().
 */
bool power_manager_is_critical_battery(void);

/**
 * Enter deep sleep with configured wake sources.
 * Used for critical battery, extended WiFi loss.
 */
void power_manager_deep_sleep(uint64_t duration_us);


/**
 * Acquire PM lock to prevent light sleep during critical operations
 * (e.g., SPI transactions for e-paper refresh).
 */
esp_err_t power_manager_acquire_wake_lock(void);

/**
 * Release PM lock to allow light sleep to resume.
 */
esp_err_t power_manager_release_wake_lock(void);

#ifdef __cplusplus
}
#endif
