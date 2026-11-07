/* Add a printer (network search or manual) / edit an existing one (arg >= 0). */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "printers.h"
#include "discovery.h"
#include "wifi_mgr.h"

static int s_edit;
static lv_obj_t *s_ta_name, *s_ta_host, *s_ta_port, *s_ta_key;
static lv_obj_t *s_scan_btn, *s_scan_bar, *s_scan_lbl, *s_list;
static int s_shown;
static bool s_was_busy;
static disc_result_t s_res[DISC_MAX];

static lv_obj_t *field(lv_obj_t *parent, const char *label, const char *val, lv_keyboard_mode_t mode, bool pw)
{
    ui_section_title(parent, label);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, FONT_L, 0);
    lv_textarea_set_text(ta, val ? val : "");
    lv_textarea_set_password_mode(ta, pw);
    ui_attach_keyboard(ta, mode);
    return ta;
}

static void save_cb(lv_event_t *e)
{
    printer_cfg_t c = {0};
    strlcpy(c.host, lv_textarea_get_text(s_ta_host), sizeof(c.host));
    if (!c.host[0]) { ui_toast("Enter a hostname or IP address"); return; }
    /* accept "host:port" in the host field */
    char *colon = strchr(c.host, ':');
    int port = atoi(lv_textarea_get_text(s_ta_port));
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    c.port = port > 0 && port < 65536 ? port : 7125;
    strlcpy(c.name, lv_textarea_get_text(s_ta_name), sizeof(c.name));
    if (!c.name[0]) strlcpy(c.name, c.host, sizeof(c.name));
    strlcpy(c.api_key, lv_textarea_get_text(s_ta_key), sizeof(c.api_key));
    if (s_edit >= 0) {
        printers_update(s_edit, &c);
        ui_toast("Printer updated");
        ui_nav_back();
        return;
    }
    if (printers_find_host(c.host, c.port) >= 0) { ui_toast("That printer is already added"); return; }
    if (printers_add(&c) < 0) { ui_toast("Printer list is full"); return; }
    ui_toast("Printer added");
    ui_nav_home();
}

static void do_delete(void *u)
{
    printers_remove(s_edit);
    ui_toast("Printer removed");
    ui_nav_home();
}

static void delete_cb(lv_event_t *e)
{
    ui_confirm("Remove printer?", "It will be removed from this display. Nothing changes on the printer.",
               "Remove", COL_ERR, do_delete, NULL);
}

static void scan_cb(lv_event_t *e)
{
    if (discovery_busy()) { discovery_cancel(); return; }
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) { ui_toast("Connect to Wi-Fi first"); return; }
    discovery_start();
    s_shown = -1;
}

static void nickname_done(const char *text, void *user)
{
    int i = (int)(intptr_t)user;
    disc_result_t *r = &s_res[i];
    if (printers_find_host(r->host, r->port) >= 0) { ui_toast("Already added"); return; }
    printer_cfg_t c = {0};
    strlcpy(c.name, text, sizeof(c.name));
    strlcpy(c.host, r->host, sizeof(c.host));
    c.port = r->port;
    if (printers_add(&c) < 0) { ui_toast("Printer list is full"); return; }
    char m[96];
    snprintf(m, sizeof(m), "Added %s", c.name);
    ui_toast(m);
    s_shown = -1;   /* rebuild the list so the row shows "Added" */
}

static void result_add_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    disc_result_t *r = &s_res[i];
    if (printers_find_host(r->host, r->port) >= 0) { ui_toast("Already added"); return; }
    ui_prompt(ICON_EDIT "  Nickname for this printer", r->name, nickname_done, (void *)(intptr_t)i);
}

