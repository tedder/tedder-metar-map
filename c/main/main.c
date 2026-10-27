/*
 * tedder-metar-map: one LED per airport, colored by flight category.
 *
 * Board: Adafruit QT Py ESP32-S2 (or similar). LED string on GPIO 5 (led_gpio setting), optional
 * SSD1306 OLED on I2C, onboard NeoPixel as a status light, BOOT button (GPIO 0) for unlock and
 * setup mode. Weather comes from metar-service as one CSV line per airport.
 *
 * Settings live in NVS (namespace tmm_cfg): setup page, USB provisioning (tools/provision.py) or
 * the per-board config file (<config base>/<boardid>.json). Firmware updates are signed and come
 * from <firmware base>/{stable,beta}.bin; a new image must fetch weather once before it is kept.
 * Key names and defaults: docs/config-options.md.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_core_dump.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "mdns.h"
#include "captive_dns.h"
#include "tx_tune.h"
#include "radio_verdict.h"
#include "oled_ssd1306.h"
#include "airports.h"
#include "ping/ping_sock.h"
#include <ctype.h>
#include "esp_crt_bundle.h"
#include "psa/crypto.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "led_strip.h"
#include "driver/gpio.h"

#define FIRMWARE_VERSION    "0.5.5"

/* LED string. The strip is always driven as LED_MAX pixels: LEDs past the airport list get
 * explicit zeros, so a longer strip than the list stays dark without a count setting. */
#define LED_GPIO_DEFAULT    5

/* Onboard NeoPixel (QT Py ESP32-S2): data GPIO 39, power GPIO 38. Status light, not a map LED. */
#define STATUS_PIX_GPIO     39
#define STATUS_PIX_PWR_GPIO 38

/* BOOT button: GPIO 0 to ground, pulled up (pressed = low). Any press shorter than BTN_LONG_MIN_MS
 * unlocks saving for UNLOCK_MS; a hold of BTN_LONG_MIN_MS opens the setup network. Holding it while
 * the board powers up or resets starts the ROM download mode instead (that's the chip, not us). */
#define BTN_GPIO            0
#define BTN_POLL_MS         10
#define BTN_DEBOUNCE_N      5
#define BTN_LONG_MIN_MS     9000
#define BTN_ACK_MS          600
#define UNLOCK_MS           (15 * 60 * 1000)  /* saving allowed this long after a press */

#define NVS_NAMESPACE       "tmm_cfg"
#define DEFAULT_SSID        "DEFAULT_SSID"
#define DEFAULT_PASS        "DEFAULT_PASS"
#define DEFAULT_URL_PFX     "https://fly.tedder.me/metar?ids="
#define DEFAULT_BRI_IDX     4                 /* 25 %; the 0.4 firmware ran at a fixed 20 % */
#define BRIGHTNESS_LEVELS_INIT { 0.0f, 0.03f, 0.07f, 0.15f, 0.25f, 0.4f, 0.55f, 0.75f, 1.0f }  /* 0 = off */
/* The weather service answers a cache miss for a long list in 1-10 s, sometimes over 15 s
 * (CloudFront logs 2026-10-22, 47 airports); 0.4.x waited 30 s. */
#define WEATHER_TIMEOUT_MS  30000
#define AIRPORTS_MAX_TEXT   1600              /* NVS "airports": tokens joined by '\n' */

/* Shown for an airport with no current report, and for every airport when our own weather is old
 * (metar-service sends the same color for "no report"). */
#define NO_REPORT_R         75
#define NO_REPORT_G         75
#define NO_REPORT_B         50

#define DBG_HTTP_TIMEOUT       0x01
#define DBG_HTTP_NON_200       0x02
#define DBG_PARSE_ERROR        0x04
#define DBG_WIFI_DISCONNECT    0x08
#define DBG_OTA_FAILED         0x10
#define DBG_WDT_FIRED          0x20
#define DBG_HTTPD_START_FAIL   0x40
#define DBG_HTTPD_DEAD         0x80
#define DBG_STALE_FETCH_REBOOT 0x100
#define DBG_CFG_REJECTED       0x200
#define DBG_OTA_BAD_IMAGE      0x400
#define DBG_OTA_DOWNLOAD       0x800
#define DBG_OTA_ROLLED_BACK    0x1000
#define DBG_CFG_FETCH_FAIL     0x2000
#define DBG_LED_INIT_FAIL      0x4000
#define DBG_FLAG_COUNT         15
/* Cleared by the next success of the same kind instead of waiting out the 24 h expiry. */
#define DBG_FETCH_FLAGS        (DBG_HTTP_TIMEOUT | DBG_HTTP_NON_200 | DBG_PARSE_ERROR)
#define DBG_OTA_FLAGS          (DBG_OTA_FAILED | DBG_OTA_BAD_IMAGE | DBG_OTA_DOWNLOAD)

#define POLL_PERIOD_MS      ((uint32_t)s_poll_sec * 1000)
#define RESP_MAX_BYTES      (LED_MAX * 48)    /* one CSV line per airport */
#define URL_MAX             (160 + LED_MAX * 5 + 200)
/* OTA:    <firmware base>/stable.bin or /beta.bin  (NVS "ota_ch": "" or "beta")
 * Config: <config base>/<boardid>.json
 * Bases: NVS "fw_base_url" / "cfg_base_url", else the defaults below. docs/config-options.md */
#define FIRMWARE_BASE_URL   "https://tedder.me/esp32/tedder-metar-map/bin"
#define CONFIG_BASE_URL     "https://tedder.me/esp32/tedder-metar-map/cfg"
#define OTA_INT_MIN_DEFAULT 360     /* NVS "ota_int", minutes, 15..1440 */
#define CFG_INT_MIN_DEFAULT 60      /* NVS "cfg_int", minutes, 5..1440 */
#define CFG_MAX_BYTES       4096
#define WIFI_SLOTS          3       /* slot 0 = setup page / provisioning; 1-2 = config pusher */
#define WIFI_SLOT_FAILS     3       /* reconnect failures before trying the next saved network */
/* No STA IP this long (with saved WiFi) → open setup AP while still retrying STA */
#define FALLBACK_AP_MS      ((int64_t)s_fb_min * 60000)   /* NVS fb_sec overrides it only with test_hooks=1 */
#define FALLBACK_RETRY_MS   60000
#define LOCAL_STALE_MS      ((int64_t)s_stale_min * 60000)
#define HARD_REBOOT_MS      ((int64_t)s_stale_reboot_h * 3600000)
#define TWDT_TIMEOUT_MS     60000
#define WIFI_PROBE_TIMEOUT_MS 25000

/** Chunk size for streaming HTML (bounded copies per syscall; avoids one giant socket write). */
#define HTTP_HTML_CHUNK       512

#define WIFI_CONNECTED_BIT  BIT0

static const char *TAG = "metarmap";

typedef enum {
    STAGE_BOOT = 0,
    STAGE_WIFI_INIT,
    STAGE_WIFI_STARTED,
    STAGE_ASSOCIATED,
    STAGE_GOT_IP,
    STAGE_HTTP_REQ,
    STAGE_HTTP_OK,
} stage_t;

typedef struct {
    stage_t stage;
    bool    has_data;      /* at least one weather reply parsed since boot */
    int64_t last_fetch_ms;
    int     n_reports;     /* airports with a current category in the last reply */
} state_t;

// Core state
static EventGroupHandle_t s_wifi_event_group;
static led_strip_handle_t s_strip;
static led_strip_handle_t s_status_pix;
static bool               s_led_rgbw;
static char               s_boardid[13];
static SemaphoreHandle_t  s_state_lock;   /* s_state and s_slots */
static state_t            s_state = { .stage = STAGE_BOOT };
static slot_t             s_slots[LED_MAX];
static int                s_slot_n;       /* entries in the airport list (lit or dark) */
static char              *s_last_body;    /* last weather reply, for the Technical card */
static volatile bool      s_oled_ok;
static volatile bool      s_leds_dirty = true;
static esp_netif_t *s_sta_netif;

// NVS config
static nvs_handle_t s_nvs;
static bool         s_nvs_ready;
/** NVS contains a non-empty `ssid` key — Wi-Fi was saved at least once. */
static bool         s_wifi_ssid_from_nvs;
static char         s_cfg_ssid[32];   // wifi ssid max 32 bytes
static char         s_cfg_pass[64];
static char         s_cfg_url_pfx[128];
static char         s_airports_text[AIRPORTS_MAX_TEXT];

// Brightness
static const float  k_bri_levels[] = BRIGHTNESS_LEVELS_INIT;
static volatile uint8_t s_bri_idx       = DEFAULT_BRI_IDX;
static uint8_t          s_bri_saved_idx = DEFAULT_BRI_IDX;
static int64_t          s_bri_last_tap;

static const char *bri_label(uint8_t idx)
{
    static char buf[8];
    if (k_bri_levels[idx] <= 0.0f) return "off";
    snprintf(buf, sizeof(buf), "%.0f%%", (double)(k_bri_levels[idx] * 100.0f));
    return buf;
}

/* Page swatch for an LED color: LEDs are driven at low values but look bright, so scale the
   brightest channel to 255, then mix 30% toward white so dark blue reads on a gray page. */
static unsigned sw_hex(unsigned c)
{
    unsigned r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    unsigned m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (!m) return 0x000000;
    r = r * 255 / m; g = g * 255 / m; b = b * 255 / m;
    r += (255 - r) * 3 / 10; g += (255 - g) * 3 / 10; b += (255 - b) * 3 / 10;
    return (r << 16) | (g << 8) | b;
}

static volatile int64_t s_btn_ack_until_ms;
/* Button test hooks: POST /test/button?ms=N holds a simulated press through the real debounce
 * path; the status page shows the raw GPIO and the last press so wiring can be confirmed. */
static volatile int64_t s_btn_sim_until_ms;
static volatile int     s_btn_gpio_level = 1;
static volatile int64_t s_btn_last_ms;          /* duration of last completed press */
static volatile int64_t s_btn_last_at_ms;       /* when it ended (0 = never) */
static volatile bool    s_btn_last_physical;
/* Save lock: on the LAN (and in automatic setup mode) saving needs a BOOT button press within
 * UNLOCK_MS. Manual setup mode was entered by a 9 s hold, so it counts as unlocked. */
static volatile int64_t s_unlock_until_ms;

// AP mode
static volatile bool     s_ap_mode;
static volatile uint8_t  s_ap_clients;
static esp_netif_t      *s_ap_netif;
/** During captive-portal "Save WiFi" STA probe: allow disconnect handler to reconnect (ignore bare AP-mode STA suppression). */
static volatile bool     s_wifi_probe_active;

// Telemetry (sent as &t= on every METAR fetch; see metar_url_build)
static uint32_t          s_boot_count;     /* NVS "boots", incremented once per boot */
static int               s_reset_reason;   /* esp_reset_reason() at boot */
static volatile uint32_t s_wifi_drops;     /* STA disconnects since boot */
static volatile uint32_t s_fetch_fails;    /* failed METAR fetches since boot */
static volatile uint32_t s_fetch_count;    /* good METAR fetches since boot */
static char              s_crash[200];     /* last crash summary (NVS "crash"), "" = none */

static char              s_cfg_ota_ch[8];  /* "" or "beta" */
static volatile bool     s_fallback_ap;    /* setup AP opened automatically; STA still retried */
static volatile int64_t  s_wifi_down_since_ms;  /* 0 = STA has IP */

// Saved networks: slot 0 = s_cfg_ssid/s_cfg_pass ("ssid"/"pass"), slots 1-2 = "ssid1".."pass2"
static char              s_extra_ssid[WIFI_SLOTS - 1][33];
static char              s_extra_pass[WIFI_SLOTS - 1][64];
static volatile int      s_wifi_slot;
static int               s_wifi_fail_streak;

// Intervals + config pusher state
static uint16_t          s_ota_int_min = OTA_INT_MIN_DEFAULT;
static uint16_t          s_cfg_int_min = CFG_INT_MIN_DEFAULT;
static uint32_t          s_cfg_rev;        /* NVS "cfg_rev": last applied config file rev */
static int64_t           s_cfg_last_ms;
static char              s_cfg_result[80] = "not checked yet";
static char              s_fw_base[128];   /* "" = FIRMWARE_BASE_URL */
static char              s_cfg_base[128];  /* "" = CONFIG_BASE_URL */
/* Telemetry: config pusher outcome of the last check */
enum { CFG_ST_NOT_CHECKED = 0, CFG_ST_NO_FILE, CFG_ST_CURRENT, CFG_ST_REJECTED, CFG_ST_FETCH_ERROR };
static volatile uint8_t  s_cfg_status;
static TaskHandle_t      s_ota_task;
static TaskHandle_t      s_cfg_task;
static TaskHandle_t      s_poll_task;
static SemaphoreHandle_t s_cfg_mutex;      /* cfg_task vs setup page vs POST /test/config */
/* ETag of the last config file processed / firmware file checked: a HEAD with the same ETag
 * skips the download. RAM only, so the first check after boot always does the full fetch. */
#define ETAG_MAX 72
static char              s_cfg_etag[ETAG_MAX];
static char              s_ota_etag[ETAG_MAX];
static char              s_ota_etag_url[160];
static char              s_ota_result[80] = "not checked yet";
/* Bench units only (NVS test_hooks=1, set over USB): /test/ endpoints, fb_sec. */
static bool              s_test_hooks;
static int64_t           s_fallback_ap_ms = 5 * 60 * 1000;
static bool              s_fb_sec_test;     /* fb_sec (test hooks) overrides wifi_fallback_minutes */

/* Tunable settings (k_settings below). LED hardware ones are read once at boot. */
static volatile int      s_poll_sec = 300, s_fb_min = 5, s_stale_min = 60, s_stale_reboot_h = 12;
static volatile int      s_status_pixel = 1, s_verbose_tel = 1;
/* No-report (grey) airport LEDs, percent of the map brightness by day and during night dimming */
static volatile int      s_nr_day_pct = 30, s_nr_night_pct = 10;
/* WiFi max TX power in dBm (8..17). The autotune lowers it when the network stops answering
 * (main/tx_tune.c). TMM_TX_DEFAULT is a test-build knob (CMakeLists.txt). */
#ifndef TMM_TX_DEFAULT
#define TMM_TX_DEFAULT      17
#endif
static volatile int      s_tx_dbm = TMM_TX_DEFAULT;
/* telemetry: last STA disconnect reason since boot (0 = none), previous weather fetch time */
static volatile uint16_t s_last_disc_reason;
static volatile uint32_t s_last_fetch_ms;
/* TX autotune state (main/tx_tune.c). Calls run under s_tx_mux; actions after (tx_do). */
static tx_tune_t         s_tx = { .cur = TMM_TX_DEFAULT, .cap = TMM_TX_DEFAULT };
static portMUX_TYPE      s_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int      s_led_order, s_led_model, s_led_gpio = LED_GPIO_DEFAULT;
/* Night dimming: local time from SNTP + POSIX TZ string; NVS nd_start/nd_end (minutes), nd_bri, tz */
static volatile bool     s_nd_on;
static volatile int      s_nd_start, s_nd_end, s_nd_bri = 1;     /* ND_DEFAULT_BRI */
static bool              s_nd_default;      /* on only because nothing set it (no nd_start, no nd_off) */
#define ND_DEFAULT_START  (22 * 60)
#define ND_DEFAULT_END    (7 * 60)
#define ND_DEFAULT_TZ     "PST8PDT,M3.2.0,M11.1.0"
#define ND_DEFAULT_BRI    1                 /* 3 % */
static char              s_tz[48];
static volatile bool     s_night_now, s_night_override;
/* Category color overrides: 0 = use the server color, else 0x1000000 | rrggbb (NVS col_vfr..) */
static volatile uint32_t s_col[4];
/* LED test pattern (POST /pattern) */
enum { PAT_NONE, PAT_COLORS, PAT_ORDER, PAT_CATEGORIES, PAT_ONE };
static volatile int      s_pattern;
static volatile int      s_pattern_led;     /* PAT_ONE: 0-based LED */
static volatile int64_t  s_pattern_start_ms, s_pattern_until_ms;

// Debug flags
static SemaphoreHandle_t s_dbg_lock;
static uint16_t          s_dbg_flags;
static int64_t           s_dbg_set_ms[DBG_FLAG_COUNT];

// Web server
static httpd_handle_t    s_httpd;

// Recent warnings and errors (GET /log), RAM only
#define LOGRING_SIZE 6144
static char             *s_logring;
static size_t            s_logring_pos;     /* next write */
static bool              s_logring_wrapped;
static portMUX_TYPE      s_logring_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------- helpers ----------

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/** pdMS_TO_TICKS multiplies in 32-bit TickType_t: at CONFIG_FREERTOS_HZ=1000 anything over
 *  ~71.6 min wraps (6 h became 125 s, so OTA checked every ~2 min through 0.3.21). */
static TickType_t minutes_to_ticks(uint32_t min)
{
    return (TickType_t)(((uint64_t)min * 60000ULL * configTICK_RATE_HZ) / 1000ULL);
}

static void set_stage(stage_t s)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s > s_state.stage) s_state.stage = s;
    xSemaphoreGive(s_state_lock);
}

static void regress_stage(stage_t s)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s < s_state.stage) s_state.stage = s;
    xSemaphoreGive(s_state_lock);
}

static inline uint8_t clamp_add(uint8_t v, int delta)
{
    int x = (int)v + delta;
    if (x < 0)   return 0;
    if (x > 255) return 255;
    return (uint8_t)x;
}

// ---------- NVS config ----------

static void settings_load(void);
static void tx_power_load(void);

static void cfg_load(void)
{
    if (!s_cfg_mutex) s_cfg_mutex = xSemaphoreCreateMutex();
    s_nvs_ready           = false;
    s_wifi_ssid_from_nvs = false;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed (%s), using defaults (NVS saves disabled)",
                 NVS_NAMESPACE, esp_err_to_name(err));
        snprintf(s_cfg_ssid,    sizeof(s_cfg_ssid),    "%.31s", DEFAULT_SSID);
        snprintf(s_cfg_pass,    sizeof(s_cfg_pass),    "%s", DEFAULT_PASS);
        s_airports_text[0] = '\0';
        snprintf(s_cfg_url_pfx, sizeof(s_cfg_url_pfx), "%s", DEFAULT_URL_PFX);
        s_bri_idx = s_bri_saved_idx = DEFAULT_BRI_IDX;
        tx_power_load();
        return;
    }
    s_nvs_ready = true;

    size_t len;
    len = sizeof(s_cfg_ssid);
    if (nvs_get_str(s_nvs, "ssid", s_cfg_ssid, &len) == ESP_OK && s_cfg_ssid[0] != '\0')
        s_wifi_ssid_from_nvs = true;
    else
        snprintf(s_cfg_ssid, sizeof(s_cfg_ssid), "%.31s", DEFAULT_SSID);

    len = sizeof(s_cfg_pass);
    if (nvs_get_str(s_nvs, "pass", s_cfg_pass, &len) != ESP_OK)
        snprintf(s_cfg_pass, sizeof(s_cfg_pass), "%s", DEFAULT_PASS);

    len = sizeof(s_airports_text);
    if (nvs_get_str(s_nvs, "airports", s_airports_text, &len) != ESP_OK)
        s_airports_text[0] = '\0';
    char perr[64];
    if (!airports_parse(s_airports_text, s_slots, &s_slot_n, perr, sizeof(perr))) {
        ESP_LOGW(TAG, "saved airport list not valid (%s); starting with none", perr);
        s_slot_n = 0;
    }
    len = sizeof(s_crash);
    if (nvs_get_str(s_nvs, "crash", s_crash, &len) != ESP_OK)
        s_crash[0] = '\0';

    len = sizeof(s_cfg_url_pfx);
    if (nvs_get_str(s_nvs, "url_prefix", s_cfg_url_pfx, &len) != ESP_OK)
        snprintf(s_cfg_url_pfx, sizeof(s_cfg_url_pfx), "%s", DEFAULT_URL_PFX);

    uint8_t bri = DEFAULT_BRI_IDX;
    nvs_get_u8(s_nvs, "bri_idx", &bri);
    if (bri > 8) bri = DEFAULT_BRI_IDX;
    s_bri_idx = s_bri_saved_idx = bri;

    len = sizeof(s_cfg_ota_ch);
    if (nvs_get_str(s_nvs, "ota_ch", s_cfg_ota_ch, &len) != ESP_OK)
        s_cfg_ota_ch[0] = '\0';

    for (int i = 0; i < WIFI_SLOTS - 1; i++) {
        char k[8];
        snprintf(k, sizeof(k), "ssid%d", i + 1);
        len = sizeof(s_extra_ssid[i]);
        if (nvs_get_str(s_nvs, k, s_extra_ssid[i], &len) != ESP_OK)
            s_extra_ssid[i][0] = '\0';
        snprintf(k, sizeof(k), "pass%d", i + 1);
        len = sizeof(s_extra_pass[i]);
        if (nvs_get_str(s_nvs, k, s_extra_pass[i], &len) != ESP_OK)
            s_extra_pass[i][0] = '\0';
    }
    uint16_t v16;
    if (nvs_get_u16(s_nvs, "ota_int", &v16) == ESP_OK && v16 >= 15 && v16 <= 1440)
        s_ota_int_min = v16;
    if (nvs_get_u16(s_nvs, "cfg_int", &v16) == ESP_OK && v16 >= 5 && v16 <= 1440)
        s_cfg_int_min = v16;
    nvs_get_u32(s_nvs, "cfg_rev", &s_cfg_rev);
    len = sizeof(s_fw_base);
    if (nvs_get_str(s_nvs, "fw_base_url", s_fw_base, &len) != ESP_OK) s_fw_base[0] = '\0';
    len = sizeof(s_cfg_base);
    if (nvs_get_str(s_nvs, "cfg_base_url", s_cfg_base, &len) != ESP_OK) s_cfg_base[0] = '\0';
    settings_load();
    tx_power_load();
    s_fallback_ap_ms = FALLBACK_AP_MS;
    uint8_t th = 0;
    nvs_get_u8(s_nvs, "test_hooks", &th);
    s_test_hooks = th == 1;
    if (s_test_hooks && nvs_get_u16(s_nvs, "fb_sec", &v16) == ESP_OK && v16 >= 30 && v16 <= 3600)
        { s_fallback_ap_ms = (int64_t)v16 * 1000; s_fb_sec_test = true; }
    if (s_test_hooks)
        ESP_LOGW(TAG, "test hooks ON (/test/*), fallback setup AP after %d s", (int)(s_fallback_ap_ms / 1000));

    uint32_t boots = 0;
    nvs_get_u32(s_nvs, "boots", &boots);
    s_boot_count = boots + 1;
    if (nvs_set_u32(s_nvs, "boots", s_boot_count) != ESP_OK || nvs_commit(s_nvs) != ESP_OK)
        ESP_LOGW(TAG, "boot count save failed");
}

static void cfg_save_str(const char *key, const char *val)
{
    if (!s_nvs_ready) {
        ESP_LOGW(TAG, "cfg_save_str(%s): NVS not available", key);
        return;
    }
    esp_err_t e = nvs_set_str(s_nvs, key, val);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str(%s): %s", key, esp_err_to_name(e));
        return;
    }
    e = nvs_commit(s_nvs);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "nvs_commit after %s: %s", key, esp_err_to_name(e));
}

static void cfg_save_u8(const char *key, uint8_t val)
{
    if (!s_nvs_ready) {
        ESP_LOGW(TAG, "cfg_save_u8(%s): NVS not available", key);
        return;
    }
    esp_err_t e = nvs_set_u8(s_nvs, key, val);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_u8(%s): %s", key, esp_err_to_name(e));
        return;
    }
    e = nvs_commit(s_nvs);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "nvs_commit after %s: %s", key, esp_err_to_name(e));
}

static void cfg_save_u16(const char *key, uint16_t val)
{
    if (!s_nvs_ready) return;
    if (nvs_set_u16(s_nvs, key, val) != ESP_OK || nvs_commit(s_nvs) != ESP_OK)
        ESP_LOGW(TAG, "nvs save %s failed", key);
}

static void cfg_save_u32(const char *key, uint32_t val)
{
    if (!s_nvs_ready) return;
    if (nvs_set_u32(s_nvs, key, val) != ESP_OK || nvs_commit(s_nvs) != ESP_OK)
        ESP_LOGW(TAG, "nvs save %s failed", key);
}

