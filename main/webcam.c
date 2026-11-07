/*
 * Webcam: snapshot loop -> hardware JPEG decode -> PPA scale/rotate/mirror
 * -> triple buffer -> LVGL image. All pixel work is done by P4 hardware.
 */
#include "webcam.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* task stacks in PSRAM: keeps internal SRAM for Wi-Fi/SDIO DMA */
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "http_util.h"

static const char *TAG = "webcam";

#define JPEG_IN_MAX   (1024 * 1024)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define CACHE_ALIGN 128

typedef struct {
    volatile bool stop;
    char url[320];
    char api_key[65];
    bool flip_h, flip_v;
    int  rotation;
    int  max_w, max_h;

    uint8_t *out[3];
    size_t   out_size;
    int      out_w, out_h;
    int      front, ready, writing;   /* buffer indices, -1 = none */

    char  status[64];
    float fps;
} cam_ctx_t;

static SemaphoreHandle_t s_mtx;
static cam_ctx_t *s_cur;

static void resolve_url(const printer_cfg_t *p, const webcam_t *cam, char *out, size_t cap)
{
    char u[220];
    if (cam->snapshot_url[0]) {
        strlcpy(u, cam->snapshot_url, sizeof(u));
    } else {
        /* Derive a snapshot URL from the stream URL (mjpg-streamer/ustreamer/camera-streamer). */
        strlcpy(u, cam->stream_url, sizeof(u));
        char *a = strstr(u, "action=stream");
        if (a) {
            char tail[200];
            strlcpy(tail, a + strlen("action=stream"), sizeof(tail));
            snprintf(a, sizeof(u) - (a - u), "action=snapshot%s", tail);
        } else if ((a = strstr(u, "/stream"))) {
            char tail[200];
            strlcpy(tail, a + strlen("/stream"), sizeof(tail));
            snprintf(a, sizeof(u) - (a - u), "/snapshot%s", tail);
        }
    }
    /* Relative URLs are relative to the web UI (port 80), not Moonraker. */
    if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) strlcpy(out, u, cap);
    else if (u[0] == '/') snprintf(out, cap, "http://%s%s", p->host, u);
    else snprintf(out, cap, "http://%s/%s", p->host, u);
    /* We have no TLS client; most LAN cams are fine over http. */
    if (!strncmp(out, "https://", 8)) memmove(out + 4, out + 5, strlen(out + 5) + 1);
}

static void set_status(cam_ctx_t *c, const char *s)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(c->status, s, sizeof(c->status));
    xSemaphoreGive(s_mtx);
}

