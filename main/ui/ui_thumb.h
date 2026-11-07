#pragma once
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdint.h>

/* A print thumbnail bound to an lv_image, refreshed only when the poller's version changes. */
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *data;
    uint32_t ver;
    bool     shown;
} ui_thumb_t;

/* Returns true if image now has a thumbnail. */
bool ui_thumb_update(ui_thumb_t *t, lv_obj_t *img, int printer_idx, int box_px);
void ui_thumb_free(ui_thumb_t *t);
