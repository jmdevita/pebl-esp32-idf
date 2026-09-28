/**
 * WiFi Manager — ESP-IDF port of Arduino WiFi connection + captive portal.
 *
 * Replaces:
 *   WiFi.begin()/WiFiMulti → esp_wifi_init/set_config/start/connect
 *   tzapu/WiFiManager      → Custom SoftAP + esp_http_server with branded HTML
 *   WiFi.setTxPower()      → esp_wifi_set_max_tx_power()
 *
 * After connection, enables WIFI_PS_MIN_MODEM for DTIM power save
 * (critical for light sleep with WiFi maintained).
 */

#include "wifi_manager.h"
#include "wifi_credential_manager.h"
#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "WIFI";

/* Event group for WiFi state machine */
static EventGroupHandle_t s_wifi_events = NULL;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* TX power levels in 0.25dBm units */
#define TX_POWER_LOW    44   /* 11.0 dBm — ~40% power savings */
#define TX_POWER_MED    60   /* 15.0 dBm */
#define TX_POWER_HIGH   78   /* 19.5 dBm — maximum range */

/* WiFi power state persisted across deep sleep via RTC memory.
 * Tracks the last successful TX power level and fallback mode:
 * - current_power: the level that succeeded last time — next boot starts here to skip
 *   lower levels that are already known to work (or fail).
 * - total_failed_wakes: counts boots where all 9 attempts (3 levels × 3) failed.
 * - After max_failed_wakes total failures: enter fallback mode (skip WiFi entirely for
 *   6 wakes, ~6 hours at 60-min sleep intervals), then retry from LOW.
 * - On success: reset total_failed_wakes and record which power level worked. */
RTC_DATA_ATTR static struct {
    int8_t current_power;          /* TX_POWER_LOW/MED/HIGH — last level that succeeded */
    uint8_t total_failed_wakes;    /* Boots where all TX power levels failed */
    bool fallback_mode;            /* WiFi-disabled mode after repeated total failures */
    uint8_t fallback_wake_count;   /* Wakes since entering fallback */
} s_wifi_power_state = { TX_POWER_LOW, 0, false, 0 };

static int s_retry_count = 0;
static bool s_connected = false;
static esp_netif_t *s_sta_netif = NULL;

/* One-time driver bring-up guard. wifi_manager_connect() is not re-entrant:
 * the netif, event group and event handlers may only be created/registered
 * once. It is legitimately called twice in one boot (boot diagnostics connect,
 * then the normal step-12 connect), and start_portal() may also need the
 * driver up — so the STA init is factored into ensure_wifi_initialized() and
 * gated by this flag to avoid leaking the event group, asserting on a duplicate
 * netif (IDF v5), or double-registering handlers (ESP-M4). */
static bool s_wifi_initialized = false;
/* SoftAP netif — created once and reused across portal sessions. Creating it on
 * every wifi_manager_start_portal() call leaks a netif on a second session (ESP-2). */
static esp_netif_t *s_ap_netif = NULL;

/**
 * Generate a 4-character hash from a string using polynomial rolling hash.
 * Multiplier 31, mod 1679616 (36^4), result as zero-padded base-36.
 */
