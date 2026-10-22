/*
 * tedder-metar-map — ESP-IDF firmware
 *
 * FAT on internal flash via TinyUSB MSC + wear levelling (partition "storage").
 * Config is visible on a PC as a USB mass-storage volume when the lun is switched
 * to the host (see /usb web page). App and host cannot use the filesystem at once.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"

extern const uint8_t amazon_root_ca1_pem_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t amazon_root_ca1_pem_end[]   asm("_binary_AmazonRootCA1_pem_end");
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#include "soc/rtc_cntl_reg.h"
#include "ff.h"

#include "oled_ssd1306.h"
#include "tmm_log.h"
#include "tmm_msc.h"
#define FIRMWARE_VERSION "0.4.32"

#define STATSBOX_ENABLED 0
#define OTA_ENABLED      1
#define OTA_URL          "https://tedder.me/esp32/tedder-metar-map.bin"
#define OTA_EVERY_FETCHES 8

#define WIFI_SSID_DEFAULT ""
#define WIFI_PASS_DEFAULT ""
#define WIFI_TIMEOUT_MS   15000

#define METAR_SERVICE_URL "https://fly.tedder.me/metar"
#define FETCH_INTERVAL_SEC (9 * 60)
#define HTTP_TIMEOUT_MS    30000

#define LED_STRIP_GPIO  5
#define LED_STRIP_COUNT 74

#define FS_MOUNT_POINT "/fatfs"
#define CONFIG_FILE    FS_MOUNT_POINT "/config.txt"
#define AIRPORTS_FILE  FS_MOUNT_POINT "/airports.txt"
#define ERROR_LOG_FILE FS_MOUNT_POINT "/errors.txt"

#define WATCHDOG_TIMEOUT_SEC (60 * 60)
#define MAX_AIRPORTS         96
#define MAX_ICAO_LEN         7
#define TMM_WIFI_SSID_BUF    64
#define TMM_WIFI_PASS_BUF    64
#define URL_BUF_SIZE         2048
#define HTTP_RX_BUF_SIZE     8192
#define WEB_POST_MAX         8192

#define DBG_DISPLAY_INIT_FAILED     (1u << 0)
#define DBG_DISPLAY_UPDATE_FAILED   (1u << 1)
#define DBG_WIFI_ENABLE_FAILED      (1u << 2)
#define DBG_METAR_FETCH_FAILED      (1u << 3)
#define DBG_ERROR_LOG_USED          (1u << 4)
#define DBG_HTTP_OUT_OF_RETRIES     (1u << 5)
#define DBG_NTP_FAILED              (1u << 6)
#define DBG_HTTP_RUNTIME            (1u << 7)
#define DBG_HTTP_UNEXPECTED         (1u << 8)
#define DBG_WIFI_CONNECT_FAILED     (1u << 9)
#define DBG_MAIN_LOOP_EXCEPTION     (1u << 10)
#define DBG_NVM_WRITE_FAILED        (1u << 11)
#define DBG_MEMORY_ERROR            (1u << 12)
#define DBG_IPV6_INCOMPAT           (1u << 13)
#define DBG_NEOPIXEL_INIT_FAILED    (1u << 14)
#define DBG_WATCHDOG_RESET          (1u << 15)
#define DBG_REQUESTS_INIT_FAILED    (1u << 16)
#define DBG_OTA_BEGIN_FAILED        (1u << 17)
#define DBG_OTA_PERFORM_FAILED      (1u << 18)
#define DBG_OTA_FINISH_FAILED       (1u << 19)
#define DBG_NEOPIXEL_REFRESH_FAILED (1u << 20)

typedef struct {
    char icao[MAX_ICAO_LEN];
    char cat[8];
    uint8_t r, g, b;
    int age_mins;
    bool stale;
    bool valid;
} airport_led_t;

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static int s_wifi_retry_num;
static uint32_t s_debug_flags;
static airport_led_t s_airports[MAX_AIRPORTS];
static int s_airport_count;
static char s_board_id[13] = "000000000000";
static FILE *s_error_log;
static led_strip_handle_t s_strip;
static int s_boot_leds;  /* progress bar cursor */

/* Light one more violet LED to show boot progress. Writes directly to strip. */
static void boot_progress(void)
{
    if (!s_strip || s_boot_leds >= LED_STRIP_COUNT) {
        return;
    }
    if (s_boot_leds % 2 == 0) {
        led_strip_set_pixel(s_strip, s_boot_leds, 0, 40, 40);   /* cyan */
    } else {
        led_strip_set_pixel(s_strip, s_boot_leds, 15, 0, 40);   /* violet */
    }
    s_boot_leds++;
    led_strip_refresh(s_strip);
}
static httpd_handle_t s_httpd;
static uint32_t s_fetch_count;
static uint32_t s_fetch_fail_count;
static uint32_t s_render_count;
static char s_wifi_ssid[TMM_WIFI_SSID_BUF];
static char s_wifi_pass[TMM_WIFI_PASS_BUF];
static bool s_storage_ok;
static bool s_oled_ok;
static char s_last_http_err[128];
static volatile bool s_ota_requested;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 3

