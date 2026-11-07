/*
 * UI shell: header bar + one content area. Navigation destroys the old page
 * (its destroy hook first, then all its widgets) before building the next,
 * so a page that is not on screen owns no widgets, timers or buffers.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "app_settings.h"
#include "wifi_mgr.h"
#include "printers.h"
#include "updater.h"

static const char *TAG = "ui";

/* ── palettes ────────────────────────────────────────────────────────── */
static const ui_palette_t PAL_DARK = {
    .bg = LV_COLOR_MAKE(0x0E, 0x11, 0x16), .header = LV_COLOR_MAKE(0x15, 0x1A, 0x21),
    .card = LV_COLOR_MAKE(0x1A, 0x20, 0x29), .card2 = LV_COLOR_MAKE(0x23, 0x2B, 0x36),
    .border = LV_COLOR_MAKE(0x2E, 0x38, 0x46), .text = LV_COLOR_MAKE(0xE8, 0xED, 0xF2),
    .muted = LV_COLOR_MAKE(0x8B, 0x97, 0xA6), .accent = LV_COLOR_MAKE(0x3B, 0xA7, 0xF0),
    .ok = LV_COLOR_MAKE(0x3C, 0xCB, 0x7F), .warn = LV_COLOR_MAKE(0xF5, 0xA5, 0x24),
    .err = LV_COLOR_MAKE(0xF0, 0x56, 0x4A), .hot = LV_COLOR_MAKE(0xFF, 0x7A, 0x45),
};
static const ui_palette_t PAL_LIGHT = {
    .bg = LV_COLOR_MAKE(0xEE, 0xF1, 0xF5), .header = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    .card = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), .card2 = LV_COLOR_MAKE(0xE4, 0xE9, 0xEF),
    .border = LV_COLOR_MAKE(0xD3, 0xDA, 0xE3), .text = LV_COLOR_MAKE(0x17, 0x1C, 0x24),
    .muted = LV_COLOR_MAKE(0x5C, 0x68, 0x78), .accent = LV_COLOR_MAKE(0x1E, 0x7F, 0xD8),
    .ok = LV_COLOR_MAKE(0x23, 0x9A, 0x58), .warn = LV_COLOR_MAKE(0xE0, 0x8A, 0x00),
    .err = LV_COLOR_MAKE(0xD6, 0x3A, 0x2F), .hot = LV_COLOR_MAKE(0xE4, 0x5E, 0x22),
};
ui_palette_t g_pal;

lv_color_t ui_contrast(lv_color_t bg)
{
    return lv_color_brightness(bg) > 150 ? lv_color_hex(0x111418) : lv_color_white();
}

/* ── fonts: Montserrat + MDI icon fallback ───────────────────────────── */
extern const uint8_t mdi_ttf_start[] asm("_binary_mdi_ttf_start");
extern const uint8_t mdi_ttf_end[] asm("_binary_mdi_ttf_end");
lv_font_t g_font_16, g_font_20, g_font_24, g_font_32, g_font_48;

static void font_with_icons(lv_font_t *dst, const lv_font_t *base, int px)
{
    *dst = *base;
    /* Icons are a touch larger than the text cap height so they read as icons. */
    lv_font_t *icons = lv_tiny_ttf_create_data(mdi_ttf_start, mdi_ttf_end - mdi_ttf_start, px * 6 / 5);
    if (icons) dst->fallback = icons;
    else ESP_LOGW(TAG, "icon font %d px failed", px);
}

static void fonts_init(void)
{
    font_with_icons(&g_font_16, &lv_font_montserrat_16, 16);
    font_with_icons(&g_font_20, &lv_font_montserrat_20, 20);
    font_with_icons(&g_font_24, &lv_font_montserrat_24, 24);
    font_with_icons(&g_font_32, &lv_font_montserrat_32, 32);
    font_with_icons(&g_font_48, &lv_font_montserrat_48, 48);
}

#define HEADER_H 68
#define NAV_DEPTH 8

static const ui_page_t *s_pages[PAGE_COUNT];
static struct { page_id_t id; int arg; } s_stack[NAV_DEPTH];
static int s_depth;
static const ui_page_t *s_cur;

static lv_display_t *s_disp;
static lv_obj_t *s_header, *s_content, *s_back_btn, *s_title, *s_wifi_lbl, *s_clock_lbl, *s_upd_btn;
static lv_obj_t *s_kb, *s_kb_ta;

