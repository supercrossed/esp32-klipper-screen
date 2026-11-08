/*
 * Webcam: MJPEG stream (or a snapshot loop if the camera has no stream)
 * -> hardware JPEG decode -> PPA scale/rotate/mirror -> triple buffer -> LVGL
 * image. All pixel work is done by P4 hardware.
 *
 * The stream is one long HTTP response (multipart/x-mixed-replace) in which the
 * server pushes each new frame, so there is no request round trip per frame.
 * Every part has a small header block; ustreamer, mjpg-streamer and
 * camera-streamer all send Content-Length, which lets us read the JPEG straight
 * into the decoder's input buffer. Without it we scan for the JPEG end marker.
 */
#include "webcam.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* task stacks in PSRAM: keeps internal SRAM for Wi-Fi/SDIO DMA */
#include "freertos/semphr.h"
#include "freertos/queue.h"
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
    char url[320];          /* snapshot URL */
    char stream_url[320];   /* MJPEG stream URL, "" = none */
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

/* Relative URLs are relative to the web UI (port 80), not Moonraker. */
static void make_abs(const printer_cfg_t *p, const char *u, char *out, size_t cap)
{
    if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) strlcpy(out, u, cap);
    else if (u[0] == '/') snprintf(out, cap, "http://%s%s", p->host, u);
    else snprintf(out, cap, "http://%s/%s", p->host, u);
    /* We have no TLS client; most LAN cams are fine over http. */
    if (!strncmp(out, "https://", 8)) memmove(out + 4, out + 5, strlen(out + 5) + 1);
}

/* Swap the stream/snapshot marker in a URL (action=stream, /stream and the like). */
static bool swap_kind(char *u, size_t cap, const char *from_q, const char *to_q,
                      const char *from_p, const char *to_p)
{
    char tail[220];
    const char *from = from_q, *to = to_q;
    char *a = strstr(u, from_q);
    if (!a) { a = strstr(u, from_p); from = from_p; to = to_p; }
    if (!a) return false;
    strlcpy(tail, a + strlen(from), sizeof(tail));
    snprintf(a, cap - (a - u), "%s%s", to, tail);
    return true;
}

static void resolve_urls(const printer_cfg_t *p, const webcam_t *cam, cam_ctx_t *c)
{
    char u[220];
    if (cam->snapshot_url[0]) {
        strlcpy(u, cam->snapshot_url, sizeof(u));
    } else {
        strlcpy(u, cam->stream_url, sizeof(u));
        swap_kind(u, sizeof(u), "action=stream", "action=snapshot", "/stream", "/snapshot");
    }
    make_abs(p, u, c->url, sizeof(c->url));

    c->stream_url[0] = 0;
    if (cam->stream_url[0]) {
        strlcpy(u, cam->stream_url, sizeof(u));
    } else {
        strlcpy(u, cam->snapshot_url, sizeof(u));
        if (!swap_kind(u, sizeof(u), "action=snapshot", "action=stream", "/snapshot", "/stream")) u[0] = 0;
    }
    if (u[0]) make_abs(p, u, c->stream_url, sizeof(c->stream_url));
}

static void set_status(cam_ctx_t *c, const char *s)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(c->status, s, sizeof(c->status));
    xSemaphoreGive(s_mtx);
}

/* ---- MJPEG stream reader ---- */

#define CARRY_SIZE 4096

typedef struct {
    esp_http_client_handle_t h;
    uint8_t carry[CARRY_SIZE];   /* bytes read but not consumed yet */
    int pos, len;
} mjpeg_t;

static void mjpeg_close(mjpeg_t *m)
{
    if (m->h) {
        esp_http_client_close(m->h);
        esp_http_client_cleanup(m->h);
        m->h = NULL;
    }
    m->pos = m->len = 0;
}

/* -> HTTP status (200 = streaming), or -1 if the connection failed. */
static int mjpeg_open(mjpeg_t *m, const char *url, const char *api_key)
{
    mjpeg_close(m);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 4000,
        .buffer_size = 16384,       /* fewer, larger socket reads */
        .buffer_size_tx = 1024,
        .user_agent = "KlipperRemote-ESP32P4",
    };
    m->h = esp_http_client_init(&cfg);
    if (!m->h) return -1;
    if (api_key && api_key[0]) esp_http_client_set_header(m->h, "X-Api-Key", api_key);
    for (int redirects = 0; redirects < 4; redirects++) {
        if (esp_http_client_open(m->h, 0) != ESP_OK || esp_http_client_fetch_headers(m->h) < 0) {
            mjpeg_close(m);
            return -1;
        }
        int st = esp_http_client_get_status_code(m->h);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            if (esp_http_client_set_redirection(m->h) != ESP_OK) { mjpeg_close(m); return st; }
            esp_http_client_close(m->h);
            continue;
        }
        if (st != 200) mjpeg_close(m);
        return st;
    }
    mjpeg_close(m);
    return -1;
}