static void cfg_erase(const char *key)
{
    if (!s_nvs_ready) return;
    esp_err_t e = nvs_erase_key(s_nvs, key);
    if ((e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(s_nvs) != ESP_OK)
        ESP_LOGW(TAG, "nvs erase %s failed", key);
}

// ---------- tunable settings ----------

enum { SET_INT, SET_BOOL, SET_ENUM };
/* Byte order on the wire; a trailing W = 4-channel (RGBW) LEDs, the white channel is left off. */
static const char *const k_led_order_opts[] = { "RGB", "GRB", "BRG", "BGR", "RBG", "GBR", "RGBW", "GRBW", NULL };
static const char *const k_led_model_opts[] = { "ws2812", "sk6812", "ws2811", NULL };

typedef struct {
    const char        *key;     /* config file key */
    const char        *nvs;     /* NVS key (u16) */
    uint8_t            kind;
    int                lo, hi, def;   /* SET_ENUM: index into opts */
    const char *const *opts;
    bool               reboot;  /* LED hardware: read once at boot */
    volatile int      *val;
} setting_t;

static const setting_t k_settings[] = {
    { "metar_poll_seconds",          "poll_sec",    SET_INT,  60, 3600,  300,  NULL, false, &s_poll_sec },
    { "wifi_fallback_minutes",       "fb_min",      SET_INT,  1,  120,   5,    NULL, false, &s_fb_min },
    { "metar_stale_minutes",         "stale_min",   SET_INT,  10, 1440,  60,   NULL, false, &s_stale_min },
    { "metar_stale_reboot_hours",    "stale_rb_hr", SET_INT,  1,  48,    12,   NULL, false, &s_stale_reboot_h },
    { "status_pixel",                "status_pix",  SET_BOOL, 0,  1,     1,    NULL, false, &s_status_pixel },
    { "verbose_telemetry",           "verbose_tel", SET_BOOL, 0,  1,     1,    NULL, false, &s_verbose_tel },
    { "no_report_day_percent",       "nr_day_pct",  SET_INT,  0,  100,   30,   NULL, false, &s_nr_day_pct },
    { "no_report_night_percent",     "nr_night_pct", SET_INT, 0,  100,   10,   NULL, false, &s_nr_night_pct },
    { "wifi_tx_power_dbm",           "tx_dbm",      SET_INT,  8,  17,    TMM_TX_DEFAULT, NULL, false, &s_tx_dbm },
    { "led_order",                   "led_order",   SET_ENUM, 0,  7,     0,    k_led_order_opts, true, &s_led_order },
    { "led_model",                   "led_model",   SET_ENUM, 0,  2,     0,    k_led_model_opts, true, &s_led_model },
    { "led_gpio",                    "led_gpio",    SET_INT,  0,  46,    LED_GPIO_DEFAULT, NULL, true, &s_led_gpio },
};
#define SETTINGS_N (sizeof(k_settings) / sizeof(k_settings[0]))

static const char *const k_cat_names[] = { "VFR", "MVFR", "IFR", "LIFR" };
static const char *const k_col_nvs[]   = { "col_vfr", "col_mvfr", "col_ifr", "col_lifr" };
/* metar-service colors (index.js catColor), for the test pattern */
static const uint8_t     k_cat_rgb[4][3] = { { 0, 127, 0 }, { 0, 0, 127 }, { 127, 0, 0 }, { 255, 22, 199 } };

static int cat_index(const char *name)
{
    for (int k = 0; k < 4; k++)
        if (strcasecmp(name, k_cat_names[k]) == 0) return k;
    return -1;
}

/** Stored value (NVS, else default); differs from *val for LED settings until the reboot. */
static int setting_stored(const setting_t *st)
{
    uint16_t v;
    if (s_nvs_ready && nvs_get_u16(s_nvs, st->nvs, &v) == ESP_OK && v >= st->lo && v <= st->hi)
        return v;
    return st->def;
}

static void setting_fmt(const setting_t *st, int v, char *buf, size_t sz)
{
    if (st->kind == SET_BOOL)      snprintf(buf, sz, "%s", v ? "true" : "false");
    else if (st->kind == SET_ENUM) snprintf(buf, sz, "%s", st->opts[v]);
    else                           snprintf(buf, sz, "%d", v);
}

/** After settings_load (needs s_tx_dbm); again whenever wifi_tx_power_dbm changes. */
static void tx_power_load(void)
{
    uint16_t v;
    uint8_t  t = 0;
    /* out of range (e.g. 20 from before 0.4.13) = unset: default level, autotune on */
    bool fixed = s_nvs_ready && nvs_get_u16(s_nvs, "tx_dbm", &v) == ESP_OK && v >= 8 && v <= 17;
    if (s_nvs_ready) nvs_get_u8(s_nvs, "tx_auto", &t);
#ifdef TMM_TX_TEST
    /* bench: step-up test 90 s after a level change, 3 min watch, 5 min after a failure */
    tx_tune_params_t p = TX_TUNE_DEFAULTS;
    p.up_every_ms = 90 * 1000; p.up_watch_ms = 3 * 60 * 1000; p.up_retry_fail_ms = 5 * 60 * 1000;
#else
    tx_tune_params_t p = TX_TUNE_DEFAULTS;
#endif
    taskENTER_CRITICAL(&s_tx_mux);
    tx_tune_init(&s_tx, &p, s_tx_dbm, fixed, t, now_ms());
    taskEXIT_CRITICAL(&s_tx_mux);
}

static void settings_load(void)
{
    for (size_t i = 0; i < SETTINGS_N; i++) *k_settings[i].val = setting_stored(&k_settings[i]);

    uint16_t a, b;
    uint8_t  bri;
    uint8_t off = 0;
    s_nd_on = s_nvs_ready && nvs_get_u16(s_nvs, "nd_start", &a) == ESP_OK &&
              nvs_get_u16(s_nvs, "nd_end", &b) == ESP_OK && a < 1440 && b < 1440 && a != b;
    if (s_nvs_ready) nvs_get_u8(s_nvs, "nd_off", &off);
    s_nd_default = !s_nd_on && off != 1;
    if (s_nd_on) { s_nd_start = a; s_nd_end = b; }
    else if (s_nd_default) { s_nd_on = true; s_nd_start = ND_DEFAULT_START; s_nd_end = ND_DEFAULT_END; }
    if (s_nvs_ready && nvs_get_u8(s_nvs, "nd_bri", &bri) == ESP_OK && bri <= 8) s_nd_bri = bri;
    size_t len = sizeof(s_tz);
    if (!s_nvs_ready || nvs_get_str(s_nvs, "tz", s_tz, &len) != ESP_OK)
        snprintf(s_tz, sizeof(s_tz), "%s", ND_DEFAULT_TZ);           /* not saved */
    setenv("TZ", s_tz, 1);
    tzset();

    for (int k = 0; k < 4; k++) {
        uint32_t c = 0;
        if (s_nvs_ready) nvs_get_u32(s_nvs, k_col_nvs[k], &c);
        s_col[k] = (c & 0x1000000u) ? c : 0;
    }
}

/** One setting from a config file: absent → *out = -2 (no change), null → -1 (back to the
 *  default), else the value. Returns false when the value is not valid. */
static bool setting_parse(const setting_t *st, const cJSON *j, int *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, st->key);
    *out = -2;
    if (!v) return true;
    if (cJSON_IsNull(v)) { *out = -1; return true; }
    if (st->kind == SET_BOOL) {
        if (!cJSON_IsBool(v)) return false;
        *out = cJSON_IsTrue(v) ? 1 : 0;
        return true;
    }
    if (st->kind == SET_ENUM) {
        if (!cJSON_IsString(v)) return false;
        for (int k = 0; st->opts[k]; k++)
            if (strcasecmp(v->valuestring, st->opts[k]) == 0) { *out = k; return true; }
        return false;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < st->lo || v->valuedouble > st->hi ||
        v->valuedouble != (double)(int)v->valuedouble)
        return false;
    *out = (int)v->valuedouble;
    return true;
}

/** Returns 1 when the value changed. LED settings only save (and ask for a reboot). */
static int setting_apply(const setting_t *st, int v, bool *need_reboot)
{
    int old = st->reboot ? setting_stored(st) : *st->val;
    int nv  = v == -1 ? st->def : v;
    if (v == -1) cfg_erase(st->nvs);
    else if (nv != old) cfg_save_u16(st->nvs, (uint16_t)nv);
    if (nv == old) return 0;
    if (st->reboot) *need_reboot = true;
    else            *st->val = nv;
    char a[16], b[16];
    setting_fmt(st, old, a, sizeof(a));
    setting_fmt(st, nv, b, sizeof(b));
    ESP_LOGI(TAG, "[cfg] %s %s -> %s%s", st->key, a, b, st->reboot ? " (after reboot)" : "");
    return 1;
}

static int parse_hhmm(const char *t)
{
    int h, m;
    char extra;
    if (sscanf(t, "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

typedef struct { int start, end, bri; char tz[48]; } night_cfg_t;

/** night_dim: absent → 0, false → 1 (off), valid object → 2, null → 3 (default), invalid → -1. */
static int night_parse(const cJSON *v, night_cfg_t *o)
{
    if (!v) return 0;
    if (cJSON_IsFalse(v)) return 1;
    if (cJSON_IsNull(v)) return 3;
    if (!cJSON_IsObject(v)) return -1;
    const cJSON *st = cJSON_GetObjectItem(v, "start"), *en = cJSON_GetObjectItem(v, "end");
    const cJSON *b  = cJSON_GetObjectItem(v, "brightness"), *tz = cJSON_GetObjectItem(v, "tz");
    if (!cJSON_IsString(st) || !cJSON_IsString(en)) return -1;
    o->start = parse_hhmm(st->valuestring);
    o->end   = parse_hhmm(en->valuestring);
    if (o->start < 0 || o->end < 0 || o->start == o->end) return -1;
    if (!cJSON_IsNumber(b) || b->valuedouble < 0 || b->valuedouble > 8 ||
        b->valuedouble != (double)(int)b->valuedouble)
        return -1;
    o->bri = (int)b->valuedouble;
    if (tz) {
        if (!cJSON_IsString(tz) || !tz->valuestring[0] || strlen(tz->valuestring) >= sizeof(o->tz) ||
            strchr(tz->valuestring, ' '))
            return -1;
        snprintf(o->tz, sizeof(o->tz), "%s", tz->valuestring);
    } else if (s_tz[0]) {
        snprintf(o->tz, sizeof(o->tz), "%s", s_tz);
    } else {
        return -1;                                   /* first time: tz is required */
    }
    return 2;
}

static int night_apply(int kind, const night_cfg_t *o)
{
    if (kind == 1) {
        if (!s_nd_on) return 0;
        s_nd_on = s_nd_default = false;
        cfg_erase("nd_start");
        cfg_save_u8("nd_off", 1);
        ESP_LOGI(TAG, "[cfg] night_dim off");
        return 1;
    }
    if (kind == 3) {                                 /* whole default: window, brightness, tz */
        if (s_nd_default && s_nd_bri == ND_DEFAULT_BRI && strcmp(s_tz, ND_DEFAULT_TZ) == 0) return 0;
        static const char *const keys[] = { "nd_start", "nd_end", "nd_off", "nd_bri", "tz" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) cfg_erase(keys[i]);
        s_nd_on = s_nd_default = true;
        s_nd_start = ND_DEFAULT_START;
        s_nd_end   = ND_DEFAULT_END;
        s_nd_bri   = ND_DEFAULT_BRI;
        snprintf(s_tz, sizeof(s_tz), "%s", ND_DEFAULT_TZ);
        setenv("TZ", s_tz, 1);
        tzset();
        ESP_LOGI(TAG, "[cfg] night_dim back to the default (%02d:00-%02d:00)",
                 ND_DEFAULT_START / 60, ND_DEFAULT_END / 60);
        return 1;
    }
    if (kind != 2) return 0;
    if (s_nd_on && !s_nd_default && s_nd_start == o->start && s_nd_end == o->end && s_nd_bri == o->bri &&
        strcmp(s_tz, o->tz) == 0)
        return 0;
    snprintf(s_tz, sizeof(s_tz), "%s", o->tz);
    setenv("TZ", s_tz, 1);
    tzset();
    s_nd_start = o->start;
    s_nd_end   = o->end;
    s_nd_bri   = o->bri;
    s_nd_on    = true;
    s_nd_default = false;
    cfg_erase("nd_off");
    cfg_save_str("tz", s_tz);
    cfg_save_u16("nd_start", (uint16_t)s_nd_start);
    cfg_save_u16("nd_end", (uint16_t)s_nd_end);
    cfg_save_u8("nd_bri", (uint8_t)s_nd_bri);
    ESP_LOGI(TAG, "[cfg] night_dim %02d:%02d-%02d:%02d brightness %d tz %s",
             s_nd_start / 60, s_nd_start % 60, s_nd_end / 60, s_nd_end % 60, s_nd_bri, s_tz);
    return 1;
}

/** colors: absent → nothing set; null/false → all back to the server colors; object of
 *  category → "rrggbb" or null. Returns false when invalid. */
static bool colors_parse(const cJSON *v, uint32_t col[4], bool set[4])
{
    for (int k = 0; k < 4; k++) { set[k] = false; col[k] = 0; }
    if (!v) return true;
    if (cJSON_IsNull(v) || cJSON_IsFalse(v)) {
        for (int k = 0; k < 4; k++) set[k] = true;
        return true;
    }
    if (!cJSON_IsObject(v)) return false;
    const cJSON *it;
    cJSON_ArrayForEach(it, v) {
        int k = cat_index(it->string);
        if (k < 0) return false;
        set[k] = true;
        if (cJSON_IsNull(it)) continue;
        if (!cJSON_IsString(it) || strlen(it->valuestring) != 6 ||
            strspn(it->valuestring, "0123456789abcdefABCDEF") != 6)
            return false;
        col[k] = 0x1000000u | (uint32_t)strtoul(it->valuestring, NULL, 16);
    }
    return true;
}

static int colors_apply(const uint32_t col[4], const bool set[4])
{
    int changes = 0;
    for (int k = 0; k < 4; k++) {
        if (!set[k] || col[k] == s_col[k]) continue;
        s_col[k] = col[k];
        if (col[k]) cfg_save_u32(k_col_nvs[k], col[k]);
        else        cfg_erase(k_col_nvs[k]);
        ESP_LOGI(TAG, "[cfg] color %s -> %s", k_cat_names[k], col[k] ? "override" : "server color");
        changes++;
    }
    return changes;
}

/** Category color override for one LED (before brightness). */
static void color_override(const char *cat, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int k = cat_index(cat);
    if (k < 0 || !s_col[k]) return;
    *r = (uint8_t)(s_col[k] >> 16);
    *g = (uint8_t)(s_col[k] >> 8);
    *b = (uint8_t)s_col[k];
}

/** Night window from the local clock (once a second from led_task). Clock not set = day. */
static void night_update(void)
{
    bool night = false;
    time_t t = time(NULL);
    if (s_nd_on && t > 1700000000) {
        struct tm tm;
        localtime_r(&t, &tm);
        int m = tm.tm_hour * 60 + tm.tm_min;
        night = s_nd_start < s_nd_end ? (m >= s_nd_start && m < s_nd_end)
                                      : (m >= s_nd_start || m < s_nd_end);
    }
    if (!night) s_night_override = false;
    s_night_now = night;
}

/** Brightness index in use: night dimming unless a tap cancelled it for this night. */
static uint8_t bri_effective(void)
{
    return (s_night_now && !s_night_override) ? (uint8_t)s_nd_bri : s_bri_idx;
}

static void sntp_start_once(void)
{
    static bool started;
    if (started || !s_nd_on) return;
    started = true;
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&c);
    ESP_LOGI(TAG, "SNTP started (night dimming)");
}

static void appendf(char *buf, size_t sz, size_t *off, const char *fmt, ...)
{
    if (*off + 1 >= sz) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, sz - *off, fmt, ap);
    va_end(ap);
    if (n > 0) *off = (*off + (size_t)n < sz) ? *off + (size_t)n : sz - 1;
}

/** No-report brightness rows (day, night) with -/+ steppers; no unlock needed (handler_nr_step).
 *  config: on the Configure page (data-keep so the save lock leaves the buttons alone). */
static void nr_rows(char *buf, size_t sz, size_t *off, bool config)
{
    static const char *const which[] = { "day", "night" };
    static const char *const label[] = { "No report, day", "No report, night" };
    int v[2] = { s_nr_day_pct, s_nr_night_pct };
    const char *keep = config ? " data-keep=1" : "";
    const char *back = config ? "<input type=hidden name=back value=config>" : "";
    for (int k = 0; k < 2; k++)
        appendf(buf, sz, off,
                "<tr><td>%s</td><td><div class=step>"
                "<form action='/nr_step' method='POST'><input type=hidden name=which value=%s>"
                "<input type=hidden name=dir value=down>%s<button%s%s aria-label='dimmer'>&minus;</button></form>"
                "<b>%d %%</b>"
                "<form action='/nr_step' method='POST'><input type=hidden name=which value=%s>"
                "<input type=hidden name=dir value=up>%s<button%s%s aria-label='brighter'>+</button></form>"
                "</div></td></tr>",
                label[k], which[k], back, v[k] <= 0 ? " disabled" : "", keep, v[k],
                which[k], back, v[k] >= 100 ? " disabled" : "", keep);
}

/** Status page: one table row per setting; values changed from the default in bold. */
static void settings_rows(char *buf, size_t sz, size_t *off)
{
    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_t *st = &k_settings[i];
        char v[16], nxt[32] = "";
        setting_fmt(st, *st->val, v, sizeof(v));
        if (st->reboot && setting_stored(st) != *st->val) {
            char w[16];
            setting_fmt(st, setting_stored(st), w, sizeof(w));
            snprintf(nxt, sizeof(nxt), " (%s after reboot)", w);
        }
        bool chg = setting_stored(st) != st->def;
        appendf(buf, sz, off, "<tr><td>%s</td><td>%s%s%s%s</td></tr>", st->key,
                chg ? "<b>" : "", v, chg ? "</b>" : "", nxt);
    }
    if (s_nd_on) {
        time_t t = time(NULL);
        appendf(buf, sz, off,
                "<tr><td>night_dim</td><td>%s%02d:%02d–%02d:%02d, brightness %d%s (tz %s; %s)</td></tr>",
                s_nd_default ? "" : "<b>", s_nd_start / 60, s_nd_start % 60, s_nd_end / 60, s_nd_end % 60,
                s_nd_bri, s_nd_default ? "" : "</b>",
                s_tz, t <= 1700000000 ? "clock not set yet" :
                s_night_now ? (s_night_override ? "night, cancelled by a tap" : "night now") : "day now");
    } else {
        appendf(buf, sz, off, "<tr><td>night_dim</td><td><b>off</b></td></tr>");
    }
    appendf(buf, sz, off, "<tr><td>colors</td><td>");
    bool any = false;
    for (int k = 0; k < 4; k++) {
        if (!s_col[k]) continue;
        unsigned c = (unsigned)(s_col[k] & 0xffffff);
        appendf(buf, sz, off, "%s<b>%s %06x</b> <span class=sw style='background:#%06x'></span>",
                any ? ", " : "", k_cat_names[k], c, sw_hex(c));
        any = true;
    }
    appendf(buf, sz, off, "%s</td></tr>", any ? "" : "server colors");
}

// ---------- debug flags ----------

static void dbg_set(uint16_t mask)
{
    xSemaphoreTake(s_dbg_lock, portMAX_DELAY);
    int64_t t = now_ms();
    for (int i = 0; i < DBG_FLAG_COUNT; i++) {
        if (mask & (1u << i)) {
            s_dbg_flags |= (uint16_t)(1u << i);
            s_dbg_set_ms[i] = t;
        }
    }
    xSemaphoreGive(s_dbg_lock);
}

/** Clear some flags now (a later success of the same kind). */
static void dbg_clear(uint16_t mask)
{
    xSemaphoreTake(s_dbg_lock, portMAX_DELAY);
    s_dbg_flags &= (uint16_t)~mask;
    for (int i = 0; i < DBG_FLAG_COUNT; i++)
        if (mask & (1u << i)) s_dbg_set_ms[i] = 0;
    xSemaphoreGive(s_dbg_lock);
}

static void dbg_clear_all(void)
{
    xSemaphoreTake(s_dbg_lock, portMAX_DELAY);
    s_dbg_flags = 0;
    memset(s_dbg_set_ms, 0, sizeof(s_dbg_set_ms));
    xSemaphoreGive(s_dbg_lock);
}

static void dbg_clear_expired(void)
{
    xSemaphoreTake(s_dbg_lock, portMAX_DELAY);
    int64_t t = now_ms();
    for (int i = 0; i < DBG_FLAG_COUNT; i++) {
        if ((s_dbg_flags & (1u << i)) && s_dbg_set_ms[i] &&
            (t - s_dbg_set_ms[i] > 24LL * 60 * 60 * 1000)) {
            s_dbg_flags &= (uint16_t)~(1u << i);
        }
    }
    xSemaphoreGive(s_dbg_lock);
}

static uint16_t dbg_get(void)
{
    xSemaphoreTake(s_dbg_lock, portMAX_DELAY);
    uint16_t f = s_dbg_flags;
    xSemaphoreGive(s_dbg_lock);
    return f;
}

/** Append &localip= when STA has a DHCP address (omit if AP-only / disconnected). */
static void metar_url_append_localip(char *url, size_t urlsz)
{
    if (!s_sta_netif || !url[0])
        return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0)
        return;
    size_t n = strlen(url);
    if (n + 24 >= urlsz)
        return;
    snprintf(url + n, urlsz - n, "&localip=" IPSTR, IP2STR(&ip.ip));
}

/** Append lowercase base36 of v to buf at *off. */
static void b36_append(char *buf, size_t sz, size_t *off, uint32_t v)
{
    char tmp[8];
    int  i = 0;
    do { tmp[i++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]; v /= 36; } while (v && i < 8);
    while (i > 0 && *off + 1 < sz) buf[(*off)++] = tmp[--i];
    buf[*off] = '\0';
}

/**
 * Compact telemetry, dot-separated base36, field 0 = format version (1):
 * 1.rssi(abs dBm, 0 = not associated).free_heap_kb.min_free_heap_kb.boots.uptime_min
 *   .reset_reason.wifi_drops.fetch_fails.cfg_rev.cfg_status.tx_dbm.tx_cap.tx_fixed.tx_downs.tx_ups
 *   .tx_up_fails.last_disc_reason.wifi_slot.fetch_ms.wifi_ch.leds.reports
 * leds = airport list length, reports = airports with a current category in the last reply
 * (docs/config-options.md describes each field).
 * cfg_status: 0 not checked, 1 no file, 2 current, 3 rejected, 4 fetch error.
 * Not in the CloudFront cache key; recorded via cs_uri_query in the access logs.
 */
static void telemetry_str(char *buf, size_t sz)
{
    wifi_ap_record_t ap;
    bool assoc = !s_ap_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    uint32_t rssi = (assoc && ap.rssi < 0) ? (uint32_t)(-ap.rssi) : 0;
    uint32_t f[] = {
        1, rssi,
        esp_get_free_heap_size() / 1024, esp_get_minimum_free_heap_size() / 1024,
        s_boot_count, (uint32_t)(now_ms() / 60000), (uint32_t)s_reset_reason,
        s_wifi_drops, s_fetch_fails, s_cfg_rev, s_cfg_status, (uint32_t)s_tx.cur,
        (uint32_t)s_tx.cap, s_tx.fixed ? 1u : 0u, (uint32_t)s_tx.downs, (uint32_t)s_tx.ups,
        (uint32_t)s_tx.up_fails,
        s_last_disc_reason, (uint32_t)s_wifi_slot, s_last_fetch_ms, assoc ? ap.primary : 0u,
        (uint32_t)s_slot_n, (uint32_t)s_state.n_reports,
    };
    size_t off = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (i && off + 1 < sz) { buf[off++] = '.'; buf[off] = '\0'; }
        b36_append(buf, sz, &off, f[i]);
    }
}

/** Weather URL for the whole list. Returns the number of airports in it (0 = nothing to ask). */
static int metar_url_build(char *url, size_t urlsz)
{
    char t[128];
    telemetry_str(t, sizeof(t));
    size_t off = (size_t)snprintf(url, urlsz, "%s", s_cfg_url_pfx);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int k = off < urlsz ? airport_ids(s_slots, s_slot_n, url + off, urlsz - off) : 0;
    xSemaphoreGive(s_state_lock);
    off = strlen(url);
    snprintf(url + off, urlsz - off, "&boardid=%s&output=csv&debug=0x%x&fw=%s%s%s%s%s",
             s_boardid, dbg_get(), FIRMWARE_VERSION,
             s_cfg_ota_ch[0] ? "&ch=" : "", s_cfg_ota_ch, s_verbose_tel ? "&t=" : "",
             s_verbose_tel ? t : "");
    if (s_verbose_tel) metar_url_append_localip(url, urlsz);
    return k;
}

// ---------- WiFi ----------

/**
 * ESP-IDF defaults structured STA config with threshold=WPA2 only; memset leaves
 * sae_pwe_h2e = 0 (UNSPECIFIED), which breaks many WPA3-SAE APs. WPA3 also expects
 * PMF capable. Use a permissive scan threshold so WPA2/WPA3/transitional BSSIDs match.
 */
static void wifi_sta_apply_security(wifi_sta_config_t *sta, bool has_password)
{
    sta->threshold.authmode = WIFI_AUTH_OPEN;
    if (has_password) {
        sta->pmf_cfg.capable  = true;
        sta->pmf_cfg.required = false;
        sta->sae_pwe_h2e      = WPA3_SAE_PWE_BOTH;
    }
}

static void wifi_fill_ap_config(wifi_config_t *ap_cfg)
{
    memset(ap_cfg, 0, sizeof(*ap_cfg));
    ap_cfg->ap.channel        = 6;
    ap_cfg->ap.max_connection = 4;
    ap_cfg->ap.authmode       = WIFI_AUTH_OPEN;
    snprintf((char *)ap_cfg->ap.ssid, sizeof(ap_cfg->ap.ssid),
             "metarmap-%.6s", s_boardid + 6);
    ap_cfg->ap.ssid_len = (uint8_t)strlen((char *)ap_cfg->ap.ssid);
}

/** DHCP option 12 + netif name; call again on each STA start so LWIP stays off CONFIG default ("espressif"). */
static esp_err_t wifi_apply_sta_dhcp_hostname(void)
{
    if (!s_sta_netif || !s_boardid[0])
        return ESP_ERR_INVALID_STATE;
    char dhcp_hostname[32];
    snprintf(dhcp_hostname, sizeof(dhcp_hostname), "metarmap-%.6s", s_boardid + 6);
    esp_err_t e = esp_netif_set_hostname(s_sta_netif, dhcp_hostname);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "esp_netif_set_hostname(%s): %s", dhcp_hostname, esp_err_to_name(e));
    else
        ESP_LOGD(TAG, "STA DHCP hostname: %s", dhcp_hostname);
    return e;
}

/** LWIP uses `netif->hostname` for DHCP option 12 when sending DISCOVER/REQUEST. This does
 *  not dump packets; for wire-level proof use a mirror port / AP packet capture. */
static void log_sta_dhcp_opt12(const char *when)
{
    if (!s_sta_netif)
        return;
    const char *hn = NULL;
    esp_err_t e = esp_netif_get_hostname(s_sta_netif, &hn);
    if (e != ESP_OK || hn == NULL)
        ESP_LOGW(TAG, "DHCP opt12 @ %s: get_hostname failed (%s)", when,
                 e != ESP_OK ? esp_err_to_name(e) : "null");
    else
        ESP_LOGI(TAG, "DHCP opt12 (client name) @ %s: \"%s\"", when, hn);
}

/** Lazy-create the AP netif; DHCP offers our IP as DNS (default) plus the option-114
 *  captive-portal URI so phones open the setup page on their own. */
static void ap_netif_ensure(void)
{
    if (s_ap_netif)
        return;
    s_ap_netif = esp_netif_create_default_wifi_ap();
    static char uri[] = "http://192.168.4.1/config";
    esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t e = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                         ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    if (e != ESP_OK)
        ESP_LOGW(TAG, "DHCP captive-portal URI: %s", esp_err_to_name(e));
    esp_netif_dhcps_start(s_ap_netif);
}

/** After esp_wifi_start(): cap TX power at s_tx_dbm (the API takes quarter dBm). */
static void wifi_apply_tx_power(void)
{
    esp_err_t e = esp_wifi_set_max_tx_power((int8_t)(s_tx.cur * 4));
    int8_t q = 0;
    esp_wifi_get_max_tx_power(&q);
    /* no %f: this also runs in the httpd task (cfg_apply_obj) */
    ESP_LOGI(TAG, "WiFi TX power %d dBm (cap %d%s%s%s; %s, driver %d/4 dBm)", s_tx.cur, s_tx.cap,
             s_tx.fixed ? ", set" : "", s_tx.learned ? ", learned" : "",
             s_tx.probe != TXP_IDLE ? ", step-up test" : "", esp_err_to_name(e), q);
}

