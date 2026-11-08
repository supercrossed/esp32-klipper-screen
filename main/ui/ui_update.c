/* Update dialog: release notes + confirm, then a progress view until reboot. */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include "updater.h"

static lv_obj_t *s_overlay, *s_body, *s_buttons, *s_bar, *s_status;
static lv_timer_t *s_timer;
static upd_info_t s_info;   /* static: ~2 KB, keep it off the LVGL stack */

static void close_dialog(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_overlay) lv_obj_delete(s_overlay);
    s_overlay = NULL;
}

static void later_cb(lv_event_t *e) { close_dialog(); }
static void confirm_cb(lv_event_t *e);

static void progress_tick(lv_timer_t *t)
{
    updater_get(&s_info);
    char b[96];
    if (s_info.state == UPD_INSTALLING && s_info.error[0]) {
        snprintf(b, sizeof(b), "%s", s_info.error);   /* restarting first to free memory */
    } else if (s_info.state == UPD_INSTALLING) {
        lv_bar_set_value(s_bar, s_info.progress, LV_ANIM_OFF);
        snprintf(b, sizeof(b), "Downloading and installing...  %d%%", s_info.progress);
    } else if (s_info.state == UPD_DONE) {
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        strcpy(b, ICON_CHECK "  Installed. Restarting...");
    } else if (s_info.state == UPD_ERROR) {
        snprintf(b, sizeof(b), ICON_ALERT "  %s", s_info.error);
        lv_obj_set_style_text_color(s_status, COL_ERR, 0);
        lv_obj_clean(s_buttons);
        lv_obj_set_flex_flow(s_buttons, LV_FLEX_FLOW_ROW);
        lv_obj_t *c = ui_button(s_buttons, ICON_CLOSE, "Close", COL_CARD2, later_cb, NULL);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_t *r = ui_button(s_buttons, ICON_RESTART, "Try again", COL_OK, confirm_cb, NULL);
        lv_obj_set_flex_grow(r, 1);
        lv_timer_delete(s_timer);
        s_timer = NULL;
    } else return;
    ui_set_text(s_status, b);
}

static void progress_view(void)
{
    lv_obj_clean(s_buttons);
    lv_obj_set_flex_flow(s_buttons, LV_FLEX_FLOW_COLUMN);
    s_bar = lv_bar_create(s_buttons);
    lv_obj_set_size(s_bar, LV_PCT(100), 22);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, COL_OK, LV_PART_INDICATOR);
    s_status = ui_label(s_buttons, FONT_M, COL_TEXT, "Starting download...");
    ui_label(s_buttons, FONT_S, COL_MUTED, "Keep the display powered. It restarts by itself when done.");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    s_timer = lv_timer_create(progress_tick, 250, NULL);
}

static void confirm_cb(lv_event_t *e)
{
    updater_install();
    progress_view();
}

static void build_dialog(void)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = ui_card(s_overlay);
    lv_obj_set_size(box, ui_is_portrait() ? LV_PCT(94) : 780, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_gap(box, 14, 0);

    ui_label(box, FONT_XL, COL_OK, ICON_UPDATE "  Update available");
    char t[96];
    snprintf(t, sizeof(t), "Version %s  " LV_SYMBOL_RIGHT "  %s%s", s_info.current, s_info.latest, "");
    ui_label(box, FONT_L, COL_TEXT, t);
    if (s_info.size) {
        snprintf(t, sizeof(t), "%.1f MB download", s_info.size / 1048576.0f);
        ui_label(box, FONT_S, COL_MUTED, t);
    }

    ui_section_title(box, "WHAT'S NEW");
    s_body = lv_obj_create(box);
    lv_obj_set_size(s_body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_body, ui_height() - 330, 0);
    lv_obj_set_style_bg_color(s_body, COL_CARD2, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 12, 0);
    lv_obj_set_style_pad_all(s_body, 16, 0);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_t *n = ui_label(s_body, FONT_M, COL_TEXT, s_info.notes[0] ? s_info.notes : "No release notes.");
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(n, LV_PCT(100));

    s_buttons = lv_obj_create(box);
    lv_obj_set_size(s_buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_buttons, 0, 0);
    lv_obj_set_style_border_width(s_buttons, 0, 0);
    lv_obj_set_style_pad_all(s_buttons, 0, 0);
    lv_obj_set_style_pad_gap(s_buttons, 14, 0);
    lv_obj_set_flex_flow(s_buttons, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(s_buttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b1 = ui_button(s_buttons, NULL, "Later", COL_CARD2, later_cb, NULL);
    lv_obj_set_flex_grow(b1, 1);
    lv_obj_t *b2 = ui_button(s_buttons, ICON_UPDATE, "Update now", COL_OK, confirm_cb, NULL);
    lv_obj_set_flex_grow(b2, 1);
}

void ui_update_dialog(void)
{
    close_dialog();
    updater_get(&s_info);
    if (!s_info.available || s_info.state == UPD_INSTALLING || s_info.state == UPD_DONE) return;
    build_dialog();
}

/* An install that starts on its own (after a restart-to-install) gets the progress view. */
void ui_update_tick(void)
{
    if (s_overlay || updater_state() != UPD_INSTALLING) return;
    updater_get(&s_info);
    build_dialog();
    progress_view();
}
