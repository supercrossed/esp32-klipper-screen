/*
 * Settings. arg 0: a grid of section tiles. arg 1..N: one section per page
 * (reached by tapping a tile; the header back arrow returns to the grid).
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "app_settings.h"
#include "wifi_mgr.h"
#include "printers.h"
#include "tz.h"
#include "updater.h"
#include "sound.h"
#include "battery.h"
#include "label_printer.h"
#include "ui_label.h"

enum { SEC_GRID = 0, SEC_NETWORK, SEC_PRINTERS, SEC_DISPLAY, SEC_TIME, SEC_UPDATES, SEC_DEVICE, SEC_LABEL, SEC_COUNT };

static const struct { const char *icon, *title; } SECTIONS[SEC_COUNT] = {
    [SEC_NETWORK]  = { ICON_WIFI,     "Network" },
    [SEC_PRINTERS] = { ICON_PRINTER,  "Printers" },
    [SEC_DISPLAY]  = { ICON_DISPLAY,  "Display" },
    [SEC_TIME]     = { ICON_CLOCK,    "Time" },
    [SEC_UPDATES]  = { ICON_UPDATE,   "Updates" },
    [SEC_DEVICE]   = { ICON_CHIP,     "Device" },
    [SEC_LABEL]    = { ICON_SPOOL,    "Label printer" },
};

static int s_sec;
static int s_tick;
static lv_obj_t *s_sum[SEC_COUNT];         /* grid tile summaries */
static lv_obj_t *s_info, *s_time_lbl, *s_name_ta, *s_upd_btns, *s_bat_info;
static upd_info_t s_upd;   /* static: ~2 KB */
static int s_upd_shown = -1;

static const uint16_t POLL_OPTS[] = { 500, 1000, 2000 };
static const uint16_t POLLBG_OPTS[] = { 2000, 3000, 5000, 10000 };

/* ── helpers ─────────────────────────────────────────────────────────── */
static lv_obj_t *col(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    int w = ui_width() - 32;
    lv_obj_set_size(c, w > 820 ? 820 : w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_pad_gap(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *group(lv_obj_t *parent, const char *title)
{
    lv_obj_t *c = ui_card(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_gap(c, 14, 0);
    if (title) ui_section_title(c, title);
    return c;
}

static lv_obj_t *row(lv_obj_t *parent, const char *icon, const char *label)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 64, 0);
    lv_obj_set_style_bg_opa(r, 0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char t[64];
    snprintf(t, sizeof(t), "%s  %s", icon, label);
    ui_label(r, FONT_M, COL_TEXT, t);
    return r;
}

static lv_obj_t *dropdown(lv_obj_t *r, const char *opts, int sel, lv_event_cb_t cb)
{
    lv_obj_t *d = lv_dropdown_create(r);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, sel);
    lv_obj_set_width(d, 300);
    lv_obj_set_style_text_font(d, FONT_M, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(d), FONT_M, 0);
    lv_obj_add_event_cb(d, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return d;
}

static lv_obj_t *wide_btn(lv_obj_t *parent, const char *icon, const char *txt, lv_color_t c, lv_event_cb_t cb, void *u)
{
    lv_obj_t *b = ui_button(parent, icon, txt, c, cb, u);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_min_height(b, 72, 0);
    return b;
}

static int find_u16(const uint16_t *a, int n, uint16_t v) { for (int i = 0; i < n; i++) if (a[i] == v) return i; return 0; }

/* ── callbacks ───────────────────────────────────────────────────────── */
static void open_sec_cb(lv_event_t *e) { ui_nav_push(PAGE_SETTINGS, (int)(intptr_t)lv_event_get_user_data(e)); }
static void wifi_cb(lv_event_t *e) { ui_nav_push(PAGE_WIFI, 0); }
static void add_cb(lv_event_t *e) { ui_nav_push(PAGE_ADD_PRINTER, -1); }
static void edit_cb(lv_event_t *e) { ui_nav_push(PAGE_ADD_PRINTER, (int)(intptr_t)lv_event_get_user_data(e)); }
static void poll_cb(lv_event_t *e) { g_settings.poll_ms = POLL_OPTS[lv_dropdown_get_selected(lv_event_get_target(e))]; settings_save(); }
static void pollbg_cb(lv_event_t *e) { g_settings.poll_bg_ms = POLLBG_OPTS[lv_dropdown_get_selected(lv_event_get_target(e))]; settings_save(); }

static void bright_cb(lv_event_t *e)
{
    g_settings.brightness = lv_slider_get_value(lv_event_get_target(e));
    ui_apply_brightness();
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void do_reboot(void *u) { settings_save(); esp_restart(); }

static void orient_cb(lv_event_t *e)
{
    g_settings.orientation = lv_dropdown_get_selected(lv_event_get_target(e));
    settings_save();
    ui_confirm("Restart now?", "The new orientation is applied after a restart.", "Restart", COL_ACCENT, do_reboot, NULL);
}

/* Theme changes rebuild the whole screen (including the control that was tapped):
 * apply them after the event returns. user data: (mode << 8) | accent, 0xFF = keep. */
static void theme_async(void *u)
{
    int v = (int)(intptr_t)u;
    int mode = (v >> 8) & 0xFF, accent = v & 0xFF;   /* accent 0xFE = the theme's own */
    ui_set_theme(mode == 0xFF ? -1 : mode, accent == 0xFF ? -1 : accent == 0xFE ? ACCENT_THEME : accent);
}
static void mode_cb(lv_event_t *e)
{
    int m = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (m == g_settings.theme_mode || m > 2) return;
    lv_async_call(theme_async, (void *)(intptr_t)((m << 8) | 0xFF));
}
static void accent_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);   /* ACCENT_THEME = the theme's own */
    if (a == g_settings.accent) return;
    if (a == ACCENT_THEME) lv_async_call(theme_async, (void *)(intptr_t)((0xFF << 8) | 0xFE));
    else lv_async_call(theme_async, (void *)(intptr_t)((0xFF << 8) | a));
}
static void palette_async(void *u) { ui_set_palette((int)(intptr_t)u); }
static void palette_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == g_settings.theme) return;
    lv_async_call(palette_async, (void *)(intptr_t)t);
}
static void alerts_cb(lv_event_t *e)
{
    g_settings.alerts = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}