static int64_t millis(void) { return esp_timer_get_time() / 1000; }

static void error_log_close(void)
{
    if (s_error_log) {
        fclose(s_error_log);
        s_error_log = NULL;
    }
}

static void error_log_open(void)
{
    if (!s_storage_ok || tmm_msc_is_usb_mode()) {
        return;
    }
    s_error_log = fopen(ERROR_LOG_FILE, "a");
    if (s_error_log) {
        TMM_L_NOTICE(s_error_log, true, "boot fw=%s boardid=%s", FIRMWARE_VERSION, s_board_id);
    }
}

static void read_board_uid(void)
{
    uint8_t uid[16] = {0};
    esp_err_t e = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 128);
    int nonzero = 0;
    for (int i = 0; i < 16; i++) {
        if (uid[i]) {
            nonzero++;
        }
    }
    if (e == ESP_OK && nonzero > 0) {
        snprintf(s_board_id, sizeof(s_board_id), "%02x%02x%02x%02x%02x%02x",
                 uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_board_id, sizeof(s_board_id), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static const char *reset_reason_short(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SW: return "software";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_SDIO: return "reset_pin";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    default: return "unknown";
    }
}

static esp_err_t storage_fat_init(void)
{
#if SOC_USB_OTG_SUPPORTED
    esp_err_t err = tmm_msc_storage_init(FS_MOUNT_POINT);
    if (err != ESP_OK) {
        TMM_L_ERR(NULL, false, "MSC+FAT init failed: %s", esp_err_to_name(err));
        return err;
    }
    f_setlabel("0:METARMAP");
    vTaskDelay(pdMS_TO_TICKS(150));
    err = tmm_usb_device_start();
    if (err != ESP_OK) {
        TMM_L_ERR(NULL, false, "USB device start failed: %s", esp_err_to_name(err));
        return err;
    }
    TMM_L_NOTICE(NULL, false, "TinyUSB MSC + FAT mounted at %s (partition storage)", FS_MOUNT_POINT);
    return ESP_OK;
#else
    TMM_L_ERR(NULL, false, "USB OTG not supported on this chip");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void trim_line(char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    while (*line == ' ' || *line == '\t') {
        memmove(line, line + 1, strlen(line) + 1);
    }
}

static void write_default_config_txt(void)
{
    if (tmm_msc_is_usb_mode()) {
        return;
    }
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) {
        TMM_L_WARNING(s_error_log, true, "Could not write default %s", CONFIG_FILE);
        return;
    }
    fprintf(f, "WIFI_SSID=%s\n", WIFI_SSID_DEFAULT);
    fprintf(f, "WIFI_PASS=%s\n", WIFI_PASS_DEFAULT);
    fclose(f);
    TMM_L_NOTICE(s_error_log, false, "Wrote default config.txt");
}

static void load_config_txt(void)
{
    strncpy(s_wifi_ssid, WIFI_SSID_DEFAULT, sizeof(s_wifi_ssid) - 1);
    strncpy(s_wifi_pass, WIFI_PASS_DEFAULT, sizeof(s_wifi_pass) - 1);

    if (tmm_msc_is_usb_mode()) {
        return;
    }

    struct stat st;
    if (stat(CONFIG_FILE, &st) != 0) {
        write_default_config_txt();
    }

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        TMM_L_WARNING(s_error_log, true, "config.txt missing; compile defaults");
        return;
    }

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        if (!strcmp(line, "WIFI_SSID")) {
            strncpy(s_wifi_ssid, eq, sizeof(s_wifi_ssid) - 1);
        } else if (!strcmp(line, "WIFI_PASS")) {
            strncpy(s_wifi_pass, eq, sizeof(s_wifi_pass) - 1);
        }
    }
    fclose(f);
    if (s_wifi_ssid[0] == '\0') {
        strncpy(s_wifi_ssid, WIFI_SSID_DEFAULT, sizeof(s_wifi_ssid) - 1);
    }
    TMM_L_INFO(s_error_log, false, "WiFi SSID from config: %s", s_wifi_ssid);
}

static void airport_set_default(void)
{
    static const char *fallback[] = { "KPDX", "KTTD", "KHIO", "KAST", "KDLS" };
    s_airport_count = (int)(sizeof(fallback) / sizeof(fallback[0]));
    for (int i = 0; i < s_airport_count; i++) {
        memset(&s_airports[i], 0, sizeof(s_airports[i]));
        strncpy(s_airports[i].icao, fallback[i], sizeof(s_airports[i].icao) - 1);
        s_airports[i].r = 0;
        s_airports[i].g = 0;
        s_airports[i].b = 0;
        s_airports[i].valid = true;
    }
    TMM_L_WARNING(s_error_log, true, "Fallback airports (no airports.txt)");
}

