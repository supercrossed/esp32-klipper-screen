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
#include "src/display/lv_display_private.h"
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

/* Scroll benchmark (console `bench`): a timer scrolls the scrollable object
 * with the most content 20 px per step, 30 steps down and 30 back, while
 * display events time every real refresh (render + flush). */
static lv_obj_t *s_best;
static int32_t s_best_range;
static int s_step = -1, s_frames;
static int64_t s_t_start, s_t_total, s_t_worst, s_t_frame0;
/* Time spent inside the panel flush (rotate + copy to the frame buffer). */
static lv_display_flush_cb_t s_orig_flush;
static int64_t s_t_flush, s_t_flush_last;
static int s_flushes;
static int64_t s_px;

static void timed_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    bool last = lv_display_flush_is_last(d);
    int64_t t0 = esp_timer_get_time();
    s_orig_flush(d, a, px);
    int64_t dt = esp_timer_get_time() - t0;
    s_t_flush += dt;
    if (last) s_t_flush_last += dt;
    s_flushes++;
    s_px += (int64_t)lv_area_get_width(a) * lv_area_get_height(a);
}

/* Counters from the adapter's async partial rotate (absent when it is off). */
extern volatile uint32_t g_lv_async_rot_queued __attribute__((weak)), g_lv_async_rot_done __attribute__((weak)),
    g_lv_async_rot_fallback __attribute__((weak)), g_lv_async_rot_timeout __attribute__((weak));

static void find_scrollable(lv_obj_t *o)
{
    int32_t r = lv_obj_get_scroll_bottom(o) + lv_obj_get_scroll_top(o);
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && r > s_best_range) { s_best = o; s_best_range = r; }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) find_scrollable(lv_obj_get_child(o, i));
}

static void refr_cb(lv_event_t *e)
{
    if (s_step < 0) return;
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) s_t_frame0 = esp_timer_get_time();
    else if (s_t_frame0) {
        int64_t dt = esp_timer_get_time() - s_t_frame0;
        s_t_total += dt;
        if (dt > s_t_worst) s_t_worst = dt;
        s_frames++;
        s_t_frame0 = 0;
    }
}

static void bench_timer(lv_timer_t *t)
{
    if (s_step >= 60) {
        int64_t wall = esp_timer_get_time() - s_t_start;
        printf("bench: %u items, %ld px of scroll: %d frames in %lld ms (%.1f fps), render avg %lld ms, worst %lld ms\n",
               (unsigned)lv_obj_get_child_count(s_best), (long)s_best_range, s_frames, wall / 1000,
               s_frames * 1e6 / (double)wall, s_frames ? s_t_total / s_frames / 1000 : 0, s_t_worst / 1000);
        printf("bench: flush %d calls, %.1f ms/frame in flush (%.1f in the last strip), %lld kpx/frame\n", s_flushes,
               s_frames ? s_t_flush / 1000.0 / s_frames : 0, s_frames ? s_t_flush_last / 1000.0 / s_frames : 0, s_frames ? s_px / s_frames / 1000 : 0);
        if (&g_lv_async_rot_queued)
            printf("bench: async rotate queued %lu, done %lu, fell back %lu, timeouts %lu\n",
                   (unsigned long)g_lv_async_rot_queued, (unsigned long)g_lv_async_rot_done,
                   (unsigned long)g_lv_async_rot_fallback, (unsigned long)g_lv_async_rot_timeout);
        lv_display_get_default()->flush_cb = s_orig_flush;
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refr_cb, NULL);
        lv_timer_delete(t);
        s_step = -1;
        return;
    }
    lv_obj_scroll_by(s_best, 0, s_step < 30 ? -20 : 20, LV_ANIM_OFF);
    s_step++;
}