/* ── geometry ────────────────────────────────────────────────────────── */
int ui_width(void) { return lv_display_get_horizontal_resolution(s_disp); }
int ui_height(void) { return lv_display_get_vertical_resolution(s_disp) - HEADER_H; }
bool ui_is_portrait(void) { return ui_width() < lv_display_get_vertical_resolution(s_disp); }

/* ── helpers ─────────────────────────────────────────────────────────── */
lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_gap(c, 10, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *symbol, const char *txt, lv_color_t bg, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_ver(b, 14, 0);
    lv_obj_set_style_min_height(b, 60, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, ui_contrast(bg), 0);
    char t[64];
    if (symbol && txt && txt[0]) snprintf(t, sizeof(t), "%s  %s", symbol, txt);
    else snprintf(t, sizeof(t), "%s", symbol ? symbol : (txt ? txt : ""));
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, FONT_M, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *ui_section_title(lv_obj_t *parent, const char *txt)
{
    return ui_label(parent, FONT_S, COL_MUTED, txt);
}

static lv_obj_t *s_toast;
static void toast_deleted(lv_event_t *e) { if (lv_event_get_target(e) == s_toast) s_toast = NULL; }

void ui_toast(const char *msg)
{
    if (s_toast) lv_obj_delete(s_toast);   /* one at a time: newest replaces the previous */
    lv_obj_t *t = lv_obj_create(lv_layer_top());
    s_toast = t;
    lv_obj_add_event_cb(t, toast_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, COL_CARD2, 0);
    lv_obj_set_style_border_color(t, COL_ACCENT, 0);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_style_radius(t, 12, 0);
    lv_obj_set_style_pad_all(t, 16, 0);
    lv_obj_set_style_max_width(t, ui_width() - 80, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(t, FONT_M, COL_TEXT, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, ui_width() - 120, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_delete_delayed(t, 3500);
}

lv_color_t ui_status_color(int s)
{
    switch ((printer_status_t)s) {
    case PS_PRINTING:   return COL_ACCENT;
    case PS_PAUSED:     return COL_WARN;
    case PS_COMPLETE:   return COL_OK;
    case PS_STANDBY:    return lv_color_hex(0x5FB3B3);
    case PS_CANCELLED:  return COL_MUTED;
    case PS_ERROR:
    case PS_KLIPPY_DOWN:return COL_ERR;
    case PS_OFFLINE:
    default:            return lv_color_hex(0x5A6573);
    }
}

void ui_fmt_duration(char *out, size_t cap, float s)
{
    if (s < 0) { strlcpy(out, "--", cap); return; }
    int t = (int)(s + 0.5f);
    int h = t / 3600, m = (t / 60) % 60, sec = t % 60;
    if (h >= 24) snprintf(out, cap, "%dd %dh", h / 24, h % 24);
    else if (h) snprintf(out, cap, "%dh %02dm", h, m);
    else snprintf(out, cap, "%dm %02ds", m, sec);
}

/* ── keyboard ────────────────────────────────────────────────────────── */
static void kb_close(void)
{
    if (s_kb) {
        lv_obj_delete(s_kb);
        s_kb = NULL;
    }
    if (s_kb_ta) lv_obj_remove_state(s_kb_ta, LV_STATE_FOCUSED);
    s_kb_ta = NULL;
    lv_obj_set_style_pad_bottom(s_content, 16, 0);
}

static void kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) kb_close();
}

static void ta_event(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if (s_kb && s_kb_ta == ta) return;
        if (!s_kb) {
            s_kb = lv_keyboard_create(lv_layer_top());
            lv_obj_set_size(s_kb, LV_PCT(100), ui_is_portrait() ? 420 : 330);
            lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_text_font(s_kb, FONT_L, 0);
            ui_keyboard_setup(s_kb);
            lv_obj_add_event_cb(s_kb, kb_event, LV_EVENT_ALL, NULL);
        }
        lv_keyboard_mode_t mode = (lv_keyboard_mode_t)(uintptr_t)lv_event_get_user_data(e);
        if (mode == LV_KEYBOARD_MODE_NUMBER) mode = LV_KEYBOARD_MODE_USER_1;   /* our numpad has '.' and ':' */
        lv_keyboard_set_mode(s_kb, mode);
        lv_keyboard_set_textarea(s_kb, ta);
        s_kb_ta = ta;
        lv_obj_set_style_pad_bottom(s_content, lv_obj_get_height(s_kb) + 16, 0);
        lv_obj_update_layout(s_content);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    } else if (code == LV_EVENT_DELETE) {
        if (s_kb_ta == ta) kb_close();
    }
}