static bool is_valid_airport_token(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    if (strncmp(s, "OFF", 3) == 0) {
        return false;
    }
    if (strcmp(s, "NULL") == 0) {
        return false;
    }
    size_t len = strlen(s);
    return len >= 3 && len <= 4;
}

static void load_airports(void)
{
    if (!s_storage_ok || tmm_msc_is_usb_mode()) {
        airport_set_default();
        return;
    }

    FILE *f = fopen(AIRPORTS_FILE, "r");
    if (!f) {
        airport_set_default();
        return;
    }

    s_airport_count = 0;
    char line[64];
    while (fgets(line, sizeof(line), f) && s_airport_count < MAX_AIRPORTS) {
        trim_line(line);
        if (!line[0]) {
            continue;  /* skip truly blank lines — no LED slot */
        }
        for (char *p = line; *p; ++p) {
            *p = (char)toupper((unsigned char)*p);
        }
        memset(&s_airports[s_airport_count], 0, sizeof(s_airports[s_airport_count]));
        if (is_valid_airport_token(line)) {
            /* Real airport: occupies a LED slot and gets queried */
            strncpy(s_airports[s_airport_count].icao, line, sizeof(s_airports[s_airport_count].icao) - 1);
            s_airports[s_airport_count].valid = true;
        }
        /* OFF/OFF_LEGEND/NULL: placeholder — occupies a LED slot (stays dark), valid=false */
        s_airport_count++;
    }
    fclose(f);

    if (s_airport_count == 0) {
        airport_set_default();
    }
    TMM_L_INFO(s_error_log, false, "Loaded %d airports", s_airport_count);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_and_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_wifi_pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(s_sta_netif, &ip_info);
        TMM_L_NOTICE(s_error_log, false, "WiFi connected: " IPSTR, IP2STR(&ip_info.ip));
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        return true;
    }

    s_debug_flags |= DBG_WIFI_CONNECT_FAILED;
    TMM_L_ERR(s_error_log, true, "WiFi connect failed");
    return false;
}

static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static bool led_strip_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 256,  /* 4× deeper FIFO; reduces refill frequency under TinyUSB ISR load */
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        s_debug_flags |= DBG_NEOPIXEL_INIT_FAILED;
        TMM_L_ERR(s_error_log, true, "LED strip init failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return false;
    }
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);  /* send blank frame to clear any residual state */
    return true;
}

static void leds_render(void)
{
    if (!s_strip) {
        return;
    }
    int n = s_airport_count < LED_STRIP_COUNT ? s_airport_count : LED_STRIP_COUNT;
    for (int i = 0; i < n; i++) {
        led_strip_set_pixel(s_strip, i,
            (uint8_t)(s_airports[i].r / 5),
            (uint8_t)(s_airports[i].g / 5),
            (uint8_t)(s_airports[i].b / 5));
    }
    for (int i = n; i < LED_STRIP_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        s_debug_flags |= DBG_NEOPIXEL_REFRESH_FAILED;
        TMM_L_WARNING(s_error_log, true, "led_strip_refresh: %s", esp_err_to_name(err));
    } else {
        s_render_count++;
    }
}

static int find_airport_index(const char *icao)
{
    for (int i = 0; i < s_airport_count; i++) {
        if (strcmp(s_airports[i].icao, icao) == 0) {
            return i;
        }
    }
    return -1;
}

static bool parse_metar_csv(char *body)
{
    bool any = false;
    for (char *line = strtok(body, "\n"); line; line = strtok(NULL, "\n")) {
        trim_line(line);
        if (!line[0]) {
            continue;
        }
        char *saveptr = NULL;
        char *icao = strtok_r(line, ",", &saveptr);
        char *cat = strtok_r(NULL, ",", &saveptr);
        char *rgb = strtok_r(NULL, ",", &saveptr);
        char *age = strtok_r(NULL, ",", &saveptr);
        char *stale = strtok_r(NULL, ",", &saveptr);
        if (!icao || !rgb) {
            continue;
        }
        int idx = find_airport_index(icao);
        if (idx < 0) {
            continue;
        }
        int r = 0, g = 0, b = 0;
        if (sscanf(rgb, "%d-%d-%d", &r, &g, &b) != 3) {
            continue;
        }
        s_airports[idx].r = (uint8_t)r;
        s_airports[idx].g = (uint8_t)g;
        s_airports[idx].b = (uint8_t)b;
        s_airports[idx].age_mins = age ? atoi(age) : -1;
        if (cat) {
            strncpy(s_airports[idx].cat, cat, sizeof(s_airports[idx].cat) - 1);
            s_airports[idx].cat[sizeof(s_airports[idx].cat) - 1] = '\0';
        }
        s_airports[idx].stale = (stale && atoi(stale) == 1);
        any = true;
    }
    return any;
}

