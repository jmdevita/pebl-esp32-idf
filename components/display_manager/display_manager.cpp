/**
 * Display Manager — renders reactions, QR codes, status messages, and splash
 * screens on e-paper display using CalEPD directly.
 *
 * Display task architecture:
 * - Blocks on xQueueReceive() when idle → enables auto light sleep
 * - Acquires ESP_PM_NO_LIGHT_SLEEP lock during SPI refresh (2-4s)
 * - Does NOT block WebSocket: esp_websocket_client has its own task
 *
 * Display variant is selected at compile time via menuconfig:
 *   CONFIG_DISPLAY_DEPG0213BN  → Gdey0213b74 (SSD1680 BW — compatible controller)
 *   CONFIG_DISPLAY_GDEW0213I5F → Gdew0213i5f (flexible, 4G capable)
 *   CONFIG_DISPLAY_GDEY0213B74 → Gdey0213b74 (fast refresh, 4G capable)
 *
 * Screen layouts match the Arduino client (esp32_arduino_client) exactly,
 * except the pairing screen which uses espressif/qrcode instead of qrcode lib.
 */

#include "display_manager.h"
#include "diagnostics.h"
#include "config_manager.h"
#include "power_manager.h"
#include "board.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
/* lodepng is compiled as C (lodepng.c). Disable the C++ wrapper so the header
 * contains only C declarations, then wrap with extern "C" for proper linkage. */
#define LODEPNG_NO_COMPILE_CPP
extern "C" {
#include "lodepng.h"
}
#include "qrcode.h"

/* CalEPD SPI interface — handles all SPI bus configuration internally */
#include "epdspi.h"

/* CalEPD display model — selected by Kconfig.
 * Each model file provides a class inheriting from Epd (which inherits Adafruit_GFX).
 * The class handles controller-specific init sequences, LUT tables, and refresh modes. */
#if defined(CONFIG_DISPLAY_DEPG0213BN) || defined(CONFIG_DISPLAY_GDEY0213B74)
#include "goodisplay/gdey0213b74.h"
typedef Gdey0213b74 DisplayClass;

#elif defined(CONFIG_DISPLAY_GDEW0213I5F)
#include "gdew0213i5f.h"
typedef Gdew0213i5f DisplayClass;

#else
#error "No display variant selected in Kconfig — run idf.py menuconfig"
#endif

/* Adafruit GFX FreeSans fonts — included via CalEPD's Adafruit-GFX-Library dependency */
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansOblique9pt7b.h>

static const char *TAG = "DISPLAY";

/* CalEPD SPI interface object — configures SPI bus using Kconfig GPIO pins.
 * EpdSpi reads CONFIG_EINK_SPI_MOSI, CONFIG_EINK_SPI_CLK, CONFIG_EINK_SPI_CS,
 * CONFIG_EINK_DC, CONFIG_EINK_RST, CONFIG_EINK_BUSY from sdkconfig.
 * These are defined in CalEPD's Kconfig.projbuild ("Display Configuration" menu). */
static EpdSpi io;

/* Display object — model-specific class selected by Kconfig typedef above */
static DisplayClass display(io);

static bool s_display_hw_initialized = false;
static bool s_initialized = false;

/* Display dimensions from Kconfig (set via menuconfig) */
#define DISPLAY_WIDTH   CONFIG_DISPLAY_WIDTH
#define DISPLAY_HEIGHT  CONFIG_DISPLAY_HEIGHT

/* Invisible pixel inset at the bottom edge in landscape mode.
 * GDEY0213B74/DEPG0213BN have a 128-pixel buffer but only 122 visible pixels
 * on the short axis. In landscape rotation, these 6 invisible pixels appear
 * at the bottom of the screen, causing content placed near display.height()
 * to be clipped off-screen. */
#if defined(CONFIG_DISPLAY_GDEY0213B74) || defined(CONFIG_DISPLAY_DEPG0213BN)
#define SCREEN_BOTTOM_INSET 6
#else
#define SCREEN_BOTTOM_INSET 0
#endif

/* Grayscale color constants for 4-level e-paper rendering.
 * These match CalEPD's gdew_4grays.h values and map to the SSD1680's
 * dual-RAM-plane encoding: each pixel uses 1 bit in each plane,
 * giving 4 possible states (00=black, 01=dark gray, 10=light gray, 11=white). */
#define GRAY_BLACK      0
#define GRAY_DARKGREY   64
#define GRAY_LIGHTGREY  128
#define GRAY_WHITE      255

/* Layout constants — matching Arduino DisplayLimits namespace.
 *
 * Reaction screen layout:
 *   +--------------------------------------------------+
 *   | [lock 5,5]     BATTERY        [ooo battery]      |
 *   |                                                   |
 *   | [emoji]    username (bold, x=60, y=42)            |
 *   | [10,32]    "message" (italic, x=60, y=62)        |
 *   |                                                   |
 *   | Platform                 From: channel (y=92)     |
 *   +--------------------------------------------------+
 */
#define EMOJI_X          10   /* Emoji top-left X position */
#define EMOJI_Y          32   /* Emoji top-left Y position */
#define EMOJI_SIZE       64   /* Target emoji render size in pixels */
#define TEXT_X           60   /* Text column X (right of emoji) */
#define USERNAME_Y       42   /* Username Y position */
#define MESSAGE_Y        62   /* Message preview Y position */
#define CHANNEL_Y        92   /* Channel line Y position (bottom area) */

/* QR code rendering parameters */
#define QR_MODULE_SIZE   3    /* Pixels per QR module (pairing QR) */
#define QR_OFFSET_X      10   /* QR code X offset (pairing QR) */
#define QR_OFFSET_Y      5    /* QR code Y offset (pairing QR) */
#define QR_VERSION        6   /* QR version for pairing (handles URLs up to ~134 chars) */

/* Color definitions — use CalEPD's gdew_colors.h values (16-bit RGB565).
 * fillScreen() compares against EPD_WHITE (0xFFFF) to decide buffer fill value.
 * drawPixel() treats any non-zero as white, zero as black. */
#define COLOR_BLACK  EPD_BLACK   /* 0x0000 */
#define COLOR_WHITE  EPD_WHITE   /* 0xFFFF */

/* Font lookup table — maps enum indices to Adafruit GFX font structs */
enum {
    FONT_SANS_9PT,
    FONT_SANS_12PT,
    FONT_SANS_BOLD_9PT,
    FONT_SANS_BOLD_12PT,
    FONT_SANS_BOLD_18PT,
    FONT_SANS_OBLIQUE_9PT,
    FONT_COUNT
};

static const GFXfont *font_table[FONT_COUNT] = {
    &FreeSans9pt7b,         /* FONT_SANS_9PT */
    &FreeSans12pt7b,        /* FONT_SANS_12PT */
    &FreeSansBold9pt7b,     /* FONT_SANS_BOLD_9PT */
    &FreeSansBold12pt7b,    /* FONT_SANS_BOLD_12PT */
    &FreeSansBold18pt7b,    /* FONT_SANS_BOLD_18PT */
    &FreeSansOblique9pt7b,  /* FONT_SANS_OBLIQUE_9PT */
};

/* 20x20 XBM lock icons — same data as Arduino LockIcons namespace (PROGMEM).
 * Locked: shackle closed (rows 4-8), body filled (rows 9-18).
 * Unlocked: shackle open on right side, body filled. */
