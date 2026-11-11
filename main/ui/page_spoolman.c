/*
 * Spoolman browser (via a printer's Moonraker): spools / filaments /
 * manufacturers on the left, details and actions on the right.
 *
 * A spool shows its QR code: by default "web+spoolman:s-<id>", the content of
 * Spoolman's own labels, which scanner setups (AFC, Happy Hare, Spoolman) read;
 * or a link to the spool's page in Spoolman, which a phone camera opens
 * (Settings > Label printer sets the default; tap the code to switch one).
 * A filament's code is a link to its page in Spoolman. Labels print the code shown.
 *
 * New / Edit / Duplicate open the form page (page_spool_form.c). Weigh,
 * archive and delete run here. All data is freed when the pages close.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "printers.h"
#include "spoolman.h"
#include "ui_label.h"
#include "label_printer.h"

static int s_idx;
static int s_kind;             /* SM_SPOOL / SM_FILAMENT / SM_VENDOR */
static int s_sel;              /* selected row, -1 none */
static int s_focus_id = -1;    /* select this id once the list arrives */
static uint32_t s_ver;
static int s_active_shown;
static bool s_show_archived;
static bool s_qr_link;         /* spool QR: web link instead of the scanner code (setting; a tap switches) */
static int s_action;           /* 0 none; else a job started here is running */
static int s_action_id;        /* the item it is about */
static spool_item_t *s_items;  /* PSRAM copy of the current list (filtered) */
static int s_n;
static char s_pname[32];

static lv_obj_t *s_list, *s_detail, *s_status, *s_tabs, *s_arch_btn;

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
    if (s_kind == SM_VENDOR) {
        lv_obj_t *l = ui_label(s, FONT_L, COL_MUTED, ICON_PRINTER);
        lv_obj_center(l);
    }
    return s;
}

static void fmt_title(const spool_item_t *it, char *b, size_t cap)
{
    if (s_kind == SM_SPOOL) snprintf(b, cap, "#%d  %s", it->id, it->name[0] ? it->name : "(unnamed)");
    else snprintf(b, cap, "%s", it->name[0] ? it->name : "(unnamed)");
}

static const spool_item_t *selected(void) { return s_sel >= 0 && s_sel < s_n ? &s_items[s_sel] : NULL; }

static void open_form(int kind, int mode, int id)
{
    spoolman_hold();   /* the form uses the data loaded here */
    ui_nav_push(PAGE_SPOOL_FORM, SPOOL_FORM_ARG(kind, mode, id));
}

/* run a write here; refresh() reports it and reloads */
static void run(const char *method, const char *path, const char *body, int action)
{
    spoolman_job_clear();
    if (!spoolman_job(method, path, body, 1)) { ui_toast("Busy - try again"); return; }
    s_action = action;
    ui_set_text(s_status, "Saving...");
}

/* ── detail actions ──────────────────────────────────────────────────── */
static void set_active_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    spoolman_set_active(s_idx, id);
    char m[80];
    if (id >= 0) snprintf(m, sizeof(m), ICON_SPOOL "  Spool #%d set active on %s", id, s_pname);
    else snprintf(m, sizeof(m), ICON_SPOOL "  Active spool cleared on %s", s_pname);
    ui_toast(m);
}

static void edit_cb(lv_event_t *e) { const spool_item_t *it = selected(); if (it) open_form(s_kind, FM_EDIT, it->id); }
static void copy_cb(lv_event_t *e) { const spool_item_t *it = selected(); if (it) open_form(s_kind, FM_COPY, it->id); }
static void add_spool_cb(lv_event_t *e) { const spool_item_t *it = selected(); if (it) open_form(SM_SPOOL, FM_SPOOL_OF, it->id); }
static void new_cb(lv_event_t *e)
{
    char err[8];
    if (spoolman_state(err, sizeof(err)) != SM_OK) { ui_toast("Wait for Spoolman to load"); return; }
    open_form(s_kind, FM_NEW, 0);
}

static void weighed(float v, void *user)
{
    int id = (int)(intptr_t)user;
    char path[48], body[48];
    snprintf(path, sizeof(path), "/v1/spool/%d/measure", id);
    snprintf(body, sizeof(body), "{\"weight\":%.1f}", v);
    run("PUT", path, body, 1);
}

