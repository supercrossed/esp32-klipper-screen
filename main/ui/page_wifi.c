/* Wi-Fi setup: scan, pick, enter password. arg=1 on first boot. */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include "wifi_mgr.h"
#include "app_settings.h"

static lv_obj_t *s_state, *s_list, *s_scan_btn;
static lv_obj_t *s_dlg, *s_dlg_ssid, *s_dlg_pass;
static wifi_ap_t s_aps[WIFI_MAX_APS];
static int s_shown;
static bool s_first;
static char s_sel_ssid[33];

static void dlg_close(void)
{
    if (s_dlg) lv_obj_delete(s_dlg);
    s_dlg = NULL;
}

static void dlg_cancel_cb(lv_event_t *e) { dlg_close(); }

static void dlg_connect_cb(lv_event_t *e)
{
    const char *ssid = s_dlg_ssid ? lv_textarea_get_text(s_dlg_ssid) : s_sel_ssid;
    if (!ssid[0]) { ui_toast("Enter the network name"); return; }
    char ss[33];
    strlcpy(ss, ssid, sizeof(ss));
    wifi_mgr_connect(ss, s_dlg_pass ? lv_textarea_get_text(s_dlg_pass) : "", true);
    char m[64];
    snprintf(m, sizeof(m), "Connecting to %s...", ss);
    ui_toast(m);
    dlg_close();
}