static void bench_async(void *arg)
{
    s_best = NULL;
    s_best_range = 0;
    find_scrollable(lv_screen_active());
    if (!s_best || s_step >= 0) { printf("bench: nothing scrollable (or already running)\n"); return; }
    s_step = 0;
    s_frames = 0;
    s_t_total = s_t_worst = s_t_frame0 = 0;
    s_t_start = esp_timer_get_time();
    s_t_flush = s_t_flush_last = s_px = 0;
    s_flushes = 0;
    s_orig_flush = lv_display_get_default()->flush_cb;
    lv_display_get_default()->flush_cb = timed_flush;
    lv_display_add_event_cb(lv_display_get_default(), refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), refr_cb, LV_EVENT_REFR_READY, NULL);
    lv_timer_create(bench_timer, 16, NULL);
}

void screenshot_bench(void)
{
    bsp_display_lock(-1);
    lv_async_call(bench_async, NULL);
    bsp_display_unlock();
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

/* Page timing (console `page`): build time, and time until that page's first
 * frame is on the panel. Call both with the display lock held. */
static int64_t s_pg_t0, s_pg_built;

static void page_ready_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    printf("page: built in %lld ms, on screen after %lld ms\n", (s_pg_built - s_pg_t0) / 1000, (now - s_pg_t0) / 1000);
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), page_ready_cb, NULL);
}

void bench_page_begin(void) { s_pg_t0 = esp_timer_get_time(); }

void bench_page_end(void)
{
    s_pg_built = esp_timer_get_time();
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), page_ready_cb, NULL);
    lv_display_add_event_cb(lv_display_get_default(), page_ready_cb, LV_EVENT_REFR_READY, NULL);
}

/* Console `fbcheck`: compare what the panel scans out with what LVGL renders.
 * Draws anything pending, snapshots the screen, and counts the pixels that
 * differ in each of the panel's three frame buffers (rotation applied). The
 * buffer on screen must match exactly; the others may hold older frames. */
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "app_settings.h"
extern esp_lcd_panel_handle_t g_kr_panel;

static void fbcheck_async(void *arg)
{
    int orient = g_settings.orientation & 3;
    if (orient != 0 && orient != 1) { printf("fbcheck: only landscape and portrait\n"); return; }
    if (lv_obj_get_child_count(lv_layer_top()) > 0) { printf("fbcheck: something is on the top layer, try again\n"); return; }
    lv_refr_now(NULL);
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) { printf("fbcheck: snapshot failed\n"); return; }
    void *fb[3] = { 0 };
    if (!g_kr_panel || esp_lcd_dpi_panel_get_frame_buffer(g_kr_panel, 3, &fb[0], &fb[1], &fb[2]) != ESP_OK) {
        printf("fbcheck: no frame buffers\n");
        lv_draw_buf_destroy(snap);
        return;
    }
    const int W = BSP_LCD_H_RES, H = BSP_LCD_V_RES;   /* panel, portrait */
    int lw = snap->header.w, lh = snap->header.h;
    size_t sstride = snap->header.stride;
    int best = -1;
    long best_bad = -1;
    for (int k = 0; k < 3; k++) {
        if (!fb[k]) continue;
        esp_cache_msync(fb[k], (size_t)W * H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        esp_cache_msync(fb[k], (size_t)W * H * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        const uint16_t *p = fb[k];
        long bad = 0;
        int fx = -1, fy = -1;
        for (int y = 0; y < lh; y++) {
            const uint16_t *s = (const uint16_t *)(snap->data + (size_t)y * sstride);
            for (int x = 0; x < lw; x++) {
                int px = orient == 0 ? W - 1 - y : x;
                int py = orient == 0 ? x : y;
                if (p[(size_t)py * W + px] != s[x]) {
                    if (!bad) { fx = x; fy = y; }
                    bad++;
                }
            }
        }
        printf("fbcheck: fb%d %ld of %d pixels differ", k, bad, lw * lh);
        if (bad) printf(" (first at %d,%d)", fx, fy);
        printf("\n");
        if (best < 0 || bad < best_bad) { best = k; best_bad = bad; }
    }
    printf("fbcheck: %s (best fb%d, %ld differ)\n", best_bad == 0 ? "PASS" : "FAIL", best, best_bad);
    lv_draw_buf_destroy(snap);
}

void screenshot_fbcheck(void)
{
    bsp_display_lock(-1);
    lv_async_call(fbcheck_async, NULL);
    bsp_display_unlock();
}