static bool http_get_url(const char *url, char **out_body, int *out_len)
{
    *out_body = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .cert_pem = (const char *)amazon_root_ca1_pem_start,
        .cert_len = amazon_root_ca1_pem_end - amazon_root_ca1_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }

    uint32_t heap_before = esp_get_free_heap_size();
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        int sock_errno = esp_http_client_get_errno(client);
        snprintf(s_last_http_err, sizeof(s_last_http_err), "open:%s errno=%d heap=%lu",
                 esp_err_to_name(err), sock_errno, (unsigned long)heap_before);
        TMM_L_ERR(s_error_log, true, "http open failed: %s errno=%d heap=%lu",
                  esp_err_to_name(err), sock_errno, (unsigned long)heap_before);
        esp_http_client_cleanup(client);
        return false;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int len = esp_http_client_get_content_length(client);

    if (status < 200 || status >= 300) {
        snprintf(s_last_http_err, sizeof(s_last_http_err), "http_status:%d", status);
        TMM_L_WARNING(s_error_log, true, "HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char *buf = malloc((size_t)HTTP_RX_BUF_SIZE);
    if (!buf) {
        s_debug_flags |= DBG_MEMORY_ERROR;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int cap = HTTP_RX_BUF_SIZE - 1;
    int total = 0;
    if (len > 0 && len <= cap) {
        while (total < len) {
            int r = esp_http_client_read(client, buf + total, len - total);
            if (r <= 0) {
                break;
            }
            total += r;
        }
    } else {
        while (total < cap) {
            int r = esp_http_client_read(client, buf + total, cap - total);
            if (r <= 0) {
                break;
            }
            total += r;
        }
    }
    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_body = buf;
    *out_len = total;
    return true;
}

static bool fetch_metar_once(void)
{
    if (tmm_msc_is_usb_mode()) {
        return false;
    }

    char ids[1024] = {0};
    int p = 0;
    for (int i = 0; i < s_airport_count; i++) {
        if (!s_airports[i].valid) {
            continue;
        }
        p += snprintf(ids + p, sizeof(ids) - p, "%s%s", (p > 0) ? "," : "", s_airports[i].icao);
        if (p >= (int)sizeof(ids) - 8) {
            break;
        }
    }

    char url[URL_BUF_SIZE];
    snprintf(url, sizeof(url),
             "%s?ids=%s&boardid=%s&output=csv&debug=0x%lX&crash=%s&fetches=%lu&fetch_fails=%lu",
             METAR_SERVICE_URL, ids, s_board_id, (unsigned long)s_debug_flags,
             reset_reason_short(), (unsigned long)s_fetch_count, (unsigned long)s_fetch_fail_count);

    int64_t start = millis();
    char *body = NULL;
    int blen = 0;
    if (!http_get_url(url, &body, &blen)) {
        s_debug_flags |= DBG_METAR_FETCH_FAILED;
        s_fetch_fail_count++;
        return false;
    }

    bool ok = parse_metar_csv(body);
    free(body);

    int64_t elapsed = millis() - start;
    if (!ok) {
        s_debug_flags |= DBG_METAR_FETCH_FAILED;
        s_fetch_fail_count++;
        TMM_L_WARNING(s_error_log, true, "METAR parse returned no updates");
        return false;
    }

    s_fetch_count++;
    TMM_L_INFO(s_error_log, false, "METAR updated in %lld ms (fetches=%lu)", elapsed, (unsigned long)s_fetch_count);
    return true;
}

#if STATSBOX_ENABLED
static void statsbox_post_stub(void) { }
#else
static void statsbox_post_stub(void) { }
#endif

#if OTA_ENABLED
static void ota_check(void)
{
    esp_http_client_config_t http_cfg = {
        .url = OTA_URL,
        .timeout_ms = 30000,
        .cert_pem = (const char *)amazon_root_ca1_pem_start,
        .cert_len = amazon_root_ca1_pem_end - amazon_root_ca1_pem_start,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };
    esp_https_ota_handle_t ota_handle = NULL;
    if (esp_https_ota_begin(&ota_cfg, &ota_handle) != ESP_OK) {
        s_debug_flags |= DBG_OTA_BEGIN_FAILED;
        return;
    }
    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_info) == ESP_OK) {
        const esp_app_desc_t *running = esp_app_get_description();
        if (strcmp(running->version, new_app_info.version) == 0) {
            esp_https_ota_abort(ota_handle);
            return;
        }
    }
    esp_err_t perform_err = ESP_OK;
    while ((perform_err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) { }
    if (perform_err != ESP_OK) {
        s_debug_flags |= DBG_OTA_PERFORM_FAILED;
        esp_https_ota_abort(ota_handle);
        return;
    }
    esp_err_t finish_err = esp_https_ota_finish(ota_handle);
    if (finish_err == ESP_OK) {
        esp_restart();
    } else {
        s_debug_flags |= DBG_OTA_FINISH_FAILED;
    }
}
#else
static void ota_check(void) { (void)OTA_URL; }
#endif

static esp_err_t http_root_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char buf[750];
    int n = snprintf(buf, sizeof(buf),
                     "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"180\">"
                     "<title>tmm " FIRMWARE_VERSION "</title></head><body>"
                     "<h1>tedder-metar-map</h1>"
                     "<p>local0.info: fw=%s app_desc=%s</p>"
                     "<p>boardid=%s fetches=%lu fetch_fails=%lu renders=%lu debug=0x%08lx</p>"
                     "<p><a href=\"/airports\">airports</a> | "
                     "<a href=\"/config\">config</a> | "
                     "<a href=\"/usb\">USB storage</a> | <a href=\"/debug\">debug</a> | <a href=\"/log\">log</a> | "
                     "<form style=\"display:inline\" method=\"post\" action=\"/ota\">"
                     "<button type=\"submit\">check OTA</button></form></p>"
                     "</body></html>",
                     FIRMWARE_VERSION, app->version, s_board_id,
                     (unsigned long)s_fetch_count, (unsigned long)s_fetch_fail_count, (unsigned long)s_render_count, (unsigned long)s_debug_flags);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t http_debug_get(httpd_req_t *req)
{
    char buf[1100];
    time_t now_t = time(NULL);
    int n = snprintf(buf, sizeof(buf),
                     "<html><body><h1>debug</h1>"
                     "<p>fw=%s heap=%u</p><p>flags=0x%08lx</p><p>wifi=%s</p>"
                     "<p>usb_msc_host=%s</p><p>time=%ld sntp=%s</p>"
                     "<p>log=%s err=%s</p>"
                     "<p><a href=\"/\">home</a></p></body></html>",
                     FIRMWARE_VERSION,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned long)s_debug_flags,
                     wifi_is_connected() ? "up" : "down",
                     tmm_msc_is_usb_mode() ? "yes" : "no",
                     (long)now_t,
                     now_t >= 1700000000LL ? "ok" : "pending",
                     s_error_log ? "open" : "null",
                     s_last_http_err[0] ? s_last_http_err : "none");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t http_airports_get(httpd_req_t *req)
{
    /* Each row: ~120 bytes; header/footer ~300; MAX_AIRPORTS=96 → max ~11820 */
    size_t bufsz = 300 + (size_t)s_airport_count * 130;
    char *buf = malloc(bufsz);
    if (!buf) {
        httpd_resp_set_status(req, "500");
        return httpd_resp_send(req, "oom", HTTPD_RESP_USE_STRLEN);
    }
    int pos = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>airports</title>"
                    "<style>.wx{width:1.2em;height:1.2em;display:inline-block;border:1px solid #333}</style>"
                    "</head><body><h1>airports</h1>"
                    "<p><a href=\"/\">home</a> | <a href=\"/config\">config</a> | "
                    "<a href=\"/debug\">debug</a></p>"
                    "<table><tr><th></th><th>#</th><th>ICAO</th><th>cat</th><th>age</th></tr>");
    for (int i = 0; i < s_airport_count; i++) {
        const airport_led_t *a = &s_airports[i];
        char age_str[16] = "-";
        if (a->age_mins >= 0) {
            snprintf(age_str, sizeof(age_str), "%d min", a->age_mins);
        }
        pos += snprintf(buf + pos, bufsz - pos,
                        "<tr>"
                        "<td><div class=\"wx\" style=\"background-color:#%02x%02x%02x\"></div></td>"
                        "<td>%d</td><td>%s</td><td>%s</td><td>%s%s</td>"
                        "</tr>",
                        a->r, a->g, a->b,
                        i, a->icao[0] ? a->icao : "-",
                        a->cat[0] ? a->cat : "-",
                        age_str,
                        a->stale ? " (stale)" : "");
    }
    pos += snprintf(buf + pos, bufsz - pos, "</table></body></html>");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, buf, pos);
    free(buf);
    return ret;
}