static const uint8_t VOLS[] = { 0, 40, 70, 95 };
static void vol_cb(lv_event_t *e)
{
    g_settings.alert_vol = VOLS[lv_dropdown_get_selected(lv_event_get_target(e))];
    settings_save();
    sound_play(SOUND_TEST);
}
static void test_sound_cb(lv_event_t *e)
{
    if (!g_settings.alert_vol) { ui_toast("Alert sound is off"); return; }
    sound_play(SOUND_DONE);
}

static const uint16_t BAT_CAPS[] = { 0, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000 };
static const uint16_t BAT_CUTS[] = { 0, 3000, 3100, 3200, 3300, 3400, 3500 };
static void bat_cap_cb(lv_event_t *e)
{
    g_settings.bat_cap_mah = BAT_CAPS[lv_dropdown_get_selected(lv_event_get_target(e))];
    settings_save();
}
static void bat_type_cb(lv_event_t *e)
{
    g_settings.bat_type = lv_dropdown_get_selected(lv_event_get_target(e)) ? BAT_TYPE_LIPO : BAT_TYPE_LIION;
    settings_save();
}
static void bat_cut_cb(lv_event_t *e)
{
    g_settings.bat_cut_mv = BAT_CUTS[lv_dropdown_get_selected(lv_event_get_target(e))];
    settings_save();
}

static void tz_cb(lv_event_t *e)
{
    int i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (i == 0) {
        g_settings.tz_auto = 1;
        settings_save();
        wifi_mgr_request_tz();   /* runs on the Wi-Fi task, not the UI thread */
        ui_toast("Detecting time zone...");
        return;
    }
    g_settings.tz_auto = 0;
    strlcpy(g_settings.tz, TZ_TABLE[i - 1].posix, sizeof(g_settings.tz));
    setenv("TZ", g_settings.tz, 1);
    tzset();
    settings_save();
}

static void name_cb(lv_event_t *e)
{
    const char *t = lv_textarea_get_text(s_name_ta);
    if (!t[0] || !strcmp(t, g_settings.device_name)) return;
    strlcpy(g_settings.device_name, t, sizeof(g_settings.device_name));
    settings_save();
    ui_toast("Device name saved (used as network hostname after restart)");
}

