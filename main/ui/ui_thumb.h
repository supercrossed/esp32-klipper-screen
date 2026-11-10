#pragma once
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdint.h>

/* A print thumbnail bound to an lv_image, refreshed only when the poller's version changes.
 * The PNG is decoded and scaled to the box once, and the result is shared and kept in
 * a small cache, so opening a page again neither decodes nor scales it again. */
typedef struct thumb_entry thumb_entry_t;

typedef struct {
    thumb_entry_t *e;     /* cached pixels in use (holds a reference) */
    uint32_t ver;         /* poller's thumbnail version last seen */
    bool     shown;
    /* fallback when the cache cannot be used: the PNG itself, scaled by LVGL */
    lv_image_dsc_t dsc;
    uint8_t *data;
} ui_thumb_t;

/* Returns true if image now has a thumbnail. */
bool ui_thumb_update(ui_thumb_t *t, lv_obj_t *img, int printer_idx, int box_px);
/* Release the image's thumbnail. Call lv_image_set_src(img, NULL) first if the image stays alive. */
void ui_thumb_free(ui_thumb_t *t);
