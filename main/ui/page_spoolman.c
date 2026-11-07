/*
 * Spoolman browser (via a printer's Moonraker): spools / filaments list on the
 * left, details on the right. A selected spool shows its QR code
 * ("web+spoolman:s-<id>", the same content as Spoolman's printed labels) so a
 * 2D scanner can read it, and can be made the printer's active spool.
 * All data is freed when the page closes.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "printers.h"
#include "spoolman.h"

static int s_idx;
static int s_kind;             /* 0 spools, 1 filaments */
static int s_sel;              /* selected row, -1 none */
static uint32_t s_ver;
static int s_active_shown;
static spool_item_t *s_items;  /* PSRAM copy of the current list */
static int s_n;
static char s_pname[32];

static lv_obj_t *s_list, *s_detail, *s_status, *s_tabs;

/* ── helpers ─────────────────────────────────────────────────────────── */
static lv_obj_t *plain(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_gap(o, 10, 0);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *swatch(lv_obj_t *parent, int size, const spool_item_t *it)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_set_size(s, size, size);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, COL_BORDER, 0);
    lv_obj_set_style_bg_color(s, it->has_color ? lv_color_hex(it->color) : COL_CARD2, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static void fmt_title(const spool_item_t *it, char *b, size_t cap)
{
    if (s_kind == 0) snprintf(b, cap, "#%d  %s", it->id, it->name[0] ? it->name : "(unnamed)");
    else snprintf(b, cap, "%s", it->name[0] ? it->name : "(unnamed)");
}

/* ── detail panel ────────────────────────────────────────────────────── */
static void set_active_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    spoolman_set_active(s_idx, id);
    char m[80];
    if (id >= 0) snprintf(m, sizeof(m), ICON_SPOOL "  Spool #%d set active on %s", id, s_pname);
    else snprintf(m, sizeof(m), ICON_SPOOL "  Active spool cleared on %s", s_pname);
    ui_toast(m);
}