static void reboot_cb(lv_event_t *e) { ui_confirm("Restart?", "The display will restart.", "Restart", COL_ACCENT, do_reboot, NULL); }
static void do_factory(void *u) { settings_factory_reset(); esp_restart(); }
static void factory_cb(lv_event_t *e)
{
    ui_confirm("Factory reset?", "Erases Wi-Fi, all printers and settings, then restarts.", "Erase everything", COL_ERR, do_factory, NULL);
}

/* ── grid ────────────────────────────────────────────────────────────── */
static void build_grid(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);
    int cols = ui_is_portrait() ? 2 : 3;
    int w = (ui_width() - 32 - (cols - 1) * 16) / cols;
    int rows = (SEC_COUNT - 1 + cols - 1) / cols;
    int h = (ui_height() - 32 - (rows - 1) * 16) / rows;
    if (h > 220) h = 220;
    if (h < 170) h = 170;
    for (int i = 1; i < SEC_COUNT; i++) {
        lv_obj_t *t = ui_card(parent);
        lv_obj_set_size(t, w, h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(t, COL_CARD2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(t, 8, 0);
        lv_obj_add_event_cb(t, open_sec_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_label(t, FONT_XXL, COL_ACCENT, SECTIONS[i].icon);
        ui_label(t, FONT_L, COL_TEXT, SECTIONS[i].title);
        s_sum[i] = ui_label(t, FONT_S, COL_MUTED, "");
        lv_label_set_long_mode(s_sum[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(s_sum[i], w - 32, 0);
    }
}

static void refresh_grid(void)
{
    char t[96];
    esp_netif_ip_info_t ip;
    if (wifi_mgr_ip(&ip)) snprintf(t, sizeof(t), "Connected  -  %s", wifi_mgr_ssid());
    else snprintf(t, sizeof(t), "%s", wifi_mgr_state_str());
    ui_set_text(s_sum[SEC_NETWORK], t);
    snprintf(t, sizeof(t), "%d printer%s", printers_count(), printers_count() == 1 ? "" : "s");
    ui_set_text(s_sum[SEC_PRINTERS], t);
    if (g_settings.accent == ACCENT_THEME)
        snprintf(t, sizeof(t), "%s, %s  -  %d%%", ui_theme_name(g_settings.theme),
                 g_settings.theme_mode == 2 ? "auto" : g_settings.light_mode ? "light" : "dark", g_settings.brightness);
    else
        snprintf(t, sizeof(t), "%s, %s, %s  -  %d%%", ui_theme_name(g_settings.theme),
                 g_settings.theme_mode == 2 ? "auto" : g_settings.light_mode ? "light" : "dark",
                 ui_accent_name(g_settings.accent), g_settings.brightness);
    ui_set_text(s_sum[SEC_DISPLAY], t);
    if (wifi_mgr_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(t, sizeof(t), "%H:%M  -  %a %d %b", &tm);
    } else strcpy(t, "Waiting for time sync");
    ui_set_text(s_sum[SEC_TIME], t);
    snprintf(t, sizeof(t), "%s  -  v%s", g_settings.device_name, esp_app_get_description()->version);
    ui_set_text(s_sum[SEC_DEVICE], t);
    label_cfg_t lc;
    label_cfg_get(&lc);
    ui_set_text(s_sum[SEC_LABEL], lc.have ? lc.name : "Not set up");
    updater_get(&s_upd);
    if (s_upd.available) snprintf(t, sizeof(t), "Version %s available", s_upd.latest);
    else if (s_upd.state == UPD_CHECKING) strcpy(t, "Checking...");
    else snprintf(t, sizeof(t), "Up to date  -  v%s", s_upd.current);
    ui_set_text(s_sum[SEC_UPDATES], t);
    lv_obj_set_style_text_color(s_sum[SEC_UPDATES], s_upd.available ? COL_OK : COL_MUTED, 0);
}

/* ── sections ────────────────────────────────────────────────────────── */
static void build_network(lv_obj_t *c)
{
    lv_obj_t *g = group(c, "WI-FI");
    s_info = ui_label(g, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info, LV_PCT(100));
    wide_btn(g, ICON_WIFI, "Choose a network", COL_ACCENT, wifi_cb, NULL);
}

static void build_printers(lv_obj_t *c)
{
    lv_obj_t *g = group(c, "PRINTERS  (tap to edit or remove)");
    int n = printers_count();
    for (int i = 0; i < n; i++) {
        printer_cfg_t pc;
        if (!printers_get_cfg(i, &pc)) continue;
        lv_obj_t *b = lv_button_create(g);
        ui_list_row(b, 80);
        lv_obj_set_style_bg_color(b, COL_CARD2, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_text_color(b, COL_TEXT, 0);
        lv_obj_add_event_cb(b, edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[80];
        snprintf(t, sizeof(t), "%s:%u", pc.host, pc.port);
        ui_label(b, FONT_L, COL_MUTED, ICON_PRINTER);
        ui_two_line(b, FONT_M, COL_TEXT, pc.name, t);
        ui_label(b, FONT_L, COL_MUTED, ICON_EDIT);
    }
    if (!n) ui_label(g, FONT_M, COL_MUTED, "No printers added yet.");
    wide_btn(g, ICON_PLUS, "Add printer", COL_ACCENT, add_cb, NULL);

    lv_obj_t *g2 = group(c, "REFRESH RATE");
    lv_obj_t *r = row(g2, ICON_PRINTER, "Open printer");
    dropdown(r, "0.5 s\n1 s\n2 s", find_u16(POLL_OPTS, 3, g_settings.poll_ms), poll_cb);
    r = row(g2, ICON_STATUS, "Home screen tiles");
    dropdown(r, "2 s\n3 s\n5 s\n10 s", find_u16(POLLBG_OPTS, 4, g_settings.poll_bg_ms), pollbg_cb);
}

static void build_display(lv_obj_t *c)
{
    lv_obj_t *g = group(c, "APPEARANCE");
    ui_label(g, FONT_M, COL_TEXT, ICON_AUTO "  Theme");
    static const char *modes[] = { ICON_MOON "  Dark", ICON_SUN "  Light", ICON_AUTO "  Auto", "" };
    lv_obj_t *bm = lv_buttonmatrix_create(g);
    lv_buttonmatrix_set_map(bm, modes);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_buttonmatrix_set_button_ctrl(bm, g_settings.theme_mode < 3 ? g_settings.theme_mode : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, LV_PCT(100), 68);
    lv_obj_set_style_text_font(bm, FONT_M, 0);
    lv_obj_set_style_bg_opa(bm, 0, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_add_event_cb(bm, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *mh = ui_label(g, FONT_S, COL_MUTED,
                            g_settings.has_loc ? "Auto switches to light at sunrise and dark at sunset where you are."
                                               : "Auto switches at sunrise and sunset once your location is known "
                                                 "(looked up from your internet connection); until then 7:00-19:00.");
    lv_label_set_long_mode(mh, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(mh, LV_PCT(100));

    ui_label(g, FONT_M, COL_TEXT, ICON_PALETTE "  Colour theme");
    lv_obj_t *tg = lv_obj_create(g);
    lv_obj_set_size(tg, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tg, 0, 0);
    lv_obj_set_style_border_width(tg, 0, 0);
    lv_obj_set_style_pad_all(tg, 0, 0);
    lv_obj_set_style_pad_gap(tg, 12, 0);
    lv_obj_set_flex_flow(tg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(tg, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < ui_theme_count(); i++) {
        /* a small picture of the theme in the current mode: its background,
         * a card, and an accent bar */
        lv_color_t bg, card, acc, txt;
        ui_theme_preview(i, &bg, &card, &acc, &txt);
        lv_obj_t *b = lv_button_create(tg);
        lv_obj_set_size(b, 168, 108);
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 10, 0);
        lv_obj_set_style_border_width(b, i == g_settings.theme ? 4 : 1, 0);
        lv_obj_set_style_border_color(b, i == g_settings.theme ? COL_TEXT : COL_BORDER, 0);
        lv_obj_add_event_cb(b, palette_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *cd = lv_obj_create(b);
        lv_obj_remove_style_all(cd);
        lv_obj_set_size(cd, LV_PCT(100), 30);
        lv_obj_set_style_bg_opa(cd, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(cd, card, 0);
        lv_obj_set_style_radius(cd, 8, 0);
        lv_obj_align(cd, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_remove_flag(cd, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ab = lv_obj_create(cd);
        lv_obj_remove_style_all(ab);
        lv_obj_set_size(ab, LV_PCT(45), 10);
        lv_obj_set_style_bg_opa(ab, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ab, acc, 0);
        lv_obj_set_style_radius(ab, 5, 0);
        lv_obj_align(ab, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_remove_flag(ab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = ui_label(b, FONT_S, txt, ui_theme_name(i));
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    }

    ui_label(g, FONT_M, COL_TEXT, ICON_PALETTE "  Accent colour");
    lv_obj_t *sw = lv_obj_create(g);
    lv_obj_set_size(sw, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sw, 0, 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    lv_obj_set_style_pad_all(sw, 0, 0);
    lv_obj_set_style_pad_gap(sw, 12, 0);
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    for (int k = -1; k < ui_accent_count(); k++) {
        int i = k < 0 ? ACCENT_THEME : k;    /* first swatch: the theme's own accent */
        lv_color_t col = ui_accent_color(k);
        lv_obj_t *b = lv_button_create(sw);
        lv_obj_set_size(b, 104, 84);
        lv_obj_set_style_bg_color(b, col, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, i == g_settings.accent ? 4 : 0, 0);
        lv_obj_set_style_border_color(b, COL_TEXT, 0);
        lv_obj_add_event_cb(b, accent_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, FONT_S, ui_contrast(col), ui_accent_name(i));
        lv_obj_center(l);
    }
    lv_obj_t *r;

    ui_label(g, FONT_M, COL_TEXT, ICON_POWER "  Brightness");
    lv_obj_t *sl = lv_slider_create(g);
    lv_obj_set_width(sl, LV_PCT(96));
    lv_obj_set_height(sl, 18);
    lv_obj_set_style_pad_ver(sl, 16, 0);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, g_settings.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *g2 = group(c, "ORIENTATION");
    r = row(g2, ICON_RESTART, "Screen rotation");
    dropdown(r, "Landscape\nPortrait\nLandscape (flipped)\nPortrait (flipped)", g_settings.orientation, orient_cb);
    ui_label(g2, FONT_S, COL_MUTED, "Changing the rotation restarts the display.");

}

static void build_time(lv_obj_t *c)
{
    lv_obj_t *g = group(c, "CLOCK");
    s_time_lbl = ui_label(g, FONT_XL, COL_TEXT, "--:--");
    lv_obj_t *r = row(g, ICON_CLOCK, "Time zone");
    char opts[600] = "Auto (detect)";
    for (int i = 0; i < TZ_COUNT; i++) {
        strlcat(opts, "\n", sizeof(opts));
        strlcat(opts, TZ_TABLE[i].label, sizeof(opts));
    }
    int ti = tz_index(g_settings.tz);
    dropdown(r, opts, g_settings.tz_auto || ti < 0 ? 0 : ti + 1, tz_cb);
}

static void check_cb(lv_event_t *e) { updater_check_now(); }
static void view_upd_cb(lv_event_t *e) { ui_update_dialog(); }

static void build_updates(lv_obj_t *c)
{
    lv_obj_t *g = group(c, "FIRMWARE");
    s_info = ui_label(g, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info, LV_PCT(100));
    s_upd_btns = lv_obj_create(g);
    lv_obj_set_size(s_upd_btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_upd_btns, 0, 0);
    lv_obj_set_style_border_width(s_upd_btns, 0, 0);
    lv_obj_set_style_pad_all(s_upd_btns, 0, 0);
    lv_obj_set_style_pad_gap(s_upd_btns, 14, 0);
    lv_obj_set_flex_flow(s_upd_btns, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_upd_btns, LV_OBJ_FLAG_SCROLLABLE);
    s_upd_shown = -1;
    lv_obj_t *n = ui_label(c, FONT_S, COL_MUTED,
                           "The display checks GitHub for new releases a minute after starting and every 6 hours. "
                           "Updates download over Wi-Fi and keep your settings and printers.");
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(n, LV_PCT(100));
}

static void refresh_updates(void)
{
    updater_get(&s_upd);
    char t[256], when[40] = "never";
    if (s_upd.checked_at) {
        struct tm tm;
        localtime_r(&s_upd.checked_at, &tm);
        strftime(when, sizeof(when), "%H:%M, %a %d %b", &tm);
    }
    const char *st = s_upd.state == UPD_CHECKING ? "Checking for updates..."
                   : s_upd.state == UPD_AVAILABLE ? ICON_NEW "  A new version is available"
                   : s_upd.state == UPD_UP_TO_DATE ? ICON_CHECK "  You're up to date"
                   : s_upd.state == UPD_INSTALLING ? "Installing..."
                   : s_upd.state == UPD_ERROR ? s_upd.error : "Not checked yet";
    snprintf(t, sizeof(t), "Installed version:  %s\nLatest version:  %s\nLast checked:  %s\n\n%s",
             s_upd.current, s_upd.latest[0] ? s_upd.latest : "-", when, st);
    ui_set_text(s_info, t);
    lv_obj_set_style_text_color(s_info, s_upd.state == UPD_ERROR ? COL_ERR : COL_TEXT, 0);
    int key = s_upd.available ? 1 : 0;
    if (key != s_upd_shown) {
        s_upd_shown = key;
        lv_obj_clean(s_upd_btns);
        if (key) wide_btn(s_upd_btns, ICON_UPDATE, "View update", COL_OK, view_upd_cb, NULL);
        wide_btn(s_upd_btns, ICON_RESTART, "Check for updates", COL_CARD2, check_cb, NULL);
    }
}

static void build_device(lv_obj_t *c)
{
    lv_obj_t *g = group(c, "NAME");
    s_name_ta = lv_textarea_create(g);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, 31);
    lv_obj_set_width(s_name_ta, LV_PCT(100));
    lv_obj_set_style_text_font(s_name_ta, FONT_L, 0);
    lv_textarea_set_text(s_name_ta, g_settings.device_name);
    lv_obj_add_event_cb(s_name_ta, name_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(s_name_ta, name_cb, LV_EVENT_READY, NULL);
    ui_attach_keyboard(s_name_ta, LV_KEYBOARD_MODE_TEXT_LOWER);

    lv_obj_t *g2 = group(c, "ABOUT");
    s_info = ui_label(g2, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info, LV_PCT(100));

    lv_obj_t *ga = group(c, "ALERTS & SOUND");
    lv_obj_t *r = row(ga, ICON_BELL, "Show when a print finishes or fails");
    lv_obj_t *as = lv_switch_create(r);
    lv_obj_set_size(as, 96, 48);
    if (g_settings.alerts) lv_obj_add_state(as, LV_STATE_CHECKED);
    lv_obj_add_event_cb(as, alerts_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = row(ga, ICON_BELL, "Alert sound");
    int vi = 0;
    for (int i = 0; i < 4; i++) if (VOLS[i] == g_settings.alert_vol) vi = i;
    dropdown(r, "Off\nQuiet\nNormal\nLoud", vi, vol_cb);
    wide_btn(ga, ICON_BELL, "Test sound", COL_CARD2, test_sound_cb, NULL);

    lv_obj_t *gb = group(c, "BATTERY");
    s_bat_info = ui_label(gb, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(s_bat_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_bat_info, LV_PCT(100));
    r = row(gb, ICON_BATTERY, "Battery size");
    dropdown(r, "No battery\n500 mAh\n1000 mAh\n1500 mAh\n2000 mAh\n2500 mAh\n3000 mAh\n4000 mAh\n5000 mAh",
             find_u16(BAT_CAPS, 9, g_settings.bat_cap_mah), bat_cap_cb);
    r = row(gb, ICON_BATTERY, "Battery type");
    dropdown(r, "Li-ion (18650)\nLiPo (flat pack)", g_settings.bat_type == BAT_TYPE_LIPO ? 1 : 0, bat_type_cb);
    r = row(gb, ICON_POWER, "Turn off when empty");
    dropdown(r, "Never\nBelow 3.0 V\nBelow 3.1 V\nBelow 3.2 V\nBelow 3.3 V\nBelow 3.4 V\nBelow 3.5 V",
             find_u16(BAT_CUTS, 7, g_settings.bat_cut_mv), bat_cut_cb);
    lv_obj_t *note = ui_label(gb, FONT_S, COL_MUTED,
        "The charge level is worked out from the battery voltage, so it's an estimate, especially while charging. "
        "The type picks the matching voltage curve; the size is used for the time-left estimate. "
        "The board charges the battery fully (4.2 V) and that cannot be changed in software. "
#if BOARD_HAS_BAT_PROTECT
        "The board has its own battery protection; \"Turn off when empty\" still turns the screen off "
        "before that cuts in. After turning off, plug in USB and the screen starts again.");
#else
        "A protected cell is preferred but not required; keep \"Turn off when empty\" on so the cell never "
        "runs flat. After turning off, plug in USB and the screen starts again. "
        "Unplugging USB restarts the screen for a moment; that's how the board switches power. "
        "On battery, hold the POWER button to turn the screen off or on.");
#endif
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));

    lv_obj_t *g3 = group(c, "MAINTENANCE");
    wide_btn(g3, ICON_RESTART, "Restart", COL_CARD2, reboot_cb, NULL);
    wide_btn(g3, ICON_DELETE, "Factory reset", COL_ERR, factory_cb, NULL);
}

/* ── label printer ───────────────────────────────────────────────────── */
static lv_obj_t *s_lbl_info;

static void label_show(void)
{
    if (!s_lbl_info) return;
    label_cfg_t c;
    label_cfg_get(&c);
    char t[200], m[100];
    label_state_t st = label_state(m, sizeof(m));
    if (!c.have) snprintf(t, sizeof(t), ICON_INFO "  No printer chosen. Tap Choose printer with the printer on and nearby.");
    else snprintf(t, sizeof(t), ICON_CHECK "  %s  (%02X:%02X:%02X:%02X:%02X:%02X)\n%s", c.name, c.addr[5], c.addr[4],
                  c.addr[3], c.addr[2], c.addr[1], c.addr[0], label_connected() ? "Connected" : "Connects when you print");
    if (st == LABEL_ERROR || st == LABEL_DONE || st == LABEL_SENDING || st == LABEL_CONNECTING) {
        strlcat(t, "\nLast: ", sizeof(t));
        strlcat(t, m, sizeof(t));
    }
    ui_set_text(s_lbl_info, t);
}

static void label_chosen(void *u) { label_show(); }
static void label_choose_cb(lv_event_t *e) { ui_label_choose(label_chosen, NULL); }
static void label_forget_cb(lv_event_t *e) { label_forget(); ui_toast("Label printer forgotten"); label_show(); }

static void label_test_cb(lv_event_t *e) { ui_label_test(); }

static void label_opt(int which, lv_event_t *e)
{
    int v = lv_dropdown_get_selected(lv_event_get_target(e));
    label_cfg_t c;
    label_cfg_get(&c);
    if (which == 0) c.dx_mm = v - 3;
    else if (which == 1) c.dy_mm = v - 3;
    else c.density = v + 1;
    label_cfg_set(&c);
}
static void label_qr_cb(lv_event_t *e) { label_set_qr_link(lv_dropdown_get_selected(lv_event_get_target(e)) == 1); }
static void label_dx_cb(lv_event_t *e) { label_opt(0, e); }
static void label_dy_cb(lv_event_t *e) { label_opt(1, e); }
static void label_dark_cb(lv_event_t *e) { label_opt(2, e); }

static void build_label(lv_obj_t *c)
{
    label_cfg_t lc;
    label_cfg_get(&lc);
    lv_obj_t *g = group(c, "LABEL PRINTER  (Bluetooth)");
    s_lbl_info = ui_label(g, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(s_lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_info, LV_PCT(100));
    lv_obj_t *h = ui_label(g, FONT_S, COL_MUTED,
                           "For Bluetooth label printers that speak TSPL, like the Clabel CT221D, with 50 x 30 mm labels. "
                           "Spoolman spools and filaments get a Print label button. Bluetooth starts on the first print, "
                           "and the printer is connected only while printing (it disconnects 30 s after the last label).");
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
    wide_btn(g, ICON_SEARCH, lc.have ? "Choose another printer" : "Choose printer", COL_ACCENT, label_choose_cb, NULL);
    wide_btn(g, ICON_SPOOL, "Test print", COL_CARD2, label_test_cb, NULL);
    if (lc.have) wide_btn(g, ICON_DELETE, "Forget this printer", COL_CARD2, label_forget_cb, NULL);

    lv_obj_t *g2 = group(c, "ADJUST");
    dropdown(row(g2, ICON_SPOOL, "Spool QR code"), "Spoolman code (scanners)\nWeb link (phones)", label_qr_link() ? 1 : 0, label_qr_cb);
    static const char *const NUDGE = "-3 mm\n-2 mm\n-1 mm\n0\n+1 mm\n+2 mm\n+3 mm";
    dropdown(row(g2, ICON_RIGHT, "Move right"), NUDGE, lc.dx_mm + 3, label_dx_cb);
    dropdown(row(g2, ICON_DOWN, "Move down"), NUDGE, lc.dy_mm + 3, label_dy_cb);
    dropdown(row(g2, ICON_TUNE, "Darkness"), "1\n2\n3\n4\n5\n6\n7\n8 (default)\n9\n10\n11\n12\n13\n14\n15",
             lc.density - 1, label_dark_cb);
    label_show();
}

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int arg)
{
    s_sec = (arg > 0 && arg < SEC_COUNT) ? arg : SEC_GRID;
    s_tick = 0;
    memset(s_sum, 0, sizeof(s_sum));
    s_info = s_time_lbl = s_name_ta = s_bat_info = s_lbl_info = NULL;
    if (s_sec == SEC_GRID) { build_grid(parent); refresh_grid(); return; }

    char title[48];
    snprintf(title, sizeof(title), "Settings  -  %s", SECTIONS[s_sec].title);
    ui_set_title(title);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *c = col(parent);
    switch (s_sec) {
    case SEC_NETWORK:  build_network(c); break;
    case SEC_PRINTERS: build_printers(c); break;
    case SEC_DISPLAY:  build_display(c); break;
    case SEC_TIME:     build_time(c); break;
    case SEC_UPDATES:  build_updates(c); refresh_updates(); break;
    case SEC_DEVICE:   build_device(c); break;
    case SEC_LABEL:    build_label(c); break;
    }
}

static void refresh(void)
{
    if (s_tick++ % 2) return;   /* 2 Hz */
    char t[400];
    if (s_sec == SEC_GRID) { refresh_grid(); return; }
    if (s_sec == SEC_UPDATES && s_info) { refresh_updates(); return; }
    if (s_sec == SEC_LABEL) { label_show(); return; }
    if (s_sec == SEC_NETWORK && s_info) {
        esp_netif_ip_info_t ip;
        char mac[20];
        wifi_mgr_mac_str(mac, sizeof(mac));
        if (wifi_mgr_ip(&ip))
            snprintf(t, sizeof(t), ICON_CHECK "  Connected to %s\nIP " IPSTR "   Gateway " IPSTR "\nSignal %d dBm   MAC %s",
                     wifi_mgr_ssid(), IP2STR(&ip.ip), IP2STR(&ip.gw), wifi_mgr_rssi(), mac);
        else
            snprintf(t, sizeof(t), ICON_WIFI_OFF "  %s%s%s\nMAC %s", wifi_mgr_state_str(), wifi_mgr_ssid()[0] ? "  -  " : "",
                     wifi_mgr_ssid(), mac);
        ui_set_text(s_info, t);
    } else if (s_sec == SEC_TIME && s_time_lbl) {
        if (wifi_mgr_time_valid()) {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            strftime(t, sizeof(t), "%H:%M:%S   %a %d %b %Y", &tm);
        } else strcpy(t, "Waiting for the time...");
        ui_set_text(s_time_lbl, t);
    } else if (s_sec == SEC_DEVICE && s_info) {
        const esp_app_desc_t *app = esp_app_get_description();
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        int64_t up = esp_timer_get_time() / 1000000;
        snprintf(t, sizeof(t),
                 "Firmware %s  (ESP-IDF %s)\nWaveshare %s\nESP32-P4 rev %d.%d, %d cores  -  Wi-Fi via ESP32-C6\n"
                 "Free RAM %u KB internal, %u KB PSRAM\nUptime %lldd %02lld:%02lld:%02lld\nIcons: Material Design Icons (Apache-2.0)",
                 app->version, esp_get_idf_version(), BOARD_NAME, ci.revision / 100, ci.revision % 100, ci.cores,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 up / 86400, (up / 3600) % 24, (up / 60) % 60, up % 60);
        ui_set_text(s_info, t);
        if (s_bat_info) {
            bat_info_t b;
            battery_get(&b);
            if (!g_settings.bat_cap_mah) snprintf(t, sizeof(t), "Battery monitoring is off (No battery).");
            else if (b.state == BAT_NONE) snprintf(t, sizeof(t), "No battery connected.");
            else {
                int n = snprintf(t, sizeof(t), "%s  -  %s%d%%  -  %d.%02d V", battery_state_str(b.state),
                                 b.state == BAT_CHARGING ? "about " : "", b.soc, b.mv / 1000, (b.mv % 1000) / 10);
                if (b.minutes_left >= 0)
                    snprintf(t + n, sizeof(t) - n, "\nAbout %d h %02d min left at this brightness", b.minutes_left / 60,
                             b.minutes_left % 60);
            }
            ui_set_text(s_bat_info, t);
        }
    }
}

const ui_page_t page_settings = { "Settings", create, refresh, NULL };