/** After esp_wifi_init(). Safe whether or not STA was ever started. */
static void wifi_start_config_ap(void)
{
    ap_netif_ensure();
    wifi_config_t ap_cfg;
    wifi_fill_ap_config(&ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_apply_tx_power();
    ESP_LOGI(TAG, "AP started, SSID=%s, IP=192.168.4.1", (char *)ap_cfg.ap.ssid);
}

static const char *slot_ssid(int i) { return i == 0 ? s_cfg_ssid : s_extra_ssid[i - 1]; }
static const char *slot_pass(int i) { return i == 0 ? s_cfg_pass : s_extra_pass[i - 1]; }
static bool slot_used(int i) { return i == 0 ? s_wifi_ssid_from_nvs : s_extra_ssid[i - 1][0] != '\0'; }

static bool wifi_any_saved(void)
{
    for (int i = 0; i < WIFI_SLOTS; i++)
        if (slot_used(i)) return true;
    return false;
}

/** Next used slot after `from` (wrapping); `from` itself if it's the only one. */
static int wifi_next_slot(int from)
{
    for (int k = 1; k <= WIFI_SLOTS; k++) {
        int i = (from + k) % WIFI_SLOTS;
        if (slot_used(i)) return i;
    }
    return from;
}

static void wifi_sta_use_slot(int i)
{
    wifi_config_t c;
    memset(&c, 0, sizeof(c));
    /* ssid is a 32-byte field that may be full (no NUL); buffers are zeroed above */
    memcpy(c.sta.ssid, slot_ssid(i), strnlen(slot_ssid(i), sizeof(c.sta.ssid)));
    memcpy(c.sta.password, slot_pass(i), strnlen(slot_pass(i), sizeof(c.sta.password) - 1));
    wifi_sta_apply_security(&c.sta, c.sta.password[0] != '\0');
    esp_wifi_set_config(WIFI_IF_STA, &c);
    s_wifi_slot        = i;
    s_wifi_fail_streak = 0;
    ESP_LOGI(TAG, "WiFi: using saved network %d \"%s\"", i, slot_ssid(i));
}

/** Carry out tx_tune actions (outside s_tx_mux: these call WiFi and NVS). */
static void tx_do(int act, const char *why)
{
    if (!act || act == TXT_OURS) return;
    ESP_LOGW(TAG, "TX autotune (%s): now %d dBm, act 0x%x (down %d, up %d, failed up %d)%s%s%s",
             why, s_tx.cur, act, s_tx.downs, s_tx.ups, s_tx.up_fails,
             act & TXT_RECONNECT ? " step-up test: rejoining" : "", act & TXT_SAVE ? " saved" : "",
             act & TXT_ERASE ? " back at the cap" : "");
    if (act & TXT_APPLY) wifi_apply_tx_power();
    if (act & TXT_SAVE) cfg_save_u8("tx_auto", (uint8_t)s_tx.learned);
    if (act & TXT_ERASE) cfg_erase("tx_auto");
    if (act & TXT_RECONNECT) esp_wifi_disconnect();   /* the event handler reconnects */
}

/* Radio check running (POST /radio_check): a disconnect ends it, not counted as a drop. */
static volatile bool s_rc_active;
static volatile int  s_rc_drop_reason;

#define TX_CALL(expr) ({ taskENTER_CRITICAL(&s_tx_mux); int a_ = (expr); taskEXIT_CRITICAL(&s_tx_mux); a_; })

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "stage: STA started");
        set_stage(STAGE_WIFI_STARTED);
        wifi_apply_sta_dhcp_hostname();
        log_sta_dhcp_opt12("sta_start");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "stage: associated to AP");
        set_stage(STAGE_ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        /* In plain AP-only provisioning we intentionally ignore STA churn.
           During APSTA probe, STA must be allowed to disconnect/reauth/DHCP-retry. */
        if (!s_wifi_probe_active && s_wifi_down_since_ms == 0)
            s_wifi_down_since_ms = now_ms();
        s_last_disc_reason = disc->reason;
        if (s_ap_mode && !s_wifi_probe_active) {
            ESP_LOGI(TAG, "STA disconnected (reason=%u) — suppressed while AP-only",
                     (unsigned)disc->reason);
            dbg_set(DBG_WIFI_DISCONNECT);
            return;
        }
        if (s_rc_active) {
            s_rc_drop_reason = disc->reason;
            s_rc_active      = false;
            ESP_LOGW(TAG, "STA disconnected reason=%u rssi=%d [radio check]", (unsigned)disc->reason,
                     disc->rssi);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            regress_stage(STAGE_WIFI_STARTED);
            wifi_apply_tx_power();
            esp_wifi_connect();
            return;
        }
        int tx_act = TX_CALL(tx_tune_disconnect(&s_tx, disc->reason, disc->rssi, now_ms()));
        ESP_LOGW(TAG, "STA disconnected reason=%u rssi=%d%s%s", (unsigned)disc->reason, disc->rssi,
                 s_wifi_probe_active ? " [probe]" : "", tx_act & TXT_OURS ? " [TX step-up test]" : "");
        if (!s_wifi_probe_active && !(tx_act & TXT_OURS)) {
            dbg_set(DBG_WIFI_DISCONNECT);
            s_wifi_drops++;
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        regress_stage(STAGE_WIFI_STARTED);
        tx_do(tx_act, "disconnect");
        if (tx_act & (TXT_LOWERED | TXT_OURS)) s_wifi_fail_streak = 0;   /* same network, new level */
        else if (!s_wifi_probe_active && ++s_wifi_fail_streak >= WIFI_SLOT_FAILS) {
            int nx = wifi_next_slot(s_wifi_slot);
            if (nx != s_wifi_slot) wifi_sta_use_slot(nx);
            else s_wifi_fail_streak = 0;
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "stage: got ip " IPSTR, IP2STR(&evt->ip_info.ip));
        log_sta_dhcp_opt12("got_ip");
        set_stage(STAGE_GOT_IP);
        tx_do(TX_CALL(tx_tune_got_ip(&s_tx, now_ms())), "got IP");
        if (!s_wifi_probe_active) {
            s_wifi_down_since_ms = 0;
            s_wifi_fail_streak   = 0;
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        ESP_LOGI(TAG, "AP: client connected (%d total)", s_ap_clients);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        ESP_LOGI(TAG, "AP: client disconnected (%d total)", s_ap_clients);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Always create STA netif: AP-only (factory) boot still needs it for
     * wifi_probe_sta_credentials() — DHCP + IP_EVENT_STA_GOT_IP won't fire without it. */
    if (!s_sta_netif)
        s_sta_netif = esp_netif_create_default_wifi_sta();

    (void)wifi_apply_sta_dhcp_hostname();

    const bool need_config_ap = !wifi_any_saved();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    set_stage(STAGE_WIFI_INIT);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    if (need_config_ap) {
        ESP_LOGW(TAG, "no WiFi ssid in NVS — config AP only (first boot / unprovisioned)");
        s_ap_mode    = true;
        s_ap_clients = 0;
        wifi_start_config_ap();
        return;
    }

    s_wifi_down_since_ms = now_ms();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_sta_use_slot(slot_used(0) ? 0 : wifi_next_slot(0));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_apply_tx_power();
    /* USB powered: no modem sleep. With the default (min modem) OTA range downloads stalled
     * 5-40 s between 33 KB pieces on some runs (CDN answered each in ~1 ms). */
    esp_wifi_set_ps(WIFI_PS_NONE);
}

/** Saved WiFi unreachable for FALLBACK_AP_MS: open the setup AP alongside STA (APSTA).
 *  STA retries are paced by watchdog_task so scans don't keep kicking phones off the AP. */
static void fallback_ap_start(void)
{
    ESP_LOGW(TAG, "no WiFi for %d s — opening setup AP, still retrying \"%s\"",
             (int)(s_fallback_ap_ms / 1000), s_cfg_ssid);
    ap_netif_ensure();
    wifi_config_t ap_cfg;
    wifi_fill_ap_config(&ap_cfg);
    s_ap_clients  = 0;
    s_fallback_ap = true;
    s_ap_mode     = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static void fallback_ap_stop(void)
{
    ESP_LOGI(TAG, "WiFi back — closing fallback setup AP");
    s_fallback_ap = false;
    s_ap_mode     = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
}

/** boardid: first 6 bytes of the eFuse optional unique ID (what the 0.4 firmware sent, so the
 *  CloudFront logs and config file names stay the same), else the WiFi STA MAC. */
static void load_boardid(void)
{
    uint8_t uid[16] = { 0 };
    bool ok = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 128) == ESP_OK;
    int nz = 0;
    for (int i = 0; i < 6; i++) nz |= uid[i];
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    const uint8_t *b = ok && nz ? uid : mac;
    snprintf(s_boardid, sizeof(s_boardid), "%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    ESP_LOGI(TAG, "boardid=%s (%s) mac=%02x:%02x:%02x:%02x:%02x:%02x fw=%s", s_boardid,
             b == uid ? "eFuse unique ID" : "MAC", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             FIRMWARE_VERSION);
}

// ---------- HTTP / METAR ----------

typedef struct {
    char *buf;
    int   len, cap;
    bool  overflow;
} resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int room = ctx->cap - 1 - ctx->len;
        int n    = evt->data_len < room ? evt->data_len : room;
        if (n < evt->data_len) ctx->overflow = true;
        if (n > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, n);
            ctx->len += n;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

/** Apply a whole reply to the LED slots. Call with s_state_lock held. Returns rows parsed. */
static int apply_reply(char *body)
{
    int reports = 0;
    int rows = airports_apply_reply(s_slots, s_slot_n, body, &reports);
    s_state.n_reports = reports;
    return rows;
}

static void ota_mark_valid_once(void);

static void fetch_once(void)
{
    char *url = malloc(URL_MAX);
    resp_ctx_t ctx = { .buf = malloc(RESP_MAX_BYTES), .cap = RESP_MAX_BYTES };
    if (!url || !ctx.buf) {
        ESP_LOGE(TAG, "fetch: out of memory");
        free(url); free(ctx.buf);
        return;
    }
    ctx.buf[0] = '\0';
    int n_air = metar_url_build(url, URL_MAX);
    if (n_air == 0) {
        ESP_LOGI(TAG, "no airports in the list; nothing to fetch");
        free(url); free(ctx.buf);
        return;
    }
    const char *qs = strchr(url, '?');
    ESP_LOGI(TAG, "METAR request (%d airports) %.200s%s", n_air, qs ? qs : url, strlen(qs ? qs : url) > 200 ? "..." : "");

    set_stage(STAGE_HTTP_REQ);
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
        .timeout_ms    = WEATHER_TIMEOUT_MS,
        .buffer_size_tx = 2048,
    };
    if (strncasecmp(url, "https://", 8) == 0)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    int64_t   t0  = now_ms();
    esp_err_t err = esp_http_client_perform(client);
    s_last_fetch_ms = (uint32_t)(now_ms() - t0);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(url);
    /* any HTTP answer (404/500/503 too) proves the radio link; only transport errors count */
    tx_do(TX_CALL(tx_tune_fetch(&s_tx, err == ESP_OK, now_ms())), "weather check");

    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) ESP_LOGE(TAG, "http perform failed: %s", esp_err_to_name(err));
        else               ESP_LOGE(TAG, "http status %d", status);
        dbg_set(err != ESP_OK ? DBG_HTTP_TIMEOUT : DBG_HTTP_NON_200);
        s_fetch_fails++;
        free(ctx.buf);
        return;
    }
    set_stage(STAGE_HTTP_OK);
    if (ctx.overflow) ESP_LOGW(TAG, "weather reply over %d bytes, rest ignored", RESP_MAX_BYTES);

    char *keep = strdup(ctx.buf);              /* strtok_r below cuts ctx.buf up */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int rows = apply_reply(ctx.buf);
    if (rows > 0) {
        s_state.has_data      = true;
        s_state.last_fetch_ms = now_ms();
        free(s_last_body);
        s_last_body = keep;
        keep = NULL;
    }
    int reports = s_state.n_reports;
    xSemaphoreGive(s_state_lock);
    free(keep);
    free(ctx.buf);

    if (rows == 0) {
        ESP_LOGW(TAG, "could not parse the weather reply");
        dbg_set(DBG_PARSE_ERROR);
        s_fetch_fails++;
        return;
    }
    s_fetch_count++;
    s_leds_dirty = true;
    dbg_clear(DBG_FETCH_FLAGS);
    ESP_LOGI(TAG, "weather: %d rows, %d with a current category, %u ms", rows, reports, (unsigned)s_last_fetch_ms);
    ota_mark_valid_once();
}

// ---------- LED render ----------

static uint8_t s_pixels[LED_MAX][3];      /* this frame, before brightness */
static uint8_t s_sent[LED_MAX][3];        /* last frame written to the strip */

/** LEDs to use for status displays (boot, setup): the list, or 8 when it's empty. */
static int vis_n(void) { return s_slot_n > 0 ? s_slot_n : 8; }

static void put(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i >= LED_MAX) return;
    s_pixels[i][0] = r; s_pixels[i][1] = g; s_pixels[i][2] = b;
}

static void clear_frame(void) { memset(s_pixels, 0, sizeof(s_pixels)); }

/** Write the frame to the strip (scaled by bri) if it changed or once a second (a glitched LED
 *  recovers). Every LED_MAX pixel is sent, so LEDs past the list are always off. */
static void show(float bri)
{
    static int64_t last_ms;
    uint8_t out[LED_MAX][3];
    for (int i = 0; i < LED_MAX; i++)
        for (int c = 0; c < 3; c++) out[i][c] = (uint8_t)(s_pixels[i][c] * bri + 0.5f);   /* round: keeps dim greys grey */
    if (!s_strip) return;
    if (!s_leds_dirty && memcmp(out, s_sent, sizeof(out)) == 0 && now_ms() - last_ms < 1000) return;
    for (int i = 0; i < LED_MAX; i++) {
        if (s_led_rgbw) led_strip_set_pixel_rgbw(s_strip, i, out[i][0], out[i][1], out[i][2], 0);
        else            led_strip_set_pixel(s_strip, i, out[i][0], out[i][1], out[i][2]);
    }
    led_strip_refresh(s_strip);
    memcpy(s_sent, out, sizeof(out));
    s_leds_dirty = false;
    last_ms = now_ms();
}

static void wheel(int hue_deg, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int pos = (hue_deg * 255) / 360;
    if (pos < 0) pos = 0;
    if (pos > 255) pos = 255;
    if (pos < 85) {
        *r = 255 - pos * 3; *g = pos * 3;       *b = 0;
    } else if (pos < 170) {
        pos -= 85;
        *r = 0;             *g = 255 - pos * 3; *b = pos * 3;
    } else {
        pos -= 170;
        *r = pos * 3;       *g = 0;             *b = 255 - pos * 3;
    }
}

static void render_rainbow(void)
{
    int n = vis_n();
    int hue = (int)(now_ms() / 25) % 360;
    for (int i = 0; i < n; i++) {
        uint8_t r, g, b;
        wheel((hue + i * (360 / n)) % 360, &r, &g, &b);
        put(i, r / 4, g / 4, b / 4);
    }
}

/** Why the unit can't join, as a dim color (tx_tune_led); false = nothing to say. */
static bool join_problem_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (tx_tune_led(&s_tx)) {
    case TXL_BAD_PASS:        *r = 35; *g = 0;  *b = 0; return true;   /* red */
    case TXL_NO_ANSWER:
    case TXL_NO_ANSWER_FLOOR: *r = 40; *g = 14; *b = 0; return true;   /* amber */
    default:                  return false;
    }
}

static void render_progress(stage_t stage)
{
    int lit = (int)stage;
    if (lit > vis_n()) lit = vis_n();
    uint8_t r = 30, g = 30, b = 30;
    if (join_problem_rgb(&r, &g, &b) && tx_tune_led(&s_tx) == TXL_NO_ANSWER_FLOOR &&
        (now_ms() % 1000) >= 500)
        r = g = b = 0;                                     /* blink: nothing left to try */
    for (int i = 0; i < lit; i++) put(i, r, g, b);
}

static void render_ap_mode(void)
{
    /* Manual setup: solid blue. Fallback (saved WiFi unreachable, still retrying): blink 1 Hz,
     * alternating with the join-problem color (amber/red) when there is one, else off. LED 1 is
     * red until a phone joins the setup network, then green. */
    uint8_t r = 0, g = 0, b = 30;
    if (s_fallback_ap && (now_ms() % 1000) >= 500) {
        b = 0;
        join_problem_rgb(&r, &g, &b);
    }
    for (int i = 1; i < vis_n(); i++) put(i, r, g, b);
    put(0, s_ap_clients > 0 ? 0 : 30, s_ap_clients > 0 ? 30 : 0, 0);
}

/** POST /pattern: colors = red, green, blue, white (1.5 s each; checks led_order); order = one
 *  white LED stepping from LED 1 (0.5 s each; checks wiring and the list); categories = VFR,
 *  MVFR, IFR, LIFR colors with any overrides (2 s each); one = a single LED white, the rest off
 *  (find an LED on the map). */
static void render_pattern(void)
{
    static const uint8_t prim[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };
    int64_t t = now_ms() - s_pattern_start_ms;
    int n = vis_n();
    if (s_pattern == PAT_ONE) {
        if ((t / 400) % 2 == 0) put(s_pattern_led, 255, 255, 255);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (s_pattern == PAT_COLORS) {
            const uint8_t *c = prim[(t / 1500) % 4];
            put(i, c[0], c[1], c[2]);
        } else if (s_pattern == PAT_ORDER) {
            uint8_t v = (i == (int)((t / 500) % n)) ? 255 : 0;
            put(i, v, v, v);
        } else {
            int k = (int)((t / 2000) % 4);
            uint8_t r = k_cat_rgb[k][0], g = k_cat_rgb[k][1], b = k_cat_rgb[k][2];
            color_override(k_cat_names[k], &r, &g, &b);
            put(i, r, g, b);
        }
    }
}

/** Weather: each LED its airport's color; dark LEDs off. local_stale: every airport LED shows
 *  the no-report color (our weather is too old to trust). No-report LEDs are scaled by
 *  no_report_day_percent / no_report_night_percent (night = during night dimming), so the
 *  reporting airports can stand out. Call with s_state_lock held. */
static void render_weather(bool local_stale)
{
    bool night = s_night_now && !s_night_override;
    int  nr_pct = night ? s_nr_night_pct : s_nr_day_pct;
    for (int i = 0; i < s_slot_n; i++) {
        const slot_t *s = &s_slots[i];
        if (!s->icao[0]) continue;
        uint8_t r = NO_REPORT_R, g = NO_REPORT_G, b = NO_REPORT_B;
        bool no_report = true;
        if (!local_stale && s->has) {
            r = s->r; g = s->g; b = s->b;
            color_override(s->cat, &r, &g, &b);
            no_report = s->stale_remote || !s->cat[0];
        } else if (!local_stale) {
            r = g = b = 0;                    /* not in the reply (yet) */
        }
        if (no_report && nr_pct < 100) {
            r = (uint8_t)((r * nr_pct + 50) / 100);
            g = (uint8_t)((g * nr_pct + 50) / 100);
            b = (uint8_t)((b * nr_pct + 50) / 100);
        }
        put(i, r, g, b);
    }
}

/* Onboard status NeoPixel: dim fixed colors, not scaled by the map brightness. */
static void status_pixel_update(bool online, bool local_stale)
{
    if (!s_status_pix) return;
    int64_t t = now_ms();
    float   ph = (sinf(2.0f * 3.14159265f * (float)(t % 4000) / 4000.0f) + 1.0f) / 2.0f;   /* 0..1 */
    bool    blink = (t % 1000) < 500;
    uint8_t r = 0, g = 0, b = 0;
    if (t < s_btn_ack_until_ms) {
        b = 60;                                                  /* button press seen */
    } else if (s_ap_mode) {
        if (!s_fallback_ap || blink) b = 30;                     /* setup network */
    } else if (now_ms() < s_unlock_until_ms) {
        g = 25; b = 25;                                          /* saving unlocked */
    } else if (!s_status_pixel || s_pattern != PAT_NONE) {
        /* off */
    } else if (!online) {
        if (!join_problem_rgb(&r, &g, &b)) { r = 40; g = 14; }
        if (!blink) r = g = b = 0;                               /* connecting / no WiFi */
    } else if (local_stale) {
        r = (uint8_t)(4 + 36 * ph); g = (uint8_t)(1 + 12 * ph);  /* weather is old */
    } else {
        g = (uint8_t)(2 + 18 * ph);                              /* all good: slow green breath */
    }
    led_strip_set_pixel(s_status_pix, 0, r, g, b);
    led_strip_refresh(s_status_pix);
}

static void led_task(void *arg)
{
    int64_t night_ms = 0;
    while (1) {
        if (now_ms() - night_ms >= 1000) { night_ms = now_ms(); night_update(); }
        if (s_pattern != PAT_NONE && now_ms() >= s_pattern_until_ms) s_pattern = PAT_NONE;

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        state_t snap = s_state;
        bool local_stale = snap.has_data && (now_ms() - snap.last_fetch_ms > LOCAL_STALE_MS);
        clear_frame();
        bool weather = false;
        if (s_pattern != PAT_NONE) {
            render_pattern();
        } else if (s_ap_mode) {
            render_ap_mode();
        } else if (snap.has_data) {
            render_weather(local_stale);
            weather = true;
        } else if (snap.stage == STAGE_BOOT) {
            render_rainbow();
        } else {
            render_progress(snap.stage);
        }
        xSemaphoreGive(s_state_lock);

        /* Status displays (setup network, progress, rainbow, join-problem colors) are dim by design
         * and skip brightness; the test pattern is at least 25 % so it can be judged. */
        float bri = 1.0f;
        if (s_pattern != PAT_NONE) {
            bri = k_bri_levels[bri_effective()];
            if (bri < 0.25f) bri = 0.25f;
        } else if (weather) {
            bri = k_bri_levels[bri_effective()];
        }
        show(bri);
        bool online = (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
        status_pixel_update(online, local_stale);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void poll_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    while (1) {
        sntp_start_once();
        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) fetch_once();
        /* 1 s steps so a new metar_poll_seconds takes effect right away; a saved airport list
         * wakes this early (xTaskNotifyGive) */
        int64_t t0 = now_ms();
        while (now_ms() - t0 < (int64_t)POLL_PERIOD_MS)
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) break;
    }
}

// ---------- OLED (optional) ----------

static void oled_task(void *arg)
{
    while (1) {
        char l1[40], l2[40], l3[40];   /* the panel shows 21 characters */
        bool online = (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
        esp_netif_ip_info_t ip = { 0 };
        if (s_ap_mode)        snprintf(l1, sizeof(l1), "setup metarmap-%.6s", s_boardid + 6);
        else if (online && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK)
                              snprintf(l1, sizeof(l1), IPSTR, IP2STR(&ip.ip));
        else                  snprintf(l1, sizeof(l1), "connecting WiFi");
        snprintf(l2, sizeof(l2), "v%s %d/%d wx", FIRMWARE_VERSION, s_state.n_reports, s_slot_n);
        int64_t ago = s_state.has_data ? (now_ms() - s_state.last_fetch_ms) / 60000 : -1;
        if (now_ms() < s_unlock_until_ms) snprintf(l3, sizeof(l3), "unlocked %d min",
                                                   (int)((s_unlock_until_ms - now_ms()) / 60000) + 1);
        else if (ago < 0)                 snprintf(l3, sizeof(l3), "no weather yet");
        else                              snprintf(l3, sizeof(l3), "wx %lld min ago f=%x", (long long)ago, dbg_get());
        oled_show_three_lines(l1, l2, l3);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------- OTA ----------

/** Rollback: a freshly OTA'd image boots PENDING_VERIFY; confirm it after the first good
 *  METAR fetch. If it crashes or reboots first, the bootloader returns to the old image. */
static void ota_mark_valid_once(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "[OTA] first fetch OK — image marked valid");
    }
}

/** The bootloader marks an image that never confirmed itself invalid and boots the old one.
 *  Flag that once per rolled-back version (NVS rb_seen), not on every later boot. */
static void ota_check_rolled_back(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (!bad || esp_ota_get_partition_description(bad, &d) != ESP_OK || !s_nvs_ready) return;
    char seen[32] = "";
    size_t len = sizeof(seen);
    nvs_get_str(s_nvs, "rb_seen", seen, &len);
    if (strcmp(seen, d.version) == 0) return;
    ESP_LOGW(TAG, "[OTA] %s failed to start; bootloader rolled back to this image", d.version);
    dbg_set(DBG_OTA_ROLLED_BACK);
    if (nvs_set_str(s_nvs, "rb_seen", d.version) != ESP_OK || nvs_commit(s_nvs) != ESP_OK)
        ESP_LOGW(TAG, "nvs save rb_seen failed");
}

typedef struct { char *buf; size_t sz; } etag_out_t;

static esp_err_t etag_http_event(esp_http_client_event_t *evt)
{
    etag_out_t *o = (etag_out_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && o && strcasecmp(evt->header_key, "ETag") == 0)
        snprintf(o->buf, o->sz, "%s", evt->header_value);
    return ESP_OK;
}

/** HEAD url; ETag header (or "") into etag. Returns the HTTP status, or -1 if the request failed. */
static int http_head_etag(const char *url, char *etag, size_t sz)
{
    etag_out_t o = { etag, sz };
    etag[0] = '\0';
    esp_http_client_config_t hc = {
        .url               = url,
        .method            = HTTP_METHOD_HEAD,
        .event_handler     = etag_http_event,
        .user_data         = &o,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    if (!cl) return -1;
    esp_err_t err = esp_http_client_perform(cl);
    int status = err == ESP_OK ? esp_http_client_get_status_code(cl) : -1;
    esp_http_client_cleanup(cl);
    return status;
}

static void ota_url(char *buf, size_t sz)
{
    snprintf(buf, sz, "%s/%s.bin", s_fw_base[0] ? s_fw_base : FIRMWARE_BASE_URL,
             strcmp(s_cfg_ota_ch, "beta") == 0 ? "beta" : "stable");
}

static void ota_check(void)
{
    char url[160];
    ota_url(url, sizeof(url));
    ESP_LOGI(TAG, "[OTA] checking %s", url);

    /* Same file as the last check (ETag) → same answer; skip the 33 KB version read. */
    char etag[ETAG_MAX];
    int hs = http_head_etag(url, etag, sizeof(etag));
    bool have_etag = hs == 200 && etag[0];
    if (have_etag && strcmp(url, s_ota_etag_url) == 0 && strcmp(etag, s_ota_etag) == 0) {
        ESP_LOGI(TAG, "[OTA] unchanged (ETag %s), skipping", etag);
        snprintf(s_ota_result, sizeof(s_ota_result), "up to date (file unchanged)");
        dbg_clear(DBG_OTA_FLAGS);
        return;
    }
    if (hs == 403 || hs == 404) {                /* S3 via CloudFront: missing key -> 403 */
        ESP_LOGI(TAG, "[OTA] no firmware file on this channel (HTTP %d)", hs);
        snprintf(s_ota_result, sizeof(s_ota_result), "no firmware file on this channel (HTTP %d)", hs);
        return;
    }

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    /* Range requests: a same-version check fetches one 32 KB range instead of streaming the
     * whole image until abort (~720 KB per check seen in the CDN logs). */
    esp_https_ota_config_t ota_cfg = {
        .http_config           = &http_cfg,
        .partial_http_download = true,
        .max_http_request_size = 32 * 1024,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[OTA] begin failed: %s", esp_err_to_name(err));
        snprintf(s_ota_result, sizeof(s_ota_result), "could not start: %s", esp_err_to_name(err));
        dbg_set(DBG_OTA_FAILED | DBG_OTA_DOWNLOAD);
        return;
    }

    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(ota_handle, &new_desc);
    if (err == ESP_OK) {
        const esp_app_desc_t *running = esp_app_get_description();
        ESP_LOGI(TAG, "[OTA] running=%s server=%s", running->version, new_desc.version);
        if (strcmp(running->version, new_desc.version) == 0) {
            ESP_LOGI(TAG, "[OTA] same version, skipping");
            snprintf(s_ota_result, sizeof(s_ota_result), "up to date (%s)", new_desc.version);
            dbg_clear(DBG_OTA_FLAGS);
            if (have_etag) {
                snprintf(s_ota_etag, sizeof(s_ota_etag), "%s", etag);
                snprintf(s_ota_etag_url, sizeof(s_ota_etag_url), "%s", url);
            }
            esp_https_ota_abort(ota_handle);
            return;
        }
        /* Don't re-flash a version that already failed verification and was rolled back
         * (would loop: download, crash, roll back, download...). A new version clears this. */
        const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
        esp_app_desc_t bad_desc;
        if (bad && esp_ota_get_partition_description(bad, &bad_desc) == ESP_OK &&
            strcmp(bad_desc.version, new_desc.version) == 0) {
            ESP_LOGW(TAG, "[OTA] %s was rolled back earlier, skipping", new_desc.version);
            snprintf(s_ota_result, sizeof(s_ota_result), "%s failed to start earlier, skipped", new_desc.version);
            if (have_etag) {
                snprintf(s_ota_etag, sizeof(s_ota_etag), "%s", etag);
                snprintf(s_ota_etag_url, sizeof(s_ota_etag_url), "%s", url);
            }
            esp_https_ota_abort(ota_handle);
            return;
        }
    } else {
        ESP_LOGW(TAG, "[OTA] get_img_desc failed: %s", esp_err_to_name(err));
    }

    while (true) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    }

    bool handle_freed = false;
    if (err == ESP_OK) {
        /* esp_https_ota_finish() frees the handle on success AND failure: no abort after it
         * (0.3.31 double-freed here when a signature check failed, and crashed). */
        err = esp_https_ota_finish(ota_handle);
        handle_freed = true;
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[OTA] success, rebooting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }

    ESP_LOGE(TAG, "[OTA] failed: %s", esp_err_to_name(err));
    snprintf(s_ota_result, sizeof(s_ota_result), "%s: %s",
             err == ESP_ERR_OTA_VALIDATE_FAILED ? "rejected (not signed with our key, or corrupt)" : "failed",
             esp_err_to_name(err));
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        /* Not signed with our key (or corrupt): don't download the same file again every check. */
        ESP_LOGE(TAG, "[OTA] image rejected: failed verification (signature)");
        if (have_etag) {
            snprintf(s_ota_etag, sizeof(s_ota_etag), "%s", etag);
            snprintf(s_ota_etag_url, sizeof(s_ota_etag_url), "%s", url);
        }
    }
    if (!handle_freed) esp_https_ota_abort(ota_handle);
    dbg_set(DBG_OTA_FAILED |
            (err == ESP_ERR_OTA_VALIDATE_FAILED ? DBG_OTA_BAD_IMAGE : DBG_OTA_DOWNLOAD));
}

