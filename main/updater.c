/*
 * Over-the-air updates from this project's GitHub Releases.
 *
 *  - Check: GET https://api.github.com/repos/<repo>/releases/latest, compare the
 *    tag with the running version, keep the notes and the app asset URL.
 *  - Install: esp_https_ota into the inactive OTA slot, then reboot. The
 *    bootloader rolls back automatically if the new image never marks itself
 *    valid (we do that once Wi-Fi is up and the UI has run for a while).
 *
 * The check runs on a short-lived task with its stack in PSRAM. The install task
 * writes flash, so its stack must be internal RAM; that RAM fragments over a long
 * uptime, so when no big enough block is left the screen restarts first and
 * installs straight after boot (flag in NVS, one shot).
 * TLS uses software AES (CONFIG_MBEDTLS_HARDWARE_AES=n): the AES DMA engine needs
 * descriptors in internal DMA memory and failed once that was fragmented.
 */
#include "updater.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "cJSON.h"
#include "nvs.h"
#include "wifi_mgr.h"
#include "board.h"

static const char *TAG = "updater";

#define REPO        "supercrossed/esp32-klipper-screen"
#define API_URL     "https://api.github.com/repos/" REPO "/releases/latest"
#define APP_ASSET   BOARD_APP_ASSET   /* per board, see board.h */
#define CHECK_EVERY_US  (6LL * 3600 * 1000000)
#define INSTALL_STACK   8192
#define INSTALL_MIN_BLOCK (INSTALL_STACK + 12 * 1024)   /* stack + room for TLS / lwIP */

static SemaphoreHandle_t s_mtx;
static upd_info_t *s_info;           /* PSRAM */
static char s_asset_url[320];
static volatile bool s_busy;
static esp_timer_handle_t s_timer;
static bool s_marked_valid;
static bool s_auto_install;           /* restarted to install: do it as soon as the check finds it */
static bool s_restarted;              /* this boot came from a restart-to-install: never restart again */
static esp_timer_handle_t s_auto_timer;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static void set_state(upd_state_t st, const char *err)
{
    LOCK();
    s_info->state = st;
    if (err) strlcpy(s_info->error, err, sizeof(s_info->error));
    UNLOCK();
}

/* "v1.2.3" / "1.2.3" -> comparable integer; returns -1 if unparsable */
static long ver_num(const char *v)
{
    if (*v == 'v' || *v == 'V') v++;
    int a = 0, b = 0, c = 0;
    if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (long)a * 1000000 + b * 1000 + c;
}