static esp_err_t http_cfg_get(httpd_req_t *req)
{
    char *apbuf = calloc(1, 4096);
    char *html  = malloc(6144);
    if (!apbuf || !html) {
        free(apbuf); free(html);
        httpd_resp_set_status(req, "500");
        return httpd_resp_send(req, "oom", HTTPD_RESP_USE_STRLEN);
    }
    char ssid[64] = "";
    char pass[64] = "";
    if (s_storage_ok && !tmm_msc_is_usb_mode()) {
        FILE *f = fopen(AIRPORTS_FILE, "r");
        if (f) {
            size_t n = fread(apbuf, 1, 4095, f);
            apbuf[n] = '\0';
            fclose(f);
        }
        f = fopen(CONFIG_FILE, "r");
        if (f) {
            char line[160];
            while (fgets(line, sizeof(line), f)) {
                trim_line(line);
                if (!line[0] || line[0] == '#') {
                    continue;
                }
                char *eq = strchr(line, '=');
                if (!eq) {
                    continue;
                }
                *eq++ = '\0';
                if (!strcmp(line, "WIFI_SSID")) {
                    strncpy(ssid, eq, sizeof(ssid) - 1);
                } else if (!strcmp(line, "WIFI_PASS")) {
                    strncpy(pass, eq, sizeof(pass) - 1);
                }
            }
            fclose(f);
        }
    }

    int n = snprintf(html, 6144,
                       "<!DOCTYPE html><html><body>"
                       "<h1>config</h1><form method=\"post\" action=\"/config\">"
                       "<p>WIFI_SSID<br/><input name=\"ssid\" value=\"%s\" size=\"40\"/></p>"
                       "<p>WIFI_PASS<br/><input name=\"pass\" type=\"password\" value=\"%s\" size=\"40\"/></p>"
                       "<p>airports.txt<br/><textarea name=\"airports\" rows=\"18\" cols=\"48\">%s</textarea></p>"
                       "<p><input type=\"submit\" value=\"Save\"/></p></form>"
                       "<p><a href=\"/\">home</a></p></body></html>",
                       ssid, pass, apbuf);
    free(apbuf);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, html, n);
    free(html);
    return ret;
}