void ui_attach_keyboard(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    lv_obj_add_event_cb(ta, ta_event, LV_EVENT_FOCUSED, (void *)(uintptr_t)mode);
    lv_obj_add_event_cb(ta, ta_event, LV_EVENT_CLICKED, (void *)(uintptr_t)mode);
    lv_obj_add_event_cb(ta, ta_event, LV_EVENT_DELETE, NULL);
}

/* ── modal dialogs ───────────────────────────────────────────────────── */
typedef struct {
    ui_confirm_cb_t cb;
    void *user;
    lv_obj_t *mb;
} confirm_ctx_t;

static void confirm_ok(lv_event_t *e)
{
    confirm_ctx_t *c = lv_event_get_user_data(e);
    ui_confirm_cb_t cb = c->cb;
    void *user = c->user;
    lv_msgbox_close(c->mb);
    if (cb) cb(user);
}

static void confirm_cancel(lv_event_t *e)
{
    confirm_ctx_t *c = lv_event_get_user_data(e);
    lv_msgbox_close(c->mb);
}

static void confirm_free(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

void ui_confirm(const char *title, const char *text, const char *ok, lv_color_t ok_col, ui_confirm_cb_t cb, void *user)
{
    confirm_ctx_t *c = lv_malloc(sizeof(*c));
    c->cb = cb;
    c->user = user;
    lv_obj_t *mb = lv_msgbox_create(NULL);
    c->mb = mb;
    lv_obj_set_style_bg_color(mb, COL_CARD, 0);
    lv_obj_set_style_radius(mb, 16, 0);
    lv_obj_set_width(mb, ui_is_portrait() ? LV_PCT(90) : 560);
    lv_obj_t *t = lv_msgbox_add_title(mb, title);
    lv_obj_set_style_text_font(t, FONT_L, 0);
    lv_obj_t *tx = lv_msgbox_add_text(mb, text);
    lv_obj_set_style_text_font(tx, FONT_M, 0);
    lv_obj_t *bc = lv_msgbox_add_footer_button(mb, "Cancel");
    lv_obj_set_style_text_font(bc, FONT_M, 0);
    lv_obj_set_style_bg_color(bc, COL_CARD2, 0);
    lv_obj_set_height(bc, 56);
    lv_obj_add_event_cb(bc, confirm_cancel, LV_EVENT_CLICKED, c);
    lv_obj_t *bo = lv_msgbox_add_footer_button(mb, ok);
    lv_obj_set_style_text_font(bo, FONT_M, 0);
    lv_obj_set_style_bg_color(bo, ok_col, 0);
    lv_obj_set_height(bo, 56);
    lv_obj_add_event_cb(bo, confirm_ok, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(mb, confirm_free, LV_EVENT_DELETE, c);
}

typedef struct {
    ui_numpad_cb_t cb;
    void *user;
    lv_obj_t *overlay, *ta;
    int presets[6];
    int npresets;
} numpad_ctx_t;

static void numpad_close(numpad_ctx_t *c)
{
    lv_obj_delete_async(c->overlay);
}

static void numpad_btn(lv_event_t *e)
{
    numpad_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *bm = lv_event_get_target(e);
    const char *txt = lv_buttonmatrix_get_button_text(bm, lv_buttonmatrix_get_selected_button(bm));
    if (!txt) return;
    if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) lv_textarea_delete_char(c->ta);
    else if (!strcmp(txt, "Cancel")) numpad_close(c);
    else if (!strcmp(txt, "Set")) {
        const char *in = lv_textarea_get_text(c->ta);
        if (!in[0]) { numpad_close(c); return; }   /* nothing typed: don't turn the heater off */
        float v = strtof(in, NULL);
        ui_numpad_cb_t cb = c->cb;
        void *user = c->user;
        numpad_close(c);
        if (cb) cb(v, user);
    } else lv_textarea_add_text(c->ta, txt);
}

static void numpad_preset(lv_event_t *e)
{
    numpad_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    int v = (int)(intptr_t)lv_obj_get_user_data(b);
    char t[16];
    snprintf(t, sizeof(t), "%d", v);
    lv_textarea_set_text(c->ta, t);
}

static void numpad_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

void ui_numpad(const char *title, float initial, const int *presets, int npresets, ui_numpad_cb_t cb, void *user)
{
    numpad_ctx_t *c = lv_malloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->cb = cb;
    c->user = user;
    c->overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(c->overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(c->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(c->overlay, 0, 0);
    lv_obj_set_style_radius(c->overlay, 0, 0);
    lv_obj_add_event_cb(c->overlay, numpad_free, LV_EVENT_DELETE, c);

    lv_obj_t *box = ui_card(c->overlay);
    lv_obj_set_size(box, 520, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    ui_label(box, FONT_L, COL_TEXT, title);

    c->ta = lv_textarea_create(box);
    lv_textarea_set_one_line(c->ta, true);
    lv_textarea_set_accepted_chars(c->ta, "0123456789.");
    lv_textarea_set_max_length(c->ta, 6);
    lv_obj_set_width(c->ta, LV_PCT(100));
    lv_obj_set_style_text_font(c->ta, FONT_XL, 0);
    char t[16];
    snprintf(t, sizeof(t), "%d", (int)initial);
    lv_textarea_set_text(c->ta, t);

    if (npresets > 0) {
        lv_obj_t *row = lv_obj_create(box);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(row, 8, 0);
        for (int i = 0; i < npresets && i < 6; i++) {
            char p[12];
            snprintf(p, sizeof(p), "%d", presets[i]);
            lv_obj_t *b = ui_button(row, NULL, p, COL_CARD2, numpad_preset, c);
            lv_obj_set_user_data(b, (void *)(intptr_t)presets[i]);
            lv_obj_set_style_min_height(b, 50, 0);
            lv_obj_set_flex_grow(b, 1);
        }
    }

    static const char *map[] = {
        "7", "8", "9", "\n",
        "4", "5", "6", "\n",
        "1", "2", "3", "\n",
        ".", "0", LV_SYMBOL_BACKSPACE, "\n",
        "Cancel", "Set", ""
    };
    lv_obj_t *bm = lv_buttonmatrix_create(box);
    lv_buttonmatrix_set_map(bm, map);
    lv_obj_set_size(bm, LV_PCT(100), 380);
    lv_obj_set_style_text_font(bm, FONT_L, 0);
    lv_obj_set_style_bg_opa(bm, 0, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_add_event_cb(bm, numpad_btn, LV_EVENT_VALUE_CHANGED, c);
}

/* ── navigation ──────────────────────────────────────────────────────── */
void ui_set_title(const char *t) { lv_label_set_text(s_title, t); }

static void show_top(void)
{
    kb_close();
    if (s_cur && s_cur->destroy) s_cur->destroy();
    s_cur = NULL;
    lv_obj_clean(s_content);
    lv_obj_scroll_to(s_content, 0, 0, LV_ANIM_OFF);
    /* pages customise the shared content area; reset it so nothing leaks to the next page */
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(s_content, 16, 0);

    page_id_t id = s_stack[s_depth - 1].id;
    s_cur = s_pages[id];
    lv_label_set_text(s_title, s_cur->title);
    if (s_depth > 1) lv_obj_remove_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    s_cur->create(s_content, s_stack[s_depth - 1].arg);
    ESP_LOGI(TAG, "page %d (depth %d), free internal %u, psram %u", id, s_depth,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void ui_nav_push(page_id_t id, int arg)
{
    if (s_depth >= NAV_DEPTH) s_depth = NAV_DEPTH - 1;
    s_stack[s_depth].id = id;
    s_stack[s_depth].arg = arg;
    s_depth++;
    show_top();
}

void ui_nav_replace(page_id_t id, int arg)
{
    if (s_depth == 0) { ui_nav_push(id, arg); return; }
    s_stack[s_depth - 1].id = id;
    s_stack[s_depth - 1].arg = arg;
    show_top();
}

void ui_nav_back(void)
{
    if (s_depth <= 1) return;
    s_depth--;
    show_top();
}

void ui_nav_home(void)
{
    s_depth = 1;
    s_stack[0].id = PAGE_HOME;
    s_stack[0].arg = 0;
    show_top();
}

static void back_cb(lv_event_t *e) { ui_nav_back(); }
static void settings_cb(lv_event_t *e)
{
    if (s_cur == &page_settings) return;
    ui_nav_push(PAGE_SETTINGS, 0);
}

/* ── backlight (always on: no screen sleep, by design) ───────────────── */
void ui_apply_brightness(void)
{
    bsp_display_brightness_set(g_settings.brightness);
}

/* ── periodic refresh ────────────────────────────────────────────────── */
static void tick_cb(lv_timer_t *t)
{
    /* header */
    char buf[64];
    wifi_state_t ws = wifi_mgr_state();
    if (ws == WIFI_ST_CONNECTED) {
        int r = wifi_mgr_rssi();
        snprintf(buf, sizeof(buf), "%s", r > -55 ? ICON_WIFI_4 : r > -65 ? ICON_WIFI_3 : r > -75 ? ICON_WIFI_2 : ICON_WIFI_1);
        lv_obj_set_style_text_color(s_wifi_lbl, r > -75 ? COL_TEXT : COL_WARN, 0);
    } else {
        snprintf(buf, sizeof(buf), "%s  %s", ws == WIFI_ST_CONNECTING ? ICON_WIFI : ICON_WIFI_OFF, wifi_mgr_state_str());
        lv_obj_set_style_text_color(s_wifi_lbl, ws == WIFI_ST_CONNECTING ? COL_WARN : COL_ERR, 0);
    }
    ui_set_text(s_wifi_lbl, buf);
    if (wifi_mgr_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        lv_label_set_text(s_clock_lbl, buf);
    }

    char res[160];
    if (printers_last_result(res, sizeof(res))) ui_toast(res);

    /* green "Update" in the top bar whenever a newer release is known */
    upd_state_t us = updater_state();
    bool upd = updater_update_known() && us != UPD_INSTALLING && us != UPD_DONE;
    if (upd == lv_obj_has_flag(s_upd_btn, LV_OBJ_FLAG_HIDDEN)) {
        if (upd) lv_obj_remove_flag(s_upd_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_upd_btn, LV_OBJ_FLAG_HIDDEN);
    }


    if (s_cur && s_cur->refresh) s_cur->refresh();
}

/* ── init ────────────────────────────────────────────────────────────── */
static lv_obj_t *hdr_btn(const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(s_header);
    lv_obj_set_size(b, 88, 56);
    lv_obj_set_style_bg_color(b, COL_CARD2, 0);
    lv_obj_set_style_text_color(b, COL_TEXT, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, FONT_XL, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_shell(void);
static void upd_btn_cb(lv_event_t *e) { ui_update_dialog(); }

void ui_init(lv_display_t *disp)
{
    s_disp = disp;
    fonts_init();
    s_pages[PAGE_HOME] = &page_home;
    s_pages[PAGE_PRINTER] = &page_printer;
    s_pages[PAGE_WEBCAM] = &page_webcam;
    s_pages[PAGE_ADD_PRINTER] = &page_add_printer;
    s_pages[PAGE_SETTINGS] = &page_settings;
    s_pages[PAGE_WIFI] = &page_wifi;
    s_pages[PAGE_SPOOLMAN] = &page_spoolman;

    g_pal = g_settings.light_mode ? PAL_LIGHT : PAL_DARK;
    lv_theme_t *th = lv_theme_default_init(disp, COL_ACCENT, COL_WARN, !g_settings.light_mode, FONT_M);
    lv_display_set_theme(disp, th);
    build_shell();
    lv_timer_create(tick_cb, 250, NULL);

    ui_apply_brightness();
    /* First boot (no Wi-Fi saved): go straight to Wi-Fi setup. */
    ui_nav_push(PAGE_HOME, 0);
    if (!g_settings.wifi_ssid[0]) ui_nav_push(PAGE_WIFI, 1);
}

/* Header + content area. Rebuilt from scratch on a theme change. */
static void build_shell(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_color(scr, COL_TEXT, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_gap(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_header = lv_obj_create(scr);
    lv_obj_set_size(s_header, LV_PCT(100), HEADER_H);
    lv_obj_set_style_bg_color(s_header, COL_HEADER, 0);
    lv_obj_set_style_border_width(s_header, 0, 0);
    lv_obj_set_style_border_side(s_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(s_header, 0, 0);
    lv_obj_set_style_pad_hor(s_header, 10, 0);
    lv_obj_set_style_pad_ver(s_header, 0, 0);
    lv_obj_set_style_pad_gap(s_header, 12, 0);
    lv_obj_remove_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_back_btn = hdr_btn(ICON_BACK, back_cb);
    s_title = ui_label(s_header, FONT_L, COL_TEXT, "");
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_title, 1);
    s_upd_btn = ui_button(s_header, ICON_UPDATE, "Update", COL_OK, upd_btn_cb, NULL);
    lv_obj_set_style_min_height(s_upd_btn, 52, 0);
    lv_obj_set_style_pad_ver(s_upd_btn, 8, 0);
    lv_obj_add_flag(s_upd_btn, LV_OBJ_FLAG_HIDDEN);
    s_wifi_lbl = ui_label(s_header, FONT_L, COL_MUTED, "");
    s_clock_lbl = ui_label(s_header, FONT_L, COL_TEXT, "--:--");
    hdr_btn(ICON_SETTINGS, settings_cb);

    s_content = lv_obj_create(scr);
    lv_obj_set_width(s_content, LV_PCT(100));
    lv_obj_set_flex_grow(s_content, 1);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 16, 0);
    lv_obj_set_style_pad_gap(s_content, 16, 0);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
}

void ui_set_light_mode(bool light)
{
    g_settings.light_mode = light;
    settings_save();
    /* Tear down the current page and the shell, re-theme, rebuild, re-open the same page. */
    kb_close();
    if (s_cur && s_cur->destroy) s_cur->destroy();
    s_cur = NULL;
    lv_obj_clean(lv_screen_active());
    g_pal = light ? PAL_LIGHT : PAL_DARK;
    lv_theme_t *th = lv_theme_default_init(s_disp, COL_ACCENT, COL_WARN, !light, FONT_M);
    lv_display_set_theme(s_disp, th);
    build_shell();
    show_top();
}

void ui_set_text(lv_obj_t *lbl, const char *txt)
{
    const char *cur = lv_label_get_text(lbl);
    if (cur && !strcmp(cur, txt)) return;   /* avoid needless invalidation/redraw */
    lv_label_set_text(lbl, txt);
}

/* Large touch target: big icon over a short label. Grows to share its row. */
lv_obj_t *ui_tile_button(lv_obj_t *parent, const char *icon, const char *txt, lv_color_t bg, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_set_style_pad_gap(b, 6, 0);
    lv_obj_set_height(b, 112);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(b, ui_contrast(bg), 0);
    lv_obj_t *i = lv_label_create(b);
    lv_obj_set_style_text_font(i, FONT_XL, 0);
    lv_label_set_text(i, icon);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, FONT_S, 0);
    lv_label_set_text(l, txt);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

/* ── text prompt (nicknames etc.) ────────────────────────────────────── */
typedef struct {
    ui_prompt_cb_t cb;
    void *user;
    lv_obj_t *overlay, *ta;
} prompt_ctx_t;

static void prompt_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }
static void prompt_cancel(lv_event_t *e)
{
    prompt_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_delete_async(c->overlay);
}
static void prompt_ok(lv_event_t *e)
{
    prompt_ctx_t *c = lv_event_get_user_data(e);
    char txt[64];
    strlcpy(txt, lv_textarea_get_text(c->ta), sizeof(txt));
    ui_prompt_cb_t cb = c->cb;
    void *user = c->user;
    lv_obj_delete_async(c->overlay);
    if (cb && txt[0]) cb(txt, user);
}

void ui_prompt(const char *title, const char *initial, ui_prompt_cb_t cb, void *user)
{
    prompt_ctx_t *c = lv_malloc(sizeof(*c));
    c->cb = cb;
    c->user = user;
    c->overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(c->overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(c->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(c->overlay, 0, 0);
    lv_obj_set_style_radius(c->overlay, 0, 0);
    lv_obj_add_event_cb(c->overlay, prompt_free, LV_EVENT_DELETE, c);

    lv_obj_t *box = ui_card(c->overlay);
    lv_obj_set_size(box, ui_is_portrait() ? LV_PCT(92) : 640, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_pad_gap(box, 14, 0);
    ui_label(box, FONT_L, COL_TEXT, title);
    c->ta = lv_textarea_create(box);
    lv_textarea_set_one_line(c->ta, true);
    lv_textarea_set_max_length(c->ta, 31);
    lv_obj_set_width(c->ta, LV_PCT(100));
    lv_obj_set_style_text_font(c->ta, FONT_L, 0);
    lv_textarea_set_text(c->ta, initial ? initial : "");
    ui_attach_keyboard(c->ta, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(c->ta, prompt_ok, LV_EVENT_READY, c);
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *b1 = ui_button(row, ICON_CLOSE, "Cancel", COL_CARD2, prompt_cancel, c);
    lv_obj_set_flex_grow(b1, 1);
    lv_obj_t *b2 = ui_button(row, ICON_CHECK, "Save", COL_ACCENT, prompt_ok, c);
    lv_obj_set_flex_grow(b2, 1);
    lv_obj_send_event(c->ta, LV_EVENT_FOCUSED, NULL);   /* keyboard up straight away */
}