static void ota_task(void *arg)
{
    /* Here, not in app_main: reading the rolled-back image verifies its RSA signature, which
     * overflowed app_main's 3.5 KB stack and crashed every boot after a rollback (0.3.34). */
    ota_check_rolled_back();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    /* first check 2 min after WiFi; "ota_now" from the config pusher can cut this short too */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(120000));
    while (1) {
        ota_check();
        /* config pusher can wake this early ("ota_now") */
        ulTaskNotifyTake(pdTRUE, minutes_to_ticks(s_ota_int_min));
    }
}

// ---------- config pusher ----------

typedef struct {
    char buf[CFG_MAX_BYTES + 1];
    int  len;
    bool overflow;
    char etag[ETAG_MAX];
} cfg_ctx_t;

static esp_err_t cfg_http_event(esp_http_client_event_t *evt)
{
    cfg_ctx_t *c = (cfg_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && c && strcasecmp(evt->header_key, "ETag") == 0)
        snprintf(c->etag, sizeof(c->etag), "%s", evt->header_value);
    if (evt->event_id == HTTP_EVENT_ON_DATA && c) {
        if (c->len + evt->data_len > CFG_MAX_BYTES) {
            c->overflow = true;
        } else {
            memcpy(c->buf + c->len, evt->data, evt->data_len);
            c->len += evt->data_len;
            c->buf[c->len] = '\0';
        }
    }
    return ESP_OK;
}

/** Config URL: <config base>/<boardid>.json */
static void cfg_url_build(char *url, size_t sz)
{
    snprintf(url, sz, "%s/%s.json", s_cfg_base[0] ? s_cfg_base : CONFIG_BASE_URL, s_boardid);
}

/** https:// URL that fits `max` bytes (incl. NUL); "" allowed (= reset to default). */
static bool cfg_valid_https(const cJSON *v, size_t max)
{
    if (!cJSON_IsString(v)) return false;
    const char *u = v->valuestring;
    return !u[0] || (strncasecmp(u, "https://", 8) == 0 && strlen(u) < max && !strchr(u, ' '));
}

/* Keys reserved for planned features: accepted and logged, not applied yet. Anything not in
 * this list or the implemented set is ignored (logged once per file) so newer config files
 * never break older firmware. */
static const char *const k_cfg_planned[] = { NULL };   /* none right now */
static const char *const k_cfg_known[] = {
    "rev", "airports", "brightness", "ota_channel", "metar_url_prefix",
    "ota_interval_minutes", "config_interval_minutes",
    "wifi_add", "wifi_remove", "clear_flags", "ota_now", "reboot",
    "force", "wifi_primary", "firmware_base_url", "config_base_url",
    "night_dim", "colors", NULL,     /* plus every k_settings key */
};

/** Log planned/unknown keys; returns how many were planned-but-not-implemented. */
static int cfg_note_extra_keys(const cJSON *j)
{
    int planned = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, j) {
        bool known = false;
        for (size_t i = 0; !known && k_cfg_known[i]; i++)
            known = strcmp(it->string, k_cfg_known[i]) == 0;
        for (size_t i = 0; !known && i < SETTINGS_N; i++)
            known = strcmp(it->string, k_settings[i].key) == 0;
        if (known) continue;
        bool plan = false;
        for (size_t i = 0; !plan && k_cfg_planned[i]; i++)
            plan = strcmp(it->string, k_cfg_planned[i]) == 0;
        if (plan) planned++;
        ESP_LOGI(TAG, "[cfg] key \"%s\" %s", it->string,
                 plan ? "recognized, not implemented in this firmware" : "unknown, ignored");
    }
    return planned;
}

/** "airports": an array of strings (one per LED) or one string (newlines/commas/spaces).
 *  Returns the list as text for airports_parse, or NULL when the type is wrong. Caller frees. */
static char *cfg_airports_text(const cJSON *v)
{
    if (cJSON_IsString(v)) return strdup(v->valuestring);
    if (!cJSON_IsArray(v)) return NULL;
    size_t cap = (size_t)cJSON_GetArraySize(v) * 16 + 1, off = 0;
    char *t = calloc(1, cap);
    if (!t) return NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, v) {
        if (!cJSON_IsString(it) || strlen(it->valuestring) > 12) { free(t); return NULL; }
        off += (size_t)snprintf(t + off, cap - off, "%s%s", off ? "\n" : "", it->valuestring);
    }
    return t;
}

/** Replace the airport list (already parsed): LEDs, NVS, and an early weather check. */
static void airports_set(const slot_t *slots, int n)
{
    char *text = malloc(AIRPORTS_MAX_TEXT);
    if (!text) return;
    airports_format(slots, n, text, AIRPORTS_MAX_TEXT);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(s_slots, slots, sizeof(slot_t) * (size_t)n);
    s_slot_n = n;
    s_state.n_reports = 0;
    snprintf(s_airports_text, sizeof(s_airports_text), "%s", text);
    xSemaphoreGive(s_state_lock);
    cfg_save_str("airports", text);
    free(text);
    s_leds_dirty = true;
    if (s_poll_task) xTaskNotifyGive(s_poll_task);
    ESP_LOGI(TAG, "[cfg] airport list now %d LEDs", n);
}

static bool cfg_valid_wifi(const cJSON *net)
{
    const cJSON *ss = cJSON_GetObjectItem(net, "ssid");
    const cJSON *pw = cJSON_GetObjectItem(net, "pass");
    if (!cJSON_IsString(ss) || !ss->valuestring[0] || strlen(ss->valuestring) > 32) return false;
    if (pw && !cJSON_IsString(pw)) return false;
    size_t pl = pw ? strlen(pw->valuestring) : 0;
    return pl == 0 || (pl >= 8 && pl <= 63);
}

/** Optional integer field: absent → *out = -1; present must be a number within [lo, hi]. */
static bool cfg_int_field(const cJSON *j, const char *key, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    *out = -1;
    if (!v) return true;
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) return false;
    *out = v->valueint;
    return true;
}

static void wifi_extra_save(void)
{
    for (int i = 0; i < WIFI_SLOTS - 1; i++) {
        char k[8];
        snprintf(k, sizeof(k), "ssid%d", i + 1);
        cfg_save_str(k, s_extra_ssid[i]);
        snprintf(k, sizeof(k), "pass%d", i + 1);
        cfg_save_str(k, s_extra_pass[i]);
    }
}

/** Add-only WiFi semantics: never touches slot 0 or the network in use. */
static int cfg_apply_wifi(const cJSON *add, const cJSON *remove)
{
    int changes = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, remove) {
        for (int i = 1; i < WIFI_SLOTS; i++) {
            if (i != s_wifi_slot && slot_used(i) && strcmp(s_extra_ssid[i - 1], it->valuestring) == 0) {
                s_extra_ssid[i - 1][0] = s_extra_pass[i - 1][0] = '\0';
                changes++;
            }
        }
    }
    cJSON_ArrayForEach(it, add) {
        const char *ss = cJSON_GetObjectItem(it, "ssid")->valuestring;
        const cJSON *pw = cJSON_GetObjectItem(it, "pass");
        const char *pp = pw ? pw->valuestring : "";
        int found = -1;
        for (int i = 0; i < WIFI_SLOTS; i++)
            if (slot_used(i) && strcmp(slot_ssid(i), ss) == 0) found = i;
        if (found == 0 || found == s_wifi_slot) {
            ESP_LOGW(TAG, "[cfg] wifi_add \"%s\": is slot 0 or in use, left alone", ss);
            continue;
        }
        int dst = found;
        for (int i = 1; dst < 0 && i < WIFI_SLOTS; i++)
            if (!slot_used(i)) dst = i;
        for (int i = WIFI_SLOTS - 1; dst < 0 && i >= 1; i--)   /* full: replace one not in use */
            if (i != s_wifi_slot) dst = i;
        if (dst < 1) continue;
        snprintf(s_extra_ssid[dst - 1], sizeof(s_extra_ssid[0]), "%s", ss);
        snprintf(s_extra_pass[dst - 1], sizeof(s_extra_pass[0]), "%s", pp);
        changes++;
    }
    if (changes) wifi_extra_save();
    return changes;
}

/* Setup-page saves run the config file's checks too (cfg_apply_obj), but don't touch the
 * config rev, the remote-config status or the CFG_REJECTED flag. */
static bool s_apply_local;
static char s_local_err[80];
static int  s_local_changes;

static void cfg_reject(uint32_t rev, const char *why)
{
    if (s_apply_local) {
        snprintf(s_local_err, sizeof(s_local_err), "%s", why);
        ESP_LOGW(TAG, "[setup page] rejected: %s", why);
        return;
    }
    s_cfg_status = CFG_ST_REJECTED;
    snprintf(s_cfg_result, sizeof(s_cfg_result), "rejected rev %lu: %s", (unsigned long)rev, why);
    ESP_LOGW(TAG, "[cfg] %s", s_cfg_result);
    dbg_set(DBG_CFG_REJECTED);
}

static bool cfg_apply_text(const char *text);

static void cfg_check(void)
{
    char url[200];
    cfg_url_build(url, sizeof(url));

    /* Same file as last time (ETag) → nothing to do; status stays what that file gave. */
    if (s_cfg_etag[0]) {
        char etag[ETAG_MAX];
        int hs = http_head_etag(url, etag, sizeof(etag));
        if (hs == 200 && strcmp(etag, s_cfg_etag) == 0) {
            s_cfg_last_ms = now_ms();
            ESP_LOGI(TAG, "[cfg] unchanged (ETag %s)", etag);
            return;
        }
    }

    cfg_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;

    esp_http_client_config_t hc = {
        .url               = url,
        .event_handler     = cfg_http_event,
        .user_data         = ctx,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    esp_err_t err = esp_http_client_perform(cl);
    int status    = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    s_cfg_last_ms = now_ms();

    bool reboot = false;
    if (err != ESP_OK) {
        snprintf(s_cfg_result, sizeof(s_cfg_result), "fetch failed: %s", esp_err_to_name(err));
        s_cfg_status = CFG_ST_FETCH_ERROR;
        dbg_set(DBG_CFG_FETCH_FAIL);
        goto out;
    }
    if (status == 403 || status == 404) {       /* S3 via CloudFront: missing key → 403 */
        snprintf(s_cfg_result, sizeof(s_cfg_result), "no config file (HTTP %d)", status);
        dbg_clear(DBG_CFG_FETCH_FAIL);
        s_cfg_status = CFG_ST_NO_FILE;
        s_cfg_etag[0] = '\0';
        goto out;
    }
    if (status != 200) {
        snprintf(s_cfg_result, sizeof(s_cfg_result), "HTTP %d", status);
        s_cfg_status = CFG_ST_FETCH_ERROR;
        dbg_set(DBG_CFG_FETCH_FAIL);
        goto out;
    }
    dbg_clear(DBG_CFG_FETCH_FAIL);
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    reboot = cfg_apply_text(ctx->overflow ? NULL : ctx->buf);
    xSemaphoreGive(s_cfg_mutex);
    /* applied, already applied or rejected: the same file gives the same answer next time */
    snprintf(s_cfg_etag, sizeof(s_cfg_etag), "%s", ctx->etag);
out:
    free(ctx);
    if (reboot) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

/** Validate, then apply one config file (from the config URL or POST /test/config).
 *  text NULL = file over 4 KB. Call with s_cfg_mutex held. Returns true when the file asks for
 *  a reboot; the caller restarts (after replying, for the test hook). */
static bool cfg_apply_obj(const cJSON *j, uint32_t rev);

static bool cfg_apply_text(const char *text)
{
    bool reboot = false;
    cJSON *j = NULL;
    if (!text) { cfg_reject(0, "file over 4 KB"); goto out; }
    j = cJSON_Parse(text);
    if (!j) { cfg_reject(0, "not valid JSON"); goto out; }

    const cJSON *jrev = cJSON_GetObjectItem(j, "rev");
    if (!cJSON_IsNumber(jrev) || jrev->valuedouble < 1) { cfg_reject(0, "missing rev"); goto out; }
    uint32_t rev = (uint32_t)jrev->valuedouble;
    if (rev <= s_cfg_rev) {
        snprintf(s_cfg_result, sizeof(s_cfg_result), "rev %lu already applied", (unsigned long)s_cfg_rev);
        s_cfg_status = CFG_ST_CURRENT;
        goto out;
    }

    reboot = cfg_apply_obj(j, rev);
out:
    cJSON_Delete(j);
    return reboot;
}

/** Validate, then apply one config object (file, test hook, or setup page when
 *  s_apply_local). Returns true when it asks for a reboot. */
static bool cfg_apply_obj(const cJSON *j, uint32_t rev)
{
    bool reboot = false;
    /* ---- validate everything before changing anything ---- */
    const cJSON *airports = cJSON_GetObjectItem(j, "airports");
    slot_t *new_slots = NULL;
    int new_n = 0;
    const cJSON *ota_ch  = cJSON_GetObjectItem(j, "ota_channel");
    const cJSON *pfx     = cJSON_GetObjectItem(j, "metar_url_prefix");
    const cJSON *wadd    = cJSON_GetObjectItem(j, "wifi_add");
    const cJSON *wrem    = cJSON_GetObjectItem(j, "wifi_remove");
    int bri, ota_int, cfg_int;
    if (airports) {
        char *txt = cfg_airports_text(airports), perr[64] = "";
        new_slots = calloc(LED_MAX, sizeof(slot_t));
        bool ok = txt && new_slots && airports_parse(txt, new_slots, &new_n, perr, sizeof(perr));
        free(txt);
        if (!ok) {
            char why[96];
            snprintf(why, sizeof(why), "airports%s%s", perr[0] ? ": " : "", perr);
            cfg_reject(rev, why);
            goto out;
        }
    }
    if (ota_ch && (!cJSON_IsString(ota_ch) ||
                   (strcmp(ota_ch->valuestring, "stable") && strcmp(ota_ch->valuestring, "beta") &&
                    ota_ch->valuestring[0])))
        { cfg_reject(rev, "ota_channel"); goto out; }
    if (pfx && (!cJSON_IsString(pfx) || strlen(pfx->valuestring) >= sizeof(s_cfg_url_pfx) ||
                (strncasecmp(pfx->valuestring, "http://", 7) && strncasecmp(pfx->valuestring, "https://", 8))))
        { cfg_reject(rev, "metar_url_prefix"); goto out; }
    if (!cfg_int_field(j, "brightness", 0, 8, &bri))      { cfg_reject(rev, "brightness"); goto out; }
    if (!cfg_int_field(j, "ota_interval_minutes", 15, 1440, &ota_int))
        { cfg_reject(rev, "ota_interval_minutes"); goto out; }
    if (!cfg_int_field(j, "config_interval_minutes", 5, 1440, &cfg_int))
        { cfg_reject(rev, "config_interval_minutes"); goto out; }
    int setv[SETTINGS_N];
    for (size_t i = 0; i < SETTINGS_N; i++)
        if (!setting_parse(&k_settings[i], j, &setv[i])) { cfg_reject(rev, k_settings[i].key); goto out; }
    night_cfg_t nd;
    int nd_kind = night_parse(cJSON_GetObjectItem(j, "night_dim"), &nd);
    if (nd_kind < 0) { cfg_reject(rev, "night_dim"); goto out; }
    uint32_t cols[4];
    bool     cols_set[4];
    if (!colors_parse(cJSON_GetObjectItem(j, "colors"), cols, cols_set)) { cfg_reject(rev, "colors"); goto out; }
    if (wadd) {
        if (!cJSON_IsArray(wadd) || cJSON_GetArraySize(wadd) > WIFI_SLOTS - 1) { cfg_reject(rev, "wifi_add"); goto out; }
        const cJSON *n;
        cJSON_ArrayForEach(n, wadd) if (!cfg_valid_wifi(n)) { cfg_reject(rev, "wifi_add entry"); goto out; }
    }
    if (wrem) {
        if (!cJSON_IsArray(wrem)) { cfg_reject(rev, "wifi_remove"); goto out; }
        const cJSON *n;
        cJSON_ArrayForEach(n, wrem) if (!cJSON_IsString(n)) { cfg_reject(rev, "wifi_remove entry"); goto out; }
    }

    /* forced replacements: need top-level "force": true, otherwise the whole file is rejected */
    const cJSON *wprim = cJSON_GetObjectItem(j, "wifi_primary");
    const cJSON *fwb   = cJSON_GetObjectItem(j, "firmware_base_url");
    const cJSON *curl  = cJSON_GetObjectItem(j, "config_base_url");
    bool force = cJSON_IsTrue(cJSON_GetObjectItem(j, "force"));
    if ((wprim || fwb || curl) && !force)               { cfg_reject(rev, "needs \"force\": true"); goto out; }
    if (wprim && !cfg_valid_wifi(wprim))                { cfg_reject(rev, "wifi_primary"); goto out; }
    if (fwb && !cfg_valid_https(fwb, sizeof(s_fw_base)))   { cfg_reject(rev, "firmware_base_url"); goto out; }
    if (curl && !cfg_valid_https(curl, sizeof(s_cfg_base))) { cfg_reject(rev, "config_base_url"); goto out; }

    /* ---- apply ---- */
    int planned = cfg_note_extra_keys(j);
    int changes = cfg_apply_wifi(wadd, wrem);
    if (wprim) {
        const char *ss = cJSON_GetObjectItem(wprim, "ssid")->valuestring;
        const cJSON *pw = cJSON_GetObjectItem(wprim, "pass");
        /* keep the old primary as a backup in an extra slot (not the one in use) */
        if (s_wifi_ssid_from_nvs && strcmp(s_cfg_ssid, ss) != 0) {
            int dst = -1;
            for (int i = 1; dst < 0 && i < WIFI_SLOTS; i++) if (!slot_used(i)) dst = i;
            for (int i = WIFI_SLOTS - 1; dst < 0 && i >= 1; i--) if (i != s_wifi_slot) dst = i;
            if (dst >= 1) {
                snprintf(s_extra_ssid[dst - 1], sizeof(s_extra_ssid[0]), "%s", s_cfg_ssid);
                snprintf(s_extra_pass[dst - 1], sizeof(s_extra_pass[0]), "%s", s_cfg_pass);
                wifi_extra_save();
            }
        }
        snprintf(s_cfg_ssid, sizeof(s_cfg_ssid), "%.31s", ss);
        snprintf(s_cfg_pass, sizeof(s_cfg_pass), "%s", pw ? pw->valuestring : "");
        cfg_save_str("ssid", s_cfg_ssid);
        cfg_save_str("pass", s_cfg_pass);
        s_wifi_ssid_from_nvs = true;
        ESP_LOGW(TAG, "[cfg] forced: primary WiFi now \"%s\" (used on next reconnect/reboot)", s_cfg_ssid);
        changes++;
    }
    if (fwb) {
        snprintf(s_fw_base, sizeof(s_fw_base), "%s", fwb->valuestring);
        size_t L = strlen(s_fw_base);
        while (L && s_fw_base[L - 1] == '/') s_fw_base[--L] = '\0';
        cfg_save_str("fw_base_url", s_fw_base);
        ESP_LOGW(TAG, "[cfg] forced: firmware base %s", s_fw_base[0] ? s_fw_base : FIRMWARE_BASE_URL);
        changes++;
    }
    if (curl) {
        snprintf(s_cfg_base, sizeof(s_cfg_base), "%s", curl->valuestring);
        size_t L = strlen(s_cfg_base);
        while (L && s_cfg_base[L - 1] == '/') s_cfg_base[--L] = '\0';
        cfg_save_str("cfg_base_url", s_cfg_base);
        ESP_LOGW(TAG, "[cfg] forced: config base %s", s_cfg_base[0] ? s_cfg_base : CONFIG_BASE_URL);
        changes++;
    }
    if (airports) { airports_set(new_slots, new_n); changes++; }
    if (ota_ch) {
        snprintf(s_cfg_ota_ch, sizeof(s_cfg_ota_ch), "%s",
                 strcmp(ota_ch->valuestring, "beta") == 0 ? "beta" : "");
        cfg_save_str("ota_ch", s_cfg_ota_ch); changes++;
    }
    if (pfx) {
        snprintf(s_cfg_url_pfx, sizeof(s_cfg_url_pfx), "%s", pfx->valuestring);
        cfg_save_str("url_prefix", s_cfg_url_pfx); changes++;
    }
    if (bri >= 0) {
        s_bri_idx = s_bri_saved_idx = (uint8_t)bri;
        cfg_save_u8("bri_idx", (uint8_t)bri); changes++;
    }
    if (ota_int > 0) { s_ota_int_min = (uint16_t)ota_int; cfg_save_u16("ota_int", s_ota_int_min); changes++; }
    if (cfg_int > 0) { s_cfg_int_min = (uint16_t)cfg_int; cfg_save_u16("cfg_int", s_cfg_int_min); changes++; }
    bool led_reboot = false;
    int tx_before = s_tx.cur;
    for (size_t i = 0; i < SETTINGS_N; i++)
        if (setv[i] != -2) {
            changes += setting_apply(&k_settings[i], setv[i], &led_reboot);
            if (k_settings[i].val == &s_tx_dbm) cfg_erase("tx_auto");   /* set or null: relearn */
        }
    tx_power_load();
    if (s_tx.cur != tx_before) wifi_apply_tx_power();
    changes += night_apply(nd_kind, &nd);
    changes += colors_apply(cols, cols_set);
    if (!s_fb_sec_test) s_fallback_ap_ms = FALLBACK_AP_MS;

    if (s_apply_local) {
        s_local_changes = changes;
        ESP_LOGI(TAG, "[setup page] saved (%d change%s)", changes, changes == 1 ? "" : "s");
    } else {
        s_cfg_rev = rev;
        cfg_save_u32("cfg_rev", s_cfg_rev);
        s_cfg_status = CFG_ST_CURRENT;
        snprintf(s_cfg_result, sizeof(s_cfg_result), "applied rev %lu (%d change%s%s)",
                 (unsigned long)rev, changes, changes == 1 ? "" : "s",
                 planned ? ", some keys need newer firmware" : "");
        ESP_LOGI(TAG, "[cfg] %s", s_cfg_result);
    }

    /* ---- one-shot actions, after settings are saved ---- */
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "clear_flags"))) dbg_clear_all();
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "ota_now")) && s_ota_task) xTaskNotifyGive(s_ota_task);
    reboot = cJSON_IsTrue(cJSON_GetObjectItem(j, "reboot")) || led_reboot;
    if (led_reboot) ESP_LOGI(TAG, "[cfg] LED settings changed: rebooting to apply");
out:
    free(new_slots);
    return reboot;
}

static void cfg_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));   /* let the first METAR fetch go first */
    while (1) {
        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)
            cfg_check();
        /* POST /test/check what=config wakes this early */
        ulTaskNotifyTake(pdTRUE, minutes_to_ticks(s_cfg_int_min));
    }
}

// ---------- web server ----------

static void url_decode(char *dst, const char *src, size_t dstsz)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di < dstsz - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

static bool get_param(const char *body, const char *key, char *val, size_t valsz)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[256];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(val, raw, valsz);
    return true;
}

static int read_body(httpd_req_t *req, char *buf, size_t bufsz)
{
    int rem = (int)req->content_len;
    if (rem > (int)(bufsz - 1)) rem = (int)(bufsz - 1);
    int off = 0;
    while (rem > 0) {
        int n = httpd_req_recv(req, buf + off, rem);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        off += n;
        rem -= n;
    }
    buf[off] = '\0';
    return off;
}

