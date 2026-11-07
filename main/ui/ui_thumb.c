#include "ui_thumb.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "printers.h"

static uint32_t png_dim(const uint8_t *p, int off)
{
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) | ((uint32_t)p[off + 2] << 8) | p[off + 3];
}

void ui_thumb_free(ui_thumb_t *t)
{
    if (t->data) {
        lv_image_cache_drop(&t->dsc);
        heap_caps_free(t->data);
    }
    t->data = NULL;
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
    t->data = png;
    memset(&t->dsc, 0, sizeof(t->dsc));
    t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t->dsc.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    t->dsc.header.w = png_dim(png, 16);
    t->dsc.header.h = png_dim(png, 20);
    t->dsc.data = png;
    t->dsc.data_size = len;
    lv_image_set_src(img, &t->dsc);
    /* Fit inside box_px x box_px. */
    lv_obj_set_size(img, box_px, box_px);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    t->shown = true;
    return true;
}