namespace LockIcons {
    static const int WIDTH = 20;
    static const int HEIGHT = 20;

    static const unsigned char locked_bits[] = {
        0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
        0xc0,0x3f,0x00, 0xe0,0x79,0x00, 0x70,0xe0,0x00, 0x70,0xe0,0x00,
        0x70,0xe0,0x00, 0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01,
        0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01,
        0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01, 0x00,0x00,0x00
    };

    /* Unlocked padlock — shackle open on right side, body filled.
     * Same 20x20 XBM format as Arduino LockIcons::unlocked_bits. */
    static const unsigned char unlocked_bits[] = {
        0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
        0xc0,0x03,0x00, 0xe0,0xc1,0x01, 0x70,0x80,0x03, 0x70,0x00,0x07,
        0x70,0x00,0x0e, 0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01,
        0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01,
        0xf8,0xff,0x01, 0xf8,0xff,0x01, 0xf8,0xff,0x01, 0x00,0x00,0x00
    };

}

/* Forward declarations */
static void render_status_4line(const char *line1, const char *line2,
                                const char *line3, const char *line4,
                                bool show_lock);
static void render_broadcast(const display_event_t *evt);

/* =========================================================================
 * Display driver helpers — direct CalEPD access
 * ========================================================================= */

static bool display_hw_init(uint8_t rotation)
{
    if (s_display_hw_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing CalEPD display driver (rotation=%d)", rotation);

    display.init();
    display.setRotation(rotation);

    s_display_hw_initialized = true;
    ESP_LOGI(TAG, "Display driver initialized (%dx%d)",
             display.width(), display.height());
    return true;
}

static inline void set_font(int font_id)
{
    if (font_id < FONT_COUNT && font_table[font_id]) {
        display.setFont(font_table[font_id]);
    }
}

static bool supports_grayscale(void)
{
#if defined(CONFIG_DISPLAY_DEPG0213BN) || defined(CONFIG_DISPLAY_GDEY0213B74) || defined(CONFIG_DISPLAY_GDEW0213I5F)
    return true;
#else
    return false;
#endif
}

/**
 * Enable 4-gray mode permanently at init. Called once from display_manager_init().
 * Text drawn with EPD_BLACK (0) and EPD_WHITE (0xFFFF) maps correctly to the
 * black and white gray levels in drawPixel (color >> 6 → 0 or 3), so there's
 * no need to switch back to mono mode for text screens.
 * This matches the Arduino client which stays in 4-gray mode permanently.
 */
static void enable_grayscale(void)
{
#if defined(CONFIG_DISPLAY_DEPG0213BN) || defined(CONFIG_DISPLAY_GDEY0213B74) || defined(CONFIG_DISPLAY_GDEW0213I5F)
    display.setMonoMode(false);
    ESP_LOGI(TAG, "4-level grayscale mode enabled");
#endif
}

/* =========================================================================
 * Common drawing helpers — matching Arduino DisplayManager exactly
 * ========================================================================= */

/**
 * Draw 3 circles in top-right corner indicating battery level.
 * Level 0-3: filled circles = charged segments, outline = empty.
 * Matches Arduino drawBatteryIndicator() exactly.
 */
static void draw_battery_indicator(void)
{
    /* Hide indicator entirely on boards without battery sense hardware
     * (custom PCB v1.1 — floating ADC). A misleading "0%" indicator is worse
     * than no indicator at all. USB icon / source detection still works. */
    if (!power_manager_has_valid_battery_reading()) return;

    uint8_t percent = power_manager_get_battery_percent();
    int level;
    if (percent < 25) level = 0;
    else if (percent < 50) level = 1;
    else if (percent < 75) level = 2;
    else level = 3;

    /* Skip if no battery detected (voltage in "no battery" range) */
    int mv = power_manager_get_battery_mv();
    if (mv < BATTERY_NO_BATTERY_MIN) return;

    const int16_t circleRadius  = 4;
    const int16_t circleSpacing = 12;
    const int16_t xStart = display.width() - 50;
    const int16_t y = 10;

    for (int i = 0; i < 3; i++) {
        int16_t x = xStart + (i * circleSpacing);
        if (i < level) {
            display.fillCircle(x, y, circleRadius, COLOR_BLACK);
        } else {
            display.drawCircle(x, y, circleRadius, COLOR_BLACK);
        }
    }
}

/**
 * Draw power status text at top-center. Only shown when on battery power.
 * Uses the built-in 5x7 font (setFont(NULL)) for small text.
 * Matches Arduino drawPowerStatusIndicator().
 */
static void draw_power_status_text(void)
{
    if (power_manager_get_source() != POWER_SOURCE_BATTERY) {
        return;
    }

    /* Without valid battery readings the % is meaningless — show plain
     * "BATTERY" instead of urgency labels that would be guesses. */
    const char *text;
    if (!power_manager_has_valid_battery_reading()) {
        text = "BATTERY";
    } else {
        uint8_t percent = power_manager_get_battery_percent();
        if (percent < 5)       text = "CHARGE NOW";
        else if (percent < 30) text = "LOW BATTERY";
        else                   text = "BATTERY";
    }

    display.setFont(NULL);  /* Built-in 5x7 font */
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(display.width() / 2 - (int16_t)(w / 2), 8);
    display.print(text);
    /* Restore font — caller should set their desired font after this */
}

/**
 * Draw locked padlock icon at (5, 5) — 20x20 XBM bitmap.
 */
static void draw_lock_icon(void)
{
    display.drawXBitmap(5, 5, LockIcons::locked_bits,
                        LockIcons::WIDTH, LockIcons::HEIGHT, COLOR_BLACK);
}

/**
 * Draw unlocked padlock icon at (5, 5) — 20x20 XBM bitmap.
 * Shown on blank/idle screens when no encrypted reaction is displayed.
 */
static void draw_unlock_icon(void)
{
    display.drawXBitmap(5, 5, LockIcons::unlocked_bits,
                        LockIcons::WIDTH, LockIcons::HEIGHT, COLOR_BLACK);
}


/**
 * Truncate a string to fit within maxWidth pixels using the current font.
 * If the string is too long, characters are removed from the end and "..."
 * is appended. Result is written to dest (must be at least dest_size bytes).
 * x_offset is the starting X position for text bounds calculation.
 */
static void truncate_to_fit(char *dest, size_t dest_size, const char *src,
                            int16_t x_offset, int16_t max_width)
{
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(dest, x_offset, 0, &x1, &y1, &w, &h);

    if ((int16_t)w <= max_width) {
        return;
    }

    /* Progressively shorten until it fits (leave room for "...") */
    size_t len = strlen(dest);
    while (len > 1) {
        len--;
        dest[len] = '\0';

        /* Check with "..." appended */
        char test[128];
        snprintf(test, sizeof(test), "%s...", dest);
        display.getTextBounds(test, x_offset, 0, &x1, &y1, &w, &h);
        if ((int16_t)w <= max_width) {
            snprintf(dest + len, dest_size - len, "...");
            return;
        }
    }

    /* Absolute minimum: just "..." */
    strncpy(dest, "...", dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/* =========================================================================
 * Rendering functions
 * ========================================================================= */

/**
 * Decode a PNG and render it directly to the display using drawPixel().
 *
 * In grayscale mode (4-level e-paper), RGBA luminance is quantized to 4 levels:
 *   0-63    → GRAY_BLACK      (0)
 *   64-127  → GRAY_DARKGREY   (64)
 *   128-191 → GRAY_LIGHTGREY  (128)
 *   192-255 → GRAY_WHITE      (255)
 *
 * In mono mode, a simple threshold at 128 is used.
 *
 * Transparent pixels (alpha < 128) are skipped, preserving the background.
 *
 * This renders pixel-by-pixel rather than building an intermediate bitmap,
 * which avoids the complexity of a 2-bit packed buffer format and works
 * directly with CalEPD's dual-framebuffer grayscale encoding.
 */
static void render_png_emoji(const uint8_t *png_data, size_t png_size,
                              int16_t dest_x, int16_t dest_y)
{
    if (!png_data || png_size == 0) {
        return;
    }

    unsigned char *rgba = NULL;
    unsigned w, h;
    unsigned error = lodepng_decode32(&rgba, &w, &h, png_data, png_size);
    if (error) {
        ESP_LOGE(TAG, "PNG decode failed: %s", lodepng_error_text(error));
        free(rgba);  /* lodepng may partially allocate before failing */
        return;
    }

    bool grayscale = supports_grayscale();

    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned idx = (y * w + x) * 4;
            uint8_t r = rgba[idx], g = rgba[idx + 1], b = rgba[idx + 2], a = rgba[idx + 3];

            /* Alpha-composite onto white background: blends semi-transparent
             * pixels with white (255) so anti-aliased edges render as the
             * correct intermediate gray rather than solid black/dark. */
            r = (uint8_t)((r * a + 255 * (255 - a)) / 255);
            g = (uint8_t)((g * a + 255 * (255 - a)) / 255);
            b = (uint8_t)((b * a + 255 * (255 - a)) / 255);

            uint8_t lum = (uint8_t)((r + g + b) / 3);

            /* Gamma correction (γ=2.0): darkens mid-tones so bright colors
             * like yellow don't wash out into light gray on e-paper.
             * Formula: out = 255 * (in/255)^γ — implemented via integer math
             * with a lookup would be ideal, but squared/255 is a good γ=2.0
             * approximation that's cheap to compute. */
            lum = (uint8_t)((uint16_t)lum * lum / 255);

            uint16_t color;
            if (grayscale) {
                /* Quantize to 4 levels — thresholds tuned for gamma-corrected
                 * values where mid-tones are compressed toward the dark end. */
                if (lum < 32) {
                    color = GRAY_BLACK;
                } else if (lum < 96) {
                    color = GRAY_DARKGREY;
                } else if (lum < 176) {
                    color = GRAY_LIGHTGREY;
                } else {
                    color = GRAY_WHITE;
                }
            } else {
                color = (lum < 128) ? COLOR_BLACK : COLOR_WHITE;
            }

            /* Skip white pixels — background is already white from fillScreen() */
            if ((!grayscale && color == COLOR_WHITE) ||
                (grayscale && color == GRAY_WHITE)) {
                continue;
            }

            display.drawPixel(dest_x + (int16_t)x, dest_y + (int16_t)y, color);
        }
    }

    free(rgba);
}

/**
 * Render a reaction screen on the e-paper display.
 * Layout matches the Arduino DisplayManager::showReaction() exactly:
 *
 *   +--------------------------------------------------+
 *   | [lock 5,5]     BATTERY        [ooo battery]      |
 *   |                                                   |
 *   | [emoji]    username (bold, x=60, y=42)            |
 *   | [10,32]    "message" (italic, x=60, y=62)        |
 *   |                                                   |
 *   | Platform                 From: channel (y=92)     |
 *   +--------------------------------------------------+
 */
static void render_reaction(const reaction_data_t *reaction)
{
    ESP_LOGI(TAG, "Rendering reaction from %s in %s%s",
             reaction->user, reaction->channel,
             reaction->is_encrypted ? " [encrypted]" : "");

    bool use_gray = supports_grayscale();
    display.fillScreen(use_gray ? GRAY_WHITE : COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    /* Top strip: lock icon, power status text, battery indicator */
    draw_battery_indicator();
    draw_power_status_text();
    if (reaction->is_encrypted) {
        draw_lock_icon();
    }

    /* Draw emoji PNG at (10, 32) */
    if (reaction->emoji_png_data && reaction->emoji_png_size > 0) {
        render_png_emoji(reaction->emoji_png_data, reaction->emoji_png_size,
                         EMOJI_X, EMOJI_Y);
    }

    /* Username — bold, truncated to fit right of emoji */
    set_font(FONT_SANS_BOLD_9PT);
    {
        char truncated[64];
        truncate_to_fit(truncated, sizeof(truncated), reaction->user,
                        TEXT_X, display.width() - TEXT_X - 5);
        display.setCursor(TEXT_X, USERNAME_Y);
        display.print(truncated);
    }

    /* Message preview — italic, truncated to fit */
    if (reaction->message_preview[0] != '\0') {
        set_font(FONT_SANS_OBLIQUE_9PT);
        char truncated[64];
        truncate_to_fit(truncated, sizeof(truncated), reaction->message_preview,
                        TEXT_X, display.width() - TEXT_X - 5);
        display.setCursor(TEXT_X, MESSAGE_Y);
        display.print(truncated);
    }

    /* Channel — italic, right-aligned at y=92, "From: #channel" */
    set_font(FONT_SANS_OBLIQUE_9PT);
    {
        char channel_label[48];
        snprintf(channel_label, sizeof(channel_label), "From: %s", reaction->channel);
        char truncated[64];
        truncate_to_fit(truncated, sizeof(truncated), channel_label,
                        0, display.width() - TEXT_X - 5);
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(truncated, 0, 0, &x1, &y1, &w, &h);
        display.setCursor(display.width() - (int16_t)w - 5, CHANNEL_Y);
        display.print(truncated);
    }

    /* Platform label — small font, bottom-left, first letter capitalized */
    if (reaction->platform[0] != '\0') {
        display.setFont(NULL);  /* Built-in 5x7 font */
        char plat[16];
        strncpy(plat, reaction->platform, sizeof(plat) - 1);
        plat[sizeof(plat) - 1] = '\0';
        plat[0] = (char)toupper((unsigned char)plat[0]);
        display.setCursor(10, display.height() - SCREEN_BOTTOM_INSET - 14);
        display.print(plat);
    }

    display.update();
}

/**
 * Render a broadcast alert — structured card layout with header bar and message box.
 * Layout:
 *   +--------------------------------------------------+
 *   | [lock]         BATTERY          [ooo]            |
 *   |                                                   |
 *   | ████████████████████████████████████████████████ |
 *   | █     #channel  ·  Announcement                █ |
 *   | ████████████████████████████████████████████████ |
 *   | ┌──────────────────────────────────────────────┐ |
 *   | │           Source Name (bold)                 │ |
 *   | │      Message line 1 (centered)              │ |
 *   | │      Message line 2 (centered)              │ |
 *   | └──────────────────────────────────────────────┘ |
 *   | Platform                                        |
 *   +--------------------------------------------------+
 */
static void render_broadcast(const display_event_t *evt)
{
    const auto *b = &evt->data.broadcast;
    ESP_LOGI(TAG, "Rendering broadcast from %s: %.40s", b->source, b->message);

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    /* Top strip: lock icon, power status, battery indicator */
    draw_battery_indicator();
    draw_power_status_text();
    if (b->encrypted) {
        draw_lock_icon();
    }

    /* Layout constants */
    const int16_t margin = 5;
    const int16_t dw = display.width();
    const int16_t header_y = 26;
    const int16_t header_h = 20;
    const int16_t box_y = header_y + header_h;
    const int16_t box_h = 58;
    const int16_t box_inner_w = dw - 2 * margin;

    /* Header bar — filled black rectangle with white centered text.
     * Shows "#channel · Announcement" or just "Announcement" if no channel. */
    display.fillRect(margin, header_y, box_inner_w, header_h, COLOR_BLACK);
    {
        display.setFont(NULL);
        display.setTextColor(COLOR_WHITE);

        char header_text[96];
        if (b->channel[0] != '\0') {
            snprintf(header_text, sizeof(header_text), "#%s  %c  Announcement",
                     b->channel, 0xF9);  /* 0xF9 = middle dot in default font */
        } else {
            snprintf(header_text, sizeof(header_text), "Announcement");
        }

        int16_t tx, ty;
        uint16_t tw, th;
        display.getTextBounds(header_text, 0, 0, &tx, &ty, &tw, &th);
        int16_t hx = margin + (box_inner_w - (int16_t)tw) / 2;
        int16_t hy = header_y + (header_h - (int16_t)th) / 2 - ty;
        display.setCursor(hx, hy);
        display.print(header_text);

        display.setTextColor(COLOR_BLACK);
    }

    /* Message box — outlined rectangle below header bar */
    display.drawRect(margin, box_y, box_inner_w, box_h, COLOR_BLACK);

    /* Source name — bold, centered inside the box */
    set_font(FONT_SANS_BOLD_9PT);
    {
        char truncated[64];
        truncate_to_fit(truncated, sizeof(truncated), b->source,
                        margin + 4, box_inner_w - 8);
        int16_t sx, sy;
        uint16_t sw, sh;
        display.getTextBounds(truncated, 0, 0, &sx, &sy, &sw, &sh);
        display.setCursor(margin + (box_inner_w - (int16_t)sw) / 2, box_y + 16);
        display.print(truncated);
    }

    /* Message body — regular font, word-wrapped across up to 2 lines, centered.
     * Uses space-based wrapping to avoid splitting words mid-character. */
    set_font(FONT_SANS_9PT);
    {
        int16_t max_w = box_inner_w - 8;
        const char *msg = b->message;
        size_t msg_len = strlen(msg);

        char full[256];
        truncate_to_fit(full, sizeof(full), msg, margin + 4, max_w);
        size_t full_len = strlen(full);

        bool was_truncated = (msg_len > full_len) ||
            (full_len >= 3 && full[full_len-1] == '.' &&
             full[full_len-2] == '.' && full[full_len-3] == '.');

        if (!was_truncated) {
            int16_t mx, my;
            uint16_t mw, mh;
            display.getTextBounds(full, 0, 0, &mx, &my, &mw, &mh);
            display.setCursor(margin + (box_inner_w - (int16_t)mw) / 2, box_y + 34);
            display.print(full);
        } else {
            size_t split = full_len >= 3 ? full_len - 3 : full_len;
            while (split > 0 && msg[split] != ' ') {
                split--;
            }
            if (split == 0) {
                split = full_len >= 3 ? full_len - 3 : full_len;
            }

            /* Line 1 */
            char line1[128];
            size_t l1_len = split < sizeof(line1) - 1 ? split : sizeof(line1) - 1;
            memcpy(line1, msg, l1_len);
            line1[l1_len] = '\0';
            {
                int16_t lx, ly;
                uint16_t lw, lh;
                display.getTextBounds(line1, 0, 0, &lx, &ly, &lw, &lh);
                display.setCursor(margin + (box_inner_w - (int16_t)lw) / 2, box_y + 34);
                display.print(line1);
            }

            /* Line 2 */
            const char *remainder = msg + split;
            while (*remainder == ' ') remainder++;
            if (*remainder != '\0') {
                char line2[128];
                truncate_to_fit(line2, sizeof(line2), remainder, margin + 4, max_w);
                int16_t lx, ly;
                uint16_t lw, lh;
                display.getTextBounds(line2, 0, 0, &lx, &ly, &lw, &lh);
                display.setCursor(margin + (box_inner_w - (int16_t)lw) / 2, box_y + 52);
                display.print(line2);
            }
        }
    }

    /* Platform label — small font, bottom-left */
    if (b->platform[0] != '\0') {
        display.setFont(NULL);
        char plat[16];
        strncpy(plat, b->platform, sizeof(plat) - 1);
        plat[sizeof(plat) - 1] = '\0';
        plat[0] = (char)toupper((unsigned char)plat[0]);
        display.setCursor(10, display.height() - SCREEN_BOTTOM_INSET - 14);
        display.print(plat);
    }

    display.update();
}

/**
 * Render pairing code screen — matches Arduino DisplayManager::showPairingCode().
 * Shows pairing code in an outlined box with instructions to enter it
 * in the Slack/Discord bot's /link command. No QR code — the code is
 * short enough (XXXX-XXXX) to type manually.
 *
 * Layout:
 *   [battery top-right]  [power status top-center]
 *   "Pair Your Device"              (FreeSans9pt, x=10, y=30)
 *   ┌─────────────┐
 *   │  ABCD-1234  │                (FreeSansBold9pt, outlined box at y=38)
 *   └─────────────┘
 *   "Type /link in Slack"           (FreeSans9pt, below box)
 *   "or Discord to pair"            (FreeSans9pt, below)
 *   "ID: 2cbcbba86c74"             (built-in 5x7, bottom)
 */
static void render_pairing_code(const char *code)
{
    ESP_LOGI(TAG, "Rendering pairing code: %s", code);

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();
    draw_power_status_text();

    /* Title */
    set_font(FONT_SANS_9PT);
    display.setCursor(10, 30);
    display.print("Pair Your Device");

    /* Pairing code in outlined box for visual prominence */
    set_font(FONT_SANS_BOLD_9PT);
    int16_t tx, ty;
    uint16_t tw, th;
    display.getTextBounds(code, 0, 0, &tx, &ty, &tw, &th);
    const int boxPad = 6;
    const int boxX = 8;
    const int boxY = 38;
    const int boxW = tw + boxPad * 2;
    const int boxH = th + boxPad * 2;
    display.drawRect(boxX, boxY, boxW, boxH, COLOR_BLACK);
    display.setCursor(boxX + boxPad - tx, boxY + boxPad - ty);
    display.print(code);

    /* Instructions below the box */
    int instrY = boxY + boxH + 14;
    set_font(FONT_SANS_9PT);
    display.setCursor(10, instrY);
    display.print("Type /link in Slack");
    display.setCursor(10, instrY + 16);
    display.print("or Discord to pair");

    /* Device ID in small font at bottom */
    const app_config_t *cfg = config_manager_get_config();
    char id_label[28];
    snprintf(id_label, sizeof(id_label), "ID: %.20s", cfg->device.id);
    display.setFont(NULL);  /* Built-in 5x7 font */
    display.setCursor(10, instrY + 34);
    display.print(id_label);

    display.update();
}

/**
 * Render a 4-line left-aligned status message with battery and optional lock icon.
 * Matches Arduino DisplayManager::showMessage() layout:
 * - Battery indicator + power status text in top strip
 * - Optional lock icon at (5, 5)
 * - Up to 4 lines at x=10, y positions: 40, 60, 80, 100
 * - Empty lines are skipped
 */
static void render_status_4line(const char *line1, const char *line2,
                                const char *line3, const char *line4,
                                bool show_lock)
{
    ESP_LOGI(TAG, "Rendering status: %s / %s / %s / %s",
             line1, line2, line3 ? line3 : "", line4 ? line4 : "");

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();
    draw_power_status_text();
    if (show_lock) {
        draw_lock_icon();
    } else {
        draw_unlock_icon();
    }

    set_font(FONT_SANS_9PT);

    const int16_t x = 10;
    const int16_t y_start = 40;
    const int16_t y_spacing = 20;
    const char *lines[] = { line1, line2, line3, line4 };

    for (int i = 0; i < 4; i++) {
        if (lines[i] && lines[i][0] != '\0') {
            display.setCursor(x, y_start + (i * y_spacing));
            display.print(lines[i]);
        }
    }

    display.update();
}

/**
 * Render the splash/boot screen.
 * Split-screen layout matching the Arduino client:
 * - Left half: black background with "pe" in white
 * - Right half: white background with "bl" in black
 * - Version string in small font, bottom-right corner
 */
static void render_splash(void)
{
    ESP_LOGI(TAG, "Rendering splash screen");

    display.fillScreen(COLOR_WHITE);

    /* Fill left half black */
    int16_t half_width = display.width() / 2;
    display.fillRect(0, 0, half_width, display.height(), COLOR_BLACK);

    /* Measure full "pebl" text to center it across both halves */
    set_font(FONT_SANS_BOLD_18PT);
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds("pebl", 0, 0, &tbx, &tby, &tbw, &tbh);

    int16_t x = (display.width() - tbw) / 2 - tbx;
    int16_t y = (display.height() - tbh) / 2 - tby;

    /* "pe" in white on the black left half */
    display.setTextColor(COLOR_WHITE);
    display.setCursor(x, y);
    display.print("pe");

    /* Measure "pe" width to position "bl" correctly */
    int16_t pex, pey;
    uint16_t pew, peh;
    display.getTextBounds("pe", 0, 0, &pex, &pey, &pew, &peh);

    /* "bl" in black on the white right half */
    display.setTextColor(COLOR_BLACK);
    display.setCursor(x + pew + pex - tbx, y);
    display.print("bl");

    /* Version in small font, bottom-right corner */
    const char *version = "v" APP_VERSION;
    set_font(FONT_SANS_9PT);
    int16_t vx, vy;
    uint16_t vw, vh;
    display.getTextBounds(version, 0, 0, &vx, &vy, &vw, &vh);
    display.setCursor(display.width() - vw - 5, display.height() - SCREEN_BOTTOM_INSET - 5);
    display.print(version);

    display.update();
}

/* Dot animation state (disconnected screen).
 * Region coordinates stored during full render, reused by partial refresh. */
static bool s_dot_animating = false;
static uint8_t s_dot_count = 0;
static int16_t s_dot_cx = 0;     /* center X of dot row */
static int16_t s_dot_cy = 0;     /* center Y of dot row */
static int16_t s_dot_region_y = 0; /* top of partial refresh region */

/* Card and dot layout constants */
static const int16_t CARD_PAD_X = 16;     /* horizontal padding inside card */
static const int16_t CARD_PAD_Y = 12;     /* vertical padding inside card */
static const int16_t CARD_R = 6;          /* corner radius */
static const int16_t DOT_RADIUS = 3;
static const int16_t DOT_SPACING = 14;    /* center-to-center */
static const int16_t DOT_REGION_H = 16;   /* partial refresh height for dots */

/**
 * Render boot status: card layout with "Connecting to Server" centered.
 * Battery indicator in top-right. No dot animation here — the boot-to-connected
 * transition is typically ~2-3s, too fast for dots and the partial→full refresh
 * transition causes visible artifacts.
 */
static void render_boot_status(const char * /* device_name */)
{
    ESP_LOGI(TAG, "Rendering boot status");

    int16_t visible_h = display.height() - SCREEN_BOTTOM_INSET;

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();

    /* Measure text first so the card auto-sizes around it with consistent padding,
     * preventing the text from clipping or touching the rounded border. */
    set_font(FONT_SANS_BOLD_9PT);
    const char *msg = "Connecting to Server...";
    int16_t tx, ty;
    uint16_t tw, th;
    display.getTextBounds(msg, 0, 0, &tx, &ty, &tw, &th);

    int16_t card_w = (int16_t)tw + CARD_PAD_X * 2;
    int16_t card_h = (int16_t)th + CARD_PAD_Y * 2;
    int16_t card_x = (display.width() - card_w) / 2;
    int16_t card_y = (visible_h - card_h) / 2;
    display.fillRoundRect(card_x, card_y, card_w, card_h, CARD_R, COLOR_WHITE);
    display.drawRoundRect(card_x, card_y, card_w, card_h, CARD_R, COLOR_BLACK);

    /* Center text within the card (getTextBounds returns offset tx/ty relative to baseline). */
    int16_t text_x = card_x + (card_w - (int16_t)tw) / 2 - tx;
    int16_t text_y = card_y + (card_h - (int16_t)th) / 2 - ty;
    display.setCursor(text_x, text_y);
    display.print(msg);

    display.update();
}

/**
 * Animate dots via partial refresh (~300ms).
 * Cycles: ● → ● ● → ● ● ● → ● → ...
 * Called from display_task on a timer while waiting for connection.
 */
static void animate_dots(void)
{
    if (!s_dot_animating) return;

    s_dot_count = (s_dot_count % 3) + 1;

    board_acquire_wake_lock();
    display.setMonoMode(true);

    /* Clear dot region */
    int16_t region_x = s_dot_cx - DOT_SPACING - DOT_RADIUS - 2;
    int16_t region_w = DOT_SPACING * 2 + DOT_RADIUS * 2 + 4;
    display.fillRect(region_x, s_dot_region_y, region_w, DOT_REGION_H, EPD_WHITE);

    /* Draw dots centered: 1 dot at center, 2 dots spread, 3 dots spread */
    if (s_dot_count == 1) {
        display.fillCircle(s_dot_cx, s_dot_cy, DOT_RADIUS, EPD_BLACK);
    } else if (s_dot_count == 2) {
        display.fillCircle(s_dot_cx - DOT_SPACING / 2, s_dot_cy, DOT_RADIUS, EPD_BLACK);
        display.fillCircle(s_dot_cx + DOT_SPACING / 2, s_dot_cy, DOT_RADIUS, EPD_BLACK);
    } else {
        display.fillCircle(s_dot_cx - DOT_SPACING, s_dot_cy, DOT_RADIUS, EPD_BLACK);
        display.fillCircle(s_dot_cx, s_dot_cy, DOT_RADIUS, EPD_BLACK);
        display.fillCircle(s_dot_cx + DOT_SPACING, s_dot_cy, DOT_RADIUS, EPD_BLACK);
    }

    display.updateWindow(region_x, s_dot_region_y, region_w, DOT_REGION_H);
    display.setMonoMode(false);
    board_release_wake_lock();
}

/**
 * Render connected screen: checkmark + "Connected!" + "Waiting for Reactions.."
 * All vertically centered as a group. Optional lock icon.
 * Matches Arduino DisplayManager::showConnectedScreen().
 */
static void render_connected(bool show_lock)
{
    ESP_LOGI(TAG, "Rendering connected screen");

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();
    draw_power_status_text();
    if (show_lock) {
        draw_lock_icon();
    } else {
        draw_unlock_icon();
    }

    /* Measure text to compute total group height for vertical centering */
    set_font(FONT_SANS_BOLD_9PT);
    int16_t c1x, c1y;
    uint16_t c1w, c1h;
    display.getTextBounds("Connected!", 0, 0, &c1x, &c1y, &c1w, &c1h);

    set_font(FONT_SANS_9PT);
    int16_t c2x, c2y;
    uint16_t c2w, c2h;
    display.getTextBounds("Waiting for Reactions..", 0, 0, &c2x, &c2y, &c2w, &c2h);

    const int16_t checkSize = 20;
    const int16_t gap1 = 10;  /* gap between checkmark and "Connected!" */
    const int16_t gap2 = 6;   /* gap between "Connected!" and subtitle */
    int16_t totalHeight = checkSize + gap1 + (int16_t)c1h + gap2 + (int16_t)c2h;
    int16_t startY = (display.height() - totalHeight) / 2;
    int16_t centerX = display.width() / 2;

    /* Draw thick checkmark (V-shape with 3px line thickness) */
    int16_t lx = centerX - 10, ly = startY + 10;
    int16_t mx = centerX - 3,  my = startY + 18;
    int16_t rx = centerX + 10, ry = startY + 2;
    for (int t = 0; t < 3; t++) {
        display.drawLine(lx, ly + t, mx, my + t, COLOR_BLACK);
        display.drawLine(mx, my + t, rx, ry + t, COLOR_BLACK);
    }

    /* "Connected!" bold, centered */
    int16_t textY = startY + checkSize + gap1;
    set_font(FONT_SANS_BOLD_9PT);
    display.setCursor(centerX - (int16_t)c1w / 2, textY + (int16_t)c1h);
    display.print("Connected!");

    /* "Waiting for Reactions.." regular, centered below */
    int16_t subY = textY + (int16_t)c1h + gap2;
    set_font(FONT_SANS_9PT);
    display.setCursor(centerX - (int16_t)c2w / 2, subY + (int16_t)c2h);
    display.print("Waiting for Reactions..");

    display.update();
}

/**
 * Render disconnected screen: X icon + "Connection Lost" + subtitle.
 * All vertically centered as a group. Battery indicator only (no power text).
 * Matches Arduino DisplayManager::showDisconnectedScreen().
 */
static void render_disconnected(const char *subtitle)
{
    const char *sub = (subtitle && subtitle[0] != '\0') ? subtitle : "Reconnecting";
    ESP_LOGI(TAG, "Rendering disconnected screen: %s", sub);

    int16_t visible_h = display.height() - SCREEN_BOTTOM_INSET;

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();

    /* Measure text for vertical centering (including dot area) */
    set_font(FONT_SANS_BOLD_9PT);
    int16_t c1x, c1y;
    uint16_t c1w, c1h;
    display.getTextBounds("Connection Lost", 0, 0, &c1x, &c1y, &c1w, &c1h);

    set_font(FONT_SANS_9PT);
    int16_t c2x, c2y;
    uint16_t c2w, c2h;
    display.getTextBounds(sub, 0, 0, &c2x, &c2y, &c2w, &c2h);

    const int16_t iconSize = 20;
    const int16_t gap1 = 10;
    const int16_t gap2 = 6;
    const int16_t dotGap = 20;  /* gap from subtitle baseline to dot center */
    int16_t totalHeight = iconSize + gap1 + (int16_t)c1h + gap2 + (int16_t)c2h
                        + dotGap + DOT_RADIUS * 2;
    int16_t startY = (visible_h - totalHeight) / 2;
    int16_t centerX = display.width() / 2;

    /* Draw thick X icon (16px, 3px line thickness) */
    int16_t ix = centerX - 8;
    int16_t iy = startY + 2;
    int16_t iSize = 16;
    for (int t = 0; t < 3; t++) {
        display.drawLine(ix, iy + t, ix + iSize, iy + iSize + t, COLOR_BLACK);
        display.drawLine(ix + iSize, iy + t, ix, iy + iSize + t, COLOR_BLACK);
    }

    /* "Connection Lost" bold, centered */
    int16_t textY = startY + iconSize + gap1;
    set_font(FONT_SANS_BOLD_9PT);
    display.setCursor(centerX - (int16_t)c1w / 2, textY + (int16_t)c1h);
    display.print("Connection Lost");

    /* Subtitle regular, centered below */
    int16_t subY = textY + (int16_t)c1h + gap2;
    set_font(FONT_SANS_9PT);
    int16_t subBaseline = subY + (int16_t)c2h;
    display.setCursor(centerX - (int16_t)c2w / 2, subBaseline);
    display.print(sub);

    /* Store dot region for partial refresh animation */
    s_dot_cx = centerX;
    s_dot_cy = subBaseline + dotGap;
    s_dot_region_y = s_dot_cy - DOT_RADIUS - 2;

    /* Draw first dot */
    s_dot_count = 1;
    display.fillCircle(s_dot_cx, s_dot_cy, DOT_RADIUS, COLOR_BLACK);

    display.update();
    s_dot_animating = true;
}

/**
 * Render WiFi provisioning screen: WiFi QR code + setup instructions.
 * QR encodes WIFI:T:nopass;S:<ssid>;P:;; for auto-connect on phone scan.
 * Matches Arduino DisplayManager::showProvisioningMode() layout.
 */
static void render_wifi_provision(const char *ssid, const char *ip)
{
    ESP_LOGI(TAG, "Rendering WiFi provisioning: SSID=%s IP=%s", ssid, ip);

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();
    draw_power_status_text();

    /* Title "WiFi Setup" bold at (10, 28) */
    set_font(FONT_SANS_BOLD_9PT);
    display.setCursor(10, 28);
    display.print("WiFi Setup");

    /* Generate WiFi QR code using espressif/qrcode component.
     * Uses callback model — we render the QR inline in this function
     * via a simple lambda-like context. */
    char qr_data[96];
    snprintf(qr_data, sizeof(qr_data), "WIFI:T:nopass;S:%s;P:;;", ssid);

    /* WiFi QR is simple enough for version 3 (29x29 modules) */
    esp_qrcode_config_t qr_cfg = {
        .display_func_with_cb = NULL,
        .max_qrcode_version = 3,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
        .user_data = NULL,
    };

    /* Since espressif/qrcode uses callbacks and we need to render inline with
     * other display elements, we'll use our own QR rendering approach here.
     * Draw QR manually using the callback to capture the data. */
    struct wifi_qr_ctx {
        const char *ssid;
        const char *ip;
    } ctx = { .ssid = ssid, .ip = ip };

    /* Use a static callback that captures our context via user_data */
    struct WifiQrHelper {
        static void callback(esp_qrcode_handle_t qrcode, void *user_data) {
            wifi_qr_ctx *c = (wifi_qr_ctx *)user_data;
            int qr_size = esp_qrcode_get_size(qrcode);

            const uint8_t scale = 2;
            const int16_t qrX = 10, qrY = 43;

            /* Draw QR modules */
            for (int y = 0; y < qr_size; y++) {
                for (int x = 0; x < qr_size; x++) {
                    if (esp_qrcode_get_module(qrcode, x, y)) {
                        display.fillRect(qrX + x * scale, qrY + y * scale,
                                         scale, scale, COLOR_BLACK);
                    }
                }
            }

            /* Instruction text right of QR (small built-in font) */
            int16_t textX = qrX + qr_size * scale + 7;
            display.setFont(NULL);
            display.setCursor(textX, qrY);
            display.print("Network:");
            display.setCursor(textX, qrY + 10);
            display.print(c->ssid);
            display.setCursor(textX, qrY + 24);
            display.print("Scan QR or go to");
            display.setCursor(textX, qrY + 34);
            display.print("Settings > WiFi");
            display.setCursor(textX, qrY + 48);
            display.print("Portal: ");
            display.print(c->ip);

            display.update();
        }
    };

    qr_cfg.display_func_with_cb = WifiQrHelper::callback;
    qr_cfg.user_data = &ctx;

    esp_err_t ret = esp_qrcode_generate(&qr_cfg, qr_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi QR generation failed: %s", esp_err_to_name(ret));
        /* Fallback to text-only */
        render_status_4line("WiFi Setup", "Connect to:", ssid, ip, false);
    }
}

/**
 * Render low battery warning: "LOW BATTERY" large centered, "PLEASE CHARGE" below.
 * Battery indicator shows low-level circles in top-right.
 * Matches Arduino DisplayManager::showLowBatteryWarning().
 */
static void render_low_battery(void)
{
    ESP_LOGI(TAG, "Rendering low battery warning");

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();

    /* "LOW BATTERY" — FreeSans12pt, centered, slightly above center */
    set_font(FONT_SANS_12PT);
    const char *line1 = "LOW BATTERY";
    int16_t x1, y1;
    uint16_t w1, h1;
    display.getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);
    int16_t y1_pos = display.height() / 2 - 10;
    display.setCursor((display.width() - (int16_t)w1) / 2, y1_pos);
    display.print(line1);

    /* Instructions — FreeSans9pt, centered, 25px below */
    set_font(FONT_SANS_9PT);
    const char *line2 = "Charge, then toggle";
    const char *line3 = "switch off and on";
    int16_t x2, y2;
    uint16_t w2, h2;
    display.getTextBounds(line2, 0, 0, &x2, &y2, &w2, &h2);
    display.setCursor((display.width() - (int16_t)w2) / 2, y1_pos + 25);
    display.print(line2);

    uint16_t w3, h3;
    display.getTextBounds(line3, 0, 0, &x2, &y2, &w3, &h3);
    display.setCursor((display.width() - (int16_t)w3) / 2, y1_pos + 45);
    display.print(line3);

    display.update();
}

/**
 * Render purchase QR code screen for expired trial.
 * "Trial Expired" title, QR code (version 5 or 6), device ID.
 * Falls back to text-only status if URL is too long for QR.
 * Matches Arduino DisplayManager::showPurchaseQRCode().
 */
static void render_purchase_qr(const char *url, const char *device_id)
{
    ESP_LOGI(TAG, "Rendering purchase QR: %s (device: %s)", url, device_id);

    size_t url_len = strlen(url);

    /* Pick QR version by URL length:
     * Version 5 (37 modules) handles up to ~106 alphanumeric chars
     * Version 6 (41 modules) handles up to ~134 alphanumeric chars */
    int qr_version;
    if (url_len <= 106) {
        qr_version = 5;
    } else if (url_len <= 134) {
        qr_version = 6;
    } else {
        /* URL too long for QR — fall back to text-only */
        render_status_4line("Trial Expired", "Purchase key at:", url, device_id, false);
        return;
    }

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);

    draw_battery_indicator();
    draw_power_status_text();

    /* Render QR via espressif/qrcode callback */
    struct purchase_qr_ctx {
        const char *device_id;
    } ctx = { .device_id = device_id };

    struct PurchaseQrHelper {
        static void callback(esp_qrcode_handle_t qrcode, void *user_data) {
            purchase_qr_ctx *c = (purchase_qr_ctx *)user_data;
            int qr_size = esp_qrcode_get_size(qrcode);

            const uint8_t scale = 2;
            const int16_t qrX = 5, qrY = 28;

            for (int y = 0; y < qr_size; y++) {
                for (int x = 0; x < qr_size; x++) {
                    if (esp_qrcode_get_module(qrcode, x, y)) {
                        display.fillRect(qrX + x * scale, qrY + y * scale,
                                         scale, scale, COLOR_BLACK);
                    }
                }
            }

            /* Right column: title then instructions */
            int16_t textX = qrX + qr_size * scale + 5;
            int16_t textY = qrY + 10;

            /* "Trial Expired" bold at top of right column */
            set_font(FONT_SANS_BOLD_9PT);
            display.setCursor(textX, textY);
            display.print("Trial Expired");

            /* Switch to small font for instructions */
            display.setFont(NULL);
            textY += 14;
            display.setCursor(textX, textY);
            display.print("Scan to purchase");
            textY += 10;
            display.setCursor(textX, textY);
            display.print("activation key");
            textY += 14;
            display.setCursor(textX, textY);
            display.print("ID:");
            textY += 10;
            display.setCursor(textX, textY);
            /* Show first 20 chars of device ID */
            char short_id[21];
            strncpy(short_id, c->device_id, 20);
            short_id[20] = '\0';
            display.print(short_id);

            display.update();
        }
    };

    esp_qrcode_config_t qr_cfg = {
        .display_func_with_cb = PurchaseQrHelper::callback,
        .max_qrcode_version = qr_version,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
        .user_data = &ctx,
    };

    esp_err_t ret = esp_qrcode_generate(&qr_cfg, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Purchase QR generation failed: %s", esp_err_to_name(ret));
        render_status_4line("Trial Expired", "Purchase key at:", url, device_id, false);
    }
}