static void generate_short_hash(const char *input, char *out, size_t out_size)
{
    uint32_t hash = 0;
    for (const char *p = input; *p; p++) {
        hash = hash * 31 + (uint8_t)*p;
    }
    hash = hash % 1679616;  /* 36^4 */

    static const char base36[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[5];
    for (int i = 3; i >= 0; i--) {
        tmp[i] = base36[hash % 36];
        hash /= 36;
    }
    tmp[4] = '\0';
    strncpy(out, tmp, out_size - 1);
    out[out_size - 1] = '\0';
}

/**
 * Set device hostname for network discovery after WiFi connects.
 * Format: {sanitized_device_name}-{4_char_hash_of_device_id}
 * Sanitized per RFC 1123: lowercase, alphanumeric + hyphens, max 63 chars.
 * Fallback: "pebl-device-{hash}" if name sanitizes to empty.
 */
static void set_hostname(void)
{
    const app_config_t *cfg = config_manager_get_config();

    char hash[5];
    generate_short_hash(cfg->device.id, hash, sizeof(hash));

    /* Sanitize device name per RFC 1123 */
    char sanitized[54];  /* Leave room for "-" + 4-char hash within 63-char limit */
    size_t j = 0;
    for (size_t i = 0; cfg->device.name[i] && j < sizeof(sanitized) - 1; i++) {
        char c = cfg->device.name[i];
        if (c >= 'A' && c <= 'Z') {
            sanitized[j++] = c + 32;  /* lowercase */
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            sanitized[j++] = c;
        } else if (c == ' ' || c == '_' || c == '-') {
            /* Collapse consecutive hyphens */
            if (j > 0 && sanitized[j - 1] != '-') {
                sanitized[j++] = '-';
            }
        }
        /* Skip other characters */
    }
    sanitized[j] = '\0';

    /* Trim leading/trailing hyphens */
    size_t start = 0;
    while (sanitized[start] == '-') start++;
    size_t end = strlen(sanitized);
    while (end > start && sanitized[end - 1] == '-') end--;
    sanitized[end] = '\0';

    char hostname[64];
    if (start >= end) {
        /* Name sanitized to empty — use fallback */
        snprintf(hostname, sizeof(hostname), "pebl-device-%s", hash);
    } else {
        snprintf(hostname, sizeof(hostname), "%s-%s", sanitized + start, hash);
    }

    if (s_sta_netif) {
        esp_err_t ret = esp_netif_set_hostname(s_sta_netif, hostname);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Hostname set: %s", hostname);
        } else {
            ESP_LOGW(TAG, "Failed to set hostname: %s", esp_err_to_name(ret));
        }
    }
}
static httpd_handle_t s_portal_server = NULL;
static TaskHandle_t s_dns_task_handle = NULL;
/* Cooperative shutdown flag for the DNS task. stop_portal() sets it and waits
 * for the task to close its own socket and self-delete, rather than vTaskDelete()
 * while the task is blocked in recvfrom() — which leaks the UDP socket every
 * portal cycle. */
static volatile bool s_dns_task_stop = false;

/* Cached WiFi scan results — populated by portal_do_scan(), served by portal_root_handler().
 * Avoids blocking the root page load with a 3-5 second scan. */