/** While captive AP is up: try STA credentials (DHCP), then restore AP-only unless probe succeeds. */
static bool wifi_probe_sta_credentials(const char *ssid, const char *pass)
{
    wifi_config_t restore_cfg;
    memset(&restore_cfg, 0, sizeof(restore_cfg));
    snprintf((char *)restore_cfg.sta.ssid, sizeof(restore_cfg.sta.ssid), "%s", s_cfg_ssid);
    snprintf((char *)restore_cfg.sta.password, sizeof(restore_cfg.sta.password), "%s", s_cfg_pass);
    wifi_sta_apply_security(&restore_cfg.sta, restore_cfg.sta.password[0] != '\0');

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    snprintf((char *)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), "%.31s", ssid);
    snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", pass ? pass : "");
    wifi_sta_apply_security(&wcfg.sta, pass && pass[0]);

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    s_wifi_probe_active = true;

    /* STA config is rejected in WIFI_MODE_AP (ESP_ERR_WIFI_MODE); enable APSTA first. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi probe: APSTA failed: %s", esp_err_to_name(err));
        s_wifi_probe_active = false;
        return false;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi probe: set_config failed: %s", esp_err_to_name(err));
        s_wifi_probe_active = false;
        esp_wifi_set_mode(WIFI_MODE_AP);
        return false;
    }

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_PROBE_TIMEOUT_MS));

    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;

    s_wifi_probe_active = false;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(400));

    if (!ok) {
        ESP_LOGW(TAG, "WiFi probe: timeout or could not get IP");
        esp_wifi_set_config(WIFI_IF_STA, &restore_cfg);
        esp_wifi_set_mode(WIFI_MODE_AP);
        return false;
    }

    ESP_LOGI(TAG, "WiFi probe: OK for \"%s\"", ssid);
    /* Drop STA before returning so poll_task (WIFI_CONNECTED_BIT) stays blocked until reboot. */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(400));
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_set_mode(WIFI_MODE_AP);
    return true;
}

/** Plain-language color: green, red, IFR blue, etc. */
/** Plain-language strip color. Uses the server's flight category when present (server colors:
 *  VFR green 0-127-0, MVFR blue 0-0-127, IFR red 127-0-0, LIFR magenta 255-22-199,
 *  no report dim warm white 75-75-50); otherwise describes the RGB. */
/** Plain-language color for one airport. Uses the server's flight category when present (server
 *  colors: VFR green 0-127-0, MVFR blue 0-0-127, IFR red 127-0-0, LIFR magenta 255-22-199,
 *  no report dim warm white 75-75-50). */
static const char *cat_plain(const char *cat)
{
    static const struct { const char *cat, *name; } k[] = {
        { "VFR", "green (VFR)" }, { "MVFR", "blue (MVFR)" },
        { "IFR", "red (IFR)" },   { "LIFR", "magenta (LIFR)" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (cat && strcmp(cat, k[i].cat) == 0) return k[i].name;
    return "dim warm white (no current report)";
}

/** What the LEDs show right now, matching led_task's order. */
static void root_led_text(char *out, size_t outsz, bool has_data, stage_t stage, bool local_stale)
{
    if (s_pattern != PAT_NONE) {
        snprintf(out, outsz, "LED test pattern running for %lld more s.",
                 (long long)((s_pattern_until_ms - now_ms() + 999) / 1000));
        return;
    }
    if (s_ap_mode) {
        if (s_fallback_ap)
            snprintf(out, outsz,
                     "Fallback setup: saved Wi‑Fi unreachable for %d+ min, so the setup network "
                     "is open while it keeps retrying (about once a minute when no phone is "
                     "connected). The map LEDs <b>blink</b> blue.", (int)(s_fallback_ap_ms / 60000));
        else
            snprintf(out, outsz, "Setup mode: the map LEDs are solid blue.");
        size_t len = strlen(out);
        snprintf(out + len, outsz - len,
                 " LED 1 is <b>red</b> when no phone is connected to the setup network, "
                 "<b>green</b> when at least one is.");
        return;
    }
    if (!has_data) {
        if (s_slot_n == 0)
            snprintf(out, outsz, "No airport list yet: set one on the <a href='/config'>Configure</a> page.");
        else if (stage == STAGE_BOOT)
            snprintf(out, outsz, "Startup rainbow.");
        else
            snprintf(out, outsz, "Connecting (%d of %d steps): the first LEDs light dim white, one per step; "
                     "amber or red means the WiFi network doesn't answer or rejected the password.",
                     (int)stage, (int)STAGE_HTTP_OK);
        return;
    }
    if (local_stale) {
        snprintf(out, outsz, "<b>Weather is old</b> (no good weather check for %d+ min), so every airport "
                 "shows dim warm white until the next good check.", s_stale_min);
        return;
    }
    snprintf(out, outsz, "Each airport LED shows its flight category: green VFR, blue MVFR, red IFR, "
             "magenta LIFR; dim warm white = no current report. OFF entries stay dark.");
}

/** Transfer-Encoding: chunked; ~HTTP_HTML_CHUNK bytes per chunk (page is mostly ASCII). */
static esp_err_t http_send_html_chunked(httpd_req_t *req, const char *html)
{
    if (!html)
        return ESP_ERR_INVALID_ARG;
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    for (size_t off = 0; html[off];) {
        size_t n = 0;
        while (html[off + n] && n < HTTP_HTML_CHUNK)
            n++;
        esp_err_t r = httpd_resp_send_chunk(req, html + off, n);
        if (r != ESP_OK)
            return r;
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Page styles (all pages): cards, two-column tables, forms, phone width, dark mode. */
static const char k_root_css[] =
    "<style>"
    ":root{--bg:#f3f3f1;--card:#fff;--line:#ddd;--dim:#666;--fg:#1d1d1d;--link:#0b5fc2;--subbg:#f4f4f1;--subline:#c8c8c4}"
    "@media(prefers-color-scheme:dark){:root{--bg:#171717;--card:#242424;--line:#383838;--dim:#9a9a9a;--fg:#e4e4e4;--link:#7ab4ff;--subbg:#2f2f2f;--subline:#505050}}"
    "*{box-sizing:border-box}a{color:var(--link)}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 -apple-system,system-ui,sans-serif}"
    "main{max-width:44em;margin:0 auto;padding:10px 16px 24px}"
    "h1{font-size:1.35em;margin:.3em 0 0}"
    ".sub{color:var(--dim);font-size:.88em;margin-bottom:6px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 14px;margin:12px 0}"
    "h2{font-size:.8em;margin:0 0 6px;color:var(--dim);text-transform:uppercase;letter-spacing:.06em}"
    ".big{font-size:1.25em;margin:2px 0 6px}"
    ".sw{display:inline-block;width:.95em;height:.95em;border-radius:3px;vertical-align:-.12em;border:1px solid #8888}"
    ".big .sw{width:1.3em;height:1.3em;vertical-align:-.25em}"
    "table{border-collapse:collapse;width:100%}"
    "td{padding:3px 8px 3px 0;vertical-align:top;border-top:1px solid var(--line)}"
    "tr:first-child td{border-top:0}"
    "td{overflow-wrap:anywhere}td:first-child{color:var(--dim);width:34%}"
    ".set td:first-child{width:auto;overflow-wrap:normal;font:.82em/1.9 ui-monospace,Menlo,monospace}"
    "code{font-size:.82em;word-break:break-all}"
    ".step{display:flex;align-items:center;gap:12px}.step b{min-width:3em;text-align:center}.step button{min-width:2.6em}"
    "form{display:inline}button{font:inherit;font-size:.9em;padding:3px 10px;margin:2px 4px 2px 0}"
    "summary{cursor:pointer;color:var(--dim)}"
    "nav a{margin-right:14px}"
    "label.f{display:block;margin:6px 0 2px;color:var(--dim);font-size:.9em}"
    "input,select{font:inherit;padding:5px 7px;width:100%;max-width:26em;border:1px solid var(--line);"
    "border-radius:6px;background:var(--bg);color:var(--fg)}"
    "input[type=checkbox],input[type=radio]{width:auto;margin:3px 6px 0 0}"
    "form.blk{display:block}form.blk button{margin-top:8px}"
    "button:disabled{opacity:.6;cursor:not-allowed}"
    ".sv{background:#1f8a3b;color:#fff;border:2px solid #1f8a3b;border-radius:5px}"
    ".sv:disabled{background:var(--subbg);color:var(--dim);border-color:#c33;opacity:1}"
    ".lk{border:2px solid #c33;background:#c3333318}.ok{border:2px solid #2a2;background:#22aa2218}"
    ".danger{border:2px solid #c80;background:#ffb30014}.danger h2{color:#c60}"
    ".on{color:#d33;font-weight:bold}"
    ".big2{font-size:2em;font-weight:bold;margin:4px 0}"
    "h3{font-size:.95em;margin:14px 0 2px}"
    ".row{display:flex;gap:10px;flex-wrap:wrap;align-items:flex-end}.row>div{flex:1 1 9em;min-width:0}"
    ".row input,.row select{max-width:none}"
    ".check,.radio{display:flex;align-items:flex-start;gap:2px;margin:8px 0 2px}"
    ".check small,.radio small{display:block;color:var(--dim)}"
    ".note{font-size:.85em;color:var(--dim)}"
    ".lh{display:none;color:#c33;font-size:.85em;margin-left:4px}button:disabled:hover+.lh{display:inline}.net,.sb{border:1px solid var(--subline);background:var(--subbg);border-radius:8px;padding:4px 12px 10px;margin:10px 0}.sb h3{margin-top:4px}"
    "fieldset{border:0;padding:0;margin:0}fieldset:disabled{opacity:.5}"
    "details>summary{font-weight:600;letter-spacing:.06em;text-transform:uppercase;font-size:.8em;cursor:pointer}"
    "details[open]>summary{margin-bottom:6px}"
    ".group>summary{color:var(--dim)}"
    ".danger>summary{color:#c60}"
    ".xdanger{border:2px solid #c33;background:#c3333318;border-radius:10px;padding:8px 12px;margin-top:14px}"
    ".xdanger>summary{color:#d33}"
    ".confirm{margin-top:8px;padding:8px 10px;border:1px solid #c33;border-radius:8px;background:#c3333318}"
    "[hidden]{display:none!important}"
    ".mask{-webkit-text-security:disc}"
    "</style>";


static void page_head(char *h, size_t cap, size_t *o, const char *title, const char *extra_head)
{
    appendf(h, cap, o,
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>%s</title>%s%s</head><body><main><h1>%s</h1>"
            "<div class=sub>METAR map metarmap-%.6s · v" FIRMWARE_VERSION "</div>",
            title, k_root_css, extra_head ? extra_head : "", title, s_boardid + 6);
}

static const char k_page_nav[] =
    "<nav><a href='/'>Status</a><a href='/airports'>Airports</a><a href='/config'>Configure</a>"
    "<a href='/debug'>Debug</a><a href='/log'>Log</a></nav>"
    "</main></body></html>";

/** Short reply (saved / error) in the shared style. status NULL = 200. */
static void send_page(httpd_req_t *req, const char *status, const char *title, const char *body_html)
{
    size_t cap = sizeof(k_root_css) + strlen(body_html) + 768, o = 0;
    char *h = (char *)malloc(cap);
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!h) { httpd_resp_sendstr(req, body_html); return; }
    h[0] = '\0';
    page_head(h, cap, &o, title, NULL);
    appendf(h, cap, &o, "<div class=card>%s</div>%s", body_html, k_page_nav);
    http_send_html_chunked(req, h);
    free(h);
}

/** GET /debug — human-readable list of every debug bit and ON/off state. */
static esp_err_t handler_debug(httpd_req_t *req)
{
    static const struct {
        uint8_t      bit;
        const char *id;
        const char *blurb;
    } rows[] = {
        { 0, "HTTP_TIMEOUT",
          "METAR fetch: esp_http_client_perform failed (timeout or transport error)." },
        { 1, "HTTP_NON_200", "METAR fetch: HTTP status not 200." },
        { 2, "PARSE_ERROR", "METAR response body: CSV parse failed." },
        { 3, "WIFI_DISCONNECT", "STA disconnected from AP (see Wi-Fi reason in logs)." },
        { 4, "OTA_FAILED", "Firmware update failed (any reason; see 10, 11)." },
        { 5, "WDT_FIRED",
          "Last boot: task WDT, RTC WDT, or panic (esp_reset_reason); not the stale-weather restart." },
        { 6, "HTTPD_START_FAIL", "httpd_start() failed — local web UI unavailable." },
        { 7, "HTTPD_DEAD", "httpd_queue_work failed — server task may be unhealthy." },
        { 9, "CFG_REJECTED", "Config pusher file rejected (see status page Config line)." },
        { 10, "OTA_BAD_IMAGE", "Update downloaded but failed verification (not signed with our key, or corrupt)." },
        { 11, "OTA_DOWNLOAD", "Update could not be downloaded or written (network, HTTP or flash error)." },
        { 12, "OTA_ROLLED_BACK", "A new update failed to start; the bootloader went back to this version." },
        { 13, "CFG_FETCH_FAIL", "Config file could not be fetched (network or HTTP error)." },
        { 8, "STALE_FETCH_REBOOT",
          "Deliberate restart: no good weather check for metar_stale_reboot_hours (default 12)." },
        { 14, "LED_INIT_FAIL", "The LED string driver did not start (check led_gpio, led_order, led_model)." },
    };

    uint16_t     f   = dbg_get();
    const size_t cap = 8192;
    char        *h   = (char *)malloc(cap);
    if (!h) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "OOM");
        return ESP_OK;
    }
    size_t o = 0;
    h[0] = '\0';
    page_head(h, cap, &o, "Debug flags", NULL);
    appendf(h, cap, &o,
            "<div class=card><div class=big2>0x%x</div>"
            "<p style='margin:2px 0'>%s Weather, update and config flags clear at the next success of the "
            "same kind; the rest 24 h after they were last set, or with Clear. "
            "Sent to the server as <code>debug=</code> on every weather check.</p>"
            "<form action='/clear_flags' method='POST'><button>Clear all flags</button></form></div>"
            "<div class=card><table>",
            (unsigned)f, f ? "Flags that are on are marked in red." : "No flags are on.");
    for (int bit = 0; bit < DBG_FLAG_COUNT; bit++)
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            if (rows[i].bit != bit) continue;
            bool on = f & (1u << bit);
            appendf(h, cap, &o, "<tr><td><code>0x%x</code><br>%s</td><td>%s<br><small>%s</small></td></tr>",
                    1u << bit, on ? "<span class=on>ON</span>" : "off", rows[i].id, rows[i].blurb);
        }
    appendf(h, cap, &o, "</table></div><div class=card><h2>Last crash</h2><p style='margin:2px 0'>%s</p>"
            "<small>Saved from the crash dump at the next boot. Decode the addresses with the ELF of that "
            "version: <code>xtensa-esp32s2-elf-addr2line -pfiaC -e tedder-metar-map.elf ADDR...</code></small></div>%s",
            s_crash[0] ? s_crash : "none recorded", k_page_nav);
    esp_err_t err;
    if (o + 1 >= cap) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Page too large");
        err = ESP_FAIL;
    } else {
        err = http_send_html_chunked(req, h);
    }
    free(h);
    return err;
}

static const char *reset_reason_name(int r)
{
    static const char *const n[] = { "unknown", "power on", "reset pin", "software", "crash",
                                     "interrupt watchdog", "task watchdog", "watchdog",
                                     "deep sleep", "brownout", "SDIO", "USB", "JTAG" };
    return (r >= 0 && r < (int)(sizeof(n) / sizeof(n[0]))) ? n[r] : "other";
}

static void fmt_duration(char *buf, size_t sz, int64_t s)
{
    if (s < 0)          snprintf(buf, sz, "never");
    else if (s < 120)   snprintf(buf, sz, "%lld s", (long long)s);
    else if (s < 7200)  snprintf(buf, sz, "%lld min", (long long)(s / 60));
    else if (s < 172800) snprintf(buf, sz, "%lld h %lld min", (long long)(s / 3600), (long long)(s % 3600 / 60));
    else                snprintf(buf, sz, "%lld d %lld h", (long long)(s / 86400), (long long)(s % 86400 / 3600));
}

static esp_err_t handler_root(httpd_req_t *req)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    state_t snap = s_state;
    int cnt[6] = { 0 };                       /* VFR MVFR IFR LIFR no-report dark */
    for (int i = 0; i < s_slot_n; i++) {
        const slot_t *sl = &s_slots[i];
        if (!sl->icao[0]) { cnt[5]++; continue; }
        int k = sl->has && !sl->stale_remote ? cat_index(sl->cat) : -1;
        cnt[k >= 0 ? k : 4]++;
    }
    char raw[480] = "—";
    if (s_last_body) snprintf(raw, sizeof(raw), "%s", s_last_body);
    xSemaphoreGive(s_state_lock);

    bool local_stale = snap.has_data && (now_ms() - snap.last_fetch_ms > LOCAL_STALE_MS);
    int64_t since_s  = snap.has_data ? (now_ms() - snap.last_fetch_ms) / 1000 : -1;
    uint16_t flags   = dbg_get();

    char *url = malloc(URL_MAX);
    if (url) metar_url_build(url, URL_MAX);
    wifi_ap_record_t ap;
    int rssi = (!s_ap_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    bool sta_up = (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;

    char ledw[600];
    root_led_text(ledw, sizeof(ledw), snap.has_data, snap.stage, local_stale);
    char ago[40], up[40], cfg_ago[40];
    fmt_duration(ago, sizeof(ago), since_s);
    fmt_duration(up, sizeof(up), now_ms() / 1000);
    fmt_duration(cfg_ago, sizeof(cfg_ago), s_cfg_last_ms ? (now_ms() - s_cfg_last_ms) / 1000 : -1);

    const size_t cap = 14336;
    char *h = (char *)malloc(cap);
    if (!h) {
        free(url);
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body>OOM</body></html>");
        return ESP_OK;
    }
    size_t o = 0;
    h[0] = '\0';
    appendf(h, cap, &o,
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta http-equiv='refresh' content='30'><title>METAR map</title>%s</head><body><main>"
            "<h1>METAR map</h1>"
            "<div class=sub>metarmap-%.6s · v" FIRMWARE_VERSION " · %s</div>",
            k_root_css, s_boardid + 6,
            s_ap_mode ? (s_fallback_ap ? "fallback setup network open" : "setup mode")
                      : sta_up ? "online" : "WiFi disconnected");

    /* Weather */
    static const unsigned sw[5] = { 0x007f00, 0x00007f, 0x7f0000, 0xff16c7, 0x4b4b32 };
    static const char *const nm[5] = { "VFR", "MVFR", "IFR", "LIFR", "no report" };
    appendf(h, cap, &o, "<div class=card><h2>Weather</h2>");
    if (snap.has_data) {
        appendf(h, cap, &o, "<div class=big>");
        for (int k = 0; k < 5; k++)
            if (cnt[k]) appendf(h, cap, &o, "<span class=sw style='background:#%06x'></span> %d %s&nbsp;&nbsp; ",
                                sw_hex(sw[k]), cnt[k], nm[k]);
        appendf(h, cap, &o, "</div>");
    } else {
        appendf(h, cap, &o, "<div class=big>No weather yet</div>");
    }
    char stale[96] = "no";
    if (local_stale) snprintf(stale, sizeof(stale), "<b>yes</b> — no good weather check for %d+ min", s_stale_min);
    appendf(h, cap, &o,
            "<table><tr><td>Airports</td><td>%d LEDs: %d airports, %d dark &middot; <a href='/airports'>list</a></td></tr>"
            "<tr><td>Fetched</td><td>%s%s (every %d s)</td></tr>"
            "<tr><td>Old</td><td>%s</td></tr></table></div>",
            s_slot_n, s_slot_n - cnt[5], cnt[5], ago, since_s >= 0 ? " ago" : "", s_poll_sec,
            snap.has_data ? stale : "—");

    /* LEDs */
    appendf(h, cap, &o,
            "<div class=card><h2>What the LEDs show</h2><p style='margin:2px 0 8px'>%s</p>"
            "<table><tr><td>Brightness</td><td>"
            "<form action='/bri_step' method='POST'><input type='hidden' name='dir' value='down'>"
            "<button%s aria-label='dimmer'>&minus;</button></form> "
            "<b>%s</b> "
            "<form action='/bri_step' method='POST'><input type='hidden' name='dir' value='up'>"
            "<button%s aria-label='brighter'>+</button></form>%s</td></tr>",
            ledw, s_bri_idx == 0 ? " disabled" : "", bri_label(s_bri_idx),
            s_bri_idx == 8 ? " disabled" : "",
            bri_effective() != s_bri_idx ? " — night dimming now (+ or &minus; cancels it until morning)" : "");
    nr_rows(h, cap, &o, false);
    appendf(h, cap, &o, "<tr><td>LED test</td><td>");
    static const char *const pat[][2] = { { "colors", "Colors" }, { "order", "Order" },
                                          { "categories", "Categories" } };
    for (int k = 0; k < 3; k++)
        appendf(h, cap, &o, "<form action='/pattern' method='POST'><input type='hidden' name='mode' "
                            "value='%s'><button>%s</button></form>", pat[k][0], pat[k][1]);
    appendf(h, cap, &o, "</td></tr></table></div>");

    /* Device */
    char wifi[160];
    if (s_ap_mode)    snprintf(wifi, sizeof(wifi), "setup network metarmap-%.6s", s_boardid + 6);
    else if (sta_up)  snprintf(wifi, sizeof(wifi), "%s, %d dBm; transmit %d dBm%s", slot_ssid(s_wifi_slot),
                               rssi, s_tx.cur, s_tx.probe != TXP_IDLE ? " (testing a step up)"
                               : s_tx.cur < s_tx.cap ? " (lowered by autotune)" : "");
    else {
        static const char *const why[] = {
            [TXL_NONE]            = "",
            [TXL_NO_ANSWER]       = "; the network doesn't answer, lowering transmit power",
            [TXL_NO_ANSWER_FLOOR] = "; the network doesn't answer even at low power (hardware?)",
            [TXL_BAD_PASS]        = "; a password was rejected",
        };
        snprintf(wifi, sizeof(wifi), "disconnected (trying %s%s)", slot_ssid(s_wifi_slot),
                 why[tx_tune_led(&s_tx)]);
    }
    char btn[160] = "no press since boot";
    if (s_btn_last_at_ms) {
        char t[40];
        fmt_duration(t, sizeof(t), (now_ms() - s_btn_last_at_ms) / 1000);
        snprintf(btn, sizeof(btn), "last press %lld ms, %s, %s ago",
                 (long long)s_btn_last_ms, s_btn_last_physical ? "physical" : "simulated", t);
    }
    int64_t unl = s_unlock_until_ms - now_ms();
    appendf(h, cap, &o,
            "<div class=card><h2>Device</h2><table>"
            "<tr><td>WiFi</td><td>%s</td></tr>"
            "<tr><td>Updates</td><td>%s channel; %s</td></tr>"
            "<tr><td>Uptime</td><td>%s, boot #%lu, last reset: %s (%d)</td></tr>"
            "<tr><td>Memory</td><td>%lu KB free, %lu KB lowest</td></tr>"
            "<tr><td>Problems since boot</td><td>%lu failed weather checks, %lu WiFi drops</td></tr>"
            "<tr><td>BOOT button</td><td>%s; %s; saving %s</td></tr>"
            "<tr><td>Display</td><td>%s</td></tr>"
            "<tr><td>Debug flags</td><td>0x%x <a href='/debug'>details</a> "
            "<form action='/clear_flags' method='POST'><button>Clear</button></form></td></tr>",
            wifi, s_cfg_ota_ch[0] ? s_cfg_ota_ch : "stable", s_ota_result, up, (unsigned long)s_boot_count,
            reset_reason_name(s_reset_reason), s_reset_reason,
            (unsigned long)(esp_get_free_heap_size() / 1024),
            (unsigned long)(esp_get_minimum_free_heap_size() / 1024),
            (unsigned long)s_fetch_fails, (unsigned long)s_wifi_drops,
            s_btn_gpio_level ? "up" : "pressed now", btn,
            unl > 0 ? "unlocked" : "locked", s_oled_ok ? "OLED found" : "no OLED", flags);
    if (s_crash[0])
        appendf(h, cap, &o, "<tr><td>Last crash</td><td>%s</td></tr>", s_crash);
    appendf(h, cap, &o, "</table></div>");

    /* Remote config */
    char nets[160] = "";
    for (int i = 0; i < WIFI_SLOTS; i++) {
        if (!slot_used(i)) continue;
        size_t L = strlen(nets);
        snprintf(nets + L, sizeof(nets) - L, "%s%s%s", L ? ", " : "", slot_ssid(i),
                 i == s_wifi_slot ? " (in use)" : "");
    }
    appendf(h, cap, &o,
            "<div class=card><h2>Remote config</h2><table>"
            "<tr><td>Config file</td><td>rev %lu · %s%s%s%s</td></tr>"
            "<tr><td>Checks</td><td>firmware every %u min, config every %u min</td></tr>"
            "<tr><td>Networks</td><td>%s</td></tr>",
            (unsigned long)s_cfg_rev, s_cfg_last_ms ? "checked " : "", s_cfg_last_ms ? cfg_ago : "",
            s_cfg_last_ms ? " ago · " : "", s_cfg_result,
            s_ota_int_min, s_cfg_int_min, nets[0] ? nets : "none");
    if (s_fw_base[0] || s_cfg_base[0])
        appendf(h, cap, &o, "<tr><td>Overrides</td><td>%s%s%s%s</td></tr>",
                s_fw_base[0] ? "firmware " : "", s_fw_base, s_cfg_base[0] ? " config " : "", s_cfg_base);
    appendf(h, cap, &o, "</table></div>");

    /* Settings */
    appendf(h, cap, &o, "<div class=card><details><summary>Settings</summary><table class=set>");
    settings_rows(h, cap, &o);
    appendf(h, cap, &o, "</table><small>Bold = changed from the default. Set on the Configure page or "
                        "with the config file (docs/config-options).</small></details></div>");

    /* Technical */
    appendf(h, cap, &o,
            "<div class=card><details><summary>Technical</summary><table>"
            "<tr><td>Board ID</td><td>%s</td></tr>"
            "<tr><td>Weather URL</td><td><code>%s</code></td></tr>"
            "<tr><td>Last reply</td><td><code>%s</code></td></tr>"
            "</table></details></div>"
            "%s",
            s_boardid, url ? url : "—", raw, k_page_nav);
    free(url);

    esp_err_t err = ESP_OK;
    if (o + 1 >= cap) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body>Page too large</body></html>");
        err = ESP_FAIL;
    } else {
        err = http_send_html_chunked(req, h);
    }
    free(h);
    return err;
}