static void show_pw_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    lv_textarea_set_password_mode(s_dlg_pass, !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

static void open_dialog(const char *ssid, bool secure)
{
    dlg_close();
    strlcpy(s_sel_ssid, ssid ? ssid : "", sizeof(s_sel_ssid));
    s_dlg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dlg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_dlg, 0, 0);
    lv_obj_set_style_radius(s_dlg, 0, 0);

    lv_obj_t *box = ui_card(s_dlg);
    lv_obj_set_size(box, ui_is_portrait() ? LV_PCT(92) : 640, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    s_dlg_ssid = NULL;
    s_dlg_pass = NULL;
    if (ssid) {
        char t[64];
        snprintf(t, sizeof(t), ICON_WIFI "  %s", ssid);
        ui_label(box, FONT_L, COL_TEXT, t);
    } else {
        ui_label(box, FONT_L, COL_TEXT, "Hidden network");
        ui_section_title(box, "NETWORK NAME (SSID)");
        s_dlg_ssid = lv_textarea_create(box);
        lv_textarea_set_one_line(s_dlg_ssid, true);
        lv_textarea_set_max_length(s_dlg_ssid, 32);
        lv_obj_set_width(s_dlg_ssid, LV_PCT(100));
        lv_obj_set_style_text_font(s_dlg_ssid, FONT_L, 0);
        ui_attach_keyboard(s_dlg_ssid, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
    if (secure) {
        ui_section_title(box, "PASSWORD");
        s_dlg_pass = lv_textarea_create(box);
        lv_textarea_set_one_line(s_dlg_pass, true);
        lv_textarea_set_password_mode(s_dlg_pass, true);
        lv_textarea_set_max_length(s_dlg_pass, 64);
        lv_obj_set_width(s_dlg_pass, LV_PCT(100));
        lv_obj_set_style_text_font(s_dlg_pass, FONT_L, 0);
        ui_attach_keyboard(s_dlg_pass, LV_KEYBOARD_MODE_TEXT_LOWER);
        if (ssid && !strcmp(ssid, g_settings.wifi_ssid)) lv_textarea_set_text(s_dlg_pass, g_settings.wifi_pass);
        lv_obj_t *cb = lv_checkbox_create(box);
        lv_checkbox_set_text(cb, "Show password");
        lv_obj_set_style_text_font(cb, FONT_M, 0);
        lv_obj_add_event_cb(cb, show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *c = ui_button(row, NULL, "Cancel", COL_CARD2, dlg_cancel_cb, NULL);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_t *k = ui_button(row, ICON_CHECK, "Connect", COL_ACCENT, dlg_connect_cb, NULL);
    lv_obj_set_flex_grow(k, 1);
    /* Focus the first field so the keyboard comes up straight away. */
    lv_obj_t *first = s_dlg_ssid ? s_dlg_ssid : s_dlg_pass;
    if (first) lv_obj_send_event(first, LV_EVENT_FOCUSED, NULL);
}

static void ap_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    open_dialog(s_aps[i].ssid, s_aps[i].secure);
}

static void hidden_cb(lv_event_t *e) { open_dialog(NULL, true); }
static void scan_cb(lv_event_t *e) { wifi_mgr_scan_start(); s_shown = -2; }
static void do_forget(void *u) { wifi_mgr_forget(); ui_toast("Wi-Fi forgotten"); }
static void forget_cb(lv_event_t *e)
{
    ui_confirm("Forget network?", "The saved Wi-Fi credentials will be erased.", "Forget", COL_ERR, do_forget, NULL);
}
static void done_cb(lv_event_t *e) { ui_nav_home(); }

static const char *bars(int rssi)
{
    return rssi > -55 ? "||||" : rssi > -67 ? "|||" : rssi > -78 ? "||" : "|";
}

static void rebuild(int n)
{
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(row, COL_CARD2, 0);
        lv_obj_set_style_bg_color(row, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, ap_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        bool cur = !strcmp(s_aps[i].ssid, wifi_mgr_ssid()) && wifi_mgr_state() == WIFI_ST_CONNECTED;
        lv_obj_t *l = ui_label(row, FONT_L, cur ? COL_OK : COL_TEXT, s_aps[i].ssid);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(70));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        char r[40];
        snprintf(r, sizeof(r), "%s%s  %d dBm", s_aps[i].secure ? ICON_WIFI_LOCK "  " : "", bars(s_aps[i].rssi), s_aps[i].rssi);
        lv_obj_t *rl = ui_label(row, FONT_S, COL_MUTED, r);
        lv_obj_align(rl, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void create(lv_obj_t *parent, int arg)
{
    s_first = arg == 1;
    s_shown = -2;
    s_dlg = NULL;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    ui_set_title(s_first ? "Welcome - connect to Wi-Fi" : "Wi-Fi");

    lv_obj_t *top = ui_card(parent);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_state = ui_label(top, FONT_M, COL_TEXT, "");
    lv_obj_set_flex_grow(s_state, 1);
    s_scan_btn = ui_button(top, ICON_SEARCH, "Scan", COL_ACCENT, scan_cb, NULL);
    ui_button(top, ICON_EDIT, "Hidden", COL_CARD2, hidden_cb, NULL);
    if (g_settings.wifi_ssid[0]) ui_button(top, ICON_DELETE, "Forget", COL_CARD2, forget_cb, NULL);
    if (s_first) ui_button(top, ICON_RIGHT, "Skip", COL_CARD2, done_cb, NULL);

    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_gap(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    wifi_mgr_scan_start();
}

static void refresh(void)
{
    char t[128];
    esp_netif_ip_info_t ip;
    if (wifi_mgr_ip(&ip)) snprintf(t, sizeof(t), ICON_CHECK "  Connected to %s  -  " IPSTR, wifi_mgr_ssid(), IP2STR(&ip.ip));
    else if (wifi_mgr_state() == WIFI_ST_CONNECTING) snprintf(t, sizeof(t), "Connecting to %s...", wifi_mgr_ssid());
    else snprintf(t, sizeof(t), "%s", wifi_mgr_state_str());
    ui_set_text(s_state, t);

    if (wifi_mgr_scan_busy()) {
        if (s_shown != -1) {
            s_shown = -1;
            lv_obj_clean(s_list);
            ui_label(s_list, FONT_M, COL_MUTED, ICON_SEARCH "  Scanning...");
        }
        return;
    }
    int n = wifi_mgr_scan_results(s_aps, WIFI_MAX_APS);
    if (n >= 0 && n != s_shown) {
        s_shown = n;
        rebuild(n);
        if (!n) ui_label(s_list, FONT_M, COL_MUTED, "No networks found. Tap Scan to try again.");
    }
    /* First-boot flow: once connected, continue to the home screen. */
    static int connected_ticks;
    if (s_first && wifi_mgr_state() == WIFI_ST_CONNECTED && !s_dlg) {
        if (++connected_ticks > 8) { connected_ticks = 0; s_first = false; ui_nav_home(); }
    } else connected_ticks = 0;
}

static void destroy(void)
{
    dlg_close();
}

const ui_page_t page_wifi = { "Wi-Fi", create, refresh, destroy };
