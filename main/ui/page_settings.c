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
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "app_settings.h"
#include "wifi_mgr.h"
#include "printers.h"
#include "tz.h"
#include "updater.h"

enum { SEC_GRID = 0, SEC_NETWORK, SEC_PRINTERS, SEC_DISPLAY, SEC_TIME, SEC_UPDATES, SEC_DEVICE, SEC_COUNT };

static const struct { const char *icon, *title; } SECTIONS[SEC_COUNT] = {
    [SEC_NETWORK]  = { ICON_WIFI,     "Network" },
    [SEC_PRINTERS] = { ICON_PRINTER,  "Printers" },
    [SEC_DISPLAY]  = { ICON_DISPLAY,  "Display" },
    [SEC_TIME]     = { ICON_CLOCK,    "Time" },
    [SEC_UPDATES]  = { ICON_UPDATE,   "Updates" },
    [SEC_DEVICE]   = { ICON_CHIP,     "Device" },
};

static int s_sec;
static int s_tick;
static lv_obj_t *s_sum[SEC_COUNT];         /* grid tile summaries */
static lv_obj_t *s_info, *s_time_lbl, *s_name_ta, *s_upd_btns;
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

static void theme_async(void *u) { ui_set_light_mode((bool)(intptr_t)u); }
static void theme_cb(lv_event_t *e)
{
    bool light = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    /* rebuilds the whole screen, including this switch: do it after the event returns */
    lv_async_call(theme_async, (void *)(intptr_t)light);
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
    for (int i = 1; i < SEC_COUNT; i++) {
        lv_obj_t *t = ui_card(parent);
        lv_obj_set_size(t, w, 220);
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
    snprintf(t, sizeof(t), "%s mode  -  %d%% brightness", g_settings.light_mode ? "Light" : "Dark", g_settings.brightness);
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
        lv_obj_set_size(b, LV_PCT(100), 80);
        lv_obj_set_style_bg_color(b, COL_CARD2, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_text_color(b, COL_TEXT, 0);
        lv_obj_add_event_cb(b, edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[96];
        snprintf(t, sizeof(t), ICON_PRINTER "  %s", pc.name);
        lv_obj_t *l1 = ui_label(b, FONT_M, COL_TEXT, t);
        lv_obj_align(l1, LV_ALIGN_LEFT_MID, 4, -12);
        snprintf(t, sizeof(t), "%s:%u", pc.host, pc.port);
        lv_obj_t *l2 = ui_label(b, FONT_S, COL_MUTED, t);
        lv_obj_align(l2, LV_ALIGN_LEFT_MID, 38, 16);
        lv_obj_t *l3 = ui_label(b, FONT_L, COL_MUTED, ICON_EDIT);
        lv_obj_align(l3, LV_ALIGN_RIGHT_MID, -4, 0);
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
    lv_obj_t *r = row(g, ICON_DISPLAY, "Light mode");
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 96, 48);
    if (g_settings.light_mode) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
    lv_obj_t *n = ui_label(g, FONT_S, COL_MUTED,
                           "Time comes from NTP (pool.ntp.org, time.google.com). Auto looks up the zone from your public IP once.");
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(n, LV_PCT(100));
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

    lv_obj_t *g3 = group(c, "MAINTENANCE");
    wide_btn(g3, ICON_RESTART, "Restart", COL_CARD2, reboot_cb, NULL);
    wide_btn(g3, ICON_DELETE, "Factory reset", COL_ERR, factory_cb, NULL);
}

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int arg)
{
    s_sec = (arg > 0 && arg < SEC_COUNT) ? arg : SEC_GRID;
    s_tick = 0;
    memset(s_sum, 0, sizeof(s_sum));
    s_info = s_time_lbl = s_name_ta = NULL;
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
    }
}

static void refresh(void)
{
    if (s_tick++ % 2) return;   /* 2 Hz */
    char t[400];
    if (s_sec == SEC_GRID) { refresh_grid(); return; }
    if (s_sec == SEC_UPDATES && s_info) { refresh_updates(); return; }
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
        } else strcpy(t, "Waiting for NTP time sync...");
        ui_set_text(s_time_lbl, t);
    } else if (s_sec == SEC_DEVICE && s_info) {
        const esp_app_desc_t *app = esp_app_get_description();
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        int64_t up = esp_timer_get_time() / 1000000;
        snprintf(t, sizeof(t),
                 "Firmware %s  (ESP-IDF %s)\nESP32-P4 rev %d.%d, %d cores  -  Wi-Fi via ESP32-C6\n"
                 "Free RAM %u KB internal, %u KB PSRAM\nUptime %lldd %02lld:%02lld:%02lld\nIcons: Material Design Icons (Apache-2.0)",
                 app->version, esp_get_idf_version(), ci.revision / 100, ci.revision % 100, ci.cores,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 up / 86400, (up / 3600) % 24, (up / 60) % 60, up % 60);
        ui_set_text(s_info, t);
    }
}

const ui_page_t page_settings = { "Settings", create, refresh, NULL };