/* Read up to n more bytes onto the end of the carry buffer. */
static bool carry_fill(mjpeg_t *m, int n)
{
    if (m->pos > 0) {
        memmove(m->carry, m->carry + m->pos, m->len - m->pos);
        m->len -= m->pos;
        m->pos = 0;
    }
    if (n > CARRY_SIZE - m->len) n = CARRY_SIZE - m->len;
    if (n <= 0) return false;
    int r = esp_http_client_read(m->h, (char *)m->carry + m->len, n);
    if (r <= 0) return false;
    m->len += r;
    return true;
}

/* Next frame into out. -> JPEG length, or -1 on a stream error. */
static int mjpeg_next(mjpeg_t *m, uint8_t *out, size_t cap)
{
    long clen = -1;
    /* Part header: [CRLF] --boundary CRLF Content-Type: ... CRLF Content-Length: N CRLF CRLF.
     * esp_http_client_read() waits until the requested count has arrived, so
     * ask for small amounts here; the JPEG that follows is always larger. */
    for (;;) {
        while (m->pos < m->len && (m->carry[m->pos] == '\r' || m->carry[m->pos] == '\n')) m->pos++;
        const uint8_t *b = m->carry + m->pos;
        int avail = m->len - m->pos;
        if (avail >= 2 && b[0] == 0xFF && b[1] == 0xD8) break;   /* bare JPEG, no part header */
        int e = -1;
        for (int i = 0; i + 3 < avail; i++) {
            if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') { e = i; break; }
        }
        if (e >= 0) {
            for (int l = 0; l < e;) {
                int nl = l;
                while (nl < e && b[nl] != '\n') nl++;
                if (nl - l > 15 && !strncasecmp((const char *)b + l, "Content-Length:", 15))
                    clen = strtol((const char *)b + l + 15, NULL, 10);
                l = nl + 1;
            }
            m->pos += e + 4;
            break;
        }
        if (avail > CARRY_SIZE - 512) return -1;               /* not a multipart stream */
        if (!carry_fill(m, 256)) return -1;
    }

    size_t got = m->len - m->pos;
    if (clen > 0) {
        if ((size_t)clen > cap) return -1;
        if (got > (size_t)clen) got = clen;
        memcpy(out, m->carry + m->pos, got);
        m->pos += got;
        while (got < (size_t)clen) {
            int r = esp_http_client_read(m->h, (char *)out + got, clen - got);
            if (r <= 0) return -1;
            got += r;
        }
        return clen;
    }

    /* No length: read until the JPEG end marker FF D9. 0xFF bytes inside the
     * compressed data are always byte-stuffed, so it cannot appear early. */
    if (got > cap) return -1;
    memcpy(out, m->carry + m->pos, got);
    m->pos = m->len = 0;
    size_t scanned = 0;
    for (;;) {
        for (size_t i = scanned; i + 1 < got; i++) {
            if (out[i] == 0xFF && out[i + 1] == 0xD9) {
                size_t end = i + 2, rest = got - end;
                if (rest > CARRY_SIZE) rest = CARRY_SIZE;
                memcpy(m->carry, out + end, rest);   /* start of the next part */
                m->len = rest;
                return end;
            }
        }
        scanned = got ? got - 1 : 0;
        if (got + 1024 > cap) return -1;
        int r = esp_http_client_read(m->h, (char *)out + got, 1024);
        if (r <= 0) return -1;
        got += r;
    }
}

/* ---- capture: a reader task fetches frames, a decoder task shows them ----
 *
 * Two JPEG input buffers: while the decoder works on one, the reader is
 * already pulling the next frame off the network into the other. */

#define NIN 2
#define PIPE_END 0xFF

typedef struct {
    cam_ctx_t *c;
    jpeg_decoder_handle_t dec;
    ppa_client_handle_t ppa;
    uint8_t *jin[NIN];
    int      jlen[NIN];
    size_t   jin_size;
    uint8_t *jout;
    size_t   jout_size;
    QueueHandle_t free_q, full_q;   /* buffer indices */
    SemaphoreHandle_t dec_done;
    volatile bool streaming;
    volatile int  bytes;            /* received since the last fps update */
} pipe_t;