/* Markdown release body -> plain text for the dialog. Stops at the install-table divider. */
static void notes_from_markdown(const char *md, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = md;
    while (*p && o + 8 < cap) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        size_t l = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = 0;
        if (l && line[l - 1] == '\r') line[--l] = 0;
        if (!strncmp(line, "---", 3)) break;               /* install instructions follow */
        char *s = line;
        while (*s == '#') s++;                             /* headings */
        while (*s == ' ') s++;
        bool bullet = (s[0] == '-' || s[0] == '*') && s[1] == ' ';
        if (bullet) s += 2;
        /* drop markdown emphasis/code markers */
        char clean[256];
        size_t c = 0;
        for (char *q = s; *q && c < sizeof(clean) - 1; q++) {
            if (*q == '*' || *q == '`' || *q == '[') continue;
            if (*q == ']' && q[1] == '(') {            /* [text](url) -> text */
                char *close = strchr(q, ')');
                if (close) { q = close; continue; }
            }
            clean[c++] = *q;
        }
        clean[c] = 0;
        if (!(o == 0 && !clean[0])) {
            int n = snprintf(out + o, cap - o, "%s%s\n", bullet ? "\xE2\x80\xA2 " : "", clean);
            if (n > 0) o += (size_t)n < cap - o ? (size_t)n : cap - o - 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    while (o && (out[o - 1] == '\n' || out[o - 1] == ' ')) o--;
    out[o] = 0;
}

static void check_task(void *arg)
{
    char *buf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    esp_http_client_handle_t h = NULL;
    int len = 0;
    if (!buf) goto fail_mem;

    esp_http_client_config_t cfg = {
        .url = API_URL,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = "esp32-klipper-screen",
    };
    h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Accept", "application/vnd.github+json");
    if (esp_http_client_open(h, 0) != ESP_OK) { set_state(UPD_ERROR, "Couldn't reach GitHub"); goto out; }
    esp_http_client_fetch_headers(h);
    int st = esp_http_client_get_status_code(h);
    for (;;) {
        int r = esp_http_client_read(h, buf + len, 64 * 1024 - 1 - len);
        if (r <= 0) break;
        len += r;
        if (len >= 64 * 1024 - 1) break;
    }
    buf[len] = 0;
    if (st == 404) { set_state(UPD_UP_TO_DATE, NULL); goto out; }   /* no releases yet */
    if (st != 200) {
        char e[64];
        snprintf(e, sizeof(e), "GitHub returned HTTP %d", st);
        set_state(UPD_ERROR, e);
        goto out;
    }

    cJSON *j = cJSON_ParseWithLength(buf, len);
    const cJSON *tag = cJSON_GetObjectItem(j, "tag_name");
    const cJSON *body = cJSON_GetObjectItem(j, "body");
    const cJSON *assets = cJSON_GetObjectItem(j, "assets");
    char url[320] = "";
    uint32_t size = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, assets) {
        const cJSON *n = cJSON_GetObjectItem(a, "name");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, APP_ASSET)) {
            const cJSON *u = cJSON_GetObjectItem(a, "browser_download_url");
            const cJSON *sz = cJSON_GetObjectItem(a, "size");
            if (cJSON_IsString(u)) strlcpy(url, u->valuestring, sizeof(url));
            if (cJSON_IsNumber(sz)) size = sz->valueint;
        }
    }
    if (!cJSON_IsString(tag)) {
        cJSON_Delete(j);
        set_state(UPD_ERROR, "Unexpected reply from GitHub");
        goto out;
    }
    const char *latest = tag->valuestring[0] == 'v' ? tag->valuestring + 1 : tag->valuestring;
    bool newer = ver_num(latest) > ver_num(s_info->current) && url[0];
    LOCK();
    strlcpy(s_info->latest, latest, sizeof(s_info->latest));
    notes_from_markdown(cJSON_IsString(body) ? body->valuestring : "", s_info->notes, sizeof(s_info->notes));
    s_info->size = size;
    s_info->checked_at = time(NULL);
    s_info->state = newer ? UPD_AVAILABLE : UPD_UP_TO_DATE;
    s_info->available = newer;
    s_info->error[0] = 0;
    strlcpy(s_asset_url, url, sizeof(s_asset_url));
    UNLOCK();
    ESP_LOGI(TAG, "latest %s, running %s%s", s_info->latest, s_info->current, newer ? " -> update available" : "");
    cJSON_Delete(j);
    if (s_auto_install) {
        s_auto_install = false;
        if (newer) {
            esp_http_client_cleanup(h);
            heap_caps_free(buf);
            s_busy = false;
            updater_install();
            vTaskDeleteWithCaps(NULL);
        }
    }
    goto out;

fail_mem:
    set_state(UPD_ERROR, "Out of memory");
out:
    if (h) esp_http_client_cleanup(h);
    heap_caps_free(buf);
    s_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void install_task(void *arg)
{
    char url[320];
    LOCK();
    strlcpy(url, s_asset_url, sizeof(url));
    s_info->progress = 0;
    UNLOCK();
    ESP_LOGI(TAG, "installing from %s", url);

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
        .keep_alive_enable = true,
        .user_agent = "esp32-klipper-screen",
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    esp_https_ota_handle_t oh = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &oh);
    if (err != ESP_OK) { set_state(UPD_ERROR, "Download failed to start"); goto out; }

    int total = esp_https_ota_get_image_size(oh);
    while ((err = esp_https_ota_perform(oh)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(oh);
        if (total > 0) {
            LOCK();
            s_info->progress = (int)((int64_t)done * 100 / total);
            UNLOCK();
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(oh)) {
        esp_https_ota_abort(oh);
        set_state(UPD_ERROR, "Download interrupted - nothing was changed");
        goto out;
    }
    err = esp_https_ota_finish(oh);
    if (err != ESP_OK) {
        set_state(UPD_ERROR, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Downloaded image is invalid" : "Couldn't finish the update");
        goto out;
    }
    LOCK();
    s_info->progress = 100;
    s_info->state = UPD_DONE;
    UNLOCK();
    ESP_LOGI(TAG, "update installed, restarting");
    vTaskDelay(pdMS_TO_TICKS(2500));   /* let the UI show "Restarting" */
    esp_restart();
out:
    s_busy = false;
    vTaskDelete(NULL);
}

static void timer_cb(void *arg)
{
    /* First fire (a minute after boot, UI running) confirms this firmware works and
     * cancels rollback. Deliberately not tied to Wi-Fi: a router outage must not
     * roll back a good update. */
    if (!s_marked_valid) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_marked_valid = true;
    }
    updater_check_now();
}

static bool take_boot_flag(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("upd", NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, "boot", &v) == ESP_OK) {
        nvs_erase_key(h, "boot");
        nvs_commit(h);
    }
    nvs_close(h);
    return v == 1;
}