/** GET /airports: every LED in strip order, its airport and current color; Find blinks it. */
static esp_err_t handler_airports(httpd_req_t *req)
{
    const size_t cap = 4096;
    char *h = (char *)malloc(cap);
    slot_t *sl = (slot_t *)malloc(sizeof(slot_t) * LED_MAX);
    if (!h || !sl) {
        free(h); free(sl);
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "OOM");
        return ESP_OK;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int n = s_slot_n;
    memcpy(sl, s_slots, sizeof(slot_t) * (size_t)n);
    xSemaphoreGive(s_state_lock);

    size_t o = 0;
    h[0] = '\0';
    page_head(h, cap, &o, "Airports", NULL);
    appendf(h, cap, &o, "<div class=card><p style='margin:2px 0'>%d LEDs, in strip order (LED 1 is the one "
            "wired to the board). <b>Find</b> blinks that LED white for 30 s. Change the list on the "
            "<a href='/config'>Configure</a> page.</p></div><div class=card>"
            "<form action='/pattern' method='POST'><input type=hidden name=mode value=one>"
            "<table><tr><td>LED</td><td>Airport</td><td>Now</td><td></td></tr>", n);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send_chunk(req, h, (ssize_t)o);
    for (int i = 0; i < n && err == ESP_OK; i++) {
        const slot_t *s = &sl[i];
        uint8_t r = s->r, g = s->g, b = s->b;
        color_override(s->cat, &r, &g, &b);
        char now[96], age[24] = "";
        if (!s->icao[0])        snprintf(now, sizeof(now), "dark (OFF)");
        else if (!s->has)       snprintf(now, sizeof(now), "not in the last reply");
        else {
            snprintf(now, sizeof(now), "<span class=sw style='background:#%06x'></span> %s",
                     sw_hex(((unsigned)r << 16) | ((unsigned)g << 8) | b),
                     s->stale_remote ? "dim warm white (no current report)" : cat_plain(s->cat));
            if (s->age_min >= 0 && !s->stale_remote) snprintf(age, sizeof(age), ", %d min old", s->age_min);
        }
        int len = snprintf(h, cap, "<tr><td>%d</td><td>%s</td><td>%s%s</td><td><button name=led value=%d>Find</button></td></tr>",
                           i + 1, s->icao[0] ? s->icao : "—", now, age, i + 1);
        err = httpd_resp_send_chunk(req, h, len);
    }
    if (err == ESP_OK) {
        int len = snprintf(h, cap, "</table></form></div>%s", k_page_nav);
        httpd_resp_send_chunk(req, h, len);
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(h);
    free(sl);
    return err;
}

/** GET /log: recent warnings and errors since boot (RAM ring, oldest first). */
static esp_err_t handler_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!s_logring) return httpd_resp_sendstr(req, "(log buffer not available)\n");
    char *copy = malloc(LOGRING_SIZE + 1);
    if (!copy) return httpd_resp_sendstr(req, "(out of memory)\n");
    size_t n = 0;
    taskENTER_CRITICAL(&s_logring_mux);
    if (s_logring_wrapped) {
        memcpy(copy, s_logring + s_logring_pos, LOGRING_SIZE - s_logring_pos);
        n = LOGRING_SIZE - s_logring_pos;
    }
    memcpy(copy + n, s_logring, s_logring_pos);
    n += s_logring_pos;
    taskEXIT_CRITICAL(&s_logring_mux);
    copy[n] = '\0';
    char head[160];
    snprintf(head, sizeof(head), "metarmap-%.6s v%s, up %lld s. Warnings and errors since boot (newest last):\n\n",
             s_boardid + 6, FIRMWARE_VERSION, (long long)(now_ms() / 1000));
    httpd_resp_send_chunk(req, head, (ssize_t)strlen(head));
    const char *start = copy;
    if (s_logring_wrapped) { const char *nl = strchr(copy, '\n'); if (nl) start = nl + 1; }   /* cut partial line */
    httpd_resp_send_chunk(req, start, (ssize_t)strlen(start));
    free(copy);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool saves_unlocked(void)
{
    return (s_ap_mode && !s_fallback_ap) || now_ms() < s_unlock_until_ms;
}

/** Same-site check: a browser sends Origin (or at least Referer) with a form POST; it must name
 *  this unit. Neither header (curl, scripts) is allowed. Skipped in setup mode, where only
 *  phones on the unit's own network can reach it. */
static bool request_same_site(httpd_req_t *req)
{
    if (s_ap_mode) return true;
    char host[64] = "", hdr[160] = "", want[80];
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    snprintf(want, sizeof(want), "http://%s", host);
    if (httpd_req_get_hdr_value_str(req, "Origin", hdr, sizeof(hdr)) == ESP_OK)
        return host[0] && strcmp(hdr, want) == 0;
    if (httpd_req_get_hdr_value_str(req, "Referer", hdr, sizeof(hdr)) == ESP_OK) {
        size_t n = strlen(want);
        return host[0] && strncmp(hdr, want, n) == 0 && (hdr[n] == '/' || hdr[n] == '\0');
    }
    return true;
}

static void send_403_page(httpd_req_t *req, const char *msg)
{
    char body[400];
    snprintf(body, sizeof(body), "<p>%s</p><p><a href='/config'>Back to Configure</a></p>", msg);
    send_page(req, "403 Forbidden", "Not saved", body);
}

/** Gate for every settings save. need_unlock=false: same-site check only (clear flags). */
static bool save_allowed(httpd_req_t *req, bool need_unlock)
{
    if (!request_same_site(req)) {
        ESP_LOGW(TAG, "save rejected: request from another site");
        send_403_page(req, "Rejected: this request came from another web page.");
        return false;
    }
    if (need_unlock && !saves_unlocked()) {
        ESP_LOGW(TAG, "save rejected: locked (no button tap)");
        send_403_page(req, "Locked: press the BOOT button on the map's controller board once to unlock saving for 15 minutes.");
        return false;
    }
    return true;
}

/** GET /unlock: lock state for the /config page (polled each second). */
static esp_err_t handler_unlock(httpd_req_t *req)
{
    bool setup = s_ap_mode && !s_fallback_ap;
    int64_t left = s_unlock_until_ms - now_ms();
    char js[96];
    snprintf(js, sizeof(js), "{\"unlocked\":%s,\"setup\":%s,\"remaining_s\":%d}",
             saves_unlocked() ? "true" : "false", setup ? "true" : "false",
             (!setup && left > 0) ? (int)((left + 999) / 1000) : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, js);
    return ESP_OK;
}

// ---------- setup page: settings, admin, actions ----------

/** Form field by exact name (at the start of the body or after '&'), URL-decoded. */
static bool form_get(const char *body, const char *key, char *val, size_t valsz)
{
    size_t kl = strlen(key);
    for (const char *p = body; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == body || p[-1] == '&') && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '&');
            size_t len = e ? (size_t)(e - v) : strlen(v);
            char *raw = malloc(len + 1);          /* the airport list can be a few KB */
            if (!raw) return false;
            memcpy(raw, v, len);
            raw[len] = '\0';
            url_decode(val, raw, valsz);
            free(raw);
            return true;
        }
    }
    return false;
}

/** Checkbox: the form lists its checkboxes in "bools" (names joined by '.'); a listed box
 *  that isn't in the body was unchecked. Returns -1 when this form doesn't have the box. */
static int form_bool(const char *body, const char *bools, const char *key)
{
    char want[40];
    snprintf(want, sizeof(want), ".%s.", key);
    char list[160];
    snprintf(list, sizeof(list), ".%s.", bools);
    if (!strstr(list, want)) return -1;
    char v[8];
    return form_get(body, key, v, sizeof(v)) && (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
}

/** Setup page save: run a JSON object through the config file's checks. Returns true when
 *  the change needs a reboot; err gets the rejected key ("" = saved). */
static bool local_apply(const cJSON *j, int *changes, char *err, size_t errsz)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_apply_local   = true;
    s_local_err[0]  = '\0';
    s_local_changes = 0;
    bool reboot = cfg_apply_obj(j, 0);
    s_apply_local = false;
    *changes = s_local_changes;
    snprintf(err, errsz, "%s", s_local_err);
    xSemaphoreGive(s_cfg_mutex);
    return reboot && !err[0];
}

/* Time zones offered for night dimming (POSIX TZ strings). */
static const char *const k_tz[][2] = {
    { "PST8PDT,M3.2.0,M11.1.0", "Pacific (Portland)" },
    { "MST7MDT,M3.2.0,M11.1.0", "Mountain (Denver)" },
    { "MST7", "Arizona (no daylight time)" },
    { "CST6CDT,M3.2.0,M11.1.0", "Central (Nashville)" },
    { "EST5EDT,M3.2.0,M11.1.0", "Eastern (Atlanta, Charlotte)" },
    { "AKST9AKDT,M3.2.0,M11.1.0", "Alaska" },
    { "HST10", "Hawaii" },
    { "UTC0", "UTC" },
};

static void add_form_number(cJSON *j, const char *key, const char *v)
{
    char *end;
    double d = strtod(v, &end);
    if (v[0] && !*end) cJSON_AddNumberToObject(j, key, d);
    else               cJSON_AddStringToObject(j, key, v);   /* fails the number check by name */
}

static void send_save_result(httpd_req_t *req, const char *what, int changes, const char *err, bool reboot)
{
    char msg[300];
    if (err[0]) {
        snprintf(msg, sizeof(msg), "<p>Nothing was saved: <b>%s</b> is not valid. Check the value "
                 "and try again.</p><p><a href='/config'>Back to Configure</a></p>", err);
        send_page(req, HTTPD_400, "Not saved", msg);
        return;
    }
    snprintf(msg, sizeof(msg), "<p>%s: %d change%s.%s</p><p><a href='/config'>Back to Configure</a></p>",
             what, changes, changes == 1 ? "" : "s",
             reboot ? " The map restarts now to apply the LED strip settings; then use the LED "
                      "test on the status page." : "");
    send_page(req, NULL, "Saved", msg);
}

/** POST /save_settings: map behavior, timing, LED string, telemetry (whatever the form sends). */
static esp_err_t handler_save_settings(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    char *body = (char *)malloc(1024);
    if (!body) { httpd_resp_sendstr(req, "OOM"); return ESP_OK; }
    read_body(req, body, 1024);
    char bools[160] = "", v[64];
    form_get(body, "bools", bools, sizeof(bools));
    cJSON *j = cJSON_CreateObject();
    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_t *st = &k_settings[i];
        if (st->kind == SET_BOOL) {
            int b = form_bool(body, bools, st->key);
            if (b >= 0) cJSON_AddBoolToObject(j, st->key, b);
        } else if (form_get(body, st->key, v, sizeof(v))) {
            if (st->kind == SET_ENUM) cJSON_AddStringToObject(j, st->key, v);
            else                      add_form_number(j, st->key, v);
        }
    }
    for (const char *const *k = (const char *const[]){ "ota_interval_minutes", "config_interval_minutes", NULL }; *k; k++)
        if (form_get(body, *k, v, sizeof(v))) add_form_number(j, *k, v);
    int nd = form_bool(body, bools, "nd_on");
    if (nd == 0) {
        cJSON_AddFalseToObject(j, "night_dim");
    } else if (nd == 1) {
        cJSON *o = cJSON_AddObjectToObject(j, "night_dim");
        if (form_get(body, "nd_start", v, sizeof(v))) cJSON_AddStringToObject(o, "start", v);
        if (form_get(body, "nd_end", v, sizeof(v)))   cJSON_AddStringToObject(o, "end", v);
        if (form_get(body, "nd_bri", v, sizeof(v)))   add_form_number(o, "brightness", v);
        if (form_get(body, "nd_tz", v, sizeof(v)))    cJSON_AddStringToObject(o, "tz", v);
    }
    free(body);
    int changes;
    char err[80];
    bool reboot = local_apply(j, &changes, err, sizeof(err));
    cJSON_Delete(j);
    send_save_result(req, "Settings saved", changes, err, reboot);
    if (reboot) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return ESP_OK;
}

#define ADMIN_TRIES      5
#define ADMIN_WINDOW_MS  (10 * 60 * 1000)
static int64_t s_admin_fail_ms[ADMIN_TRIES];   /* RAM only: a reboot clears it */

static bool admin_hash_stored(char *hex, size_t sz)
{
    size_t len = sz;
    return s_nvs_ready && nvs_get_str(s_nvs, "admin_hash", hex, &len) == ESP_OK && strlen(hex) == 64;
}

/** Minutes until another try is allowed (0 = allowed now). */
static int admin_locked_min(void)
{
    int64_t t = now_ms(), oldest = INT64_MAX;
    int n = 0;
    for (int i = 0; i < ADMIN_TRIES; i++)
        if (s_admin_fail_ms[i] && t - s_admin_fail_ms[i] < ADMIN_WINDOW_MS) {
            n++;
            if (s_admin_fail_ms[i] < oldest) oldest = s_admin_fail_ms[i];
        }
    return n < ADMIN_TRIES ? 0 : (int)((ADMIN_WINDOW_MS - (t - oldest) + 59999) / 60000);
}

static void admin_fail(void)
{
    int k = 0;
    for (int i = 1; i < ADMIN_TRIES; i++) if (s_admin_fail_ms[i] < s_admin_fail_ms[k]) k = i;
    s_admin_fail_ms[k] = now_ms();
}

/** Typed phrase -> lowercase, single spaces -> SHA-256("tmm-admin-v1:<boardid>:<phrase>"). */
static bool admin_phrase_ok(const char *typed)
{
    char want[72];
    if (!admin_hash_stored(want, sizeof(want))) return false;
    char norm[128];
    size_t n = 0;
    bool space = false;
    for (const char *p = typed; *p && n + 2 < sizeof(norm); p++) {
        if (isspace((unsigned char)*p)) { space = n > 0; continue; }
        if (space) { norm[n++] = ' '; space = false; }
        norm[n++] = (char)tolower((unsigned char)*p);
    }
    norm[n] = '\0';
    char in[180];
    int len = snprintf(in, sizeof(in), "tmm-admin-v1:%s:%s", s_boardid, norm);
    uint8_t d[32];
    size_t olen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)in, (size_t)len, d, sizeof(d), &olen) != PSA_SUCCESS)
        return false;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++) diff |= (uint8_t)(hex[i] ^ tolower((unsigned char)want[i]));
    return diff == 0;
}

/** POST /save_admin: firmware/config base URLs behind the admin phrase. */
static esp_err_t handler_save_admin(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    char want[72];
    if (!admin_hash_stored(want, sizeof(want))) {
        send_page(req, "403 Forbidden", "Not saved", "<p>This map has no admin phrase. It can only "
                  "be set over USB (<code>tools/provision.py --admin-phrase</code>).</p>");
        return ESP_OK;
    }
    int wait = admin_locked_min();
    if (wait) {
        char msg[160];
        snprintf(msg, sizeof(msg), "<p>Too many wrong admin phrases. Try again in %d min.</p>", wait);
        send_page(req, "403 Forbidden", "Not saved", msg);
        return ESP_OK;
    }
    char body[512], phrase[96] = "", fw[128] = "", cf[128] = "";
    read_body(req, body, sizeof(body));
    form_get(body, "phrase", phrase, sizeof(phrase));
    bool has_fw = form_get(body, "firmware_base_url", fw, sizeof(fw));
    bool has_cf = form_get(body, "config_base_url", cf, sizeof(cf));
    if (!admin_phrase_ok(phrase)) {
        admin_fail();
        int left = 0;
        for (int i = 0; i < ADMIN_TRIES; i++)
            if (!s_admin_fail_ms[i] || now_ms() - s_admin_fail_ms[i] >= ADMIN_WINDOW_MS) left++;
        ESP_LOGW(TAG, "admin: wrong phrase (%d tries left)", left);
        char msg[200];
        snprintf(msg, sizeof(msg), "<p>Wrong admin phrase. Nothing was saved. %d tr%s left before a "
                 "10-minute lockout.</p><p><a href='/config'>Back to Configure</a></p>", left, left == 1 ? "y" : "ies");
        send_page(req, "403 Forbidden", "Not saved", msg);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "admin: phrase accepted");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddTrueToObject(j, "force");
    if (has_fw && strcmp(fw, s_fw_base[0] ? s_fw_base : FIRMWARE_BASE_URL) != 0)
        cJSON_AddStringToObject(j, "firmware_base_url", strcmp(fw, FIRMWARE_BASE_URL) == 0 ? "" : fw);
    if (has_cf && strcmp(cf, s_cfg_base[0] ? s_cfg_base : CONFIG_BASE_URL) != 0)
        cJSON_AddStringToObject(j, "config_base_url", strcmp(cf, CONFIG_BASE_URL) == 0 ? "" : cf);
    int changes;
    char err[80];
    local_apply(j, &changes, err, sizeof(err));
    cJSON_Delete(j);
    send_save_result(req, "Admin settings saved", changes, err, false);
    return ESP_OK;
}

/** POST /action: what = ota_now, reboot, reset. */
static esp_err_t handler_action(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    char body[64], what[16] = "";
    read_body(req, body, sizeof(body));
    form_get(body, "what", what, sizeof(what));
    if (strcmp(what, "ota_now") == 0) {
        if (s_ota_task) xTaskNotifyGive(s_ota_task);
        ESP_LOGI(TAG, "setup page: update check now");
        send_page(req, NULL, "Checking for an update", "<p>The map is checking now. If there is "
                  "a new version, it installs it and restarts (about a minute). Otherwise nothing "
                  "changes.</p>");
        return ESP_OK;
    }
    if (strcmp(what, "reboot") == 0 || strcmp(what, "reset") == 0) {
        bool reset = strcmp(what, "reset") == 0;
        if (reset) {
            for (size_t i = 0; i < SETTINGS_N; i++) cfg_erase(k_settings[i].nvs);
            static const char *const more[] = { "nd_start", "nd_end", "nd_off", "nd_bri", "tz", "col_vfr",
                                                "col_mvfr", "col_ifr", "col_lifr", "ota_int", "cfg_int",
                                                "tx_auto" };   /* WiFi, airports, brightness, channel kept */
            for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) cfg_erase(more[i]);
            ESP_LOGW(TAG, "setup page: map settings reset to defaults");
        }
        ESP_LOGI(TAG, "setup page: restart");
        send_page(req, NULL, reset ? "Settings reset" : "Restarting",
                  reset ? "<p>Map settings are back to their defaults (WiFi, airport list, brightness "
                          "and update channel kept). The map restarts now.</p>"
                        : "<p>The map shows the startup rainbow, then the weather in about 30 s.</p>");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    send_page(req, HTTPD_400, "Unknown action", "<p>Use ota_now, reboot or reset.</p>");
    return ESP_OK;
}

static const setting_t *setting_by_key(const char *key)
{
    for (size_t i = 0; i < SETTINGS_N; i++)
        if (strcmp(k_settings[i].key, key) == 0) return &k_settings[i];
    abort();                                   /* only called with keys from k_settings */
}

static const char *sel(bool on) { return on ? " selected" : ""; }
static const char *chk(bool on) { return on ? " checked" : ""; }

static esp_err_t handler_config(httpd_req_t *req)
{
    const size_t cap = 24576;
    char *h = (char *)malloc(cap);
    if (!h) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "OOM");
        return ESP_OK;
    }
    bool setup = s_ap_mode && !s_fallback_ap;
    size_t o = 0;
    h[0] = '\0';
    page_head(h, cap, &o, "Configure", NULL);

    /* lock box */
    appendf(h, cap, &o, "<div id='lock' class='card %s'%s>%s</div>",
            setup ? "" : (saves_unlocked() ? "ok" : "lk"), setup ? " style='display:none'" : "",
            setup ? "" : "Checking lock&hellip;");

    /* Airports */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    appendf(h, cap, &o,
            "<form class='card blk' action='/save_airports' method='POST'><h2>Airports</h2>"
            "<label class=f for=ap>One line per LED, in strip order (LED 1 first). An airport code "
            "(KPDX, S48) lights that LED; <code>OFF</code> leaves it dark. Up to %d lines.</label>"
            "<textarea id=ap name='airports' rows=14 style='width:100%%;max-width:26em;font:15px ui-monospace,Menlo,monospace'>%s</textarea>"
            "<div class=note>%d lines now. After saving, the map checks the list once and says which "
            "codes have no current weather report.</div><button>Save airports</button></form>",
            LED_MAX, s_airports_text, s_slot_n);
    xSemaphoreGive(s_state_lock);

    /* Map behavior */
    bool tz_known = false;
    for (size_t i = 0; i < sizeof(k_tz) / sizeof(k_tz[0]); i++) tz_known |= strcmp(s_tz, k_tz[i][0]) == 0;
    int nds = s_nd_on ? s_nd_start : 22 * 60, nde = s_nd_on ? s_nd_end : 7 * 60;
    appendf(h, cap, &o,
            "<details class='card group'><summary>Map behavior</summary>"
            "<div class=note>Changes here stay until a later config file changes the same setting.</div>"
            "<form class=blk action='/save_settings' method='POST'>"
            "<input type=hidden name=bools value='nd_on.status_pixel'>"
            "<h3>Night dimming</h3><div class=sb>"
            "<label class=check><input type=checkbox id=nd_on name=nd_on value=true%s><span>Dim the map at night"
            "<small>The brightness + or &minus; on the status page cancels dimming until morning.</small></span></label>"
            "<fieldset id=ndf%s><div class=row>"
            "<div><label class=f for=nds>From</label><input id=nds type=time name=nd_start value='%02d:%02d'></div>"
            "<div><label class=f for=nde>Until</label><input id=nde type=time name=nd_end value='%02d:%02d'></div></div>"
            "<div class=row><div><label class=f for=ndb>Night brightness</label><select id=ndb name=nd_bri>",
            chk(s_nd_on), s_nd_on ? "" : " disabled", nds / 60, nds % 60, nde / 60, nde % 60);
    for (int i = 0; i < 9; i++)
        appendf(h, cap, &o, "<option value='%d'%s>%s</option>", i, sel(s_nd_bri == i), bri_label((uint8_t)i));
    appendf(h, cap, &o, "</select></div><div><label class=f for=ndz>Time zone</label><select id=ndz name=nd_tz>");
    for (size_t i = 0; i < sizeof(k_tz) / sizeof(k_tz[0]); i++)
        appendf(h, cap, &o, "<option value='%s'%s>%s</option>", k_tz[i][0],
                sel(strcmp(s_tz, k_tz[i][0]) == 0 || (!s_tz[0] && i == 0)), k_tz[i][1]);
    if (s_tz[0] && !tz_known) appendf(h, cap, &o, "<option value='%s' selected>%s</option>", s_tz, s_tz);
    appendf(h, cap, &o,
            "</select></div></div></fieldset></div>"
            "<h3>Status light</h3><div class=sb>"
            "<label class=check><input type=checkbox name=status_pixel value=true%s><span>Use the small light on "
            "the controller board<small>Slow green breathing = all good, amber = weather is old or no WiFi. "
            "Setup mode and button presses always show.</small></span></label></div>"
            "<button>Save map settings</button></form></details>",
            chk(s_status_pixel));

    /* No-report brightness: -/+ like the status page brightness, no BOOT press needed */
    appendf(h, cap, &o, "<div class=card><h2>Airports with no report</h2><table>");
    nr_rows(h, cap, &o, true);
    appendf(h, cap, &o, "</table><div class=note>Brightness of the grey no-report lights, as a percent of "
                        "the map brightness; night is during night dimming. Lower it so the reporting "
                        "airports stand out; 0 turns them off. Saved right away, no button press needed.</div></div>");

    /* Danger zone: WiFi */
    appendf(h, cap, &o,
            "<details class='card danger'%s><summary>Danger zone</summary>"
            "<h3>WiFi</h3><form class=blk action='/save_wifi' method='POST'>"
            "<div class=net><label class=f for=ssid>Main network name (SSID)</label><input id=ssid name='ssid' value='%s'>"
            "<label class=f for=pass>Password</label><input id=pass name='pass' class=mask autocomplete=off data-1p-ignore data-lpignore=true placeholder='(unchanged)'>"
            "<label class=check><input type='checkbox' name='notest' value='1'><span>Save without testing"
            "<small>For a network that's not in range here.</small></span></label></div>"
            "<h4 style='margin:12px 0 0;font-size:.9em'>Backup networks</h4>"
            "<div class=note>The map moves to the next saved network after 3 failed reconnects.</div>",
            setup ? " open" : "", s_wifi_ssid_from_nvs ? s_cfg_ssid : "");
    for (int i = 1; i < WIFI_SLOTS; i++) {
        bool used = slot_used(i);
        appendf(h, cap, &o,
                "<div class=net><div class=row>"
                "<div><label class=f for=b%ds>Backup %d name</label><input id=b%ds name=b%ds value='%s' placeholder='(empty)'></div>"
                "<div><label class=f for=b%dp>Password</label><input id=b%dp name=b%dp class=mask autocomplete=off data-1p-ignore data-lpignore=true placeholder='%s'></div></div>"
                "<label class=check><input type=checkbox name=b%dr value=1%s><span>Remove this network%s%s</span></label></div>",
                i, i, i, i, used ? s_extra_ssid[i - 1] : "",
                i, i, i, used ? "(unchanged)" : "",
                i, used && i != s_wifi_slot ? "" : " disabled",
                used ? "" : "<small>Empty slot: type a name above to add one.</small>",
                used && i == s_wifi_slot ? "<small>In use now, so it can't be removed.</small>" : "");
    }
    appendf(h, cap, &o, "<button>Save WiFi (restarts)</button></form>");

    /* Danger zone: weather source + channel */
    appendf(h, cap, &o,
            "<h3>Weather source and updates</h3>"
            "<form class=blk action='/save_url' method='POST'>"
            "<label class=f for=pfx>Weather URL prefix (the airport codes are added at the end)</label>"
            "<input id=pfx name='url_prefix' value='%s'><button>Save URL prefix</button></form>"
            "<form class=blk action='/save_channel' method='POST'>"
            "<label class=f for=ch>Update channel</label><select id=ch name='ota_ch'>"
            "<option value=''%s>stable</option><option value='beta'%s>beta</option></select>"
            "<button>Save channel</button></form>",
            s_cfg_url_pfx, sel(!s_cfg_ota_ch[0]), sel(s_cfg_ota_ch[0] != 0));

    /* Danger zone: timing */
    appendf(h, cap, &o,
            "<h3>Timing</h3><form class=blk action='/save_settings' method='POST'>"
            "<div class=row><div><label class=f for=tp>Weather check every (s)</label>"
            "<input id=tp type=number name=metar_poll_seconds min=60 max=3600 value='%d'></div>"
            "<div><label class=f for=tf>Open setup network after no WiFi for (min)</label>"
            "<input id=tf type=number name=wifi_fallback_minutes min=1 max=120 value='%d'></div></div>"
            "<div class=row><div><label class=f for=ts>Show \"weather is old\" after (min)</label>"
            "<input id=ts type=number name=metar_stale_minutes min=10 max=1440 value='%d'></div>"
            "<div><label class=f for=tr>Restart after no weather for (h)</label>"
            "<input id=tr type=number name=metar_stale_reboot_hours min=1 max=48 value='%d'></div></div>"
            "<div class=row><div><label class=f for=to>Firmware update check every (min)</label>"
            "<input id=to type=number name=ota_interval_minutes min=15 max=1440 value='%u'></div>"
            "<div><label class=f for=tc>Config file check every (min)</label>"
            "<input id=tc type=number name=config_interval_minutes min=5 max=1440 value='%u'></div></div>"
            "<button>Save timing</button></form>",
            s_poll_sec, s_fb_min, s_stale_min, s_stale_reboot_h, s_ota_int_min, s_cfg_int_min);

    /* Danger zone: LED string + telemetry */
    int lo = setting_stored(setting_by_key("led_order")), lm = setting_stored(setting_by_key("led_model")),
        lg = setting_stored(setting_by_key("led_gpio"));
    appendf(h, cap, &o,
            "<h3>LED string</h3><form class=blk action='/save_settings' method='POST'>"
            "<div class=row><div><label class=f for=lo>Color order</label><select id=lo name=led_order>");
    for (int i = 0; k_led_order_opts[i]; i++)
        appendf(h, cap, &o, "<option%s>%s</option>", sel(lo == i), k_led_order_opts[i]);
    appendf(h, cap, &o, "</select></div><div><label class=f for=lm>LED type</label><select id=lm name=led_model>");
    for (int i = 0; k_led_model_opts[i]; i++)
        appendf(h, cap, &o, "<option value='%s'%s>%s</option>", k_led_model_opts[i], sel(lm == i), k_led_model_opts[i]);
    appendf(h, cap, &o,
            "</select></div><div><label class=f for=lg>Data pin (GPIO)</label>"
            "<input id=lg type=number name=led_gpio min=0 max=46 value='%d'></div></div>"
            "<div class=note>Saving a change restarts the map. Then run the LED test on the status page: Colors "
            "must show red, green, blue, white (otherwise change the color order; RGBW and GRBW are for "
            "4-channel LEDs); Order must step one LED from 1 to the last.</div><button>Save LED string</button></form>"
            "<h3>Telemetry</h3><form class=blk action='/save_settings' method='POST'>"
            "<input type=hidden name=bools value='verbose_telemetry'>"
            "<label class=check><input type=checkbox name=verbose_telemetry value=true%s><span>Send health numbers "
            "with each weather check<small>Signal strength, memory, uptime, restarts, failures, config rev. Helps "
            "find problems remotely.</small></span></label><button>Save telemetry</button></form>",
            lg, chk(s_verbose_tel));

    /* Danger zone: admin only */
    char ah[72];
    bool has_admin = admin_hash_stored(ah, sizeof(ah));
    appendf(h, cap, &o,
            "<details class=xdanger><summary>Admin only: can cut the map off</summary>"
            "<div class=note>A wrong address here stops updates or remote config for good; the fix is a "
            "USB reflash. Needs this map's four-word admin phrase. 5 wrong tries locks this for 10 minutes.%s</div>"
            "<form class=blk action='/save_admin' method='POST'>"
            "<label class=f for=adm>Admin phrase for board <code>%s</code></label>"
            "<input id=adm name=phrase autocomplete=off autocapitalize=none spellcheck=false data-1p-ignore "
            "data-lpignore=true placeholder='four words'%s>"
            "<label class=f for=fwb>Firmware address</label><input id=fwb name=firmware_base_url value='%s'>"
            "<label class=f for=cfb>Config file address</label><input id=cfb name=config_base_url value='%s'>"
            "<button%s>Save admin settings</button></form></details></details>",
            has_admin ? "" : " <b>This map has no admin phrase yet (set over USB).</b>",
            s_boardid, has_admin ? "" : " disabled",
            s_fw_base[0] ? s_fw_base : FIRMWARE_BASE_URL, s_cfg_base[0] ? s_cfg_base : CONFIG_BASE_URL,
            has_admin ? "" : " disabled data-keep=1");

    /* Actions */
    appendf(h, cap, &o,
            "<div class=card><h2>Actions</h2>"
            "<form action='/action' method='POST'><input type=hidden name=what value=ota_now><button>Check for update now</button></form>"
            "<form action='/action' method='POST'><input type=hidden name=what value=reboot><button>Restart</button></form>"
            "<button type=button id=rst>Reset settings to defaults&hellip;</button>"
            "<div class=confirm id=rstc hidden>Puts every map setting back to its default (night dimming, "
            "status light, no-report brightness, timing, LED string, telemetry, colors). WiFi, airport list, "
            "brightness and update channel are kept. The map restarts."
            "<div><form action='/action' method='POST'><input type=hidden name=what value=reset>"
            "<button>Reset settings</button></form><button type=button id=rstn>Cancel</button></div></div></div>");

    /* script: lock box, toggles, reset confirm */
    appendf(h, cap, &o,
            "<script>"
            "var L=document.getElementById('lock');"
            "function btns(on){document.querySelectorAll('form button').forEach(function(b){if(b.dataset.keep)return;b.disabled=!on;"
            "if(/save_/.test(b.form.getAttribute('action')))b.classList.add('sv');"
            "b.title=on?'':'Press the BOOT button on the controller board once to enable saving.';"
            "var n=b.nextElementSibling,h=n&&n.className=='lh';if(!on&&!h){n=document.createElement('small');n.className='lh';"
            "n.textContent='Press the BOOT button on the controller board once to enable saving.';b.after(n);}else if(on&&h)n.remove();});}"
            "btns(%d);"
            "function u(){fetch('/unlock',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
            "btns(j.unlocked);"
            "if(j.setup){L.style.display='none';return;}"
            "if(j.unlocked){var s=j.remaining_s;L.className='card ok';"
            "L.innerHTML='<b>Unlocked.</b> Saving allowed for '+Math.floor(s/60)+':'+('0'+s%%60).slice(-2)+'.';}"
            "else{L.className='card lk';L.innerHTML='<b>Press the BOOT button on the controller board once</b> to "
            "unlock saving for 15 minutes. Until then the Save buttons are grey with a red outline; they turn green when unlocked.';}"
            "}).catch(function(){});}"
            "u();setInterval(u,2000);"
            "function tie(c,f){var a=document.getElementById(c),b=document.getElementById(f);"
            "a.addEventListener('change',function(){b.disabled=!a.checked;});}"
            "tie('nd_on','ndf');"
            "var R=document.getElementById('rstc');"
            "document.getElementById('rst').onclick=function(){R.hidden=false;};"
            "document.getElementById('rstn').onclick=function(){R.hidden=true;};"
            "</script>%s",
            saves_unlocked() ? 1 : 0, k_page_nav);

    esp_err_t err;
    if (o + 1 >= cap) {
        ESP_LOGE(TAG, "/config page over %u bytes", (unsigned)cap);
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Page too large");
        err = ESP_FAIL;
    } else {
        err = http_send_html_chunked(req, h);
    }
    free(h);
    return err;
}

