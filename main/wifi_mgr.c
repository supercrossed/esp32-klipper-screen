/*
 * Wi-Fi manager.
 *
 * The P4 has no radio: every esp_wifi_* call is an RPC over SDIO to the
 * onboard ESP32-C6 (esp_hosted + esp_wifi_remote). Two rules follow:
 *   - never ESP_ERROR_CHECK them (a missing/slow C6 would boot-loop us)
 *   - never call them from the event loop; event handlers only post to a
 *     queue and a dedicated task does the RPCs.
 */
#include "wifi_mgr.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "mdns.h"
#include "app_settings.h"
#include "tz.h"

static const char *TAG = "wifi";

typedef enum { ACT_CONNECT, ACT_RECONNECT, ACT_SCAN, ACT_SCAN_DONE, ACT_DISCONNECT, ACT_ONLINE } act_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mtx;
static esp_netif_t *s_sta;
static volatile wifi_state_t s_state = WIFI_ST_OFF;
static esp_netif_ip_info_t s_ip;
static char s_ssid[33];
static int s_retries;
static volatile int8_t s_rssi;
static bool s_started;
static bool s_sntp;
static bool s_tz_done;

static void on_time_sync(struct timeval *tv)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[40];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "time synced: %s", b);
}

static wifi_ap_t s_aps[WIFI_MAX_APS];
static int s_ap_count = -1;
static volatile bool s_scan_busy;

static void post(act_t a)
{
    if (s_q) xQueueSend(s_q, &a, 0);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected, reason %d", d ? d->reason : -1);
        memset(&s_ip, 0, sizeof(s_ip));
        if (s_state == WIFI_ST_CONNECTING || s_state == WIFI_ST_CONNECTED) post(ACT_RECONNECT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        /* esp_wifi_connect() runs its own internal scan; only report scans we asked for. */
        if (s_scan_busy) post(ACT_SCAN_DONE);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_ip = e->ip_info;
        s_retries = 0;
        s_state = WIFI_ST_CONNECTED;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&s_ip.ip));
        if (!s_sntp) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
            cfg.wait_for_sync = false;
            cfg.sync_cb = on_time_sync;
            if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp = true;
        }
        post(ACT_ONLINE);
    }
}

static bool radio_up(void)
{
    if (s_started) return true;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (is the C6 up?)", esp_err_to_name(err));
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);   /* latency matters more than power here */
    s_started = true;
    s_state = WIFI_ST_IDLE;
    ESP_LOGI(TAG, "radio started (on the C6)");

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_settings.device_name);
    }
    return true;
}

static void do_connect(void)
{
    if (!s_ssid[0]) { s_state = WIFI_ST_IDLE; return; }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_settings.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_settings.wifi_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_NONE);   /* no modem sleep, ever: keeps latency low and links stable */
    s_retries = 0;
    s_state = WIFI_ST_CONNECTING;
    ESP_LOGI(TAG, "connecting to \"%s\"", s_ssid);
    esp_wifi_connect();
}