static void weigh_cb(lv_event_t *e)
{
    const spool_item_t *it = selected();
    if (!it) return;
    if (it->spool_g < 0) {
        ui_toast(ICON_ALERT "  Set the empty spool weight first (spool, filament or manufacturer)");
        return;
    }
    char t[96];
    snprintf(t, sizeof(t), ICON_SPOOL "  Weigh #%d (g, with spool)", it->id);
    float now = it->remaining_g >= 0 ? it->remaining_g + it->spool_g : 0;
    ui_numpad(t, now, NULL, 0, weighed, (void *)(intptr_t)it->id);
}

static void archive_cb(lv_event_t *e)
{
    const spool_item_t *it = selected();
    if (!it) return;
    char path[48];
    snprintf(path, sizeof(path), "/v1/spool/%d", it->id);
    run("PATCH", path, it->archived ? "{\"archived\":false}" : "{\"archived\":true}", 2);
    s_action_id = it->id;
}

static void do_delete(void *user)
{
    static const char *const P[SM_KINDS] = { "/v1/spool", "/v1/filament", "/v1/vendor" };
    char path[48];
    snprintf(path, sizeof(path), "%s/%d", P[s_kind], (int)(intptr_t)user);
    run("DELETE", path, NULL, 3);
}

/* how many spools use this filament / filaments use this manufacturer */
static int users_of(int kind, int id)
{
    if (kind == SM_SPOOL) return 0;
    spool_item_t *all = heap_caps_malloc(SPOOL_MAX * sizeof(spool_item_t), MALLOC_CAP_SPIRAM);
    if (!all) return 0;
    int n = spoolman_items(kind == SM_FILAMENT ? SM_SPOOL : SM_FILAMENT, all, SPOOL_MAX), c = 0;
    for (int i = 0; i < n; i++) c += (kind == SM_FILAMENT ? all[i].filament_id : all[i].vendor_id) == id;
    heap_caps_free(all);
    return c;
}

static void delete_cb(lv_event_t *e)
{
    const spool_item_t *it = selected();
    if (!it) return;
    int users = users_of(s_kind, it->id);
    if (users) {   /* Spoolman refuses these with an unhelpful error; say why up front */
        char m[128];
        if (s_kind == SM_FILAMENT)
            snprintf(m, sizeof(m), ICON_ALERT "  %d spool%s still use%s this filament - delete %s first (archived ones too)",
                     users, users == 1 ? "" : "s", users == 1 ? "s" : "", users == 1 ? "it" : "them");
        else
            snprintf(m, sizeof(m), ICON_ALERT "  %d filament%s still use%s this manufacturer - change or delete %s first",
                     users, users == 1 ? "" : "s", users == 1 ? "s" : "", users == 1 ? "it" : "them");
        ui_toast(m);
        return;
    }
    static const char *const N[SM_KINDS] = { "spool", "filament", "manufacturer" };
    char title[64], text[160];
    snprintf(title, sizeof(title), "Delete %s?", N[s_kind]);
    snprintf(text, sizeof(text), "%s #%d \"%s\" is removed from Spoolman for good.%s", N[s_kind], it->id, it->name,
             s_kind == SM_SPOOL ? " Archiving keeps its history instead." : "");
    text[0] = text[0] - 'a' + 'A';
    ui_confirm(title, text, "Delete", COL_ERR, do_delete, (void *)(intptr_t)it->id);
}

static void qr_toggle_cb(lv_event_t *e);

/* the QR content shown for an item (and printed on its label); "" if none */
static void qr_data(const spool_item_t *it, char *out, size_t cap)
{
    char web[96];
    spoolman_web_url(web, sizeof(web));
    out[0] = 0;
    if (s_kind == SM_SPOOL) {
        if (s_qr_link && web[0]) snprintf(out, cap, "%s/spool/show/%d", web, it->id);
        else snprintf(out, cap, "web+spoolman:s-%d", it->id);
    } else if (s_kind == SM_FILAMENT && web[0]) snprintf(out, cap, "%s/filament/show/%d", web, it->id);
}