/**
 * Render network diagnostics results.
 * Uses built-in 5x7 font (not FreeSans) to fit ~13 lines on the 122px display.
 * Layout:
 *   Network Diagnostics
 *   WiFi:  OK  (SSID, -62dBm)
 *   DNS:   OK
 *   HTTPS: OK
 *   WS:    OK
 *   Auth:  OK
 *   All checks passed (or hint on failure)
 *   <blank>
 *   10.0.1.42 | FW 1.5.8
 *   device-id | 138KB | 72%
 */
static void render_diagnostics(const diag_result_t *r)
{
    ESP_LOGI(TAG, "Rendering diagnostics (wifi=%d dns=%d https=%d ws=%d auth=%d)",
             r->wifi_ok, r->dns_ok, r->https_ok, r->ws_ok, r->auth_ok);

    display.fillScreen(COLOR_WHITE);
    display.setTextColor(COLOR_BLACK);
    display.setFont(NULL);  /* Built-in 5x7 font: 6px wide, 9px line height */

    int16_t y = 2;
    const int16_t x = 2;
    const int16_t line_h = 9;

    /* Title */
    display.setCursor(x, y);
    display.print("Network Diagnostics");
    y += line_h + 2;  /* Extra gap after title */

    /* Check results. Skipped checks (dependency not met) show "--" instead of
     * "FAIL" so IT can distinguish "failed" from "never tested".
     * Dependency chain: WiFi -> DNS -> HTTPS -> WS -> Auth */
    char line[64];
    if (r->wifi_ok) {
        snprintf(line, sizeof(line), "WiFi:  OK  (%s, %ddBm)", r->ssid, r->rssi);
    } else {
        snprintf(line, sizeof(line), "WiFi:  FAIL");
    }
    display.setCursor(x, y);
    display.print(line);
    y += line_h;

    display.setCursor(x, y);
    display.print(!r->wifi_ok ? "DNS:   --" : r->dns_ok ? "DNS:   OK" : "DNS:   FAIL");
    y += line_h;

    display.setCursor(x, y);
    display.print(!r->dns_ok ? "HTTPS: --" : r->https_ok ? "HTTPS: OK" : "HTTPS: FAIL");
    y += line_h;

    display.setCursor(x, y);
    display.print(!r->https_ok ? "WS:    --" : r->ws_ok ? "WS:    OK" : "WS:    FAIL");
    y += line_h;

    display.setCursor(x, y);
    display.print(!r->ws_ok ? "Auth:  --" : r->auth_ok ? "Auth:  OK" : "Auth:  FAIL");
    y += line_h + 2;

    /* Summary or failure hint */
    display.setCursor(x, y);
    if (r->auth_ok) {
        display.print("All checks passed");
    } else if (r->auth_hint[0] != '\0') {
        snprintf(line, sizeof(line), "-> %s", r->auth_hint);
        display.print(line);
    }
    y += line_h + 4;

    /* Device info footer (two lines) */
    snprintf(line, sizeof(line), "%s | FW %s",
             r->ip_addr[0] ? r->ip_addr : "No IP", r->firmware_version);
    display.setCursor(x, y);
    display.print(line);
    y += line_h;

    if (r->battery_pct >= 0) {
        snprintf(line, sizeof(line), "%.20s | %luKB | %d%%",
                 r->device_id, (unsigned long)(r->free_heap / 1024), r->battery_pct);
    } else {
        snprintf(line, sizeof(line), "%.20s | %luKB",
                 r->device_id, (unsigned long)(r->free_heap / 1024));
    }
    display.setCursor(x, y);
    display.print(line);
    y += line_h + 4;

    /* Dismiss instruction at bottom */
    display.setCursor(x, y);
    display.print("Press button to continue");

    display.update();
}