static void rebuild_results(int n)
{
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        disc_result_t *r = &s_res[i];
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), 84);
        lv_obj_set_style_bg_color(row, COL_CARD2, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n1 = ui_label(row, FONT_M, COL_TEXT, r->name);
        lv_obj_align(n1, LV_ALIGN_TOP_LEFT, 0, 4);
        char d[96];
        snprintf(d, sizeof(d), "%s:%u  -  %s  (%s)", r->host, r->port, r->klippy, r->via);
        lv_obj_t *n2 = ui_label(row, FONT_S, COL_MUTED, d);
        lv_obj_align(n2, LV_ALIGN_BOTTOM_LEFT, 0, -4);
        bool added = printers_find_host(r->host, r->port) >= 0;
        lv_obj_t *b = ui_button(row, added ? ICON_CHECK : ICON_PLUS, added ? "Added" : "Add",
                                COL_ACCENT, result_add_cb, (void *)(intptr_t)i);
        if (added) lv_obj_add_state(b, LV_STATE_DISABLED);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void create(lv_obj_t *parent, int arg)
{
    s_edit = arg;
    s_shown = -1;
    s_was_busy = false;
    printer_cfg_t c = { .port = 7125 };
    if (arg >= 0) {
        printers_get_cfg(arg, &c);
        ui_set_title("Edit printer");
    } else ui_set_title("Add printer");

    bool portrait = ui_is_portrait();
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    int colw = portrait ? ui_width() - 32 : (ui_width() - 32 - 16) / 2;

    if (arg < 0) {
        lv_obj_t *sc = ui_card(parent);
        lv_obj_set_size(sc, colw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
        ui_label(sc, FONT_L, COL_TEXT, ICON_SEARCH "  Search network");
        lv_obj_t *d = ui_label(sc, FONT_S, COL_MUTED,
                               "Looks for Moonraker via mDNS and scans this subnet on ports 7125-7127 and 80 (Mainsail/Fluidd). Takes about a minute.");
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, LV_PCT(100));
        s_scan_btn = ui_button(sc, ICON_SEARCH, "Start search", COL_ACCENT, scan_cb, NULL);
        lv_obj_set_width(s_scan_btn, LV_PCT(100));
        s_scan_bar = lv_bar_create(sc);
        lv_obj_set_size(s_scan_bar, LV_PCT(100), 10);
        lv_bar_set_range(s_scan_bar, 0, 100);
        s_scan_lbl = ui_label(sc, FONT_S, COL_MUTED, "");
        s_list = lv_obj_create(sc);
        lv_obj_set_size(s_list, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(s_list, 0, 0);
        lv_obj_set_style_border_width(s_list, 0, 0);
        lv_obj_set_style_pad_all(s_list, 0, 0);
        lv_obj_set_style_pad_gap(s_list, 8, 0);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        /* show results of a previous search immediately */
        int n = discovery_results(s_res, DISC_MAX);
        if (n) { rebuild_results(n); s_shown = n; }
    } else {
        s_scan_btn = s_scan_bar = s_scan_lbl = s_list = NULL;
    }

    lv_obj_t *mc = ui_card(parent);
    lv_obj_set_size(mc, colw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mc, LV_FLEX_FLOW_COLUMN);
    ui_label(mc, FONT_L, COL_TEXT, ICON_EDIT "  Printer details");
    s_ta_name = field(mc, "NAME", c.name, LV_KEYBOARD_MODE_TEXT_LOWER, false);
    s_ta_host = field(mc, "HOST / IP  (tap abc for hostnames)", c.host, LV_KEYBOARD_MODE_USER_1, false);
    char p[8];
    snprintf(p, sizeof(p), "%u", c.port);
    s_ta_port = field(mc, "MOONRAKER PORT (7125 direct, 80 via Mainsail/Fluidd)", p, LV_KEYBOARD_MODE_NUMBER, false);
    s_ta_key = field(mc, "API KEY (optional)", c.api_key, LV_KEYBOARD_MODE_TEXT_LOWER, true);
    lv_obj_t *row = lv_obj_create(mc);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *sb = ui_button(row, ICON_SAVE, arg >= 0 ? "Save" : "Add printer", COL_OK, save_cb, NULL);
    lv_obj_set_flex_grow(sb, 1);
    if (arg >= 0) {
        lv_obj_t *db = ui_button(row, ICON_DELETE, "Remove", COL_ERR, delete_cb, NULL);
        lv_obj_set_flex_grow(db, 1);
    }
}

static void refresh(void)
{
    if (!s_list) return;
    bool busy = discovery_busy();
    lv_bar_set_value(s_scan_bar, busy ? discovery_progress() : (s_was_busy ? 100 : 0), LV_ANIM_OFF);
    lv_label_set_text(lv_obj_get_child(s_scan_btn, 0), busy ? ICON_CLOSE "  Stop search" : ICON_SEARCH "  Start search");
    int n = discovery_results(s_res, DISC_MAX);
    if (n != s_shown) {
        s_shown = n;
        rebuild_results(n);
    }
    char t[64];
    if (busy) snprintf(t, sizeof(t), "Searching... %d%%  -  %d found", discovery_progress(), n);
    else if (s_was_busy || n) snprintf(t, sizeof(t), "%d printer%s found", n, n == 1 ? "" : "s");
    else t[0] = 0;
    ui_set_text(s_scan_lbl, t);
    if (busy) s_was_busy = true;
}

static void destroy(void)
{
    discovery_cancel();   /* don't keep sweeping the subnet once the page is gone */
    s_list = NULL;
}

const ui_page_t page_add_printer = { "Add printer", create, refresh, destroy };