/* After a restart-to-install: check as soon as Wi-Fi is up (gives up after ~2 min). */
static void auto_cb(void *arg)
{
    static int tries;
    if (wifi_mgr_state() == WIFI_ST_CONNECTED || ++tries > 24) {
        esp_timer_stop(s_auto_timer);
        if (tries <= 24) updater_check_now();
        else s_auto_install = false;
    }
}

static void restart_cb(void *arg) { esp_restart(); }

void updater_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_info = heap_caps_calloc(1, sizeof(upd_info_t), MALLOC_CAP_SPIRAM);
    strlcpy(s_info->current, esp_app_get_description()->version, sizeof(s_info->current));

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
        ESP_LOGW(TAG, "first boot of a new version: will confirm it once Wi-Fi is up");
    else
        s_marked_valid = true;

    const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "upd" };
    esp_timer_create(&ta, &s_timer);
    esp_timer_start_once(s_timer, 60LL * 1000000);   /* first check a minute after boot */

    if (take_boot_flag()) {
        ESP_LOGI(TAG, "restarted to install an update");
        s_auto_install = s_restarted = true;
        const esp_timer_create_args_t aa = { .callback = auto_cb, .name = "upd_auto" };
        if (esp_timer_create(&aa, &s_auto_timer) == ESP_OK) esp_timer_start_periodic(s_auto_timer, 5LL * 1000000);
    }
}

void updater_check_now(void)
{
    if (s_busy || !s_info) return;
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
        set_state(UPD_ERROR, "No Wi-Fi");
    } else {
        s_busy = true;
        set_state(UPD_CHECKING, NULL);
        if (xTaskCreateWithCaps(check_task, "upd_check", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_busy = false;
            set_state(UPD_ERROR, "Out of memory");
        }
    }
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, CHECK_EVERY_US);
}

void updater_install(void)
{
    if (s_busy || !s_info || !s_info->available) return;   /* also allows a retry after a failure */
    s_busy = true;
    LOCK();
    s_info->state = UPD_INSTALLING;
    s_info->progress = 0;
    s_info->error[0] = 0;
    UNLOCK();
    /* internal stack: this task writes flash. Not enough left -> restart and install after boot. */
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if ((big >= INSTALL_MIN_BLOCK || s_restarted) &&
        xTaskCreate(install_task, "upd_install", INSTALL_STACK, NULL, 4, NULL) == pdPASS)
        return;
    if (s_restarted) {
        s_busy = false;
        set_state(UPD_ERROR, "Not enough memory to install. Restart the screen and try again.");
    } else {
        ESP_LOGW(TAG, "largest internal block %u: restarting to install", (unsigned)big);
        nvs_handle_t h;
        if (nvs_open("upd", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "boot", 1);
            nvs_commit(h);
            nvs_close(h);
        }
        set_state(UPD_INSTALLING, "Restarting first to free up memory. The update continues by itself after the restart.");
        static esp_timer_handle_t rt;
        const esp_timer_create_args_t ra = { .callback = restart_cb, .name = "upd_rst" };
        if (!rt) esp_timer_create(&ra, &rt);
        if (rt) esp_timer_start_once(rt, 3LL * 1000000);
        else esp_restart();
    }
}

void updater_get(upd_info_t *out)
{
    if (!s_info) { memset(out, 0, sizeof(*out)); return; }
    LOCK();
    *out = *s_info;
    UNLOCK();
}

upd_state_t updater_state(void) { return s_info ? s_info->state : UPD_IDLE; }
bool updater_update_known(void) { return s_info && s_info->available; }