static void print_label_cb(lv_event_t *e)
{
    const spool_item_t *it = selected();
    if (!it) return;
    ui_label_t l = {0};
    qr_data(it, l.qr, sizeof(l.qr));
    strlcpy(l.name, it->name[0] ? it->name : "(unnamed)", sizeof(l.name));
    snprintf(l.sub, sizeof(l.sub), "%s%s%s", it->material, it->material[0] && it->vendor[0] ? "  -  " : "", it->vendor);
    l.ext_temp = it->ext_temp;
    l.bed_temp = it->bed_temp;
    if (it->has_color) snprintf(l.note, sizeof(l.note), "#%06lX", (unsigned long)it->color);
    snprintf(l.foot, sizeof(l.foot), "%s #%d", s_kind == SM_SPOOL ? "Spool" : "Filament", it->id);
    ui_label_print(&l);
}

static void detail_line(lv_obj_t *p, const char *icon, const char *k, const char *v)
{
    if (!v || !v[0]) return;
    char t[128];
    snprintf(t, sizeof(t), "%s  %s:  %s", icon, k, v);
    lv_obj_t *l = ui_label(p, FONT_M, COL_TEXT, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
}

static void qr_card(const char *data, const char *hint, bool tappable)
{
    /* white background so any scanner reads it, regardless of theme */
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
    if (tappable) {
        lv_obj_add_flag(qc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(qc, qr_toggle_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *qr = lv_qrcode_create(qc);
    lv_qrcode_set_size(qr, 230);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, data, strlen(data));
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(qc, FONT_S, lv_color_hex(0x333333), data);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(100));
    if (hint) ui_label(qc, FONT_S, lv_color_hex(0x777777), hint);
}

/* action buttons wrap into rows: pct is the share of the row (100, 48 or 31) */
static lv_obj_t *action(lv_obj_t *row, const char *icon, const char *txt, lv_color_t c, lv_event_cb_t cb, void *u, int pct)
{
    lv_obj_t *b = ui_button(row, icon, txt, c, cb, u);
    lv_obj_set_width(b, LV_PCT(pct));   /* fixed shares (no flex grow) so the row wraps */
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(l, LV_PCT(100), 0);
    return b;
}

static void build_detail(void)
{
    lv_obj_clean(s_detail);
    const spool_item_t *it = selected();
    if (!it) {
        static const char *const H[SM_KINDS] = {
            ICON_INFO "  Select a spool to see details and its QR code.",
            ICON_INFO "  Select a filament to see details and its QR code.",
            ICON_INFO "  Select a manufacturer to see details.",
        };
        lv_obj_t *l = ui_label(s_detail, FONT_M, COL_MUTED, H[s_kind]);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }
    char t[160];

    lv_obj_t *hdr = plain(s_detail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(hdr, 16, 0);
    swatch(hdr, 64, it);
    lv_obj_t *hc = plain(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(hc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(hc, 1);
    lv_obj_set_style_pad_gap(hc, 2, 0);
    fmt_title(it, t, sizeof(t));
    if (s_kind == SM_SPOOL && it->archived) strlcat(t, "  (archived)", sizeof(t));
    if (s_kind != SM_SPOOL) { char id[16]; snprintf(id, sizeof(id), "  #%d", it->id); strlcat(t, id, sizeof(t)); }
    lv_obj_t *tl = ui_label(hc, FONT_L, COL_TEXT, t);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tl, LV_PCT(100));
    if (s_kind != SM_VENDOR) {
        snprintf(t, sizeof(t), "%s%s%s", it->vendor, it->vendor[0] && it->material[0] ? "  -  " : "", it->material);
        ui_label(hc, FONT_M, COL_MUTED, t);
    }

    if (s_kind == SM_SPOOL && it->remaining_g >= 0) {
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
    if (s_kind == SM_SPOOL) {
        detail_line(s_detail, ICON_HOME, "Location", it->location);
        detail_line(s_detail, ICON_LIST, "Lot", it->lot);
    }
    if (s_kind == SM_FILAMENT) {
        char w[48] = "";
        if (it->total_g > 0) snprintf(w, sizeof(w), ",  %.0f g", it->total_g);
        snprintf(t, sizeof(t), "%.2f mm,  %.2f g/cm³%s", it->diameter, it->density, w);
        detail_line(s_detail, ICON_SPOOL, "Filament", t);
    }
    if (it->spool_g >= 0 && s_kind != SM_VENDOR) {
        snprintf(t, sizeof(t), "%.0f g", it->spool_g);
        detail_line(s_detail, ICON_SPOOL, "Empty spool", t);
    }
    if (s_kind == SM_VENDOR && it->total_g >= 0) {
        snprintf(t, sizeof(t), "%.0f g", it->total_g);
        detail_line(s_detail, ICON_SPOOL, "Empty spool weight", t);
    }
    if (s_kind != SM_VENDOR && (it->ext_temp || it->bed_temp)) {
        snprintf(t, sizeof(t), "%d° nozzle  /  %d° bed", it->ext_temp, it->bed_temp);
        detail_line(s_detail, ICON_THERMO, "Temps", t);
    }

    char web[96];
    spoolman_web_url(web, sizeof(web));
    if (s_kind == SM_SPOOL) {
        char data[160];
        bool link = s_qr_link && web[0];
        qr_data(it, data, sizeof(data));
        qr_card(data, web[0] ? (link ? "Tap for the label code" : "Tap for a link to Spoolman") : NULL, web[0]);
    } else if (s_kind == SM_FILAMENT) {
        if (web[0]) {
            char data[160];
            snprintf(data, sizeof(data), "%s/filament/show/%d", web, it->id);
            qr_card(data, "Opens this filament in Spoolman", false);
        } else detail_line(s_detail, ICON_INFO, "QR code", "Spoolman's web address is unknown");
    }

    lv_obj_t *br = plain(s_detail, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(br, 10, 0);
    if (s_kind == SM_SPOOL) {
        bool active = it->id == spoolman_active_spool();
        if (!it->archived) {
            snprintf(t, sizeof(t), "Set active on %s", s_pname);
            if (!active) action(br, ICON_CHECK, t, COL_ACCENT, set_active_cb, (void *)(intptr_t)it->id, 100);
            else action(br, ICON_CLOSE, "Clear active spool", COL_CARD2, set_active_cb, (void *)(intptr_t)-1, 100);
        }
        action(br, ICON_SPOOL, "Print label", COL_CARD2, print_label_cb, NULL, 31);
        action(br, ICON_TUNE, "Weigh", COL_CARD2, weigh_cb, NULL, 31);
        action(br, ICON_EDIT, "Edit", COL_CARD2, edit_cb, NULL, 31);
        action(br, ICON_PLUS, "Duplicate", COL_CARD2, copy_cb, NULL, 31);
        action(br, ICON_SAVE, it->archived ? "Unarchive" : "Archive", COL_CARD2, archive_cb, NULL, 31);
        action(br, ICON_DELETE, "Delete", COL_ERR, delete_cb, NULL, 31);
    } else if (s_kind == SM_FILAMENT) {
        action(br, ICON_PLUS, "Add spool", COL_ACCENT, add_spool_cb, NULL, 48);
        action(br, ICON_EDIT, "Edit", COL_CARD2, edit_cb, NULL, 48);
        action(br, ICON_PLUS, "Duplicate", COL_CARD2, copy_cb, NULL, 48);
        action(br, ICON_DELETE, "Delete", COL_ERR, delete_cb, NULL, 48);
        if (web[0]) action(br, ICON_SPOOL, "Print label", COL_CARD2, print_label_cb, NULL, 100);
    } else {
        action(br, ICON_EDIT, "Edit", COL_CARD2, edit_cb, NULL, 48);
        action(br, ICON_DELETE, "Delete", COL_ERR, delete_cb, NULL, 48);
    }
}

static void qr_toggle_cb(lv_event_t *e)
{
    s_qr_link = !s_qr_link;
    int32_t y = lv_obj_get_scroll_y(s_detail);
    build_detail();
    lv_obj_update_layout(s_detail);
    lv_obj_scroll_to_y(s_detail, y, LV_ANIM_OFF);   /* stay on the code */
}

/* ── list ────────────────────────────────────────────────────────────── */
static void mark_rows(void)
{
    for (int k = 0; k < (int)lv_obj_get_child_count(s_list); k++) {
        lv_obj_t *r = lv_obj_get_child(s_list, k);
        lv_obj_set_style_border_color(r, k == s_sel ? COL_ACCENT : COL_CARD2, 0);
    }
}

static void row_cb(lv_event_t *e)
{
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    mark_rows();
    build_detail();
    lv_obj_scroll_to_y(s_detail, 0, LV_ANIM_OFF);
}

static void build_list(void)
{
    int keep = selected() ? selected()->id : -1;
    if (s_focus_id >= 0) keep = s_focus_id;
    int32_t list_y = lv_obj_get_scroll_y(s_list), detail_y = lv_obj_get_scroll_y(s_detail);
    bool same = keep >= 0 && s_focus_id < 0;   /* a reload: stay where the user was */
    lv_obj_clean(s_list);
    heap_caps_free(s_items);
    s_items = heap_caps_malloc(SPOOL_MAX * sizeof(spool_item_t), MALLOC_CAP_SPIRAM);
    int n = s_items ? spoolman_items(s_kind, s_items, SPOOL_MAX) : 0;
    /* archived spools only when asked for */
    s_n = 0;
    for (int i = 0; i < n; i++)
        if (s_kind != SM_SPOOL || s_show_archived || !s_items[i].archived || s_items[i].id == s_focus_id) s_items[s_n++] = s_items[i];
    s_sel = -1;
    for (int i = 0; i < s_n; i++) if (s_items[i].id == keep) s_sel = i;
    int active = spoolman_active_spool();
    s_active_shown = active;
    lv_obj_t *sel_row = NULL;
    for (int i = 0; i < s_n; i++) {
        const spool_item_t *it = &s_items[i];
        lv_obj_t *r = lv_obj_create(s_list);
        ui_list_row(r, 80);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_bg_color(r, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, i == s_sel ? COL_ACCENT : COL_CARD2, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 12, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (it->archived) lv_obj_set_style_opa(r, LV_OPA_60, 0);
        swatch(r, 44, it);
        char t[96];
        fmt_title(it, t, sizeof(t));
        if (s_kind == SM_SPOOL && it->id == active) strlcat(t, "   " ICON_CHECK, sizeof(t));
        char rem[24] = "", sub[128] = "";
        if (s_kind == SM_SPOOL && it->remaining_g >= 0) snprintf(rem, sizeof(rem), "%.0f g", it->remaining_g);
        if (s_kind == SM_VENDOR) {
            if (it->total_g >= 0) snprintf(sub, sizeof(sub), "Empty spool %.0f g", it->total_g);
        } else {
            const char *parts[] = { it->vendor, it->material, rem, it->location, it->archived ? "archived" : "" };
            for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
                if (!parts[p][0]) continue;
                if (sub[0]) strlcat(sub, "  -  ", sizeof(sub));
                strlcat(sub, parts[p], sizeof(sub));
            }
        }
        ui_two_line(r, FONT_M, COL_TEXT, t, sub);
        if (i == s_sel) sel_row = r;
    }
    static const char *const EMPTY[SM_KINDS] = { "No spools.", "No filaments.", "No manufacturers." };
    if (!s_n) ui_label(s_list, FONT_M, COL_MUTED, EMPTY[s_kind]);
    if (sel_row && s_focus_id >= 0) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF);
    }
    s_focus_id = -1;
    build_detail();
    if (same) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, list_y, LV_ANIM_OFF);
        if (s_sel >= 0) { lv_obj_update_layout(s_detail); lv_obj_scroll_to_y(s_detail, detail_y, LV_ANIM_OFF); }
    }
}

static void update_arch_btn(void)
{
    if (!s_arch_btn) return;
    if (s_kind == SM_SPOOL) lv_obj_remove_flag(s_arch_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_arch_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_arch_btn, s_show_archived ? COL_ACCENT : COL_CARD2, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_arch_btn, 0), ui_contrast(s_show_archived ? COL_ACCENT : COL_CARD2), 0);
}

static void tab_cb(lv_event_t *e)
{
    int k = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (k == s_kind) return;
    s_kind = k;
    s_sel = -1;
    update_arch_btn();
    build_list();
}

static void arch_cb(lv_event_t *e)
{
    s_show_archived = !s_show_archived;
    update_arch_btn();
    build_list();
}

static void reload_cb(lv_event_t *e) { spoolman_fetch(s_idx); }

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int idx)
{
    s_idx = idx;
    s_kind = SM_SPOOL;
    s_sel = -1;
    s_focus_id = -1;
    s_items = NULL;
    s_n = 0;
    s_action = 0;
    s_qr_link = label_qr_link();
    int fk, fid;
    if (spoolman_take_focus(&fk, &fid)) { s_kind = fk; s_focus_id = fid; }
    printer_cfg_t pc;
    if (printers_get_cfg(idx, &pc)) strlcpy(s_pname, pc.name, sizeof(s_pname));
    char t[64];
    snprintf(t, sizeof(t), "Spoolman  -  via %s", s_pname);
    ui_set_title(t);

    bool portrait = ui_is_portrait();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *top = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *map[] = { ICON_SPOOL "  Spools", ICON_MMU "  Filaments", ICON_PRINTER "  Makers", "" };
    s_tabs = lv_buttonmatrix_create(top);
    lv_buttonmatrix_set_map(s_tabs, map);
    lv_buttonmatrix_set_button_ctrl_all(s_tabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_tabs, true);
    lv_buttonmatrix_set_button_ctrl(s_tabs, s_kind, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(s_tabs, portrait ? 470 : 560, 64);
    lv_obj_set_style_text_font(s_tabs, FONT_M, 0);
    lv_obj_add_event_cb(s_tabs, tab_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* status: beside the tabs in landscape, on its own line in portrait (no room) */
    s_status = ui_label(portrait ? parent : top, FONT_S, COL_MUTED, "Loading...");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_status, lv_font_get_line_height(FONT_S));
    if (portrait) lv_obj_set_width(s_status, LV_PCT(100));
    else {
        lv_obj_set_width(s_status, 1);
        lv_obj_set_flex_grow(s_status, 1);
    }
    s_arch_btn = ui_button(top, ICON_SAVE, portrait ? NULL : "Archived", COL_CARD2, arch_cb, NULL);
    ui_button(top, ICON_PLUS, portrait ? NULL : "New", COL_ACCENT, new_cb, NULL);
    ui_button(top, ICON_RESTART, portrait ? NULL : "Reload", COL_CARD2, reload_cb, NULL);
    update_arch_btn();

    lv_obj_t *body = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_gap(body, 16, 0);
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
    if (portrait) h -= lv_font_get_line_height(FONT_S) + 16;   /* the status line */
    if (portrait) {
        lv_obj_set_size(s_list, LV_PCT(100), h / 2);
        lv_obj_set_size(s_detail, LV_PCT(100), h / 2);
    } else {
        int w = ui_width() - 32 - 16;
        lv_obj_set_size(s_list, w * 48 / 100, h);
        lv_obj_set_size(s_detail, w - w * 48 / 100, h);
    }

    s_ver = spoolman_version();
    spoolman_fetch(idx);
    build_detail();
}

static void refresh(void)
{
    char err[96];
    if (s_action) {
        int id;
        sm_state_t js = spoolman_job_state(err, sizeof(err), &id);
        if (js != SM_LOADING) {
            static const char *const DONE[] = { "", ICON_CHECK "  Weight saved", ICON_CHECK "  Saved", ICON_CHECK "  Deleted" };
            char t[128];
            if (js == SM_OK) ui_toast(DONE[s_action]);
            else { snprintf(t, sizeof(t), ICON_ALERT "  %s", err[0] ? err : "Spoolman request failed"); ui_toast(t); }
            if (js == SM_OK && s_action == 3) s_sel = -1;
            if (js == SM_OK && s_action == 2) s_focus_id = s_action_id;   /* just archived: keep it in view, to undo */
            s_action = 0;
            spoolman_job_clear();
            spoolman_fetch(s_idx);
        }
    }
    sm_state_t st = spoolman_state(err, sizeof(err));
    if (spoolman_version() != s_ver) {
        s_ver = spoolman_version();
        if (st == SM_OK) build_list();
        else if (s_kind == SM_SPOOL && s_active_shown != spoolman_active_spool()) build_list();   /* active spool changed */
    }
    if (s_action) return;
    static const char *const ONE[SM_KINDS] = { "spool", "filament", "manufacturer" };
    static const char *const MANY[SM_KINDS] = { "spools", "filaments", "manufacturers" };
    char t[96];
    if (st == SM_LOADING) snprintf(t, sizeof(t), "Loading...");
    else if (st == SM_ERROR) snprintf(t, sizeof(t), ICON_ALERT "  %s", err);
    else snprintf(t, sizeof(t), "%d %s", s_n, s_n == 1 ? ONE[s_kind] : MANY[s_kind]);
    ui_set_text(s_status, t);
}

static void destroy(void)
{
    heap_caps_free(s_items);
    s_items = NULL;
    s_n = 0;
    s_arch_btn = NULL;
    if (!spoolman_held()) spoolman_free();   /* kept for the form page, which frees it */
}

const ui_page_t page_spoolman = { "Spoolman", create, refresh, destroy };