#define MAX_CACHED_APS  20
static wifi_ap_record_t s_cached_aps[MAX_CACHED_APS];
static uint16_t s_cached_ap_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < 10) {
            s_retry_count++;
            ESP_LOGW(TAG, "Disconnected, retry %d/10", s_retry_count);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/**
 * Scan for available networks and connect to the strongest match
 * from stored credentials (seed networks + NVS).
 */
/**
 * Scan for available APs and attempt to connect to the best match.
 * Returns ESP_OK on success, ESP_FAIL if no matching network found or
 * connection failed.
 */
static esp_err_t try_connect_once(void)
{
    const app_config_t *cfg = config_manager_get_config();

    /* Scan for available APs */
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        return ESP_FAIL;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (ap_list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Match scanned APs against stored credentials, pick strongest */
    uint8_t cred_count = 0;
    const wifi_credential_t *creds = wifi_credential_manager_get_all(&cred_count);

    int best_rssi = -128;
    int best_cred_idx = -1;

    for (int i = 0; i < ap_count; i++) {
        for (int j = 0; j < cred_count; j++) {
            if (strcmp((char *)ap_list[i].ssid, creds[j].ssid) == 0) {
                if (ap_list[i].rssi > best_rssi) {
                    best_rssi = ap_list[i].rssi;
                    best_cred_idx = j;
                }
            }
        }
    }

    free(ap_list);

    if (best_cred_idx < 0) {
        ESP_LOGW(TAG, "No matching networks found (scanned %d APs, %d credentials)",
                 ap_count, cred_count);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to '%s' (RSSI: %d)", creds[best_cred_idx].ssid, best_rssi);

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, creds[best_cred_idx].ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    /* Copy up to the full field width (64) rather than field-1: a 64-character
     * hex WPA2 PSK legitimately fills all 64 bytes with no NUL terminator, and
     * strncpy(..., sizeof-1) would silently truncate it (ESP-M5). The struct is
     * zero-initialized above, so shorter ASCII passphrases stay NUL-terminated. */
    strncpy((char *)wifi_cfg.sta.password, creds[best_cred_idx].password, sizeof(wifi_cfg.sta.password));
    /* Open networks carry an empty password. Pinning the scan threshold to
     * WPA2_PSK makes the driver reject a saved open AP on every boot after the
     * first (it connected once before the threshold was consulted), so derive
     * the authmode from whether a password is present (ESP-M5). */
    wifi_cfg.sta.threshold.authmode = creds[best_cred_idx].password[0] != '\0'
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* Clear event bits before connecting so we don't see stale state */
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(cfg->wifi.timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_credential_manager_update_last_used(creds[best_cred_idx].ssid);

        /* Enable WiFi power save for DTIM beacon association.
         * MIN_MODEM: radio sleeps between DTIM beacons (~300ms at DTIM3),
         * waking only to check for buffered frames. */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        ESP_LOGI(TAG, "WiFi power save enabled (MIN_MODEM/DTIM)");

        set_hostname();
        return ESP_OK;
    }

    return ESP_FAIL;
}

/**
 * Try connecting to a known network, escalating TX power through LOW → MED → HIGH
 * within a single call (3 attempts per level, 9 total).
 *
 * Escalating within one boot prevents the previous bug where a single cold-boot
 * failure immediately triggered the captive portal even though MED or HIGH power
 * might have succeeded. Now all power levels are tried before giving up.
 *
 * current_power remembers the last level that worked, so subsequent boots start
 * there and skip lower levels that are already known to fail (or work).
 *
 * Worst-case connection time: 3 levels × 3 attempts × ~5s per scan/auth = ~45s.
 * Happy path (home network at LOW): connects in ~3-5s on the first attempt.
 */
static esp_err_t connect_to_best_network(void)
{
    const app_config_t *cfg = config_manager_get_config();

    /* Power levels to try in order: start at the last successful level to skip
     * levels known to be unnecessary. force_high_power overrides to HIGH only. */
    const int8_t power_levels[] = { TX_POWER_LOW, TX_POWER_MED, TX_POWER_HIGH };
    const int num_all_levels = 3;

    /* Find the starting index in power_levels[] that matches current_power.
     * If current_power somehow holds an unrecognized value, fall back to LOW (index 0). */
    int start_idx = 0;
    if (cfg->wifi.force_high_power) {
        start_idx = 2;  /* Skip straight to HIGH for all attempts */
    } else {
        for (int i = 0; i < num_all_levels; i++) {
            if (power_levels[i] == s_wifi_power_state.current_power) {
                start_idx = i;
                break;
            }
        }
    }

    for (int li = start_idx; li < num_all_levels; li++) {
        int8_t tx_power = power_levels[li];
        esp_wifi_set_max_tx_power(tx_power);
        ESP_LOGI(TAG, "Trying TX power level %d (%.1f dBm)", li, tx_power * 0.25f);

        for (int attempt = 1; attempt <= 3; attempt++) {
            ESP_LOGI(TAG, "  Attempt %d/3", attempt);

            if (try_connect_once() == ESP_OK) {
                /* Remember which level succeeded so the next boot can start here */
                s_wifi_power_state.current_power = tx_power;
                s_wifi_power_state.total_failed_wakes = 0;
                return ESP_OK;
            }

            if (attempt < 3) {
                /* 5s delay between attempts: allows the AP to clean up the previous
                 * association and the radio to fully power down/up between scans. */
                ESP_LOGW(TAG, "  Failed — retrying in 5s");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }

        ESP_LOGW(TAG, "TX power level %d (%.1f dBm) exhausted — escalating",
                 li, tx_power * 0.25f);
    }

    /* All 9 attempts failed across all TX power levels */
    s_wifi_power_state.total_failed_wakes++;

    /* Reset to LOW so the next successful boot reconfirms the minimum required level */
    s_wifi_power_state.current_power = TX_POWER_LOW;

    /* Enter fallback mode after too many consecutive boot failures to avoid
     * wasting battery on WiFi scans that never succeed (e.g., router offline) */
    uint8_t max_fails = cfg->wifi.max_failed_wakes > 0 ? cfg->wifi.max_failed_wakes : 10;
    if (s_wifi_power_state.total_failed_wakes >= max_fails) {
        s_wifi_power_state.fallback_mode = true;
        s_wifi_power_state.fallback_wake_count = 0;
        ESP_LOGW(TAG, "WiFi fallback mode — will skip WiFi for 6 wake cycles");
    }

    ESP_LOGW(TAG, "All connection attempts failed across all TX power levels");
    return ESP_FAIL;
}

/**
 * One-time bring-up of the STA netif, WiFi driver, event group and handlers.
 * Idempotent: safe to call from both wifi_manager_connect() (possibly twice per
 * boot) and wifi_manager_start_portal(). Leaves the driver started in STA mode.
 * Returns ESP_OK once the driver is up.
 */
static esp_err_t ensure_wifi_initialized(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialize TCP/IP and WiFi */
    ESP_ERROR_CHECK(esp_netif_init());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Register event handlers */
    esp_event_handler_instance_t instance_wifi, instance_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    wifi_event_handler, NULL, &instance_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    wifi_event_handler, NULL, &instance_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Initialize credential manager (loads NVS + merges seed networks) */
    wifi_credential_manager_init();

    s_wifi_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(void)
{
    /* Check fallback mode: skip WiFi entirely after repeated failures.
     * After 6 wakes in fallback (~6 hours at 60-min deep sleep interval),
     * reset all state and try WiFi again from LOW power.
     * Checked before driver init so a fallback wake stays cheap. */
    if (s_wifi_power_state.fallback_mode) {
        s_wifi_power_state.fallback_wake_count++;
        if (s_wifi_power_state.fallback_wake_count >= 6) {
            ESP_LOGI(TAG, "Fallback recovery — resetting WiFi power state");
            s_wifi_power_state.fallback_mode = false;
            s_wifi_power_state.current_power = TX_POWER_LOW;
            s_wifi_power_state.total_failed_wakes = 0;
            s_wifi_power_state.fallback_wake_count = 0;
        } else {
            ESP_LOGW(TAG, "WiFi fallback mode — skipping WiFi (wake %d/6)",
                     s_wifi_power_state.fallback_wake_count);
            return ESP_FAIL;
        }
    }

    esp_err_t init_ret = ensure_wifi_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    /* Skip the entire scan/connect cycle if there are no credentials — returning
     * ESP_FAIL here triggers the captive portal immediately. Without this check,
     * connect_to_best_network() would run 9 scans across 3 TX power levels (~40s)
     * before giving up, even though the outcome is certain. */
    uint8_t cred_count = 0;
    wifi_credential_manager_get_all(&cred_count);
    if (cred_count == 0) {
        ESP_LOGI(TAG, "No credentials stored — skipping connection attempts");
        return ESP_FAIL;
    }

    return connect_to_best_network();
}

bool wifi_manager_has_credentials(void)
{
    /* Loads seed networks + NVS credentials on first call (idempotent). Used by
     * app_main to decide whether a failed connect should open the captive portal
     * or just deep-sleep and retry (ESP-1). */
    wifi_credential_manager_init();
    uint8_t count = 0;
    wifi_credential_manager_get_all(&count);
    return count > 0;
}

/* System event group and bit — set by wifi_manager_start_portal().
 * When portal WiFi connects, this bit is set on the caller's event group
 * so app_main can continue the boot sequence. */
static EventGroupHandle_t s_system_events = NULL;
static EventBits_t s_wifi_connected_bit = 0;

/**
 * Shared CSS for all portal pages — matches PEBL website design language.
 * Warm cream background, dark charcoal buttons, system fonts (no Google Fonts
 * since the device has no internet during provisioning).
 */
static const char *PORTAL_CSS =
    "<style>"
    "body{background:#faf8f4;color:#1c1c1e;"
        "font-family:-apple-system,system-ui,sans-serif;"
        "margin:0;padding:20px;}"
    "h1{font-size:1.6rem;letter-spacing:2px;margin-bottom:0;text-align:center;}"
    "h3{color:#6b6560;font-weight:400;font-size:0.95rem;margin-top:4px;text-align:center;}"
    "form{max-width:320px;margin:20px auto;}"
    "label{color:#6b6560;font-size:0.85rem;display:block;margin-top:12px;}"
    "input[type='text'],input[type='password'],select{"
        "border:1px solid #eee8df;border-radius:10px;"
        "padding:10px 12px;font-size:1rem;background:#fff;"
        "width:100%;box-sizing:border-box;margin-top:4px;}"
    "input:focus{border-color:#1c1c1e;outline:none;}"
    "button,input[type='submit']{background:#1c1c1e;color:#faf8f4;"
        "border-radius:10px;border:0;line-height:2.6rem;font-size:1rem;"
        "font-weight:600;transition:opacity 0.2s;width:100%;"
        "margin-top:16px;cursor:pointer;}"
    "button:hover,input[type='submit']:hover{opacity:0.85;}"
    ".btn-outline{background:#faf8f4;color:#1c1c1e;"
        "border:1px solid #eee8df !important;}"
    ".btn-outline:hover{border-color:#1c1c1e !important;}"
    ".msg{padding:12px;border-left:4px solid #eee8df;border-radius:10px;"
        "background:#fff;margin:16px auto;max-width:320px;}"
    ".msg.S{border-left-color:#4ade80;}"
    ".msg.D{border-left-color:#c2410c;}"
    "hr{border:none;border-top:1px solid #eee8df;margin:20px 0;}"
    "</style>";

/**
 * Run a blocking WiFi scan and store results in the static cache.
 * Active scan: 100-300ms per channel × 13 channels ≈ 3-4s.
 * Thorough timing ensures we find all nearby networks.
 */
static void portal_do_scan(void)
{
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;
    esp_wifi_scan_start(&scan_cfg, true);

    s_cached_ap_count = MAX_CACHED_APS;
    esp_wifi_scan_get_ap_records(&s_cached_ap_count, s_cached_aps);
    ESP_LOGI(TAG, "WiFi scan complete: %d networks found", s_cached_ap_count);
}

/**
 * GET / — Main portal page. Serves cached scan results instantly.
 * First load triggers an initial scan if cache is empty.
 */
static esp_err_t portal_root_handler(httpd_req_t *req)
{
    /* First load: populate cache if empty */
    if (s_cached_ap_count == 0) {
        portal_do_scan();
    }

    /* Build HTML response from cached results */
    char *html = (char *)malloc(4096);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = snprintf(html, 4096,
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>pebl setup</title>%s</head><body>"
        "<h1>pebl</h1>"
        "<h3>WiFi Setup</h3>"
        "<form action='/save' method='post'>"
        "<label>Network</label>"
        "<select name='ssid'>",
        PORTAL_CSS);

    /* Add cached networks to dropdown */
    for (int i = 0; i < s_cached_ap_count && pos < 3800; i++) {
        pos += snprintf(html + pos, 4096 - pos,
            "<option value='%s'>%s</option>",
            (char *)s_cached_aps[i].ssid, (char *)s_cached_aps[i].ssid);
    }

    pos += snprintf(html + pos, 4096 - pos,
        "</select>"
        "<button type='button' id='rsbtn' class='btn-outline' "
        "style='display:inline-block;margin:8px 0 12px;font-size:0.85rem;padding:6px 14px;'"
        "onclick=\"this.textContent='Scanning...';this.disabled=true;"
        "window.location.href='/rescan';\">"
        "Rescan</button>"
        "<label>Password</label>"
        "<input type='password' name='password' id='pw' placeholder='WiFi password'>"
        "<label style='font-size:0.85rem;cursor:pointer;'>"
        "<input type='checkbox' onclick=\"var p=document.getElementById('pw');"
        "p.type=p.type==='password'?'text':'password';\"> Show password</label>"
        "<input type='submit' value='Connect'>"
        "</form>"
        "<hr>"
        "<form action='/clear' method='post' style='max-width:320px;margin:0 auto;'>"
        "<button class='btn-outline' type='submit'>Clear Saved Networks</button>"
        "</form>"
        "</body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, pos);
    free(html);
    return ESP_OK;
}

/**
 * Decode URL-encoded string in-place: %XX → byte, + → space.
 * Handles application/x-www-form-urlencoded encoding from browser forms.
 */
static void url_decode_inplace(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            /* Convert two hex digits to a byte */
            char hex[3] = { src[1], src[2], '\0' };
            char *endp;
            unsigned long val = strtoul(hex, &endp, 16);
            if (endp == hex + 2) {
                *dst++ = (char)val;
                src += 3;
            } else {
                /* Invalid %XX sequence — copy literal '%' */
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * POST /save — Handle WiFi credentials submission from portal form.
 * Saves credentials to NVS, attempts connection, and reports result.
 */
static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    /* Parse URL-encoded form data: ssid=xxx&password=yyy */
    char ssid[33] = {0};
    char password[65] = {0};

    /* Simple URL-encoded form parser */
    char *ssid_start = strstr(body, "ssid=");
    char *pass_start = strstr(body, "password=");

    if (ssid_start) {
        ssid_start += 5;
        char *end = strchr(ssid_start, '&');
        size_t len = end ? (size_t)(end - ssid_start) : strlen(ssid_start);
        if (len > sizeof(ssid) - 1) len = sizeof(ssid) - 1;
        memcpy(ssid, ssid_start, len);
    }

    if (pass_start) {
        pass_start += 9;
        char *end = strchr(pass_start, '&');
        size_t len = end ? (size_t)(end - pass_start) : strlen(pass_start);
        if (len > sizeof(password) - 1) len = sizeof(password) - 1;
        memcpy(password, pass_start, len);
    }

    /* Decode URL-encoded values in-place: %XX → byte, + → space.
     * Browsers encode form fields this way (application/x-www-form-urlencoded),
     * so passwords with special chars like !#@& would fail without this. */
    url_decode_inplace(ssid);
    url_decode_inplace(password);

    if (ssid[0] == '\0') {
        char resp[2048];
        int len = snprintf(resp, sizeof(resp),
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "%s</head><body>"
            "<div class='msg D'><b>Error:</b> No SSID provided</div>"
            "<form action='/' method='get' style='text-align:center;'>"
            "<button class='btn-outline' type='submit'>Back</button></form>"
            "</body></html>", PORTAL_CSS);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, resp, len);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Portal: saving credentials for '%s'", ssid);

    /* Save credentials to NVS */
    wifi_credential_manager_add(ssid, password);

    /* Attempt to connect with the new credentials */
    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    /* Full field width (64) so a 64-char hex PSK isn't truncated (ESP-M5);
     * struct is zero-initialized so shorter passphrases stay NUL-terminated. */
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    /* Wait briefly for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(15000));

    char resp[2048];
    int len;

    if (bits & WIFI_CONNECTED_BIT) {
        len = snprintf(resp, sizeof(resp),
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "%s</head><body>"
            "<div class='msg S'><b>Connected!</b> Device is now online.</div>"
            "<h3>You can close this page.</h3>"
            "</body></html>", PORTAL_CSS);

        /* Signal that portal WiFi connection succeeded.
         * Sets the caller's event group bit so app_main continues boot. */
        if (s_system_events) {
            xEventGroupSetBits(s_system_events, s_wifi_connected_bit);
        }

        /* Enable DTIM power save after successful connection */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        len = snprintf(resp, sizeof(resp),
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "%s</head><body>"
            "<div class='msg D'><b>Connection failed.</b> Check password and try again.</div>"
            "<form action='/' method='get' style='text-align:center;'>"
            "<button class='btn-outline' type='submit'>Back</button></form>"
            "</body></html>", PORTAL_CSS);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/**
 * POST /clear — Clear all saved WiFi networks from NVS.
 */
static esp_err_t portal_clear_handler(httpd_req_t *req)
{
    wifi_credential_manager_clear_nvs();
    ESP_LOGI(TAG, "Saved networks cleared via portal");

    /* Post/Redirect/Get pattern: redirect back to root to prevent
     * duplicate POST submissions from captive portal webview retries. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * GET /rescan — Trigger a fresh WiFi scan, then redirect to root.
 * The scan takes 3-4 seconds. The user sees "Scanning..." on the button
 * (set by JS before navigation) until the redirect completes.
 */
static esp_err_t portal_rescan_handler(httpd_req_t *req)
{
    portal_do_scan();

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * Captive portal redirect handler — iOS/Android/Windows detect captive portals
 * by requesting known URLs. Redirect them to the setup page.
 */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * Lightweight DNS server task — resolves ALL domains to 192.168.4.1.
 * This is the critical piece for captive portal auto-popup: when a phone
 * connects to the AP and tries to reach connectivity-check.gstatic.com (Android),
 * captive.apple.com (iOS), or msftconnecttest.com (Windows), our DNS intercepts
 * the query and returns the AP's IP. The OS then hits our HTTP redirect handlers,
 * triggering the captive portal popup.
 *
 * DNS response format: copy the query, set response flags, append a single
 * A record answer pointing to 192.168.4.1.
 */
static void dns_server_task(void *arg)
{
    ESP_LOGI(TAG, "DNS server started on port 53");

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Set receive timeout so the task can check for shutdown */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (!s_dns_task_stop) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;  /* Too short or timeout — loop re-checks stop flag */

        /* Build DNS response:
         * - Copy query header, set QR=1 (response), ANCOUNT=1
         * - Copy the question section as-is
         * - Append one A record answer: name pointer + type A + class IN + TTL + 4-byte IP */
        buf[2] |= 0x80;   /* QR = 1 (response) */
        buf[3] = 0x00;     /* No error */
        buf[6] = 0x00;     /* ANCOUNT high byte */
        buf[7] = 0x01;     /* ANCOUNT = 1 */

        /* Find end of question section (skip QNAME + QTYPE + QCLASS) */
        int qname_end = 12;
        while (qname_end < len && buf[qname_end] != 0) {
            qname_end += buf[qname_end] + 1;
        }
        qname_end++;        /* Skip null terminator */
        int question_end = qname_end + 4;  /* +2 QTYPE +2 QCLASS */

        if (question_end + 16 > (int)sizeof(buf)) continue;  /* Buffer overflow guard */

        /* Append answer: pointer to QNAME + A record with 192.168.4.1 */
        int pos = question_end;
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;  /* Name pointer to offset 12 (QNAME) */
        buf[pos++] = 0x00; buf[pos++] = 0x01;  /* Type A */
        buf[pos++] = 0x00; buf[pos++] = 0x01;  /* Class IN */
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x0A;  /* TTL = 10 seconds */
        buf[pos++] = 0x00; buf[pos++] = 0x04;  /* RDLENGTH = 4 */
        buf[pos++] = 192;  buf[pos++] = 168;
        buf[pos++] = 4;    buf[pos++] = 1;     /* 192.168.4.1 */

        sendto(sock, buf, pos, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }

    /* Close our own socket on the way out (stop flag set by stop_portal) so the
     * UDP socket isn't leaked across portal cycles. */
    close(sock);
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_portal(EventGroupHandle_t system_events, EventBits_t wifi_connected_bit)
{
    ESP_LOGI(TAG, "Starting captive portal (AP: pebl-setup)");

    s_system_events = system_events;
    s_wifi_connected_bit = wifi_connected_bit;

    /* The driver must be initialized before we touch set_mode/set_config, or the
     * calls abort() on an uninitialized driver — reachable on the WiFi-fallback
     * path where wifi_manager_connect() returned before ever calling esp_wifi_init()
     * (ESP-2b). ensure_wifi_initialized() is idempotent and leaves the driver
     * started in STA mode. */
    esp_err_t init_ret = ensure_wifi_initialized();
    if (init_ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start portal — WiFi init failed: %s", esp_err_to_name(init_ret));
        return init_ret;
    }

    /* Create the SoftAP netif exactly once. Doing it every portal start leaks a
     * netif (and duplicates the AP) on a second in-boot session (ESP-2). */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {};
    memcpy(ap_cfg.ap.ssid, "pebl-setup", 10);
    ap_cfg.ap.ssid_len = 10;
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 2;
    esp_err_t mode_ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(mode_ret));
        return mode_ret;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Start the driver so the AP actually beacons. The button-provisioning path
     * runs wifi_manager_disconnect() first (esp_wifi_stop()), so without this the
     * AP never comes up and ws_task blocks forever (ESP-2). On the cold-boot path
     * the driver is already started from the failed connect, so tolerate
     * ESP_ERR_WIFI_NOT_STOPPED ("already started"). */
    esp_err_t start_ret = esp_wifi_start();
    if (start_ret != ESP_OK && start_ret != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "esp_wifi_start() for portal failed: %s", esp_err_to_name(start_ret));
        return start_ret;
    }

    /* Start HTTP server for captive portal */
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.stack_size = 8192;
    httpd_cfg.max_uri_handlers = 12;

    if (httpd_start(&s_portal_server, &httpd_cfg) == ESP_OK) {
        /* Register portal page handlers */
        const httpd_uri_t root = {
            .uri = "/", .method = HTTP_GET, .handler = portal_root_handler
        };
        const httpd_uri_t save = {
            .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler
        };
        const httpd_uri_t clear = {
            .uri = "/clear", .method = HTTP_POST, .handler = portal_clear_handler
        };
        const httpd_uri_t rescan = {
            .uri = "/rescan", .method = HTTP_GET, .handler = portal_rescan_handler
        };
        /* Captive portal detection endpoints — each OS checks a different URL:
         * Android: /generate_204, /gen_204
         * iOS/macOS: /hotspot-detect.html
         * Windows: /connecttest.txt, /ncsi.txt, /redirect
         * Firefox: /canonical.html (via detectportal.firefox.com)
         * All redirect to the setup page to trigger the captive portal popup. */
        const httpd_uri_t generate_204 = {
            .uri = "/generate_204", .method = HTTP_GET, .handler = portal_redirect_handler
        };
        const httpd_uri_t gen_204 = {
            .uri = "/gen_204", .method = HTTP_GET, .handler = portal_redirect_handler
        };
        const httpd_uri_t hotspot = {
            .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = portal_redirect_handler
        };
        const httpd_uri_t connecttest = {
            .uri = "/connecttest.txt", .method = HTTP_GET, .handler = portal_redirect_handler
        };
        const httpd_uri_t ncsi = {
            .uri = "/ncsi.txt", .method = HTTP_GET, .handler = portal_redirect_handler
        };
        const httpd_uri_t redirect = {
            .uri = "/redirect", .method = HTTP_GET, .handler = portal_redirect_handler
        };
        const httpd_uri_t canonical = {
            .uri = "/canonical.html", .method = HTTP_GET, .handler = portal_redirect_handler
        };

        httpd_register_uri_handler(s_portal_server, &root);
        httpd_register_uri_handler(s_portal_server, &save);
        httpd_register_uri_handler(s_portal_server, &clear);
        httpd_register_uri_handler(s_portal_server, &rescan);
        httpd_register_uri_handler(s_portal_server, &generate_204);
        httpd_register_uri_handler(s_portal_server, &gen_204);
        httpd_register_uri_handler(s_portal_server, &hotspot);
        httpd_register_uri_handler(s_portal_server, &connecttest);
        httpd_register_uri_handler(s_portal_server, &ncsi);
        httpd_register_uri_handler(s_portal_server, &redirect);
        httpd_register_uri_handler(s_portal_server, &canonical);

        ESP_LOGI(TAG, "Captive portal HTTP server started on 192.168.4.1");
    }

    /* Start DNS server — resolves all domains to 192.168.4.1 so that
     * OS captive portal detection requests reach our HTTP server. */
    s_dns_task_stop = false;
    xTaskCreate(dns_server_task, "dns_task", 4096, NULL, 3, &s_dns_task_handle);
    return ESP_OK;
}

void wifi_manager_stop_portal(void)
{
    if (s_portal_server) {
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
        ESP_LOGI(TAG, "Captive portal HTTP server stopped");
    }

    if (s_dns_task_handle) {
        /* Signal the DNS task to break out of recvfrom() and close its own
         * socket, then wait for it to self-delete. The recv timeout is 2s, so
         * allow generous headroom. This avoids vTaskDelete()-ing a task blocked
         * in recvfrom(), which would leak the bound UDP socket each cycle. */
        s_dns_task_stop = true;
        for (int i = 0; i < 40 && s_dns_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_dns_task_handle = NULL;
        ESP_LOGI(TAG, "DNS server stopped");
    }

    /* Switch from AP+STA back to STA-only to free the SoftAP resources.
     * The AP netif (s_ap_netif) is intentionally kept for reuse on a later
     * portal session. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "Captive portal shut down, switched to STA mode");
}

void wifi_manager_disconnect(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_connected = false;
}

esp_err_t wifi_manager_reconnect(void)
{
    ESP_LOGI(TAG, "Forcing WiFi reconnect cycle");

    /* Disconnect and stop WiFi, but don't destroy the netif or re-register
     * event handlers. wifi_manager_connect() would re-init TCP/IP, create
     * a duplicate netif (memory leak), and register duplicate event handlers.
     * Instead, stop/restart the WiFi driver and re-scan for networks.
     *
     * esp_wifi_stop() is required before esp_wifi_start() — calling start()
     * on an already-started driver returns ESP_ERR_WIFI_CONN, which causes
     * ESP_ERROR_CHECK to abort() and reboot the device. */
    esp_wifi_disconnect();
    s_connected = false;

    /* 5s pause: allow AP to clean up the old association and give the radio
     * time to fully power down/up. Shorter delays risk the AP rejecting
     * the new connection attempt as a duplicate. */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Stop then restart WiFi driver to force a clean reconnect cycle */
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_start());
    return connect_to_best_network();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!s_connected) {
        return 0;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool wifi_manager_is_fallback_mode(void)
{
    return s_wifi_power_state.fallback_mode;
}
