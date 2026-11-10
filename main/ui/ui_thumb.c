#include "ui_thumb.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "printers.h"
#include "src/draw/lv_image_decoder_private.h"

static const char *TAG = "thumb";

/* Decoded thumbnails, already scaled to the box they are shown in. Entries are
 * found by (thumbnail version, box size): versions are unique across printers,
 * so a stale or reordered printer index can never show the wrong picture.
 * An entry is freed only when no image uses it, keeping the newest few spare. */
#define SPARE_MAX 8

struct thumb_entry {
    thumb_entry_t *next;
    uint32_t ver;
    int      box;
    int      refs;
    uint32_t used;           /* last-use tick, for picking which spare to drop */
    lv_image_dsc_t dsc;      /* ARGB8888, w x h, fits inside box x box */
};

static thumb_entry_t *s_cache;
static uint32_t s_tick;

static uint32_t png_dim(const uint8_t *p, int off)
{
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) | ((uint32_t)p[off + 2] << 8) | p[off + 3];
}

static void entry_destroy(thumb_entry_t *e)
{
    lv_image_cache_drop(&e->dsc);
    heap_caps_free((void *)e->dsc.data);
    heap_caps_free(e);
}

/* Drop the least recently used spares beyond SPARE_MAX. */
static void trim(void)
{
    for (;;) {
        int spare = 0;
        thumb_entry_t *old = NULL;
        for (thumb_entry_t *e = s_cache; e; e = e->next)
            if (!e->refs) {
                spare++;
                if (!old || (int32_t)(e->used - old->used) < 0) old = e;
            }
        if (spare <= SPARE_MAX || !old) return;
        for (thumb_entry_t **pp = &s_cache; *pp; pp = &(*pp)->next)
            if (*pp == old) { *pp = old->next; break; }
        entry_destroy(old);
    }
}

static thumb_entry_t *find(uint32_t ver, int box)
{
    for (thumb_entry_t *e = s_cache; e; e = e->next)
        if (e->ver == ver && e->box == box) return e;
    return NULL;
}

/* Decode the PNG and scale it to fit box x box (area average when shrinking,
 * nearest pixel when growing). NULL if the decoder gives a format we do not
 * handle, or memory runs out: the caller then falls back to LVGL scaling. */
static thumb_entry_t *make_entry(const uint8_t *png, size_t len, uint32_t ver, int box)
{
    lv_image_dsc_t src = { 0 };
    src.header.magic = LV_IMAGE_HEADER_MAGIC;
    src.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    src.header.w = png_dim(png, 16);
    src.header.h = png_dim(png, 20);
    src.data = png;
    src.data_size = len;

    lv_image_decoder_dsc_t dd;
    if (lv_image_decoder_open(&dd, &src, NULL) != LV_RESULT_OK) {
        lv_image_cache_drop(&src);
        return NULL;
    }
    thumb_entry_t *e = NULL;
    uint8_t *out = NULL;
    const lv_draw_buf_t *db = dd.decoded;
    lv_color_format_t cf = db ? (lv_color_format_t)db->header.cf : LV_COLOR_FORMAT_UNKNOWN;
    int sw = db ? db->header.w : 0, sh = db ? db->header.h : 0;
    int sstride = db ? db->header.stride : 0;
    int dw = 0, dh = 0;

    if (!db || !db->data || sw <= 0 || sh <= 0 || box <= 0) goto out;
    if (cf != LV_COLOR_FORMAT_ARGB8888 && cf != LV_COLOR_FORMAT_XRGB8888) {
        ESP_LOGW(TAG, "decoder gave format %d, using LVGL scaling", (int)cf);
        goto out;
    }
    if (sw >= sh) { dw = box; dh = (sh * box + sw / 2) / sw; }   /* contain */
    else          { dh = box; dw = (sw * box + sh / 2) / sh; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    e = heap_caps_calloc(1, sizeof(*e), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out = heap_caps_malloc((size_t)dw * dh * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!e || !out) {
        heap_caps_free(e);
        heap_caps_free(out);
        e = NULL;
        goto out;
    }
    bool has_alpha = cf == LV_COLOR_FORMAT_ARGB8888;
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            /* colours weighted by alpha, so transparent edges do not darken */
            uint32_t sb = 0, sg = 0, sr = 0, sa = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = db->data + (size_t)yy * sstride + (size_t)x0 * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    uint32_t a = has_alpha ? p[3] : 255;
                    sb += p[0] * a;
                    sg += p[1] * a;
                    sr += p[2] * a;
                    sa += a;
                    n++;
                }
            }
            uint8_t *o = out + ((size_t)y * dw + x) * 4;
            if (sa) {
                o[0] = sb / sa;
                o[1] = sg / sa;
                o[2] = sr / sa;
            } else {
                o[0] = o[1] = o[2] = 0;
            }
            o[3] = sa / n;
        }
    }
    e->ver = ver;
    e->box = box;
    e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    e->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    e->dsc.header.w = dw;
    e->dsc.header.h = dh;
    e->dsc.header.stride = dw * 4;
    e->dsc.data = out;
    e->dsc.data_size = (uint32_t)dw * dh * 4;
out:
    lv_image_decoder_close(&dd);
    lv_image_cache_drop(&src);   /* the PNG copy is freed by the caller */
    return e;
}

void ui_thumb_free(ui_thumb_t *t)
{
    if (t->e) {
        t->e->refs--;
        t->e->used = ++s_tick;
        t->e = NULL;
        trim();
    }
    if (t->data) {
        lv_image_cache_drop(&t->dsc);
        heap_caps_free(t->data);
        t->data = NULL;
    }
    t->shown = false;
}

bool ui_thumb_update(ui_thumb_t *t, lv_obj_t *img, int idx, int box_px)
{
    size_t len = 0;
    uint32_t ver = t->ver;
    uint8_t *png = printers_copy_thumb(idx, &ver, &len);
    if (ver == t->ver) return t->shown;      /* unchanged */
    t->ver = ver;
    lv_image_set_src(img, NULL);
    ui_thumb_free(t);
    if (!png || len < 24) {
        if (png) heap_caps_free(png);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        return false;
    }

    thumb_entry_t *e = find(ver, box_px);
    if (!e && (e = make_entry(png, len, ver, box_px)) != NULL) {
        e->next = s_cache;
        s_cache = e;
    }
    if (e) {
        heap_caps_free(png);
        e->refs++;
        e->used = ++s_tick;
        t->e = e;
        lv_image_set_src(img, &e->dsc);
        lv_obj_set_size(img, box_px, box_px);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);   /* already the right size */
        trim();
    } else {
        /* Fallback: let LVGL decode and scale the PNG, as before. */
        t->data = png;
        memset(&t->dsc, 0, sizeof(t->dsc));
        t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        t->dsc.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
        t->dsc.header.w = png_dim(png, 16);
        t->dsc.header.h = png_dim(png, 20);
        t->dsc.data = png;
        t->dsc.data_size = len;
        lv_image_set_src(img, &t->dsc);
        lv_obj_set_size(img, box_px, box_px);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    t->shown = true;
    return true;
}