/**
 * Clear the display to white.
 */
static void render_clear(void)
{
    ESP_LOGI(TAG, "Clearing display");
    display.fillScreen(COLOR_WHITE);
    display.update();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

esp_err_t display_manager_init(bool is_deep_sleep_wake)
{
    const app_config_t *cfg = config_manager_get_config();

    ESP_LOGI(TAG, "Initializing display (%dx%d, rotation=%d, wake=%s)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, cfg->display.rotation,
             is_deep_sleep_wake ? "deep_sleep" : "cold_boot");

    if (!display_hw_init(cfg->display.rotation)) {
        ESP_LOGE(TAG, "Display driver initialization failed");
        return ESP_FAIL;
    }
    s_initialized = true;
    enable_grayscale();

    if (!is_deep_sleep_wake) {
        render_splash();
    } else {
        ESP_LOGI(TAG, "Deep sleep wake — preserving display content");
    }

    return ESP_OK;
}

/**
 * Partial refresh of just the top status bar (battery indicator, power text, lock icon).
 * Uses UC8151D partial update commands for ~300ms refresh vs ~2100ms full refresh.
 * Only redraws the top 32 pixel rows — the rest of the display is untouched.
 */
static void render_power_change(bool show_lock)
{
    /* Draw into mono buffer for partial refresh.
     * Partial refresh is mono-only (uses _buffer, not _buffer1/_buffer2).
     * Restore grayscale mode afterward so subsequent reaction renders use 4-gray. */
    display.setMonoMode(true);

    /* Clear the top bar region in the framebuffer (32 rows, byte-aligned) */
    display.fillRect(0, 0, display.width(), 32, EPD_WHITE);

    /* Redraw top bar elements — same functions used by all other render paths */
    draw_battery_indicator();
    draw_power_status_text();
    if (show_lock) {
        draw_lock_icon();
    }

    /* Partial refresh — only sends the top 32 rows to the controller */
    display.updateWindow(0, 0, display.width(), 32);

    /* Restore grayscale mode for subsequent reaction/broadcast renders */
    display.setMonoMode(false);
}

void display_manager_render(display_event_t *evt)
{
    /* Stop boot dot animation when any new screen renders */
    s_dot_animating = false;

    /* Acquire PM lock to prevent light sleep during SPI e-paper refresh.
     * E-paper controllers require uninterrupted SPI communication during
     * the 2-4 second refresh cycle — sleeping mid-transaction corrupts
     * the display update. */
    board_acquire_wake_lock();

    switch (evt->type) {
    case DISPLAY_EVT_REACTION:
        render_reaction(&evt->data.reaction);
        break;

    case DISPLAY_EVT_PAIRING_QR:
        render_pairing_code(evt->data.pairing.code);
        break;

    case DISPLAY_EVT_STATUS:
        render_status_4line(evt->data.status.line1, evt->data.status.line2,
                            evt->data.status.line3, evt->data.status.line4,
                            evt->data.status.show_lock);
        break;

    case DISPLAY_EVT_SPLASH:
        render_splash();
        break;

    case DISPLAY_EVT_CLEAR:
        render_clear();
        break;

    case DISPLAY_EVT_BOOT_STATUS:
        render_boot_status(evt->data.boot.device_name);
        break;

    case DISPLAY_EVT_CONNECTED:
        render_connected(evt->data.connected.show_lock);
        break;

    case DISPLAY_EVT_DISCONNECTED:
        render_disconnected(evt->data.disconnected.subtitle);
        break;

    case DISPLAY_EVT_WIFI_PROVISION:
        render_wifi_provision(evt->data.wifi_provision.ssid,
                              evt->data.wifi_provision.ip);
        break;

    case DISPLAY_EVT_LOW_BATTERY:
        render_low_battery();
        break;

    case DISPLAY_EVT_PURCHASE_QR:
        render_purchase_qr(evt->data.purchase.url, evt->data.purchase.device_id);
        break;

    case DISPLAY_EVT_BROADCAST:
        render_broadcast(evt);
        break;

    case DISPLAY_EVT_DIAGNOSTICS:
        render_diagnostics(&evt->data.diagnostics.result);
        break;

    case DISPLAY_EVT_POWER_CHANGE:
        render_power_change(evt->data.power_change.show_lock);
        break;
    }

    board_release_wake_lock();

    /* Free heap-allocated emoji PNG data if present */
    if (evt->type == DISPLAY_EVT_REACTION && evt->data.reaction.emoji_png_data) {
        free(evt->data.reaction.emoji_png_data);
        evt->data.reaction.emoji_png_data = NULL;
    }
}

void display_manager_hibernate(void)
{
    if (!s_initialized) return;
    ESP_LOGI(TAG, "Hibernating e-paper controller");
    display.hibernate();
}

uint16_t display_manager_get_width(void)
{
    return DISPLAY_WIDTH;
}

uint16_t display_manager_get_height(void)
{
    return DISPLAY_HEIGHT;
}

bool display_manager_is_dot_animating(void)
{
    return s_dot_animating;
}

void display_manager_animate_dots(void)
{
    animate_dots();
}