/* Decode one JPEG, scale/rotate it into a free output buffer and publish it. */
static bool show_frame(pipe_t *p, const uint8_t *jin, int jlen, int *dec_w, int *dec_h)
{
    cam_ctx_t *c = p->c;
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jin, jlen, &info) != ESP_OK || !info.width || !info.height) {
        set_status(c, "Bad JPEG header");
        return false;
    }
    int aw = ALIGN_UP(info.width, 16), ah = ALIGN_UP(info.height, 16);
    size_t need = (size_t)aw * ah * 2;
    if (need > p->jout_size) {
        if (p->jout) heap_caps_free(p->jout);
        jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        p->jout = jpeg_alloc_decoder_mem(need, &out_cfg, &p->jout_size);
        if (!p->jout) {
            p->jout_size = 0;
            set_status(c, "Frame too large");
            vTaskDelay(pdMS_TO_TICKS(1000));
            return false;
        }
    }
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t osz = 0;
    if (jpeg_decoder_process(p->dec, &dcfg, jin, jlen, p->jout, p->jout_size, &osz) != ESP_OK) {
        set_status(c, "JPEG decode failed (progressive?)");
        return false;
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
    *dec_w = info.width;
    *dec_h = info.height;

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
            .buffer = p->jout,
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
    esp_err_t pe = ppa_do_scale_rotate_mirror(p->ppa, &op);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    c->writing = -1;
    if (pe == ESP_OK) { c->ready = w; c->out_w = ow; c->out_h = oh; }
    xSemaphoreGive(s_mtx);
    if (pe != ESP_OK) { set_status(c, "Scale failed"); return false; }
    return true;
}

static void dec_task(void *arg)
{
    pipe_t *p = arg;
    cam_ctx_t *c = p->c;
    int frames = 0, dec_w = 0, dec_h = 0;
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        uint8_t i;
        xQueueReceive(p->full_q, &i, portMAX_DELAY);
        if (i == PIPE_END) break;
        bool ok = !c->stop && show_frame(p, p->jin[i], p->jlen[i], &dec_w, &dec_h);
        xQueueSend(p->free_q, &i, portMAX_DELAY);
        if (!ok) continue;
        frames++;
        int64_t now = esp_timer_get_time();
        if (now - t0 > 1000000) {
            c->fps = frames * 1e6f / (now - t0);
            ESP_LOGD(TAG, "%.1f fps, %d KB/s", c->fps, (int)(p->bytes * 1000000LL / (now - t0) / 1024));
            p->bytes = 0;
            frames = 0;
            t0 = now;
            char m[64];
            snprintf(m, sizeof(m), "%dx%d  %.1f fps%s", dec_w, dec_h, c->fps, p->streaming ? "" : "  (snapshots)");
            set_status(c, m);
        }
    }
    xSemaphoreGive(p->dec_done);
    vTaskDeleteWithCaps(NULL);
}