static esp_err_t handler_save_wifi(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    char body[1024];
    read_body(req, body, sizeof(body));

    /* backup networks (slots 1-2): validate all before changing anything */
    char bs[WIFI_SLOTS][33], bp[WIFI_SLOTS][64];
    bool bset[WIFI_SLOTS] = { false }, brem[WIFI_SLOTS] = { false };
    for (int i = 1; i < WIFI_SLOTS; i++) {
        char k[8], r[4] = "";
        snprintf(k, sizeof(k), "b%dr", i);
        brem[i] = form_get(body, k, r, sizeof(r)) && r[0] == '1' && i != s_wifi_slot;
        snprintf(k, sizeof(k), "b%ds", i);
        bset[i] = form_get(body, k, bs[i], sizeof(bs[i])) && bs[i][0];
        snprintf(k, sizeof(k), "b%dp", i);
        if (!form_get(body, k, bp[i], sizeof(bp[i]))) bp[i][0] = '\0';
        size_t pl = strlen(bp[i]);
        if (bset[i] && !brem[i] && pl && (pl < 8 || pl > 63)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "<p>Backup %d password must be 8 to 63 characters (or empty for "
                     "an open network). Nothing was saved.</p><p><a href='/config'>Back to Configure</a></p>", i);
            send_page(req, HTTPD_400, "Not saved", msg);
            return ESP_OK;
        }
    }

    char ssid[32], pass[64];
    bool got_ssid = get_param(body, "ssid", ssid, sizeof(ssid));
    bool got_pass = get_param(body, "pass", pass, sizeof(pass));

    char new_ssid[32];
    char new_pass[64];
    if (got_ssid && ssid[0])
        snprintf(new_ssid, sizeof(new_ssid), "%.31s", ssid);
    else
        snprintf(new_ssid, sizeof(new_ssid), "%s", s_cfg_ssid);

    if (got_pass && pass[0])
        snprintf(new_pass, sizeof(new_pass), "%s", pass);
    else
        snprintf(new_pass, sizeof(new_pass), "%s", s_cfg_pass);

    if (!new_ssid[0]) {
        send_page(req, HTTPD_400, "Not saved", "<p>A network name (SSID) is required.</p>"
                  "<p><a href='/config'>Back to Configure</a></p>");
        return ESP_OK;
    }

    char notest[4] = "";
    get_param(body, "notest", notest, sizeof(notest));
    if (s_ap_mode && notest[0] != '1') {
        if (!wifi_probe_sta_credentials(new_ssid, new_pass)) {
            send_page(req, HTTPD_400, "Not saved",
                      "<p>Could not connect with that network name and password (check both, and "
                      "the signal). Nothing was changed.</p><p><a href='/config'>Back to Configure</a></p>");
            return ESP_OK;
        }
    }

    snprintf(s_cfg_ssid, sizeof(s_cfg_ssid), "%.31s", new_ssid);
    snprintf(s_cfg_pass, sizeof(s_cfg_pass), "%s", new_pass);
    cfg_save_str("ssid", s_cfg_ssid);
    cfg_save_str("pass", s_cfg_pass);
    bool extra_changed = false;
    for (int i = 1; i < WIFI_SLOTS; i++) {
        char *es = s_extra_ssid[i - 1], *ep = s_extra_pass[i - 1];
        if (brem[i] && es[0]) {
            es[0] = ep[0] = '\0';
            extra_changed = true;
        } else if (bset[i] && !brem[i] && (strcmp(es, bs[i]) != 0 || bp[i][0])) {
            if (strcmp(es, bs[i]) != 0 && !bp[i][0]) ep[0] = '\0';    /* new name, no password = open */
            snprintf(es, sizeof(s_extra_ssid[0]), "%s", bs[i]);
            if (bp[i][0]) snprintf(ep, sizeof(s_extra_pass[0]), "%s", bp[i]);
            extra_changed = true;
        }
    }
    if (extra_changed) {
        wifi_extra_save();
        ESP_LOGI(TAG, "setup page: backup networks saved");
    }

    send_page(req, NULL, "WiFi saved",
              "<p>The map is restarting to join the network. Its LEDs show the startup "
              "rainbow, then the connection progress. Give it about 30 seconds.</p>");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/** One weather request for an airport list (not the saved one). Returns how many airports the
 *  server answered for (-1 = not reached / bad reply); codes without a current report go into
 *  missing (space-separated). */
static int metar_probe(const slot_t *slots, int n, char *missing, size_t msz)
{
    missing[0] = '\0';
    char *url = malloc(URL_MAX);
    resp_ctx_t ctx = { .buf = malloc(RESP_MAX_BYTES), .cap = RESP_MAX_BYTES };
    if (!url || !ctx.buf) { free(url); free(ctx.buf); return -1; }
    ctx.buf[0] = '\0';
    size_t off = (size_t)snprintf(url, URL_MAX, "%s", s_cfg_url_pfx);
    airport_ids(slots, n, url + off, URL_MAX - off);
    off = strlen(url);
    snprintf(url + off, URL_MAX - off, "&boardid=%s&output=csv&fw=%s&check=1", s_boardid, FIRMWARE_VERSION);
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_event_handler, .user_data = &ctx, .timeout_ms = WEATHER_TIMEOUT_MS,
        .buffer_size_tx = 2048,
    };
    if (strncasecmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    free(url);
    if (err != ESP_OK || status != 200) { free(ctx.buf); return -1; }

    /* which codes came back with a current category */
    char ok[LED_MAX][5];
    int nok = 0, rows = 0;
    char *save = NULL;
    for (char *line = strtok_r(ctx.buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        csv_row_t row;
        if (!parse_csv_line(line, &row)) continue;
        rows++;
        if (row.cat[0] && !row.stale && nok < LED_MAX) snprintf(ok[nok++], 5, "%s", row.icao);
    }
    free(ctx.buf);
    size_t mo = 0;
    for (int i = 0; i < n; i++) {
        if (!slots[i].icao[0]) continue;
        bool good = false, dup = false;
        for (int k = 0; k < nok && !good; k++) good = strcmp(ok[k], slots[i].icao) == 0;
        for (int j = 0; j < i && !dup; j++) dup = strcmp(slots[j].icao, slots[i].icao) == 0;
        if (!good && !dup && mo + 6 < msz)
            mo += (size_t)snprintf(missing + mo, msz - mo, "%s%s", mo ? " " : "", slots[i].icao);
    }
    ESP_LOGI(TAG, "airport list check: %d rows, missing: %s", rows, missing[0] ? missing : "none");
    return rows;
}

static esp_err_t handler_save_airports(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    size_t bsz = AIRPORTS_MAX_TEXT * 3 + 64;          /* URL-encoded */
    char *body = malloc(bsz), *text = malloc(AIRPORTS_MAX_TEXT * 2);
    slot_t *sl = calloc(LED_MAX, sizeof(slot_t));
    if (!body || !text || !sl) {
        free(body); free(text); free(sl);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    read_body(req, body, bsz);
    if (!form_get(body, "airports", text, AIRPORTS_MAX_TEXT * 2)) text[0] = '\0';
    free(body);
    int n = 0;
    char perr[80], msg[1200];
    if (!airports_parse(text, sl, &n, perr, sizeof(perr))) {
        snprintf(msg, sizeof(msg), "<p>Nothing was saved: %s.</p><p><a href='/config'>Back to Configure</a></p>", perr);
        free(text); free(sl);
        send_page(req, HTTPD_400, "Airports not saved", msg);
        return ESP_OK;
    }
    free(text);
    airports_set(sl, n);

    int lit = 0;
    for (int i = 0; i < n; i++) lit += sl[i].icao[0] != '\0';
    char missing[400];
    bool online = !s_ap_mode && (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT);
    int chk = online && lit ? metar_probe(sl, n, missing, sizeof(missing)) : -2;
    free(sl);
    size_t mo = (size_t)snprintf(msg, sizeof(msg), "<p>Saved: %d LEDs, %d of them airports. The map shows the "
                                 "new list after the next weather check (a few seconds).</p>", n, lit);
    if (chk >= 0 && missing[0])
        snprintf(msg + mo, sizeof(msg) - mo, "<p><b>No current weather report</b> for: %s. Check those codes; "
                 "they show dim warm white until they report. (Small airports without weather stations never "
                 "report; use OFF for those LEDs if you want them dark.)</p>", missing);
    else if (chk >= 0)
        snprintf(msg + mo, sizeof(msg) - mo, "<p>Every airport in the list has a current weather report.</p>");
    else if (chk == -1)
        snprintf(msg + mo, sizeof(msg) - mo, "<p>The weather server couldn't be reached to check the codes.</p>");
    else if (lit)
        snprintf(msg + mo, sizeof(msg) - mo, "<p>The codes are checked once the map is on your WiFi: dim warm "
                 "white LEDs mean no report.</p>");
    mo = strlen(msg);
    snprintf(msg + mo, sizeof(msg) - mo, "<p><a href='/airports'>Airport list</a> &middot; <a href='/config'>Back to Configure</a></p>");
    send_page(req, NULL, "Airports saved", msg);
    return ESP_OK;
}

static esp_err_t handler_save_channel(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    char body[64];
    read_body(req, body, sizeof(body));

    char ch[8] = "";
    get_param(body, "ota_ch", ch, sizeof(ch));
    snprintf(s_cfg_ota_ch, sizeof(s_cfg_ota_ch), "%s", strcmp(ch, "beta") == 0 ? "beta" : "");
    cfg_save_str("ota_ch", s_cfg_ota_ch);

    char msg[200];
    snprintf(msg, sizeof(msg), "<p>Update channel is now <b>%s</b>. Used at the next update check "
             "(2 min after boot, then every %u min).</p>", s_cfg_ota_ch[0] ? "beta" : "stable", s_ota_int_min);
    send_page(req, NULL, "Channel saved", msg);
    return ESP_OK;
}

/** /test/ endpoints only on bench units: NVS test_hooks=1 (set over USB, never by the config file). */
static bool test_hooks_ok(httpd_req_t *req)
{
    if (s_test_hooks) return true;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "test hooks off\n");
    return false;
}

/** Test hook: apply a config JSON body exactly as if fetched from the config URL. */
static esp_err_t handler_test_config(httpd_req_t *req)
{
    if (!test_hooks_ok(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain");
    if (req->content_len > CFG_MAX_BYTES) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "over 4 KB\n");
        return ESP_OK;
    }
    char *body = calloc(1, CFG_MAX_BYTES + 1);
    if (!body) return ESP_FAIL;
    int n = read_body(req, body, CFG_MAX_BYTES + 1);
    ESP_LOGI(TAG, "[cfg] test hook: applying POSTed config (%d bytes)", n);
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool reboot = cfg_apply_text(body);
    char res[sizeof(s_cfg_result) + 2];
    snprintf(res, sizeof(res), "%s\n", s_cfg_result);
    bool rejected = s_cfg_status == CFG_ST_REJECTED;
    xSemaphoreGive(s_cfg_mutex);
    free(body);
    if (rejected) httpd_resp_set_status(req, HTTPD_400);
    httpd_resp_sendstr(req, res);
    if (reboot) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ESP_OK;
}

/** Test hook: run the config (what=config) or firmware (what=ota) check now. */
static esp_err_t handler_test_check(httpd_req_t *req)
{
    if (!test_hooks_ok(req)) return ESP_OK;
    char body[32] = "", what[8] = "";
    read_body(req, body, sizeof(body));
    get_param(body, "what", what, sizeof(what));
    TaskHandle_t t = strcmp(what, "config") == 0 ? s_cfg_task : strcmp(what, "ota") == 0 ? s_ota_task : NULL;
    httpd_resp_set_type(req, "text/plain");
    if (!t) {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_sendstr(req, "what=config|ota\n");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "test hook: %s check now", what);
    xTaskNotifyGive(t);
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

/** Test hook: apply made-up weather (same CSV as the server, one line per airport) until the
 *  next real fetch. */
static esp_err_t handler_test_weather(httpd_req_t *req)
{
    if (!test_hooks_ok(req)) return ESP_OK;
    char *body = malloc(RESP_MAX_BYTES);
    if (!body) return httpd_resp_send_500(req);
    read_body(req, body, RESP_MAX_BYTES);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int rows = apply_reply(body);
    if (rows > 0) { s_state.has_data = true; s_state.last_fetch_ms = now_ms(); }
    xSemaphoreGive(s_state_lock);
    free(body);
    s_leds_dirty = true;
    httpd_resp_set_type(req, "text/plain");
    if (rows == 0) {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_sendstr(req, "no weather CSV lines\n");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "test hook: weather, %d rows", rows);
    char r[32];
    snprintf(r, sizeof(r), "ok %d\n", rows);
    httpd_resp_sendstr(req, r);
    return ESP_OK;
}

/** Test hook: simulate holding the button for ms (50..15000) through the normal debounce path. */
static esp_err_t handler_test_button(httpd_req_t *req)
{
    if (!test_hooks_ok(req)) return ESP_OK;
    char body[32] = "", q[32] = "", val[8] = "";
    read_body(req, body, sizeof(body));
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && !get_param(body, "ms", val, sizeof(val)))
        get_param(q, "ms", val, sizeof(val));
    if (!val[0]) get_param(body, "ms", val, sizeof(val));
    int ms = atoi(val);
    if (ms < 50 || ms > 15000) {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_sendstr(req, "ms must be 50..15000\n");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "simulated button press %d ms", ms);
    s_btn_sim_until_ms = now_ms() + ms;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

static esp_err_t handler_save_url(httpd_req_t *req)
{
    if (!save_allowed(req, true)) return ESP_OK;
    char body[256];
    read_body(req, body, sizeof(body));

    char url_prefix[128];
    if (get_param(body, "url_prefix", url_prefix, sizeof(url_prefix)) && url_prefix[0]) {
        snprintf(s_cfg_url_pfx, sizeof(s_cfg_url_pfx), "%s", url_prefix);
        cfg_save_str("url_prefix", s_cfg_url_pfx);
    }

    send_page(req, NULL, "URL prefix saved",
              "<p>Used from the next weather check. If the map stops updating, put the old "
              "prefix back here.</p>");
    return ESP_OK;
}

/* Status page -/+ : same-site check only (like the LED test), saved 30 s after the last change
   like a button tap. During night dimming the first press cancels dimming until morning. */
static esp_err_t handler_bri_step(httpd_req_t *req)
{
    if (!save_allowed(req, false)) return ESP_OK;
    char body[48], dir[8] = "", back[8] = "";
    read_body(req, body, sizeof(body));
    get_param(body, "dir", dir, sizeof(dir));
    get_param(body, "back", back, sizeof(back));
    int step = strcmp(dir, "up") == 0 ? 1 : strcmp(dir, "down") == 0 ? -1 : 0;
    if (!step) {
        send_page(req, HTTPD_400, "Brightness", "<p>Use dir = up or down.</p>");
        return ESP_OK;
    }
    if (s_night_now && !s_night_override) {
        s_night_override = true;
        ESP_LOGI(TAG, "brightness (web %s) -> night dimming off until morning", dir);
    } else {
        int r = s_bri_idx + step;
        if (r >= 0 && r <= 8) {
            s_bri_idx = (uint8_t)r;
            s_bri_last_tap = now_ms();
            ESP_LOGI(TAG, "brightness (web %s) -> %d", dir, s_bri_idx);
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", strcmp(back, "config") == 0 ? "/config" : "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* -/+ for no_report_day_percent / no_report_night_percent in 10 % steps: same-site check only
   (like brightness), saved right away. */
static esp_err_t handler_nr_step(httpd_req_t *req)
{
    if (!save_allowed(req, false)) return ESP_OK;
    char body[64], which[8] = "", dir[8] = "", back[8] = "";
    read_body(req, body, sizeof(body));
    get_param(body, "which", which, sizeof(which));
    get_param(body, "dir", dir, sizeof(dir));
    get_param(body, "back", back, sizeof(back));
    int step = strcmp(dir, "up") == 0 ? 10 : strcmp(dir, "down") == 0 ? -10 : 0;
    const char *key = strcmp(which, "day") == 0 ? "no_report_day_percent" :
                      strcmp(which, "night") == 0 ? "no_report_night_percent" : NULL;
    if (!step || !key) {
        send_page(req, HTTPD_400, "No-report brightness", "<p>Use which = day or night, dir = up or down.</p>");
        return ESP_OK;
    }
    const setting_t *st = setting_by_key(key);
    int v = (*st->val + step) / 10 * 10;           /* back onto the 10 % grid */
    if (v < st->lo) v = st->lo;
    if (v > st->hi) v = st->hi;
    bool rb = false;
    setting_apply(st, v, &rb);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", strcmp(back, "config") == 0 ? "/config" : "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_pattern(httpd_req_t *req)
{
    if (!save_allowed(req, false)) return ESP_OK;
    static const char *const names[]  = { "off", "colors", "order", "categories", "one" };
    static const char *const titles[] = { "LED test stopped", "LED test: colors", "LED test: order",
                                          "LED test: categories", "Find an LED" };
    char body[96], mode[16] = "", sec[8] = "", led[8] = "";
    read_body(req, body, sizeof(body));
    get_param(body, "mode", mode, sizeof(mode));
    get_param(body, "led", led, sizeof(led));
    int seconds = get_param(body, "seconds", sec, sizeof(sec)) ? atoi(sec) : 30;
    int m = -1, which = atoi(led);
    for (int k = 0; k < 5; k++) if (strcmp(mode, names[k]) == 0) m = k;
    if (m < 0 || seconds < 5 || seconds > 300 || (m == PAT_ONE && (which < 1 || which > LED_MAX))) {
        send_page(req, HTTPD_400, "LED test", "<p>Unknown test. Use mode = colors, order, "
                  "categories, one (with led = 1..) or off, and seconds = 5 to 300.</p>");
        return ESP_OK;
    }
    s_pattern_led = which - 1;
    s_pattern_start_ms = now_ms();
    s_pattern_until_ms = now_ms() + (int64_t)seconds * 1000;
    s_pattern          = m;
    ESP_LOGI(TAG, "LED test pattern %s for %d s", names[m], seconds);

    char what[700];
    if (m == PAT_COLORS)
        snprintf(what, sizeof(what),
                 "<p><b>Expect:</b> all %d LEDs together turn <b>red</b>, then <b>green</b>, then "
                 "<b>blue</b>, then <b>white</b>, 1.5 s each, and repeat.</p>"
                 "<p><b>If not:</b> red and green swapped (or any other mix-up) means the color "
                 "order is wrong; set <code>led_order</code> (now %s). White that looks pink or "
                 "blue-ish usually means a weak power supply.</p>",
                 vis_n(), k_led_order_opts[s_led_order]);
    else if (m == PAT_ORDER)
        snprintf(what, sizeof(what),
                 "<p><b>Expect:</b> one <b>white</b> LED at a time, starting at <b>LED 1</b> (the "
                 "end wired to the board) and stepping one LED every half second to <b>LED %d</b>, "
                 "then starting over at LED 1. All other LEDs are off.</p>"
                 "<p><b>If not:</b> LEDs at the far end that never light mean the airport list (%d "
                 "lines) is shorter than the string. A dot that jumps around means a bad data "
                 "connection between two LEDs.</p>",
                 vis_n(), s_slot_n);
    else if (m == PAT_CATEGORIES)
        snprintf(what, sizeof(what),
                 "<p><b>Expect:</b> all LEDs show each flight-category color for 2 s, in order: "
                 "<span class=sw style='background:#%06lx'></span> VFR, "
                 "<span class=sw style='background:#%06lx'></span> MVFR, "
                 "<span class=sw style='background:#%06lx'></span> IFR, "
                 "<span class=sw style='background:#%06lx'></span> LIFR, and repeat.</p>"
                 "<p>These are the colors the map uses for weather, including any color "
                 "overrides.</p>",
                 (unsigned long)sw_hex(s_col[0] ? (unsigned)(s_col[0] & 0xffffff) : 0x007f00u),
                 (unsigned long)sw_hex(s_col[1] ? (unsigned)(s_col[1] & 0xffffff) : 0x00007fu),
                 (unsigned long)sw_hex(s_col[2] ? (unsigned)(s_col[2] & 0xffffff) : 0x7f0000u),
                 (unsigned long)sw_hex(s_col[3] ? (unsigned)(s_col[3] & 0xffffff) : 0xff16c7u));
    else if (m == PAT_ONE)
        snprintf(what, sizeof(what),
                 "<p><b>LED %d</b> (%s) blinks white; every other LED is off.</p>"
                 "<p><a href='/airports'>Back to the airport list</a></p>",
                 which, which <= s_slot_n && s_slots[which - 1].icao[0] ? s_slots[which - 1].icao : "dark in the list");
    else
        snprintf(what, sizeof(what), "<p>The map is back to showing the weather.</p>");

    const size_t cap = 6144;
    char *h = (char *)malloc(cap);
    if (!h) {
        httpd_resp_sendstr(req, "LED test started");
        return ESP_OK;
    }
    size_t o = 0;
    h[0] = '\0';
    page_head(h, cap, &o, titles[m], NULL);
    appendf(h, cap, &o, "<div class=card>%s", what);
    if (m)
        appendf(h, cap, &o,
                "<p id=t style='font-size:1.3em;font-weight:bold;margin:10px 0 4px'>Time left: %d s</p>"
                "<p><small>Brightness is at least 25%% during the test, even if the map is dimmed or off.</small></p>"
                "<script>var n=%d,e=document.getElementById('t');function f(){e.textContent=n>0?"
                "('Time left: '+n+' s'):'Done; the map is back to showing the weather.';"
                "if(n-->0)setTimeout(f,1000);}f();</script>",
                seconds, seconds);
    appendf(h, cap, &o, "</div><div class=card><h2>Run a test (30 s)</h2>");
    static const char *const label[] = { "Stop", "Colors", "Order", "Categories" };
    for (int i = 1; i <= 3; i++) {                      /* Colors, Order, Categories; each ends by itself */
        appendf(h, cap, &o, "<form action='/pattern' method='POST'><input type='hidden' name='mode' "
                            "value='%s'><button>%s</button></form>", names[i], label[i]);
    }
    appendf(h, cap, &o, "</div>%s", k_page_nav);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    http_send_html_chunked(req, h);
    free(h);
    return ESP_OK;
}

static esp_err_t handler_clear_flags(httpd_req_t *req)
{
    if (!save_allowed(req, false)) return ESP_OK;
    dbg_clear_all();
    send_page(req, NULL, "Flags cleared", "<p>All debug flags are off. The next weather check "
              "sends <code>debug=0x0</code>.</p>");
    return ESP_OK;
}

static void httpd_alive_ping_work(void *arg) {
    (void)arg;
}

static void check_httpd_alive(void)
{
    if (!s_httpd)
        return;
    esp_err_t e = httpd_queue_work(s_httpd, httpd_alive_ping_work, NULL);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "httpd_queue_work failed (%s)", esp_err_to_name(e));
        dbg_set(DBG_HTTPD_DEAD);
    }
}

/** Captive portal: in AP mode any unknown URL (phone OS connectivity checks) → /config.
 *  iOS needs a body with the redirect to treat it as a portal. */
static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (!s_ap_mode) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/config");
    httpd_resp_sendstr(req, "Redirect to setup");
    return ESP_OK;
}

/** Captive DNS: while in AP mode, answer every A query with the AP address (192.168.4.1);
 *  other query types get an empty answer. Ignores queries when not in AP mode. */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS: socket/bind failed");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&src, &slen);
        if (!s_ap_mode)
            continue;
        esp_netif_ip_info_t ip = { 0 };
        if (s_ap_netif) esp_netif_get_ip_info(s_ap_netif, &ip);
        int len = captive_dns_reply(buf, n, sizeof(buf), ip.ip.addr);
        if (len < 0)
            continue;
        sendto(sock, buf, len, 0, (struct sockaddr *)&src, slen);
    }
}

// ---------- radio / antenna check (POST /radio_check) ----------

/* Steps TX power 17/11/8 dBm on the live link (no rejoin): a bad antenna or match shows up as a link
 * that gets WORSE at higher power (seen on a bench board); judged at 11, 17 only warns. A disconnect during the check ends it, is not counted as
 * a WiFi drop and does not move the autotune; the normal power is restored before rejoining. */
#define RC_PINGS      20
#define RC_UP_BYTES   (256 * 1024)
enum { RCS_IDLE, RCS_RUNNING, RCS_DONE };
static volatile int      s_rc_state;
static rc_row_t          s_rc_rows[RC_LEVELS * RC_ROUNDS];
static volatile int      s_rc_n;
static char              s_rc_sink[80];
static char              s_rc_why[160];
static rc_result_t       s_rc_result;
static int64_t           s_rc_ms;
static SemaphoreHandle_t s_rc_ping_done;
static volatile uint32_t s_rc_rtt_sum, s_rc_rtt_max, s_rc_recv;

static void rc_ping_ok(esp_ping_handle_t h, void *a)
{
    uint32_t t = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    s_rc_rtt_sum += t;
    if (t > s_rc_rtt_max) s_rc_rtt_max = t;
    s_rc_recv++;
}

static void rc_ping_end(esp_ping_handle_t h, void *a) { xSemaphoreGive(s_rc_ping_done); }

static void rc_ping(rc_row_t *r)
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.gw.addr == 0) return;
    esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
    ip_addr_set_ip4_u32(&c.target_addr, ip.gw.addr);   /* IPv4-only lwIP: no .type/.u_addr */
    c.count       = RC_PINGS;
    c.interval_ms = 50;
    c.timeout_ms  = 1000;
    esp_ping_callbacks_t cb = { .on_ping_success = rc_ping_ok, .on_ping_end = rc_ping_end };
    esp_ping_handle_t h;
    s_rc_rtt_sum = s_rc_rtt_max = s_rc_recv = 0;
    xSemaphoreTake(s_rc_ping_done, 0);
    if (esp_ping_new_session(&c, &cb, &h) != ESP_OK) return;
    esp_ping_start(h);
    xSemaphoreTake(s_rc_ping_done, pdMS_TO_TICKS(30000));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    r->sent   = RC_PINGS;
    r->recv   = (int)s_rc_recv;
    r->avg_ms = s_rc_recv ? (int)(s_rc_rtt_sum / s_rc_recv) : 0;
    r->max_ms = (int)s_rc_rtt_max;
}

