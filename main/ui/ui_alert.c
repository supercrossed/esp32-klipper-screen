/* Job-ended banners: shown on top of whatever page is open, one at a time. */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include "printers.h"
#include "app_settings.h"
#include "sound.h"

static lv_obj_t *s_banner;
static int s_printer = -1;
static uint32_t s_hide_at;          /* lv_tick time to auto-dismiss; 0 = stays until tapped */

#define AUTO_HIDE_MS 10000          /* finished/cancelled banners; failures stay up */

static void dismiss(void)
{
    if (s_banner) lv_obj_delete(s_banner);
    s_banner = NULL;
}

static void dismiss_cb(lv_event_t *e) { dismiss(); }

static void open_async(void *u)
{
    int idx = s_printer;
    dismiss();
    if (idx >= 0 && idx < printers_count()) {
        ui_nav_home();
        ui_nav_push(PAGE_PRINTER, idx);
    }
}
static void open_cb(lv_event_t *e) { lv_async_call(open_async, NULL); }   /* navigating rebuilds pages */

static void show(const job_alert_t *a)
{
    dismiss();
    s_printer = a->printer;
    const char *icon, *title;
    lv_color_t col;
    switch (a->status) {
    case PS_COMPLETE:  icon = ICON_CHECK;  title = "Print finished";  col = COL_OK;    break;
    case PS_CANCELLED: icon = ICON_CANCEL; title = "Print cancelled"; col = COL_MUTED; break;
    case PS_KLIPPY_DOWN: icon = ICON_ALERT; title = "Printer stopped"; col = COL_ERR;  break;
    default:           icon = ICON_ALERT;  title = "Print failed";    col = COL_ERR;   break;
    }

    s_banner = ui_card(lv_layer_top());
    lv_obj_set_size(s_banner, ui_is_portrait() ? LV_PCT(94) : 760, LV_SIZE_CONTENT);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_border_color(s_banner, col, 0);
    lv_obj_set_style_border_width(s_banner, 3, 0);
    lv_obj_set_style_shadow_width(s_banner, 30, 0);
    lv_obj_set_style_shadow_opa(s_banner, LV_OPA_50, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_banner, 18, 0);
    lv_obj_set_style_pad_gap(s_banner, 16, 0);

    ui_label(s_banner, FONT_XXL, col, icon);
    lv_obj_t *txt = lv_obj_create(s_banner);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_style_bg_opa(txt, 0, 0);
    lv_obj_set_style_border_width(txt, 0, 0);
    lv_obj_set_style_pad_all(txt, 0, 0);
    lv_obj_set_style_pad_gap(txt, 4, 0);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(txt, LV_OBJ_FLAG_SCROLLABLE);
    char b[200];
    snprintf(b, sizeof(b), "%s  -  %s", title, a->name);
    ui_label(txt, FONT_L, COL_TEXT, b);
    char d[24] = "";
    if (a->duration_s > 0) ui_fmt_duration(d, sizeof(d), a->duration_s);
    snprintf(b, sizeof(b), "%s%s%s", a->file, d[0] ? "  -  " : "", d);
    lv_obj_t *l = ui_label(txt, FONT_S, COL_MUTED, b);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    if (a->status != PS_COMPLETE && a->status != PS_CANCELLED && a->message[0]) {
        lv_obj_t *m = ui_label(txt, FONT_S, COL_ERR, a->message);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, LV_PCT(100));
    }

    lv_obj_t *bc = lv_obj_create(s_banner);
    lv_obj_set_size(bc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bc, 0, 0);
    lv_obj_set_style_border_width(bc, 0, 0);
    lv_obj_set_style_pad_all(bc, 0, 0);
    lv_obj_set_style_pad_gap(bc, 8, 0);
    lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bc, LV_OBJ_FLAG_SCROLLABLE);
    if (a->printer >= 0) ui_button(bc, ICON_PRINTER, "Open", COL_ACCENT, open_cb, NULL);
    ui_button(bc, ICON_CLOSE, "Dismiss", COL_CARD2, dismiss_cb, NULL);

    s_hide_at = 0;
    if (a->status == PS_COMPLETE || a->status == PS_CANCELLED) {
        s_hide_at = lv_tick_get() + AUTO_HIDE_MS;
        if (!s_hide_at) s_hide_at = 1;          /* 0 means "stay" */
    }
    sound_play(a->status == PS_COMPLETE ? SOUND_DONE : a->status == PS_CANCELLED ? SOUND_TEST : SOUND_ERROR);
}

/* Called from the UI tick. */
void ui_alert_tick(void)
{
    if (s_banner) {         /* one at a time; the rest wait in the queue */
        if (s_hide_at && (int32_t)(lv_tick_get() - s_hide_at) >= 0) dismiss();
        return;
    }
    job_alert_t a;
    if (!printers_pop_alert(&a)) return;
    if (!g_settings.alerts) return;
    show(&a);
}