static esp_err_t http_cfg_post(httpd_req_t *req)
{
    if (tmm_msc_is_usb_mode() || !s_storage_ok) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "storage on USB or unavailable", HTTPD_RESP_USE_STRLEN);
    }
    int len = req->content_len;
    if (len <= 0 || len > WEB_POST_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad length", HTTPD_RESP_USE_STRLEN);
    }
    char *body = malloc(len + 1);
    char *airports = calloc(1, 4096);
    if (!body || !airports) {
        free(body); free(airports);
        httpd_resp_set_status(req, "500");
        return httpd_resp_send(req, "oom", HTTPD_RESP_USE_STRLEN);
    }
    int r = httpd_req_recv(req, body, len);
    if (r != len) {
        free(body); free(airports);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "recv", HTTPD_RESP_USE_STRLEN);
    }
    body[len] = '\0';

    char ssid[TMM_WIFI_SSID_BUF] = "";
    char pass[TMM_WIFI_PASS_BUF] = "";
    char *save = NULL;
    for (char *tok = strtok_r(body, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        if (!strcmp(tok, "ssid")) {
            strncpy(ssid, eq, sizeof(ssid) - 1);
        } else if (!strcmp(tok, "pass")) {
            strncpy(pass, eq, sizeof(pass) - 1);
        } else if (!strcmp(tok, "airports")) {
            strncpy(airports, eq, 4095);
        }
    }
    free(body);

    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) {
        free(airports);
        httpd_resp_set_status(req, "500");
        return httpd_resp_send(req, "config write fail", HTTPD_RESP_USE_STRLEN);
    }
    fprintf(f, "WIFI_SSID=%s\nWIFI_PASS=%s\n", ssid, pass);
    fclose(f);

    f = fopen(AIRPORTS_FILE, "w");
    if (!f) {
        free(airports);
        httpd_resp_set_status(req, "500");
        return httpd_resp_send(req, "airports write fail", HTTPD_RESP_USE_STRLEN);
    }
    fputs(airports, f);
    fclose(f);
    free(airports);

    strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
    strncpy(s_wifi_pass, pass, sizeof(s_wifi_pass) - 1);
    load_airports();
    leds_render();

    httpd_resp_set_hdr(req, "Location", "/config");
    httpd_resp_set_status(req, "303 See Other");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_usb_get(httpd_req_t *req)
{
    bool usb = tmm_msc_is_usb_mode();
    char html[800];
    int n = snprintf(html, sizeof(html),
                     "<html><body><h1>USB mass storage</h1>"
                     "<p>Current: <b>%s</b> the volume (host sees removable disk when on USB).</p>"
                     "<form method=\"post\" action=\"/usb\">"
                     "<input type=\"hidden\" name=\"mode\" value=\"usb\"/>"
                     "<p><button type=\"submit\">Give storage to USB (PC can edit files)</button></p></form>"
                     "<form method=\"post\" action=\"/usb\">"
                     "<input type=\"hidden\" name=\"mode\" value=\"app\"/>"
                     "<p><button type=\"submit\">Take storage back for firmware</button></p></form>"
                     "<p><a href=\"/\">home</a></p></body></html>",
                     usb ? "Host owns" : "Firmware owns");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, n);
}

