/*
 * Fullscreen webcam viewer. Covers the whole display (header included) on the
 * top layer; a tap anywhere closes it. The stream, its task and all frame
 * buffers exist only while this page is open.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include "printers.h"
#include "webcam.h"

static lv_obj_t *s_root, *s_img, *s_info, *s_hint;
static lv_image_dsc_t s_dsc;
static int s_idx;
static int s_tick;
static printer_state_t s_st;
static lv_timer_t *s_frame_timer;
static char s_name[64];

static void close_async(void *u) { ui_nav_back(); }
/* Defer: navigating deletes s_root, which is the object this event belongs to. */
static void close_cb(lv_event_t *e) { lv_async_call(close_async, NULL); }

static void frame_cb(lv_timer_t *t)
{
    if (!s_img) return;
    const uint8_t *buf;
    int w, h;
    if (webcam_take_frame(&buf, &w, &h)) {
        bool resized = s_dsc.header.w != (uint32_t)w || s_dsc.header.h != (uint32_t)h;
        s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w = w;
        s_dsc.header.h = h;
        s_dsc.header.stride = w * 2;
        s_dsc.data = buf;
        s_dsc.data_size = w * h * 2;
        lv_image_cache_drop(&s_dsc);
        if (resized || !lv_image_get_src(s_img)) {
            lv_image_set_src(s_img, &s_dsc);
            lv_obj_center(s_img);
        } else lv_obj_invalidate(s_img);
    }
}

static void create(lv_obj_t *parent, int arg)
{
    s_idx = arg / 16;
    int cam = arg % 16;
    s_tick = 0;
    printer_cfg_t cfg;
    if (!printers_get_cfg(s_idx, &cfg) || !printers_get_state(s_idx, &s_st) || cam >= s_st.nwebcams) {
        ui_label(parent, FONT_M, COL_ERR, ICON_ALERT "  Webcam not available");
        return;
    }
    snprintf(s_name, sizeof(s_name), "%s", cfg.name);
    ui_set_title(cfg.name);

    int sw = lv_display_get_horizontal_resolution(NULL);
    int sh = lv_display_get_vertical_resolution(NULL);

    /* Fullscreen black layer above everything (incl. the header). */
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_root, sw, sh);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, close_cb, LV_EVENT_CLICKED, NULL);

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_img = lv_image_create(s_root);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_img);

    s_info = ui_label(s_root, FONT_S, lv_color_white(), "");
    lv_obj_set_style_bg_color(s_info, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_info, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_info, 12, 0);
    lv_obj_set_style_pad_ver(s_info, 6, 0);
    lv_obj_set_style_radius(s_info, 8, 0);
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    s_hint = ui_label(s_root, FONT_M, lv_color_white(), ICON_CLOSE "  Tap anywhere to close");
    lv_obj_set_style_bg_color(s_hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_hint, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_hint, 12, 0);
    lv_obj_set_style_radius(s_hint, 10, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 16);

    webcam_start(&cfg, &s_st.webcams[cam], sw, sh);
    s_frame_timer = lv_timer_create(frame_cb, 30, NULL);   /* lives only as long as this page */
}

static void refresh(void)
{
    if (!s_root) return;
    if (++s_tick == 12 && s_hint) {   /* hide the hint after ~3 s */
        lv_obj_delete(s_hint);
        s_hint = NULL;
    }
    if (s_tick % 4) return;
    char st[64], j[160];
    webcam_status(st, sizeof(st));
    if (printers_get_state(s_idx, &s_st) && (s_st.status == PS_PRINTING || s_st.status == PS_PAUSED)) {
        char left[24];
        ui_fmt_duration(left, sizeof(left), s_st.eta_s);
        snprintf(j, sizeof(j), ICON_PRINTER "  %s  -  %s %d%%  -  %s left    %s", s_name,
                 printer_status_str(s_st.status), (int)(s_st.progress * 100), left, st);
    } else {
        snprintf(j, sizeof(j), ICON_PRINTER "  %s  -  %s    %s", s_name, printer_status_str(s_st.status), st);
    }
    ui_set_text(s_info, j);
}

static void destroy(void)
{
    if (s_frame_timer) lv_timer_delete(s_frame_timer);
    s_frame_timer = NULL;
    if (s_img) {
        lv_image_set_src(s_img, NULL);   /* stop referencing frame buffers before they are freed */
        lv_image_cache_drop(&s_dsc);
    }
    if (s_root) lv_obj_delete(s_root);   /* top layer is not cleaned by navigation */
    s_root = s_img = s_info = s_hint = NULL;
    webcam_stop();
}

const ui_page_t page_webcam = { "Webcam", create, refresh, destroy };