/* Fetch the next JPEG into out. -> length, or -1 (after any back-off delay). */
static int fetch_frame(pipe_t *p, mjpeg_t *mj, esp_http_client_handle_t *conn, http_buf_t *buf,
                       uint8_t *out, int *stream_frames, int *stream_tries, int *fails)
{
    cam_ctx_t *c = p->c;
    if (p->streaming) {
        if (!mj->h) {
            int st = mjpeg_open(mj, c->stream_url, c->api_key);
            if (c->stop) return -1;
            if (st != 200) {
                ESP_LOGW(TAG, "stream open: %d", st);
                if (++*stream_tries >= 2) {
                    ESP_LOGW(TAG, "no MJPEG stream, using snapshots");
                    p->streaming = false;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                return -1;
            }
            *stream_frames = 0;
            ESP_LOGI(TAG, "streaming %s", c->stream_url);
        }
        int n = mjpeg_next(mj, out, p->jin_size);
        if (c->stop) return -1;
        if (n < 4 || out[0] != 0xFF || out[1] != 0xD8) {
            ESP_LOGW(TAG, "stream read failed after %d frames", *stream_frames);
            mjpeg_close(mj);
            if (*stream_frames == 0 && ++*stream_tries >= 2) {
                ESP_LOGW(TAG, "stream gave no frames, using snapshots");
                p->streaming = false;
            } else {
                set_status(c, "Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            return -1;
        }
        ++*stream_frames;
        *stream_tries = 0;
        return n;
    }

    int st = http_request(conn, HTTP_METHOD_GET, c->url, c->api_key, NULL, NULL, 0, buf, 5000);
    if (c->stop) return -1;
    if (st != 200 || buf->len < 4 || buf->data[0] != 0xFF || buf->data[1] != 0xD8) {
        char m[64];
        if (st < 0) snprintf(m, sizeof(m), "No response from camera");
        else if (st != 200) snprintf(m, sizeof(m), "Camera HTTP %d", st);
        else snprintf(m, sizeof(m), "Not a JPEG snapshot");
        set_status(c, m);
        vTaskDelay(pdMS_TO_TICKS(++*fails > 3 ? 2000 : 300));
        return -1;
    }
    *fails = 0;
    if (buf->len > p->jin_size) return -1;
    memcpy(out, buf->data, buf->len);
    return buf->len;
}

static void cam_task(void *arg)
{
    cam_ctx_t *c = arg;
    pipe_t pp = { .c = c }, *p = &pp;
    esp_http_client_handle_t conn = NULL;
    http_buf_t buf = { .max = JPEG_IN_MAX };
    mjpeg_t *mj = NULL;
    bool dec_running = false;

    jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 200 };
    /* A just-closed stream may still be releasing the engine: give it up to 5 s. */
    for (int tries = 0; jpeg_new_decoder_engine(&ecfg, &p->dec) != ESP_OK; tries++) {
        if (tries >= 50 || c->stop) { p->dec = NULL; set_status(c, "JPEG engine busy"); goto done; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pcfg, &p->ppa) != ESP_OK) { p->ppa = NULL; set_status(c, "PPA unavailable"); goto done; }
    for (int i = 0; i < NIN; i++) {
        jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        p->jin[i] = jpeg_alloc_decoder_mem(JPEG_IN_MAX, &in_cfg, &p->jin_size);
        if (!p->jin[i]) { set_status(c, "Out of memory"); goto done; }
    }
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
    if (c->stream_url[0]) mj = heap_caps_calloc(1, sizeof(*mj), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    p->streaming = mj != NULL;   /* falls back to snapshots if the stream won't work */

    p->free_q = xQueueCreate(NIN, 1);
    p->full_q = xQueueCreate(NIN + 1, 1);
    p->dec_done = xSemaphoreCreateBinary();
    if (!p->free_q || !p->full_q || !p->dec_done) { set_status(c, "Out of memory"); goto done; }
    for (uint8_t i = 0; i < NIN; i++) xQueueSend(p->free_q, &i, 0);
    if (xTaskCreateWithCaps(dec_task, "webcam_dec", 4096, p, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        set_status(c, "Out of memory");
        goto done;
    }
    dec_running = true;

    set_status(c, "Connecting...");
    int stream_frames = 0;   /* frames seen on the stream since it last opened */
    int stream_tries = 0;    /* opens that failed before giving a single frame */
    int fails = 0;
    while (!c->stop) {
        uint8_t i;
        if (xQueueReceive(p->free_q, &i, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        int n = fetch_frame(p, mj, &conn, &buf, p->jin[i], &stream_frames, &stream_tries, &fails);
        if (n <= 0 || c->stop) { xQueueSend(p->free_q, &i, 0); continue; }
        p->jlen[i] = n;
        p->bytes += n;
        xQueueSend(p->full_q, &i, portMAX_DELAY);
    }

done:
    if (dec_running) {
        uint8_t end = PIPE_END;
        xQueueSend(p->full_q, &end, portMAX_DELAY);
        xSemaphoreTake(p->dec_done, portMAX_DELAY);
    }
    if (p->free_q) vQueueDelete(p->free_q);
    if (p->full_q) vQueueDelete(p->full_q);
    if (p->dec_done) vSemaphoreDelete(p->dec_done);
    if (mj) { mjpeg_close(mj); heap_caps_free(mj); }
    http_conn_close(&conn);
    http_buf_free(&buf);
    if (p->dec) jpeg_del_decoder_engine(p->dec);
    if (p->ppa) ppa_unregister_client(p->ppa);
    for (int i = 0; i < NIN; i++) if (p->jin[i]) heap_caps_free(p->jin[i]);
    if (p->jout) heap_caps_free(p->jout);
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
    resolve_urls(printer, cam, c);
    strlcpy(c->api_key, printer->api_key, sizeof(c->api_key));
    c->flip_h = cam->flip_h;
    c->flip_v = cam->flip_v;
    c->rotation = cam->rotation;
    c->max_w = max_w;
    c->max_h = max_h;
    c->front = c->ready = c->writing = -1;
    strcpy(c->status, "Starting...");
    ESP_LOGI(TAG, "start stream=%s snapshot=%s", c->stream_url[0] ? c->stream_url : "-", c->url);
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