static esp_err_t http_usb_post(httpd_req_t *req)
{
#if !SOC_USB_OTG_SUPPORTED
    httpd_resp_set_status(req, "501");
    return httpd_resp_send(req, "no USB OTG", HTTPD_RESP_USE_STRLEN);
#else
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400");
        return httpd_resp_send(req, "bad", HTTPD_RESP_USE_STRLEN);
    }
    if (httpd_req_recv(req, body, len) != len) {
        httpd_resp_set_status(req, "400");
        return httpd_resp_send(req, "recv", HTTPD_RESP_USE_STRLEN);
    }
    body[len] = '\0';
    char mode[16] = "";
    char *save = NULL;
    for (char *tok = strtok_r(body, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        if (!strcmp(tok, "mode")) {
            strncpy(mode, eq, sizeof(mode) - 1);
        }
    }
    error_log_close();
    esp_err_t e = ESP_OK;
    if (!strcmp(mode, "usb")) {
        e = tmm_msc_set_mount_usb();
        TMM_L_NOTICE(NULL, false, "MSC: storage mount -> USB host");
    } else if (!strcmp(mode, "app")) {
        e = tmm_msc_set_mount_app();
        TMM_L_NOTICE(NULL, false, "MSC: storage mount -> application");
        error_log_open();
    } else {
        error_log_open();
        httpd_resp_set_status(req, "400");
        return httpd_resp_send(req, "mode", HTTPD_RESP_USE_STRLEN);
    }
    if (e != ESP_OK) {
        error_log_open();
        TMM_L_ERR(s_error_log, true, "MSC mount switch failed: %s", esp_err_to_name(e));
        httpd_resp_set_status(req, "500");
        return httpd_resp_send(req, esp_err_to_name(e), HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_hdr(req, "Location", "/usb");
    httpd_resp_set_status(req, "303 See Other");
    return httpd_resp_send(req, NULL, 0);
#endif
}

static esp_err_t http_ota_post(httpd_req_t *req)
{
    s_ota_requested = true;
    httpd_resp_set_hdr(req, "Location", "/debug");
    httpd_resp_set_status(req, "303 See Other");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_bootloader_post(httpd_req_t *req)
{
    httpd_resp_send(req, "rebooting to bootloader", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(100));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    return ESP_OK;
}

static esp_err_t http_log_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (tmm_msc_is_usb_mode() || !s_storage_ok) {
        return httpd_resp_send(req, "(log unavailable: USB mode or no storage)\n", HTTPD_RESP_USE_STRLEN);
    }
    FILE *f = fopen(ERROR_LOG_FILE, "r");
    if (!f) {
        return httpd_resp_send(req, "(no log file)\n", HTTPD_RESP_USE_STRLEN);
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        httpd_resp_sendstr_chunk(req, buf);
    }
    fclose(f);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static bool http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.max_uri_handlers = 12;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        TMM_L_WARNING(s_error_log, true, "httpd_start failed");
        return false;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = http_root_get },
        { .uri = "/airports", .method = HTTP_GET, .handler = http_airports_get },
        { .uri = "/debug", .method = HTTP_GET, .handler = http_debug_get },
        { .uri = "/config", .method = HTTP_GET, .handler = http_cfg_get },
        { .uri = "/config", .method = HTTP_POST, .handler = http_cfg_post },
        { .uri = "/usb", .method = HTTP_GET, .handler = http_usb_get },
        { .uri = "/usb", .method = HTTP_POST, .handler = http_usb_post },
        { .uri = "/bootloader", .method = HTTP_POST, .handler = http_bootloader_post },
        { .uri = "/ota", .method = HTTP_POST, .handler = http_ota_post },
        { .uri = "/log", .method = HTTP_GET, .handler = http_log_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    TMM_L_NOTICE(s_error_log, false, "HTTP server port 80 (minimal UI)");
    return true;
}

static void watchdog_init(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_reconfigure(&cfg);
    }
    if (err == ESP_OK) {
        esp_task_wdt_add(NULL);
        TMM_L_NOTICE(s_error_log, false, "Watchdog %d sec", WATCHDOG_TIMEOUT_SEC);
    } else {
        TMM_L_WARNING(s_error_log, true, "Watchdog init failed: %s", esp_err_to_name(err));
    }
}

static void oled_refresh_status(void)
{
    if (!s_oled_ok) {
        return;
    }
    char l1[22] = "";
    char l2[22] = "";
    char l3[22] = "";
    if (wifi_is_connected() && s_sta_netif) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(s_sta_netif, &ip);
        snprintf(l1, sizeof(l1), IPSTR "%s", IP2STR(&ip.ip), s_httpd ? " *" : "");
    } else {
        snprintf(l1, sizeof(l1), "no wifi");
    }
    snprintf(l2, sizeof(l2), "fx=%lu flg=%lx %s", (unsigned long)s_fetch_count, (unsigned long)s_debug_flags, FIRMWARE_VERSION);
    snprintf(l3, sizeof(l3), "%s", tmm_msc_is_usb_mode() ? "USB disk" : "app fs");
    oled_show_three_lines(l1, l2, l3);
}