static void cam_task(void *arg)
{
    cam_ctx_t *c = arg;
    jpeg_decoder_handle_t dec = NULL;
    ppa_client_handle_t ppa = NULL;
    esp_http_client_handle_t conn = NULL;
    http_buf_t buf = { .max = JPEG_IN_MAX };
    uint8_t *jin = NULL, *jout = NULL;
    size_t jin_size = 0, jout_size = 0;
    int dec_w = 0, dec_h = 0;

    jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 200 };
    /* A just-closed stream may still be releasing the engine: give it up to 3 s. */
    for (int tries = 0; jpeg_new_decoder_engine(&ecfg, &dec) != ESP_OK; tries++) {
        if (tries >= 30 || c->stop) { set_status(c, "JPEG engine busy"); goto done; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pcfg, &ppa) != ESP_OK) { set_status(c, "PPA unavailable"); goto done; }
    jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jin = jpeg_alloc_decoder_mem(JPEG_IN_MAX, &in_cfg, &jin_size);
    if (!jin) { set_status(c, "Out of memory"); goto done; }
    /* Output triple buffer, allocated once at the maximum view size so the UI
     * never sees a buffer freed under it when the camera resolution changes. */
    c->out_size = ALIGN_UP((size_t)c->max_w * c->max_h * 2, CACHE_ALIGN);
    for (int i = 0; i < 3; i++) {
        uint8_t *b = heap_caps_aligned_calloc(CACHE_ALIGN, 1, c->out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!b) { set_status(c, "Out of memory"); goto done; }
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        c->out[i] = b;
        xSemaphoreGive(s_mtx);
    }

    set_status(c, "Connecting...");
    int frames = 0;
    int64_t t0 = esp_timer_get_time();
    int fails = 0;

    while (!c->stop) {
        int st = http_request(&conn, HTTP_METHOD_GET, c->url, c->api_key, NULL, NULL, 0, &buf, 5000);
        if (c->stop) break;
        if (st != 200 || buf.len < 4 || buf.data[0] != 0xFF || buf.data[1] != 0xD8) {
            char m[64];
            if (st < 0) snprintf(m, sizeof(m), "No response from camera");
            else if (st != 200) snprintf(m, sizeof(m), "Camera HTTP %d", st);
            else snprintf(m, sizeof(m), "Not a JPEG snapshot");
            set_status(c, m);
            if (++fails > 3) vTaskDelay(pdMS_TO_TICKS(2000));
            else vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        fails = 0;
        if (buf.len > jin_size) continue;
        memcpy(jin, buf.data, buf.len);

        jpeg_decode_picture_info_t info;
        if (jpeg_decoder_get_info(jin, buf.len, &info) != ESP_OK || !info.width || !info.height) {
            set_status(c, "Bad JPEG header");
            continue;
        }
        int aw = ALIGN_UP(info.width, 16), ah = ALIGN_UP(info.height, 16);
        size_t need = (size_t)aw * ah * 2;
        if (need > jout_size) {
            if (jout) heap_caps_free(jout);
            jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
            jout = jpeg_alloc_decoder_mem(need, &out_cfg, &jout_size);
            if (!jout) { jout_size = 0; set_status(c, "Frame too large"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        }
        jpeg_decode_cfg_t dcfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t osz = 0;
        if (jpeg_decoder_process(dec, &dcfg, jin, buf.len, jout, jout_size, &osz) != ESP_OK) {
            set_status(c, "JPEG decode failed (progressive?)");
            continue;
        }

        /* Output geometry: fit inside max_w x max_h keeping aspect. */
        bool rot90 = c->rotation == 90 || c->rotation == 270;
        int sw = rot90 ? info.height : info.width;
        int sh = rot90 ? info.width : info.height;
        float sc = (float)c->max_w / sw;
        if ((float)c->max_h / sh < sc) sc = (float)c->max_h / sh;
        if (sc > 4.0f) sc = 4.0f;
        sc = (int)(sc * 16) / 16.0f;          /* PPA scale has 1/16 granularity */
        if (sc < 0.0625f) sc = 0.0625f;
        /* Exactly what the PPA writes (floor of block x scale); rounding these
         * down further makes the output block overrun the picture -> error. */
        int ow = (int)(sw * sc), oh = (int)(sh * sc);
        while ((ow > c->max_w || oh > c->max_h) && sc > 0.0625f) {
            sc -= 0.0625f;
            ow = (int)(sw * sc);
            oh = (int)(sh * sc);
        }
        dec_w = info.width;
        dec_h = info.height;

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        int w = 0;
        while (w == c->front || w == c->ready) w++;
        c->writing = w;
        xSemaphoreGive(s_mtx);

        ppa_srm_rotation_angle_t ang = PPA_SRM_ROTATION_ANGLE_0;
        /* PPA rotates counter-clockwise; webcam "rotation" is clockwise. */
        if (c->rotation == 90) ang = PPA_SRM_ROTATION_ANGLE_270;
        else if (c->rotation == 180) ang = PPA_SRM_ROTATION_ANGLE_180;
        else if (c->rotation == 270) ang = PPA_SRM_ROTATION_ANGLE_90;
        ppa_srm_oper_config_t op = {
            .in = {
                .buffer = jout,
                .pic_w = aw, .pic_h = ah,
                .block_w = info.width, .block_h = info.height,
                .block_offset_x = 0, .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = c->out[w],
                .buffer_size = c->out_size,
                .pic_w = ow, .pic_h = oh,
                .block_offset_x = 0, .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = ang,
            .scale_x = sc, .scale_y = sc,
            .mirror_x = c->flip_h,
            .mirror_y = c->flip_v,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t pe = ppa_do_scale_rotate_mirror(ppa, &op);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        c->writing = -1;
        if (pe == ESP_OK) { c->ready = w; c->out_w = ow; c->out_h = oh; }
        xSemaphoreGive(s_mtx);
        if (pe != ESP_OK) { set_status(c, "Scale failed"); continue; }

        frames++;
        int64_t now = esp_timer_get_time();
        if (now - t0 > 1000000) {
            c->fps = frames * 1e6f / (now - t0);
            frames = 0;
            t0 = now;
            char m[64];
            snprintf(m, sizeof(m), "%dx%d  %.1f fps", dec_w, dec_h, c->fps);
            set_status(c, m);
        }
        vTaskDelay(1);   /* be polite to the rest of the system */
    }

done:
    http_conn_close(&conn);
    http_buf_free(&buf);
    if (dec) jpeg_del_decoder_engine(dec);
    if (ppa) ppa_unregister_client(ppa);
    if (jin) heap_caps_free(jin);
    if (jout) heap_caps_free(jout);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < 3; i++) if (c->out[i]) heap_caps_free(c->out[i]);
    if (s_cur == c) s_cur = NULL;
    xSemaphoreGive(s_mtx);
    free(c);
    ESP_LOGI(TAG, "stream stopped, buffers freed");
    vTaskDeleteWithCaps(NULL);
}

bool webcam_start(const printer_cfg_t *printer, const webcam_t *cam, int max_w, int max_h)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    webcam_stop();
    cam_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return false;
    resolve_url(printer, cam, c->url, sizeof(c->url));
    strlcpy(c->api_key, printer->api_key, sizeof(c->api_key));
    c->flip_h = cam->flip_h;
    c->flip_v = cam->flip_v;
    c->rotation = cam->rotation;
    c->max_w = max_w;
    c->max_h = max_h;
    c->front = c->ready = c->writing = -1;
    strcpy(c->status, "Starting...");
    ESP_LOGI(TAG, "start %s", c->url);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_cur = c;
    xSemaphoreGive(s_mtx);
    if (xTaskCreateWithCaps(cam_task, "webcam", 6144, c, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_cur = NULL;
        free(c);
        return false;
    }
    return true;
}

void webcam_stop(void)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_cur) {
        s_cur->stop = true;   /* task frees itself and its buffers */
        s_cur->front = -1;
        s_cur = NULL;
    }
    xSemaphoreGive(s_mtx);
}

bool webcam_take_frame(const uint8_t **buf, int *w, int *h)
{
    if (!s_mtx) return false;
    bool r = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cam_ctx_t *c = s_cur;
    if (c && c->ready >= 0) {
        c->front = c->ready;
        c->ready = -1;
        *buf = c->out[c->front];
        *w = c->out_w;
        *h = c->out_h;
        r = true;
    }
    xSemaphoreGive(s_mtx);
    return r;
}

void webcam_status(char *out, size_t cap)
{
    if (!s_mtx) { strlcpy(out, "", cap); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(out, s_cur ? s_cur->status : "Stopped", cap);
    xSemaphoreGive(s_mtx);
}
