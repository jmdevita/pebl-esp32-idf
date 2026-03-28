#pragma once

#include "esp_err.h"
#include "diagnostics.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Display event types passed through the display queue.
 */
typedef enum {
    DISPLAY_EVT_REACTION,       /* New reaction to render */
    DISPLAY_EVT_PAIRING_QR,     /* Show pairing QR code */
    DISPLAY_EVT_STATUS,         /* Show status message (WiFi, battery, etc.) */
    DISPLAY_EVT_SPLASH,         /* Show splash/boot screen */
    DISPLAY_EVT_CLEAR,          /* Clear display */
    DISPLAY_EVT_BOOT_STATUS,    /* "Connecting to Server..." + device name */
    DISPLAY_EVT_CONNECTED,      /* Checkmark + "Waiting for Reactions.." */
    DISPLAY_EVT_DISCONNECTED,   /* X icon + "Connection Lost" + subtitle */
    DISPLAY_EVT_WIFI_PROVISION, /* WiFi QR code + setup instructions */
    DISPLAY_EVT_LOW_BATTERY,    /* "LOW BATTERY" / "PLEASE CHARGE" */
    DISPLAY_EVT_PURCHASE_QR,    /* "Trial Expired" + purchase QR code */
    DISPLAY_EVT_BROADCAST,     /* Broadcast alert (text-only, full width) */
    DISPLAY_EVT_DIAGNOSTICS,   /* Network diagnostics results */
    DISPLAY_EVT_POWER_CHANGE,  /* USB↔battery transition — refresh status bar */
} display_event_type_t;

/**
 * Display layout constants — same as Arduino DisplayLimits namespace.
 */
#define DISPLAY_MAX_USER_LEN     15
#define DISPLAY_MAX_CHANNEL_LEN  15
#define DISPLAY_MAX_MESSAGE_LEN  30
#define DISPLAY_EMOJI_X          10
#define DISPLAY_EMOJI_Y          32

/**
 * Reaction data for display rendering.
 */
typedef struct {
    char user[DISPLAY_MAX_USER_LEN + 1];
    char channel[DISPLAY_MAX_CHANNEL_LEN + 1];
    char message_preview[DISPLAY_MAX_MESSAGE_LEN + 1];
    char emoji_name[64];
    char emoji_url[128];         /* Original URL — cached in RTC for restore after reconnect */
    uint8_t *emoji_png_data;     /* PNG image data (heap-allocated, display frees) */
    size_t emoji_png_size;
    char timestamp[32];
    bool is_encrypted;
    char platform[16];           /* Source platform: "slack", "discord", etc. */
} reaction_data_t;

/**
 * Display event — sent to display_task via FreeRTOS queue.
 */
typedef struct {
    display_event_type_t type;
    union {
        reaction_data_t reaction;
        struct {
            char url[256];       /* URL encoded in QR code (needs "https://" + host[128]) */
            char code[16];       /* Pairing code displayed below QR */
        } pairing;
        struct {
            char line1[64];
            char line2[64];
            char line3[64];
            char line4[64];
            bool show_lock;      /* Show lock icon in top-left corner */
        } status;
        struct {
            char device_name[32];
        } boot;
        struct {
            bool show_lock;      /* Show lock icon in top-left corner */
        } connected;
        struct {
            char subtitle[64];   /* Default: "Check WiFi/Server" */
        } disconnected;
        struct {
            char ssid[33];       /* WiFi SSID (max 32 chars + null) */
            char ip[16];         /* Portal IP address */
        } wifi_provision;
        struct {
            char url[128];       /* Purchase URL for QR code */
            char device_id[32];  /* Device ID (truncated to 8 chars on display) */
        } purchase;
        struct {
            char source[64];    /* sender / alert source label */
            char message[256];  /* broadcast text */
            char platform[16];  /* "slack", "api", etc. */
            char channel[64];   /* channel name where broadcast originated */
            bool encrypted;     /* show lock icon if true */
        } broadcast;
        struct {
            diag_result_t result;
        } diagnostics;
        struct {
            bool show_lock;      /* Preserve lock icon state from current display */
        } power_change;
    } data;
} display_event_t;

/**
 * Initialize display hardware and driver.
 * On cold boot: shows splash screen.
 * On deep sleep wake: preserves previous display content.
 */
esp_err_t display_manager_init(bool is_deep_sleep_wake);

/**
 * Render a display event.
 * Called from display_task when an event arrives on the queue.
 * Acquires PM wake lock during SPI transaction to prevent light sleep
 * mid-refresh (which would corrupt the e-paper update).
 */
void display_manager_render(display_event_t *evt);

/**
 * Get display width in pixels.
 */
uint16_t display_manager_get_width(void);

/**
 * Get display height in pixels.
 */
uint16_t display_manager_get_height(void);

#ifdef __cplusplus
}
#endif
