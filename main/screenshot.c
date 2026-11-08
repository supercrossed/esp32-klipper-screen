/*
 * Screenshots for docs and bug reports. Not started by default: the serial
 * console command `shot` starts a tiny HTTP server on port 8080 that serves
 * GET /screenshot.bmp (24-bit BMP of what is on the display, top layer included).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include "esp_timer.h"
#include "battery.h"
#include "esp_system.h"

static const char *TAG = "shot";
static httpd_handle_t s_srv;

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

typedef struct {
    SemaphoreHandle_t done;
    lv_draw_buf_t *base, *top;
} snap_job_t;

/* Runs on the LVGL task: rendering a snapshot needs that task's large stack. */
static void snap_async(void *arg)
{
    snap_job_t *j = arg;
    j->base = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    j->top = NULL;
    if (lv_obj_get_child_count(lv_layer_top()) > 0)
        j->top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    xSemaphoreGive(j->done);
}

/* One job at a time, kept off the handler's stack: a snapshot that outlives the
 * timeout still has somewhere valid to write, and the next request collects it. */
static snap_job_t s_job;
static bool s_pending;

static esp_err_t shot_handler(httpd_req_t *req)
{
    if (!s_job.done) s_job.done = xSemaphoreCreateBinary();
    if (s_pending) {   /* an earlier snapshot timed out: wait for it, then discard it */
        if (xSemaphoreTake(s_job.done, pdMS_TO_TICKS(20000)) != pdTRUE) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "previous snapshot still running");
            return ESP_FAIL;
        }
        if (s_job.base) lv_draw_buf_destroy(s_job.base);
        if (s_job.top) lv_draw_buf_destroy(s_job.top);
        s_pending = false;
    }
    s_pending = true;
    bsp_display_lock(-1);
    lv_async_call(snap_async, &s_job);
    bsp_display_unlock();
    if (xSemaphoreTake(s_job.done, pdMS_TO_TICKS(20000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot timed out");
        return ESP_FAIL;
    }
    s_pending = false;
    lv_draw_buf_t *base = s_job.base, *top = s_job.top;
    if (!base) {
        if (top) lv_draw_buf_destroy(top);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed (out of memory?)");
        return ESP_FAIL;
    }
    int w = base->header.w, h = base->header.h;
    uint32_t bstride = base->header.stride;
    int row = w * 3;   /* 1280*3 is already 4-byte aligned; pad generally anyway */
    int pad = (4 - row % 4) % 4;
    uint32_t img = (row + pad) * h;

    uint8_t hdr[54] = { 'B', 'M' };
    put32(hdr + 2, 54 + img);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(hdr + 34, img);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (char *)hdr, sizeof(hdr));

    const int ROWS = 16;
    uint8_t *chunk = heap_caps_malloc((row + pad) * ROWS, MALLOC_CAP_SPIRAM);
    esp_err_t ret = ESP_OK;
    for (int y0 = h - 1; y0 >= 0 && chunk && ret == ESP_OK; y0 -= ROWS) {
        int n = 0;
        for (int y = y0; y > y0 - ROWS && y >= 0; y--, n++) {
            uint8_t *dst = chunk + n * (row + pad);
            const uint8_t *src = base->data + y * bstride;   /* LVGL RGB888 is B,G,R in memory = BMP order */
            memcpy(dst, src, row);
            if (top && y < (int)top->header.h) {
                const uint8_t *t = top->data + y * top->header.stride;
                for (int x = 0; x < w && x < (int)top->header.w; x++) {
                    uint8_t a = t[x * 4 + 3];
                    if (!a) continue;
                    for (int c = 0; c < 3; c++)
                        dst[x * 3 + c] = (t[x * 4 + c] * a + dst[x * 3 + c] * (255 - a)) / 255;
                }
            }
            memset(dst + row, 0, pad);
        }
        ret = httpd_resp_send_chunk(req, (char *)chunk, n * (row + pad));
    }
    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(chunk);
    lv_draw_buf_destroy(base);
    if (top) lv_draw_buf_destroy(top);
    ESP_LOGI(TAG, "served %dx%d screenshot", w, h);
    return ret;
}

/* GET /bat: battery readings as text, for testing with the USB cable unplugged. */
static esp_err_t bat_handler(httpd_req_t *req)
{
    bat_info_t b;
    battery_get(&b);
    char t[200];
    snprintf(t, sizeof(t), "%lld s  %s  %d mV (reading %d mV)  %d%%  boot=%d drop=%d  reset=%d\n",
             (long long)(esp_timer_get_time() / 1000000), battery_state_str(b.state), b.mv, b.raw_mv, b.soc,
             b.boot_mv, b.boot_drop, (int)esp_reset_reason());
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, t);
}

bool screenshot_server_start(void)
{
    if (s_srv) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;
    cfg.ctrl_port = 32769;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 2;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) return false;
    httpd_uri_t u = { .uri = "/screenshot.bmp", .method = HTTP_GET, .handler = shot_handler };
    httpd_register_uri_handler(s_srv, &u);
    httpd_uri_t ub = { .uri = "/bat", .method = HTTP_GET, .handler = bat_handler };
    httpd_register_uri_handler(s_srv, &ub);
    return true;
}

/* Simulated tap for headless captures: click the clickable object under (x, y). */
static lv_point_t s_tap;

/* Runs on the LVGL task: click handlers may build whole pages. */
static void tap_async(void *arg)
{
    lv_obj_t *o = NULL;
    lv_obj_t *top = lv_layer_top();
    for (int i = (int)lv_obj_get_child_count(top) - 1; i >= 0 && !o; i--)   /* newest overlay first */
        o = lv_indev_search_obj(lv_obj_get_child(top, i), &s_tap);
    if (!o) o = lv_indev_search_obj(lv_screen_active(), &s_tap);
    while (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) o = lv_obj_get_parent(o);
    if (o) lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
}

bool screenshot_tap(int x, int y)
{
    bsp_display_lock(-1);
    s_tap.x = x;
    s_tap.y = y;
    lv_async_call(tap_async, NULL);
    bsp_display_unlock();
    return true;
}