void app_main(void)
{
    /* LED strip first — RMT has no dependencies, gives us boot progress */
    if (!led_strip_init()) {
        TMM_L_WARNING(NULL, false, "LED strip unavailable");
    }
    boot_progress();  /* 0: strip alive */

    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        s_debug_flags |= DBG_WATCHDOG_RESET;
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    boot_progress();  /* 1: NVS */

    read_board_uid();
    boot_progress();  /* 2: board UID */

    airport_set_default();
    boot_progress();  /* 3: default airports */

    s_storage_ok = (storage_fat_init() == ESP_OK);
    if (!s_storage_ok) {
        strncpy(s_wifi_ssid, WIFI_SSID_DEFAULT, sizeof(s_wifi_ssid) - 1);
        strncpy(s_wifi_pass, WIFI_PASS_DEFAULT, sizeof(s_wifi_pass) - 1);
        TMM_L_ERR(NULL, false, "Storage init failed — WiFi defaults only");
    } else {
        error_log_open();
        load_config_txt();
    }
    boot_progress();  /* 4: storage + config */

    load_airports();
    boot_progress();  /* 5: airports loaded */

    TMM_L_NOTICE(s_error_log, false, "tedder-metar-map v%s boardid=%s", FIRMWARE_VERSION, s_board_id);

    if (oled_init() == ESP_OK) {
        s_oled_ok = true;
    } else {
        s_debug_flags |= DBG_DISPLAY_INIT_FAILED;
        TMM_L_WARNING(s_error_log, true, "OLED init failed");
    }
    boot_progress();  /* 6: OLED */

    watchdog_init();
    boot_progress();  /* 7: watchdog */

    if (!wifi_init_and_connect()) {
        s_debug_flags |= DBG_WIFI_ENABLE_FAILED;
    } else if (wifi_is_connected()) {
        http_server_start();
        mdns_init();
        mdns_hostname_set("metarmap");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    boot_progress();  /* 8: wifi + http */

    /* Dim white on all slots until first METAR arrives */
    for (int i = 0; i < s_airport_count; i++) {
        s_airports[i].r = 5;
        s_airports[i].g = 5;
        s_airports[i].b = 5;
    }
    leds_render();

    statsbox_post_stub();
    oled_refresh_status();

    int64_t last_fetch_ms = 0;
    int64_t last_wifi_ms = 0;
    int64_t last_oled_ms = 0;
    uint32_t fetch_cycle = 0;

    while (1) {
        esp_task_wdt_reset();

        int64_t now = millis();
        /* Open error log if USB host ejected the volume externally */
        if (!s_error_log && s_storage_ok && !tmm_msc_is_usb_mode()) {
            error_log_open();
        }
        if (now - last_wifi_ms >= 30000) {
            last_wifi_ms = now;
            if (!wifi_is_connected()) {
                TMM_L_WARNING(s_error_log, false, "WiFi reconnect");
                s_wifi_retry_num = 0;
                esp_wifi_connect();
            } else if (!s_httpd) {
                /* WiFi connected but HTTP server not yet started (boot-time timeout race) */
                TMM_L_NOTICE(s_error_log, false, "WiFi up — late-starting HTTP server");
                http_server_start();
                mdns_init();
                mdns_hostname_set("metarmap");
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                esp_sntp_setservername(0, "pool.ntp.org");
                esp_sntp_init();
            }
        }

        if (now - last_oled_ms >= 5000) {
            last_oled_ms = now;
            oled_refresh_status();
        }

#if OTA_ENABLED
        if (s_ota_requested && wifi_is_connected()) {
            s_ota_requested = false;
            ota_check();
        }
#endif
        if (!tmm_msc_is_usb_mode() && time(NULL) >= 1700000000LL &&
            ((now - last_fetch_ms) >= (FETCH_INTERVAL_SEC * 1000LL) || last_fetch_ms == 0)) {
            bool ok = fetch_metar_once();
            if (ok) {
                leds_render();
            }
            last_fetch_ms = now;
            fetch_cycle++;
#if OTA_ENABLED
            if (fetch_cycle % OTA_EVERY_FETCHES == 0 && wifi_is_connected()) {
                ota_check();
            }
#endif
            (void)ota_check;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