static void detail_line(lv_obj_t *p, const char *icon, const char *k, const char *v)
{
    if (!v || !v[0]) return;
    char t[96];
    snprintf(t, sizeof(t), "%s  %s:  %s", icon, k, v);
    lv_obj_t *l = ui_label(p, FONT_M, COL_TEXT, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
}

static void build_detail(void)
{
    lv_obj_clean(s_detail);
    if (s_sel < 0 || s_sel >= s_n) {
        lv_obj_t *l = ui_label(s_detail, FONT_M, COL_MUTED,
                               s_kind == 0 ? ICON_INFO "  Select a spool to see details and its QR code."
                                           : ICON_INFO "  Select a filament to see details.");
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }
    const spool_item_t *it = &s_items[s_sel];
    char t[96];

    lv_obj_t *hdr = plain(s_detail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(hdr, 16, 0);
    swatch(hdr, 64, it);
    lv_obj_t *hc = plain(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(hc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(hc, 1);
    lv_obj_set_style_pad_gap(hc, 2, 0);
    fmt_title(it, t, sizeof(t));
    lv_obj_t *tl = ui_label(hc, FONT_L, COL_TEXT, t);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tl, LV_PCT(100));
    snprintf(t, sizeof(t), "%s%s%s", it->vendor, it->vendor[0] && it->material[0] ? "  -  " : "", it->material);
    ui_label(hc, FONT_M, COL_MUTED, t);

    if (s_kind == 0 && it->remaining_g >= 0) {
        float total = it->total_g > 0 ? it->total_g : 1000;
        lv_obj_t *bar = lv_bar_create(s_detail);
        lv_obj_set_size(bar, LV_PCT(100), 18);
        lv_bar_set_range(bar, 0, 1000);
        int v = (int)(it->remaining_g / total * 1000);
        lv_bar_set_value(bar, v < 0 ? 0 : v > 1000 ? 1000 : v, LV_ANIM_OFF);
        if (it->has_color) lv_obj_set_style_bg_color(bar, lv_color_hex(it->color), LV_PART_INDICATOR);
        snprintf(t, sizeof(t), "%.0f g left of %.0f g", it->remaining_g, total);
        ui_label(s_detail, FONT_M, COL_TEXT, t);
    }
    detail_line(s_detail, ICON_HOME, "Location", it->location);
    detail_line(s_detail, ICON_LIST, "Lot", it->lot);
    if (it->ext_temp || it->bed_temp) {
        snprintf(t, sizeof(t), "%d° nozzle  /  %d° bed", it->ext_temp, it->bed_temp);
        detail_line(s_detail, ICON_THERMO, "Temps", t);
    }

    if (s_kind == 0) {
        /* QR card: white background so any scanner reads it, regardless of theme */
        lv_obj_t *qc = lv_obj_create(s_detail);
        lv_obj_set_size(qc, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(qc, lv_color_white(), 0);
        lv_obj_set_style_radius(qc, 14, 0);
        lv_obj_set_style_border_width(qc, 0, 0);
        lv_obj_set_style_pad_all(qc, 16, 0);
        lv_obj_set_style_pad_gap(qc, 8, 0);
        lv_obj_set_flex_flow(qc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(qc, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(qc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *qr = lv_qrcode_create(qc);
        lv_qrcode_set_size(qr, 230);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        char data[48];
        snprintf(data, sizeof(data), "web+spoolman:s-%d", it->id);
        lv_qrcode_update(qr, data, strlen(data));
        ui_label(qc, FONT_S, lv_color_hex(0x333333), data);

        bool active = it->id == spoolman_active_spool();
        lv_obj_t *br = plain(s_detail, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(br, 12, 0);
        if (!active) {
            snprintf(t, sizeof(t), "Set active on %s", s_pname);
            ui_tile_button(br, ICON_CHECK, t, COL_ACCENT, set_active_cb, (void *)(intptr_t)it->id);
        } else {
            ui_tile_button(br, ICON_CLOSE, "Clear active spool", COL_CARD2, set_active_cb, (void *)(intptr_t)-1);
        }
    }
}

/* ── list ────────────────────────────────────────────────────────────── */
static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel = i;
    for (int k = 0; k < (int)lv_obj_get_child_count(s_list); k++) {
        lv_obj_t *r = lv_obj_get_child(s_list, k);
        lv_obj_set_style_border_color(r, k == i ? COL_ACCENT : COL_CARD2, 0);
    }
    build_detail();
    lv_obj_scroll_to_y(s_detail, 0, LV_ANIM_OFF);
}

static void build_list(void)
{
    lv_obj_clean(s_list);
    heap_caps_free(s_items);
    s_items = heap_caps_malloc(SPOOL_MAX * sizeof(spool_item_t), MALLOC_CAP_SPIRAM);
    s_n = s_items ? spoolman_items(s_kind, s_items, SPOOL_MAX) : 0;
    if (s_sel >= s_n) s_sel = -1;
    int active = spoolman_active_spool();
    s_active_shown = active;
    for (int i = 0; i < s_n; i++) {
        const spool_item_t *it = &s_items[i];
        lv_obj_t *r = lv_obj_create(s_list);
        lv_obj_set_size(r, LV_PCT(100), 84);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_bg_color(r, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, i == s_sel ? COL_ACCENT : COL_CARD2, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 12, 0);
        lv_obj_set_style_pad_ver(r, 6, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *sw = swatch(r, 44, it);
        lv_obj_align(sw, LV_ALIGN_LEFT_MID, 0, 0);
        char t[96];
        fmt_title(it, t, sizeof(t));
        if (s_kind == 0 && it->id == active) strlcat(t, "   " ICON_CHECK, sizeof(t));
        lv_obj_t *l1 = ui_label(r, FONT_M, COL_TEXT, t);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l1, LV_PCT(80));
        lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 58, 4);
        char rem[24] = "";
        if (s_kind == 0 && it->remaining_g >= 0) snprintf(rem, sizeof(rem), "  -  %.0f g", it->remaining_g);
        snprintf(t, sizeof(t), "%s%s%s%s%s%s", it->vendor, it->vendor[0] ? "  -  " : "", it->material, rem,
                 it->location[0] ? "  -  " : "", it->location);
        lv_obj_t *l2 = ui_label(r, FONT_S, COL_MUTED, t);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l2, LV_PCT(80));
        lv_obj_align(l2, LV_ALIGN_BOTTOM_LEFT, 58, -4);
    }
    if (!s_n) ui_label(s_list, FONT_M, COL_MUTED, s_kind == 0 ? "No spools." : "No filaments.");
    build_detail();
}

static void tab_cb(lv_event_t *e)
{
    int k = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (k == s_kind) return;
    s_kind = k;
    s_sel = -1;
    build_list();
}

static void reload_cb(lv_event_t *e) { spoolman_fetch(s_idx); }

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int idx)
{
    s_idx = idx;
    s_kind = 0;
    s_sel = -1;
    s_items = NULL;
    s_n = 0;
    printer_cfg_t pc;
    if (printers_get_cfg(idx, &pc)) strlcpy(s_pname, pc.name, sizeof(s_pname));
    char t[64];
    snprintf(t, sizeof(t), "Spoolman  -  via %s", s_pname);
    ui_set_title(t);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *top = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *map[] = { ICON_SPOOL "  Spools", ICON_MMU "  Filaments", "" };
    s_tabs = lv_buttonmatrix_create(top);
    lv_buttonmatrix_set_map(s_tabs, map);
    lv_buttonmatrix_set_button_ctrl_all(s_tabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_tabs, true);
    lv_buttonmatrix_set_button_ctrl(s_tabs, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(s_tabs, 420, 64);
    lv_obj_set_style_text_font(s_tabs, FONT_M, 0);
    lv_obj_add_event_cb(s_tabs, tab_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_status = ui_label(top, FONT_S, COL_MUTED, "Loading...");
    lv_obj_set_flex_grow(s_status, 1);
    ui_button(top, ICON_RESTART, "Reload", COL_CARD2, reload_cb, NULL);

    lv_obj_t *body = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_gap(body, 16, 0);
    bool portrait = ui_is_portrait();
    if (portrait) lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    s_list = lv_obj_create(body);
    lv_obj_set_style_bg_opa(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_gap(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_detail = ui_card(body);
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_detail, 12, 0);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);

    int h = ui_height() - 32 - 64 - 16;
    if (portrait) {
        lv_obj_set_size(s_list, LV_PCT(100), h / 2);
        lv_obj_set_size(s_detail, LV_PCT(100), h / 2);
    } else {
        int w = ui_width() - 32 - 16;
        lv_obj_set_size(s_list, w * 52 / 100, h);
        lv_obj_set_size(s_detail, w - w * 52 / 100, h);
    }

    s_ver = spoolman_version();
    spoolman_fetch(idx);
    build_detail();
}

static void refresh(void)
{
    char err[96];
    sm_state_t st = spoolman_state(err, sizeof(err));
    if (spoolman_version() != s_ver) {
        s_ver = spoolman_version();
        if (st == SM_OK) build_list();
        else if (s_kind == 0 && s_active_shown != spoolman_active_spool()) build_list();   /* active spool changed */
    }
    char t[96];
    if (st == SM_LOADING) snprintf(t, sizeof(t), "Loading...");
    else if (st == SM_ERROR) snprintf(t, sizeof(t), ICON_ALERT "  %s", err);
    else snprintf(t, sizeof(t), "%d %s", s_n, s_kind == 0 ? (s_n == 1 ? "spool" : "spools") : (s_n == 1 ? "filament" : "filaments"));
    ui_set_text(s_status, t);
}

static void destroy(void)
{
    heap_caps_free(s_items);
    s_items = NULL;
    s_n = 0;
    spoolman_free();
}

const ui_page_t page_spoolman = { "Spoolman", create, refresh, destroy };