static void wifi_task(void *arg)
{
    /* The C6 may not be ready the instant we boot; keep trying. */
    while (!radio_up()) vTaskDelay(pdMS_TO_TICKS(2000));
    if (s_scan_busy) post(ACT_SCAN);   /* a scan requested before the radio was ready */
    if (g_settings.wifi_ssid[0]) {
        strlcpy(s_ssid, g_settings.wifi_ssid, sizeof(s_ssid));
        do_connect();
    }

    act_t a;
    int ticks = 0;
    for (;;) {
        if (xQueueReceive(s_q, &a, pdMS_TO_TICKS(5000)) != pdTRUE) {
            if (s_state == WIFI_ST_CONNECTED) {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
            }
            /* If we gave up earlier, try again every ~30 s. */
            if (++ticks % 6 == 0 && s_state == WIFI_ST_FAILED && s_ssid[0]) do_connect();
            continue;
        }
        switch (a) {
        case ACT_CONNECT:
            do_connect();
            break;
        case ACT_RECONNECT:
            if (s_state == WIFI_ST_IDLE) break;
            s_state = WIFI_ST_CONNECTING;
            if (++s_retries > 8) {
                ESP_LOGW(TAG, "giving up on \"%s\" for now", s_ssid);
                s_state = WIFI_ST_FAILED;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(s_retries < 3 ? 500 : 3000));
            esp_wifi_connect();
            break;
        case ACT_ONLINE:
            /* Network work, so it runs here rather than in the event handler. */
            /* time zone (when on Auto) and location for the sunrise/sunset theme */
            if ((g_settings.tz_auto || !g_settings.has_loc) && !s_tz_done) s_tz_done = tz_autodetect();
            break;
        case ACT_DISCONNECT:
            s_state = WIFI_ST_IDLE;
            s_ssid[0] = 0;
            esp_wifi_disconnect();
            break;
        case ACT_SCAN: {
            wifi_scan_config_t sc = { .show_hidden = false };
            /* A pending connect attempt blocks scanning; stop it while we scan. */
            if (s_state != WIFI_ST_CONNECTED) esp_wifi_disconnect();
            if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
                ESP_LOGW(TAG, "scan start failed");
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_ap_count = 0;
                xSemaphoreGive(s_mtx);
                s_scan_busy = false;
            }
            break;
        }
        case ACT_SCAN_DONE: {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
            if (recs && n) esp_wifi_scan_get_ap_records(&n, recs);
            else n = 0;
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            int c = 0;
            for (int i = 0; i < n && c < WIFI_MAX_APS; i++) {
                if (!recs[i].ssid[0]) continue;
                bool dup = false;   /* keep only the strongest entry per SSID (records come sorted) */
                for (int j = 0; j < c; j++)
                    if (!strcmp(s_aps[j].ssid, (char *)recs[i].ssid)) { dup = true; break; }
                if (dup) continue;
                strlcpy(s_aps[c].ssid, (char *)recs[i].ssid, sizeof(s_aps[c].ssid));
                s_aps[c].rssi = recs[i].rssi;
                s_aps[c].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                s_aps[c].channel = recs[i].primary;
                c++;
            }
            s_ap_count = c;
            xSemaphoreGive(s_mtx);
            free(recs);
            s_scan_busy = false;
            ESP_LOGI(TAG, "scan done: %d networks", c);
            /* Resume connecting if we interrupted it. */
            if ((s_state == WIFI_ST_CONNECTING || s_state == WIFI_ST_FAILED) && s_ssid[0]) do_connect();
            break;
        }
        }
    }
}

void wifi_mgr_start(void)
{
    s_q = xQueueCreate(8, sizeof(act_t));
    s_mtx = xSemaphoreCreateMutex();
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, g_settings.device_name);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    setenv("TZ", g_settings.tz, 1);
    tzset();
    xTaskCreate(wifi_task, "wifi_mgr", 6144, NULL, 5, NULL);
}

wifi_state_t wifi_mgr_state(void) { return s_state; }

const char *wifi_mgr_state_str(void)
{
    switch (s_state) {
    case WIFI_ST_OFF:        return "Starting radio";
    case WIFI_ST_IDLE:       return "Not configured";
    case WIFI_ST_CONNECTING: return "Connecting";
    case WIFI_ST_CONNECTED:  return "Connected";
    case WIFI_ST_FAILED:     return "Connection failed";
    }
    return "?";
}

void wifi_mgr_connect(const char *ssid, const char *pass, bool save)
{
    strlcpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, pass ? pass : "", sizeof(g_settings.wifi_pass));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    if (save) settings_save();
    post(ACT_CONNECT);
}

void wifi_mgr_forget(void)
{
    g_settings.wifi_ssid[0] = 0;
    g_settings.wifi_pass[0] = 0;
    settings_save();
    post(ACT_DISCONNECT);
}

bool wifi_mgr_ip(esp_netif_ip_info_t *out)
{
    if (s_state != WIFI_ST_CONNECTED) return false;
    if (out) *out = s_ip;
    return true;
}

int8_t wifi_mgr_rssi(void)
{
    /* Refreshed by wifi_task; never RPC from the UI thread. */
    return s_state == WIFI_ST_CONNECTED ? s_rssi : 0;
}

const char *wifi_mgr_ssid(void) { return s_ssid; }

bool wifi_mgr_time_valid(void)
{
    return time(NULL) > 1700000000;
}

void wifi_mgr_mac_str(char *out, size_t cap)
{
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void wifi_mgr_scan_start(void)
{
    if (s_scan_busy) return;
    s_scan_busy = true;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_ap_count = -1;
    xSemaphoreGive(s_mtx);
    if (s_started) post(ACT_SCAN);   /* otherwise wifi_task starts it once the radio is up */
}

bool wifi_mgr_scan_busy(void) { return s_scan_busy; }

int wifi_mgr_scan_results(wifi_ap_t *out, int max)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_ap_count;
    if (n > 0) {
        if (n > max) n = max;
        memcpy(out, s_aps, n * sizeof(wifi_ap_t));
    }
    xSemaphoreGive(s_mtx);
    return n;
}

void wifi_mgr_request_tz(void)
{
    s_tz_done = false;
    post(ACT_ONLINE);
}