/** kB/s to `sink`/up, 0 = failed. */
static int rc_upload(const char *sink)
{
    static char buf[2048];
    char url[96];
    snprintf(url, sizeof(url), "%s/up", sink);
    esp_http_client_config_t c = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 15000 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return 0;
    int64_t t0 = now_ms();
    int kbps = 0;
    if (esp_http_client_open(h, RC_UP_BYTES) == ESP_OK) {
        int sent = 0;
        while (sent < RC_UP_BYTES && s_rc_active) {
            int w = esp_http_client_write(h, buf, sizeof(buf));
            if (w <= 0) break;
            sent += w;
        }
        if (sent == RC_UP_BYTES && esp_http_client_fetch_headers(h) >= 0 &&
            esp_http_client_get_status_code(h) == 200) {
            int64_t dt = now_ms() - t0;
            kbps = dt > 0 ? (int)((int64_t)RC_UP_BYTES / dt) : 0;   /* bytes/ms = kB/s */
        }
    }
    esp_http_client_cleanup(h);
    return kbps;
}

static void radio_check_task(void *arg)
{
    static const int lv[RC_LEVELS] = { 17, 11, 8 };
    int64_t t0 = now_ms();
    for (int round = 0; round < RC_ROUNDS && s_rc_active; round++)
        for (int k = 0; k < RC_LEVELS && s_rc_active; k++) {
            rc_row_t *r = &s_rc_rows[s_rc_n];
            memset(r, 0, sizeof(*r));
            r->dbm     = lv[k];
            r->up_kBps = -1;
            esp_wifi_set_max_tx_power((int8_t)(lv[k] * 4));
            vTaskDelay(pdMS_TO_TICKS(300));
            rc_ping(r);
            if (s_rc_active && s_rc_sink[0]) r->up_kBps = rc_upload(s_rc_sink);
            if (!s_rc_active) r->dropped = s_rc_drop_reason ? s_rc_drop_reason : -1;
            ESP_LOGI(TAG, "radio check %d dBm: ping %d/%d avg %d max %d ms, up %d kB/s%s", r->dbm, r->recv,
                     r->sent, r->avg_ms, r->max_ms, r->up_kBps, r->dropped ? ", link dropped" : "");
            s_rc_n++;
        }
    if (s_rc_active) {
        s_rc_active = false;
        wifi_apply_tx_power();   /* else the disconnect handler already did */
    }
    s_rc_result = rc_verdict(s_rc_rows, s_rc_n, s_rc_why, sizeof(s_rc_why));
    s_rc_ms     = now_ms() - t0;
    s_rc_state  = RCS_DONE;
    ESP_LOGW(TAG, "radio check: %s: %s (%lld ms)", rc_result_name(s_rc_result), s_rc_why, (long long)s_rc_ms);
    vTaskDelete(NULL);
}

static esp_err_t handler_radio_check_get(httpd_req_t *req)
{
    char *js = (char *)malloc(1536);
    if (!js) return httpd_resp_send_500(req);
    size_t o = 0;
    o += snprintf(js + o, 1536 - o, "{\"state\":\"%s\",\"tx_dbm\":%d",
                  s_rc_state == RCS_RUNNING ? "running" : s_rc_state == RCS_DONE ? "done" : "idle", s_tx.cur);
    if (s_rc_state == RCS_DONE)
        o += snprintf(js + o, 1536 - o, ",\"result\":\"%s\",\"why\":\"%s\",\"ms\":%lld",
                      rc_result_name(s_rc_result), s_rc_why, (long long)s_rc_ms);
    o += snprintf(js + o, 1536 - o, ",\"rows\":[");
    for (int i = 0; i < s_rc_n && o < 1400; i++) {
        const rc_row_t *r = &s_rc_rows[i];
        o += snprintf(js + o, 1536 - o, "%s{\"dbm\":%d,\"sent\":%d,\"recv\":%d,\"avg_ms\":%d,\"max_ms\":%d,"
                      "\"up_kBps\":%d,\"dropped\":%d}", i ? "," : "", r->dbm, r->sent, r->recv, r->avg_ms,
                      r->max_ms, r->up_kBps, r->dropped);
    }
    snprintf(js + o, 1536 - o, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, js);
    free(js);
    return ESP_OK;
}

static esp_err_t handler_radio_check_post(httpd_req_t *req)
{
    if (!save_allowed(req, false)) return ESP_OK;
    char body[160] = "", sink[sizeof(s_rc_sink)] = "";
    read_body(req, body, sizeof(body));
    get_param(body, "sink", sink, sizeof(sink));
    const char *busy = s_rc_state == RCS_RUNNING ? "a radio check is already running"
                     : s_ap_mode ? "the map is in setup mode"
                     : !(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) ? "WiFi is not connected"
                     : s_tx.probe != TXP_IDLE ? "the TX autotune is testing a step up; try again in a few minutes"
                     : NULL;
    if (!busy && sink[0] && !rc_sink_ok(sink)) busy = "sink must be http://<LAN IPv4 address>[:port]";
    if (busy) {
        char js[160];
        snprintf(js, sizeof(js), "{\"started\":false,\"why\":\"%s\"}", busy);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, js);
        return ESP_OK;
    }
    if (!s_rc_ping_done) s_rc_ping_done = xSemaphoreCreateBinary();
    snprintf(s_rc_sink, sizeof(s_rc_sink), "%s", sink);
    s_rc_n           = 0;
    s_rc_drop_reason = 0;
    s_rc_state       = RCS_RUNNING;
    s_rc_active      = true;
    if (xTaskCreate(radio_check_task, "rcheck", 6144, NULL, 3, NULL) != pdPASS) {
        s_rc_active = false;
        s_rc_state  = RCS_IDLE;
        return httpd_resp_send_500(req);
    }
    ESP_LOGI(TAG, "radio check started%s%s", sink[0] ? ", sink " : "", sink);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"started\":true}");
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Root page locals now <3K + malloc'd HTML; chunk send uses small stack per syscall */
    cfg.stack_size = 12288;
    cfg.max_uri_handlers = 26;    /* 23 registered; default 8 is too few */
    cfg.lru_purge_enable = true;  /* captive-portal phones open many sockets */

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGE(TAG, "httpd_start failed");
        dbg_set(DBG_HTTPD_START_FAIL);
        return;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/debug",        .method = HTTP_GET,  .handler = handler_debug },
        { .uri = "/config",       .method = HTTP_GET,  .handler = handler_config },
        { .uri = "/save_wifi",    .method = HTTP_POST, .handler = handler_save_wifi },
        { .uri = "/save_airports", .method = HTTP_POST, .handler = handler_save_airports },
        { .uri = "/airports",     .method = HTTP_GET,  .handler = handler_airports },
        { .uri = "/log",          .method = HTTP_GET,  .handler = handler_log },
        { .uri = "/save_url",     .method = HTTP_POST, .handler = handler_save_url },
        { .uri = "/bri_step",        .method = HTTP_POST, .handler = handler_bri_step },
        { .uri = "/nr_step",         .method = HTTP_POST, .handler = handler_nr_step },
        { .uri = "/clear_flags",     .method = HTTP_POST, .handler = handler_clear_flags },
        { .uri = "/save_channel",    .method = HTTP_POST, .handler = handler_save_channel },
        { .uri = "/test/button",     .method = HTTP_POST, .handler = handler_test_button },
        { .uri = "/test/config",     .method = HTTP_POST, .handler = handler_test_config },
        { .uri = "/test/check",      .method = HTTP_POST, .handler = handler_test_check },
        { .uri = "/test/weather",    .method = HTTP_POST, .handler = handler_test_weather },
        { .uri = "/unlock",          .method = HTTP_GET,  .handler = handler_unlock },
        { .uri = "/radio_check",     .method = HTTP_GET,  .handler = handler_radio_check_get },
        { .uri = "/radio_check",     .method = HTTP_POST, .handler = handler_radio_check_post },
        { .uri = "/pattern",         .method = HTTP_POST, .handler = handler_pattern },
        { .uri = "/save_settings",   .method = HTTP_POST, .handler = handler_save_settings },
        { .uri = "/save_admin",      .method = HTTP_POST, .handler = handler_save_admin },
        { .uri = "/action",          .method = HTTP_POST, .handler = handler_action },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_handler);
    ESP_LOGI(TAG, "web server started");
}

// ---------- AP mode + button ----------

static void enter_ap_mode(void)
{
    ESP_LOGI(TAG, "entering AP mode");
    s_ap_mode    = true;
    s_ap_clients = 0;

    esp_wifi_disconnect();
    esp_wifi_stop();

    wifi_start_config_ap();
}

static void maybe_save_brightness(void)
{
    if (s_bri_idx != s_bri_saved_idx &&
        s_bri_last_tap > 0 &&
        (now_ms() - s_bri_last_tap > 30000)) {
        cfg_save_u8("bri_idx", s_bri_idx);
        s_bri_saved_idx = s_bri_idx;
        ESP_LOGI(TAG, "brightness index %d saved", s_bri_idx);
    }
}

/* BOOT button (GPIO 0, pressed = low). Released before BTN_LONG_MIN_MS: saving unlocked for
 * UNLOCK_MS. Held BTN_LONG_MIN_MS: setup network (or restart, when already in setup mode). */
static void button_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int  integrator = 0;
    bool btn_down   = false;
    bool pressed    = false;
    int64_t press_ms = 0;
    bool press_physical = false;

    while (1) {
        int level = gpio_get_level(BTN_GPIO);
        s_btn_gpio_level = level;
        int phys = level == 0;
        int raw = phys || now_ms() < s_btn_sim_until_ms;
        if (phys && pressed) press_physical = true;

        if (raw == 1) {
            if (integrator < BTN_DEBOUNCE_N) integrator++;
        } else {
            if (integrator > 0) integrator--;
        }

        bool now_down = (integrator == BTN_DEBOUNCE_N);
        bool now_up   = (integrator == 0);

        if (now_down && !btn_down) {
            btn_down  = true;
            pressed   = true;
            press_ms  = now_ms();
            press_physical = phys;
            ESP_LOGI(TAG, "button down (GPIO%d low = pressed)", BTN_GPIO);
        } else if (now_up && btn_down) {
            btn_down = false;
            s_btn_last_ms       = now_ms() - press_ms;
            s_btn_last_at_ms    = now_ms();
            s_btn_last_physical = press_physical;
            if (pressed) {                       /* released before the long hold */
                pressed = false;
                s_unlock_until_ms  = now_ms() + UNLOCK_MS;
                s_btn_ack_until_ms = now_ms() + BTN_ACK_MS;
                ESP_LOGI(TAG, "button up %lld ms -> saving unlocked for %d min",
                         (long long)s_btn_last_ms, UNLOCK_MS / 60000);
            }
        }

        if (btn_down && pressed) {
            int64_t held = now_ms() - press_ms;
            if (held >= BTN_LONG_MIN_MS) {
                pressed = false;
                s_btn_ack_until_ms = now_ms() + BTN_ACK_MS;
                if (!s_ap_mode) {
                    ESP_LOGI(TAG, "button held %lld ms -> entering AP mode", (long long)held);
                    enter_ap_mode();
                } else {
                    ESP_LOGI(TAG, "long hold in AP mode -> reboot");
                    esp_restart();
                }
            }
        }

        maybe_save_brightness();
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

// ---------- Watchdog ----------

static void watchdog_task(void *arg)
{
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = TWDT_TIMEOUT_MS,
        /* Same idle mask as sdkconfig (CHECK_IDLE_TASK_CPU0/1): changing it unsubscribes the
         * idle tasks while the other core's idle hook may be mid-feed. */
        .idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,
        .trigger_panic  = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&wdt_cfg);
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int64_t boot_ms = now_ms();
    int64_t fallback_retry_ms = 0;

    while (1) {
        esp_task_wdt_reset();

        dbg_clear_expired();
        maybe_save_brightness();
        check_httpd_alive();

        bool sta_up = (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
        if (!s_rc_active)   /* no step-up test while the radio check owns the TX level */
            tx_do(TX_CALL(tx_tune_tick(&s_tx, sta_up && !s_ap_mode, now_ms())), "timer");
        if (!s_ap_mode && wifi_any_saved() && !sta_up && s_wifi_down_since_ms &&
            now_ms() - s_wifi_down_since_ms > s_fallback_ap_ms) {
            fallback_ap_start();
            fallback_retry_ms = now_ms();
        } else if (s_fallback_ap && !s_wifi_probe_active) {
            if (sta_up) {
                fallback_ap_stop();
            } else if (s_ap_clients == 0 && now_ms() - fallback_retry_ms >= FALLBACK_RETRY_MS) {
                /* Only while no phone is on the AP: STA scans hop channels and drop AP clients */
                fallback_retry_ms = now_ms();
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                wifi_sta_use_slot(wifi_next_slot(s_wifi_slot));
                esp_wifi_connect();
            }
        }

        /* Fallback with nobody connected still gets the stale reboot (fresh WiFi stack). An empty
         * airport list never fetches, so it never counts as stale. */
        if ((!s_ap_mode || (s_fallback_ap && s_ap_clients == 0)) && s_slot_n > 0) {
            int64_t last;
            bool has;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            last = s_state.last_fetch_ms;
            has  = s_state.has_data;
            xSemaphoreGive(s_state_lock);

            int64_t ref   = has ? last : boot_ms;
            int64_t since = now_ms() - ref;

            if (since > HARD_REBOOT_MS) {
                ESP_LOGE(TAG, "[stale-fetch] no successful METAR fetch for %lld ms — rebooting",
                         (long long)since);
                dbg_set(DBG_STALE_FETCH_REBOOT);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// ---------- log ring (GET /log) ----------

static vprintf_like_t s_log_prev;

/** Console as before, plus warnings and errors into a RAM ring for /log. With log version 1 each
 *  line is one call whose format starts with the level letter (after an optional color escape),
 *  so only W/E lines pay for the second format (this runs on the logging task's stack). */
static bool log_fmt_is_warn(const char *f)
{
    if (f[0] == '\033') { const char *m = strchr(f, 'm'); if (!m) return false; f = m + 1; }
    return (f[0] == 'W' || f[0] == 'E') && f[1] == ' ';
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_log_prev ? s_log_prev(fmt, ap) : vprintf(fmt, ap);
    if (s_logring && log_fmt_is_warn(fmt)) {
        char line[160];
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        const char *p = line;
        if (p[0] == '\033') { const char *m = strchr(p, 'm'); if (m) p = m + 1; }
        if (n > 0 && (p[0] == 'W' || p[0] == 'E') && p[1] == ' ') {
            size_t len = strlen(p);
            taskENTER_CRITICAL(&s_logring_mux);
            for (size_t i = 0; i < len; i++) {
                if (p[i] == '\033') continue;
                s_logring[s_logring_pos++] = p[i];
                if (s_logring_pos >= LOGRING_SIZE) { s_logring_pos = 0; s_logring_wrapped = true; }
            }
            taskEXIT_CRITICAL(&s_logring_mux);
        }
    }
    va_end(ap2);
    return r;
}

// ---------- crash summary ----------

/** A crash dump from before this boot: keep a one-line summary in NVS, then erase the dump so
 *  the next crash is new. Decode with addr2line and that version's ELF. */
static void crash_summary_take(void)
{
    if (esp_core_dump_image_check() != ESP_OK) return;
    esp_core_dump_summary_t *cs = malloc(sizeof(*cs));
    if (cs && esp_core_dump_get_summary(cs) == ESP_OK) {
        const esp_app_desc_t *app = esp_app_get_description();
        size_t o = (size_t)snprintf(s_crash, sizeof(s_crash), "v%s, found at boot %lu: task %.16s pc 0x%08lx bt",
                                    app->version, (unsigned long)s_boot_count, cs->exc_task,
                                    (unsigned long)cs->exc_pc);
        for (uint32_t i = 0; i < cs->exc_bt_info.depth && i < 6 && o + 12 < sizeof(s_crash); i++)
            o += (size_t)snprintf(s_crash + o, sizeof(s_crash) - o, " 0x%08lx", (unsigned long)cs->exc_bt_info.bt[i]);
        cfg_save_str("crash", s_crash);
        ESP_LOGW(TAG, "crash dump found: %s", s_crash);
    }
    free(cs);
    esp_core_dump_image_erase();
}

// ---------- entry ----------

static void leds_init(void)
{
    static const led_model_t models[] = { LED_MODEL_WS2812, LED_MODEL_SK6812, LED_MODEL_WS2811 };
    const char *order = k_led_order_opts[s_led_order];
    s_led_rgbw = strchr(order, 'W') != NULL;
    led_color_component_format_t fmt = s_led_rgbw ? LED_STRIP_COLOR_COMPONENT_FMT_GRBW
                                                  : LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    fmt.format.r_pos = (uint32_t)(strchr(order, 'R') - order);
    fmt.format.g_pos = (uint32_t)(strchr(order, 'G') - order);
    fmt.format.b_pos = (uint32_t)(strchr(order, 'B') - order);
    if (s_led_rgbw) fmt.format.w_pos = (uint32_t)(strchr(order, 'W') - order);
    led_strip_config_t strip_config = {
        .strip_gpio_num   = s_led_gpio,
        .max_leds         = LED_MAX,
        .color_component_format = fmt,
        .led_model        = models[s_led_model],
        .flags.invert_out = false,
    };
    /* S2: 4 RMT TX channels x 64 symbols. Two blocks for the map, one for the status pixel. */
    led_strip_rmt_config_t rmt_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .mem_block_symbols = 128,
        .flags.with_dma    = false,
    };
    ESP_LOGI(TAG, "LEDs: GPIO %d, %s, order %s", s_led_gpio, k_led_model_opts[s_led_model], order);
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "LED string driver failed to start");
        dbg_set(DBG_LED_INIT_FAIL);
        s_strip = NULL;
    } else {
        led_strip_clear(s_strip);
    }

    gpio_reset_pin(STATUS_PIX_PWR_GPIO);
    gpio_set_direction(STATUS_PIX_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(STATUS_PIX_PWR_GPIO, 1);
    led_strip_config_t pix_cfg = {
        .strip_gpio_num = STATUS_PIX_GPIO, .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t pix_rmt = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000,
                                       .mem_block_symbols = 64 };
    if (led_strip_new_rmt_device(&pix_cfg, &pix_rmt, &s_status_pix) != ESP_OK) {
        ESP_LOGW(TAG, "status pixel not available");
        s_status_pix = NULL;
    }
}

void app_main(void)
{
    s_logring = heap_caps_malloc(LOGRING_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_logring) s_logring = malloc(LOGRING_SIZE);
    s_log_prev = esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "tedder-metar-map v%s starting", FIRMWARE_VERSION);

    // Latch WDT/panic reset reason before NVS init clears anything
    esp_reset_reason_t reason = esp_reset_reason();
    s_reset_reason = (int)reason;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — erasing & retrying", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — erasing NVS partition & retrying", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_dbg_lock  = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();

    cfg_load();
    crash_summary_take();

    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT || reason == ESP_RST_PANIC ||
        reason == ESP_RST_INT_WDT) {
        ESP_LOGW(TAG, "reset reason %d — setting DBG_WDT_FIRED", reason);
        dbg_set(DBG_WDT_FIRED);
    }

    leds_init();
    load_boardid();
#ifdef TMM_ROLLBACK_TEST
    /* Prerelease check: this image must never be marked valid. */
    ESP_LOGE(TAG, "ROLLBACK TEST BUILD: aborting before WiFi so the bootloader rolls back");
    vTaskDelay(pdMS_TO_TICKS(2000));
    abort();
#endif
    s_oled_ok = oled_init() == ESP_OK;
    ESP_LOGI(TAG, "OLED %s", s_oled_ok ? "found" : "not found");
    ESP_LOGI(TAG, "airport list: %d LEDs", s_slot_n);

    wifi_init_sta();
    start_webserver();
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("metarmap");
        mdns_instance_name_set("METAR map");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    xTaskCreate(led_task,      "led",  6144, NULL, 5, NULL);
    xTaskCreate(poll_task,     "poll", 8192, NULL, 4, &s_poll_task);
    xTaskCreate(ota_task,      "ota",  8192, NULL, 3, &s_ota_task);
    xTaskCreate(cfg_task,      "cfg",  8192, NULL, 3, &s_cfg_task);
    xTaskCreate(watchdog_task, "wdt",  4096, NULL, 6, NULL);
    xTaskCreate(button_task,   "btn",  4096, NULL, 4, NULL);
    xTaskCreate(dns_task,      "dns",  4096, NULL, 3, NULL);
    if (s_oled_ok) xTaskCreate(oled_task, "oled", 3072, NULL, 2, NULL);
}
