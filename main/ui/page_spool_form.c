/*
 * Spoolman add / edit forms (spool, filament, manufacturer), with the same
 * fields, units and help text as Spoolman's web interface, including the
 * extra fields defined in Spoolman's settings. New filaments can be filled in
 * from Spoolman's external filament database.
 *
 * arg = SPOOL_FORM_ARG(kind, mode, id):
 *   FM_NEW       new item
 *   FM_EDIT      edit item id
 *   FM_COPY      new item prefilled from item id (Spoolman's "clone")
 *   FM_SPOOL_OF  new spool of filament id
 *
 * The list page keeps the Spoolman data loaded for us (spoolman_hold); it is
 * all freed when this page closes.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "spoolman.h"

#define NMULTI 8

/* ── state ───────────────────────────────────────────────────────────── */
typedef struct {
    int kind, mode, id;
    bool loading;               /* waiting for the item (edit / copy) */
    int step;                   /* save: 0 idle, 1 creating vendor, 2 saving item */
    lv_obj_t *grid, *save_top, *save_bot, *status;

    /* common */
    lv_obj_t *ta_name, *ta_comment, *ta_price;

    /* filament */
    int vendor_id;              /* -1 none */
    char vendor_new[64];        /* a manufacturer to create on save */
    lv_obj_t *vendor_lbl;
    bool multi;
    int32_t color;              /* -1 none */
    int32_t mcol[NMULTI];
    int nmcol;
    char direction[16];         /* coaxial / longitudinal */
    lv_obj_t *color_tabs, *color_box;
    lv_obj_t *ta_material, *ta_density, *ta_diameter, *ta_weight, *ta_spoolw, *ta_ext, *ta_bed, *ta_article;
    char external_id[96];

    /* spool */
    time_t first_used, last_used;
    lv_obj_t *first_lbl, *last_lbl;
    int filament_id;
    lv_obj_t *filament_lbl;
    lv_obj_t *ta_initial, *ta_sweight;
    int wmode;                  /* 0 used, 1 remaining, 2 measured */
    lv_obj_t *wtabs, *ta_used, *ta_rem, *ta_meas, *wnote;
    lv_obj_t *ta_location, *ta_lot, *ta_qty, *sw_archived;
    bool recompute;
    lv_obj_t *fresh;            /* number field just focused: the first key replaces its value */

    /* vendor */
    lv_obj_t *ta_empty;

    /* extra fields */
    sm_xfield_t xf[SM_XF_MAX];
    int nxf;
    lv_obj_t *xw[SM_XF_MAX], *xw2[SM_XF_MAX];   /* input(s), or the button label */
    time_t xdate[SM_XF_MAX];
    int xchoice[SM_XF_MAX];                     /* single choice index, -1 none */
    bool xsel[SM_XF_MAX][16];                   /* multi choice */

    /* external database search */
    lv_obj_t *ext_ov, *ext_ta, *ext_list, *ext_status;
    sm_ext_t *ext_hits;
    int ext_n;
    bool ext_pending;           /* search once the download is done */
} form_t;

static form_t *F;   /* PSRAM, while the page is open */

static const char *const KIND_PATH[SM_KINDS] = { "/v1/spool", "/v1/filament", "/v1/vendor" };

/* ── small helpers ───────────────────────────────────────────────────── */
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
    if (flow == LV_FLEX_FLOW_ROW) lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return o;
}

static int col_w(bool full)
{
    int w = ui_width() - 32;
    if (full || ui_is_portrait()) return w;
    return (w - 24) / 2;
}

/* A labelled field: "Label" (with * when required), the control, then help text. */
static lv_obj_t *field(const char *label, bool required, const char *help, bool full)
{
    lv_obj_t *b = plain(F->grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(b, col_w(full));
    lv_obj_set_style_pad_gap(b, 6, 0);
    lv_obj_t *lr = plain(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(lr, 6, 0);
    if (required) ui_label(lr, FONT_M, COL_ERR, "*");
    ui_label(lr, FONT_M, COL_TEXT, label);
    lv_obj_set_user_data(b, NULL);
    /* help text goes after the control: add it as the last child once built */
    if (help && help[0]) lv_obj_set_user_data(b, (void *)help);
    return b;
}

static void field_help(lv_obj_t *b)
{
    const char *help = lv_obj_get_user_data(b);
    if (!help) return;
    lv_obj_t *h = ui_label(b, FONT_S, COL_MUTED, help);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
}

static lv_obj_t *text_input(lv_obj_t *parent, int maxlen, bool multiline)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, !multiline);
    lv_textarea_set_max_length(ta, maxlen);
    lv_obj_set_width(ta, LV_PCT(100));
    if (multiline) lv_obj_set_height(ta, 110);
    lv_obj_set_style_text_font(ta, FONT_L, 0);
    ui_attach_keyboard(ta, LV_KEYBOARD_MODE_TEXT_LOWER);
    return ta;
}

static lv_obj_t *add_text(const char *label, bool req, const char *help, int maxlen, bool full)
{
    lv_obj_t *b = field(label, req, help, full);
    lv_obj_t *ta = text_input(b, maxlen, false);
    field_help(b);
    return ta;
}

/* Number fields: the first key typed after focusing replaces the value (like
 * select-all on a computer), a first backspace clears it. */
static void num_focus(lv_event_t *e) { if (F) F->fresh = lv_event_get_target(e); }

static void num_insert(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!F || F->fresh != ta) return;
    F->fresh = NULL;
    const char *in = lv_event_get_param(e);
    lv_textarea_set_text(ta, "");
    if (in && (in[0] == LV_KEY_BACKSPACE || in[0] == LV_KEY_DEL)) lv_textarea_set_insert_replace(ta, "");
}

/* number input with a unit box on the right, like Spoolman's */
static lv_obj_t *num_input(lv_obj_t *parent, const char *unit, bool neg)
{
    lv_obj_t *r = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(r, 0, 0);
    lv_obj_t *ta = lv_textarea_create(r);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 12);
    lv_textarea_set_accepted_chars(ta, neg ? "0123456789.-" : "0123456789.");
    lv_obj_set_width(ta, 220);
    lv_obj_set_style_text_font(ta, FONT_L, 0);
    ui_attach_keyboard(ta, LV_KEYBOARD_MODE_USER_3);
    lv_obj_add_event_cb(ta, num_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, num_insert, LV_EVENT_INSERT, NULL);
    if (unit && unit[0]) {
        lv_obj_t *u = lv_obj_create(r);
        lv_obj_set_size(u, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(u, 56, 0);
        lv_obj_set_style_bg_color(u, COL_CARD2, 0);
        lv_obj_set_style_border_width(u, 1, 0);
        lv_obj_set_style_border_color(u, COL_BORDER, 0);
        lv_obj_set_style_radius(u, 8, 0);
        lv_obj_set_style_pad_hor(u, 12, 0);
        lv_obj_set_style_pad_ver(u, 12, 0);
        lv_obj_remove_flag(u, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = ui_label(u, FONT_M, COL_TEXT, unit);
        lv_obj_center(l);
    }
    return ta;
}

static lv_obj_t *add_num(const char *label, bool req, const char *help, const char *unit)
{
    lv_obj_t *b = field(label, req, help, false);
    lv_obj_t *ta = num_input(b, unit, false);
    field_help(b);
    return ta;
}

/* a button that opens a picker; returns its label */
static lv_obj_t *pick_button(lv_obj_t *parent, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, 56, 0);
    lv_obj_set_style_bg_color(b, COL_CARD2, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = ui_label(b, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(90));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *a = ui_label(b, FONT_M, COL_MUTED, LV_SYMBOL_DOWN);
    lv_obj_align(a, LV_ALIGN_RIGHT_MID, 0, 0);
    return l;
}

static void set_num(lv_obj_t *ta, double v, int decimals)
{
    if (!ta) return;
    char t[24] = "";
    if (!isnan(v)) {
        snprintf(t, sizeof(t), "%.*f", decimals, v);
        if (decimals > 0 && strchr(t, '.')) {   /* 1.750 -> 1.75, 1000.0 -> 1000 */
            char *e = t + strlen(t) - 1;
            while (*e == '0') *e-- = 0;
            if (*e == '.') *e = 0;
        }
    }
    lv_textarea_set_text(ta, t);
}

static double get_num(lv_obj_t *ta)
{
    if (!ta) return NAN;
    const char *t = lv_textarea_get_text(ta);
    if (!t[0]) return NAN;
    char *end;
    double v = strtod(t, &end);
    return end == t ? NAN : v;
}

static void set_text(lv_obj_t *ta, const char *s) { if (ta) lv_textarea_set_text(ta, s ? s : ""); }

/* ISO 8601 (UTC, Spoolman) <-> time_t */
static time_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)era * 146097 + (time_t)doe - 719468;
}

static time_t parse_iso(const char *s)
{
    int Y, M, D, h = 0, m = 0, sec = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
    return days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
}

static void fmt_iso(time_t t, char *out, size_t cap)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void date_label(lv_obj_t *l, time_t t)
{
    if (!t) { lv_label_set_text(l, "Select date"); lv_obj_set_style_text_color(l, COL_MUTED, 0); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%d", &tm);
    lv_label_set_text(l, b);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
}

/* ── manufacturer field ──────────────────────────────────────────────── */
static void vendor_show(void)
{
    if (!F->vendor_lbl) return;
    spool_item_t v;
    if (F->vendor_new[0]) {
        char t[96];
        snprintf(t, sizeof(t), "%s  (new)", F->vendor_new);
        lv_label_set_text(F->vendor_lbl, t);
    } else if (F->vendor_id >= 0 && spoolman_item(SM_VENDOR, F->vendor_id, &v)) lv_label_set_text(F->vendor_lbl, v.name);
    else lv_label_set_text(F->vendor_lbl, "");
}

static spool_item_t *s_list;   /* picker data */
static int s_list_n;

static void vendor_picked(int i, const char *text, void *user)
{
    if (i < 0) { strlcpy(F->vendor_new, text, sizeof(F->vendor_new)); F->vendor_id = -1; }
    else if (i == 0) { F->vendor_id = -1; F->vendor_new[0] = 0; }
    else if (s_list && i - 1 < s_list_n) { F->vendor_id = s_list[i - 1].id; F->vendor_new[0] = 0; }
    vendor_show();
}

static void list_load(int kind)
{
    if (!s_list) s_list = heap_caps_malloc(SPOOL_MAX * sizeof(spool_item_t), MALLOC_CAP_SPIRAM);
    s_list_n = s_list ? spoolman_items(kind, s_list, SPOOL_MAX) : 0;
}

static void vendor_cb(lv_event_t *e)
{
    list_load(SM_VENDOR);
    const char **items = heap_caps_malloc((s_list_n + 1) * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!items) return;
    items[0] = "(none)";
    int sel = 0;
    for (int i = 0; i < s_list_n; i++) {
        items[i + 1] = s_list[i].name;
        if (s_list[i].id == F->vendor_id) sel = i + 1;
    }
    ui_pick(ICON_PRINTER "  Manufacturer", items, s_list_n + 1, sel, "New manufacturer", vendor_picked, NULL);
    heap_caps_free(items);
}

/* ── colour field ────────────────────────────────────────────────────── */
static void color_build(void);

static void color_single_set(int32_t rgb, void *user) { F->color = rgb; color_build(); }

static void color_multi_set(int32_t rgb, void *user)
{
    int i = (int)(intptr_t)user;
    if (i >= F->nmcol) {   /* adding */
        if (rgb >= 0 && F->nmcol < NMULTI) F->mcol[F->nmcol++] = rgb;
    } else if (rgb < 0) {  /* "No colour" removes it */
        memmove(&F->mcol[i], &F->mcol[i + 1], (F->nmcol - i - 1) * sizeof(F->mcol[0]));
        F->nmcol--;
    } else F->mcol[i] = rgb;
    color_build();
}

static void swatch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!F->multi) ui_color_pick(ICON_PALETTE "  Colour", F->color, color_single_set, NULL);
    else ui_color_pick(i >= F->nmcol ? ICON_PALETTE "  Add a colour" : ICON_PALETTE "  Colour (No colour removes it)",
                       i < F->nmcol ? F->mcol[i] : -1, color_multi_set, (void *)(intptr_t)i);
}

static lv_obj_t *swatch(lv_obj_t *p, int32_t rgb, int i)
{
    lv_obj_t *s = lv_obj_create(p);
    lv_obj_set_size(s, 56, 56);
    lv_obj_set_style_radius(s, 10, 0);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, COL_BORDER, 0);
    lv_obj_set_style_bg_color(s, rgb >= 0 ? lv_color_hex(rgb) : COL_CARD2, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, swatch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (rgb < 0) {
        lv_obj_t *l = ui_label(s, FONT_L, COL_MUTED, i >= 0 && F->multi ? ICON_PLUS : ICON_CLOSE);
        lv_obj_center(l);
    }
    return s;
}

static void direction_picked(int i, const char *text, void *user)
{
    strcpy(F->direction, i == 1 ? "longitudinal" : "coaxial");
    color_build();
}

static void direction_cb(lv_event_t *e)
{
    static const char *const D[] = { "Coextruded (colours side by side)", "Longitudinal (colours along the strand)" };
    ui_pick("Multi-colour direction", D, 2, !strcmp(F->direction, "longitudinal"), NULL, direction_picked, NULL);
}

static void color_build(void)
{
    lv_obj_clean(F->color_box);
    lv_obj_t *r = plain(F->color_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (!F->multi) {
        swatch(r, F->color, 0);
        char t[16] = "No colour";
        if (F->color >= 0) snprintf(t, sizeof(t), "#%06lX", (unsigned long)F->color);
        ui_label(r, FONT_M, F->color >= 0 ? COL_TEXT : COL_MUTED, t);
        return;
    }
    for (int i = 0; i < F->nmcol; i++) swatch(r, F->mcol[i], i);
    if (F->nmcol < NMULTI) swatch(r, -1, F->nmcol);
    lv_obj_t *d = plain(F->color_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(d, 4, 0);
    ui_label(d, FONT_S, COL_MUTED, "Direction");
    lv_obj_t *l = pick_button(d, direction_cb, NULL);
    lv_label_set_text(l, !strcmp(F->direction, "longitudinal") ? "Longitudinal" : "Coextruded");
}

static void color_tab_cb(lv_event_t *e)
{
    bool multi = lv_buttonmatrix_get_selected_button(F->color_tabs) == 1;
    if (multi == F->multi) return;
    F->multi = multi;
    if (multi && !F->nmcol && F->color >= 0) F->mcol[F->nmcol++] = F->color;
    if (!multi && F->color < 0 && F->nmcol) F->color = F->mcol[0];
    color_build();
}

static lv_obj_t *segmented(lv_obj_t *parent, const char *const *map, int sel, int w, lv_event_cb_t cb)
{
    lv_obj_t *m = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(m, map);
    lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
    lv_buttonmatrix_set_button_ctrl(m, sel, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(m, w, 60);
    lv_obj_set_style_text_font(m, FONT_M, 0);
    lv_obj_set_style_pad_all(m, 4, 0);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return m;
}

/* ── material field ──────────────────────────────────────────────────── */
static sm_material_t *s_mats;
static int s_nmats;

static void material_picked(int i, const char *text, void *user)
{
    lv_textarea_set_text(F->ta_material, text);
    /* known materials: fill in density and temperatures when still empty */
    for (int k = 0; i >= 0 && k < s_nmats; k++) {
        if (strcasecmp(s_mats[k].material, text)) continue;
        if (isnan(get_num(F->ta_density)) && s_mats[k].density > 0) set_num(F->ta_density, s_mats[k].density, 3);
        if (isnan(get_num(F->ta_ext)) && s_mats[k].ext_temp) set_num(F->ta_ext, s_mats[k].ext_temp, 0);
        if (isnan(get_num(F->ta_bed)) && s_mats[k].bed_temp) set_num(F->ta_bed, s_mats[k].bed_temp, 0);
        break;
    }
}

static void material_cb(lv_event_t *e)
{
    if (!s_mats) s_mats = heap_caps_malloc(64 * sizeof(sm_material_t), MALLOC_CAP_SPIRAM);
    s_nmats = s_mats ? spoolman_materials(s_mats, 64) : 0;
    const char **items = heap_caps_malloc((s_nmats + 1) * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!items) return;
    int sel = -1;
    for (int i = 0; i < s_nmats; i++) {
        items[i] = s_mats[i].material;
        if (!strcasecmp(items[i], lv_textarea_get_text(F->ta_material))) sel = i;
    }
    ui_pick("Material", items, s_nmats, sel, "Other material", material_picked, NULL);
    heap_caps_free(items);
}

/* text input with a "choose" button beside it */
static lv_obj_t *text_with_pick(const char *label, const char *help, int maxlen, lv_event_cb_t cb)
{
    lv_obj_t *b = field(label, false, help, false);
    lv_obj_t *r = plain(b, LV_FLEX_FLOW_ROW);
    lv_obj_t *ta = text_input(r, maxlen, false);
    lv_obj_set_width(ta, 1);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_t *bt = ui_button(r, LV_SYMBOL_DOWN, NULL, COL_CARD2, cb, NULL);
    lv_obj_set_style_min_height(bt, 50, 0);
    field_help(b);
    return ta;
}

/* ── spool fields ────────────────────────────────────────────────────── */
static void weight_recompute(void);

static void filament_show(void)
{
    spool_item_t f;
    char t[128] = "";
    if (F->filament_id >= 0 && spoolman_item(SM_FILAMENT, F->filament_id, &f)) {
        char w[24] = "";
        if (f.total_g > 0) {
            if (f.total_g >= 1000) snprintf(w, sizeof(w), ", %g kg", f.total_g / 1000);
            else snprintf(w, sizeof(w), ", %.0f g", f.total_g);
        }
        snprintf(t, sizeof(t), "%s%s%s (%s%s%.2f mm%s)", f.vendor, f.vendor[0] ? " - " : "", f.name[0] ? f.name : "(unnamed)",
                 f.material, f.material[0] ? ", " : "", f.diameter, w);
    }
    if (F->filament_lbl) lv_label_set_text(F->filament_lbl, t);
}

static int *s_fil_ids;
static void filament_picked(int i, const char *text, void *user)
{
    if (i < 0 || !s_fil_ids) return;
    F->filament_id = s_fil_ids[i];
    filament_show();
    weight_recompute();
}

static void filament_cb(lv_event_t *e)
{
    list_load(SM_FILAMENT);
    char **items = heap_caps_calloc(s_list_n ? s_list_n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    heap_caps_free(s_fil_ids);
    s_fil_ids = heap_caps_malloc((s_list_n ? s_list_n : 1) * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!items || !s_fil_ids) { heap_caps_free(items); return; }
    int sel = -1, n = 0;
    for (int i = 0; i < s_list_n; i++) {
        const spool_item_t *f = &s_list[i];
        items[n] = heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
        if (!items[n]) break;
        char w[24] = "";
        if (f->total_g > 0) {
            if (f->total_g >= 1000) snprintf(w, sizeof(w), ", %g kg", f->total_g / 1000);
            else snprintf(w, sizeof(w), ", %.0f g", f->total_g);
        }
        snprintf(items[n], 128, "%s%s%s (%s%s%.2f mm%s)", f->vendor, f->vendor[0] ? " - " : "",
                 f->name[0] ? f->name : "(unnamed)", f->material, f->material[0] ? ", " : "", f->diameter, w);
        s_fil_ids[n] = f->id;
        if (f->id == F->filament_id) sel = n;
        n++;
    }
    ui_pick(ICON_SPOOL "  Filament", (const char *const *)items, n, sel, NULL, filament_picked, NULL);
    for (int i = 0; i < n; i++) heap_caps_free(items[i]);
    heap_caps_free(items);
}

static void date_picked(time_t t, void *user)
{
    int which = (int)(intptr_t)user;
    if (which == 0) { F->first_used = t; date_label(F->first_lbl, t); }
    else { F->last_used = t; date_label(F->last_lbl, t); }
}

static void date_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    ui_date_pick(which == 0 ? "First used" : "Last used", which == 0 ? F->first_used : F->last_used, date_picked,
                 (void *)(intptr_t)which);
}

static lv_obj_t *date_field(const char *label, int which)
{
    lv_obj_t *b = field(label, false, NULL, false);
    lv_obj_t *l = pick_button(b, date_cb, (void *)(intptr_t)which);
    lv_obj_set_width(lv_obj_get_parent(l), 300);
    return l;
}

/* filament weight / empty spool weight as the spool would use them */
static void spool_basis(double *initial, double *sw)
{
    spool_item_t f;
    bool have = F->filament_id >= 0 && spoolman_item(SM_FILAMENT, F->filament_id, &f);
    *initial = get_num(F->ta_initial);
    if (isnan(*initial) && have && f.total_g > 0) *initial = f.total_g;
    *sw = get_num(F->ta_sweight);
    if (isnan(*sw) && have && f.spool_g >= 0) *sw = f.spool_g;
}

/* Used / remaining / measured: the chosen one is typed, the others follow. */
static void weight_recompute(void)
{
    if (!F->ta_used || F->recompute) return;
    F->recompute = true;
    double init, sw;
    spool_basis(&init, &sw);
    double used = NAN, rem = NAN, meas = NAN;
    if (F->wmode == 0) { used = get_num(F->ta_used); rem = init - used; meas = rem + sw; }
    else if (F->wmode == 1) { rem = get_num(F->ta_rem); used = init - rem; meas = rem + sw; }
    else { meas = get_num(F->ta_meas); rem = meas - sw; used = init - rem; }
    lv_obj_t *ta[3] = { F->ta_used, F->ta_rem, F->ta_meas };
    double v[3] = { used, rem, meas };
    for (int i = 0; i < 3; i++) {
        if (i == F->wmode) lv_obj_remove_state(ta[i], LV_STATE_DISABLED);
        else {
            lv_obj_add_state(ta[i], LV_STATE_DISABLED);
            set_num(ta[i], v[i], 1);
        }
    }
    /* measured weight needs to know the empty spool weight (as in Spoolman) */
    if (isnan(sw)) {
        lv_buttonmatrix_set_button_ctrl(F->wtabs, 2, LV_BUTTONMATRIX_CTRL_DISABLED);
        if (F->wmode == 2) {   /* the filament changed to one without a spool weight */
            F->wmode = 0;
            lv_buttonmatrix_set_button_ctrl(F->wtabs, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
            lv_obj_remove_state(F->ta_used, LV_STATE_DISABLED);
            lv_obj_add_state(F->ta_meas, LV_STATE_DISABLED);
        }
    }
    else lv_buttonmatrix_clear_button_ctrl(F->wtabs, 2, LV_BUTTONMATRIX_CTRL_DISABLED);
    F->recompute = false;
}

static void weight_changed(lv_event_t *e) { weight_recompute(); }

static void wtab_cb(lv_event_t *e)
{
    F->wmode = lv_buttonmatrix_get_selected_button(F->wtabs);
    weight_recompute();
}

static lv_obj_t *weight_input(const char *label, const char *help)
{
    lv_obj_t *b = field(label, false, help, false);
    lv_obj_t *ta = num_input(b, "g", false);
    lv_obj_add_event_cb(ta, weight_changed, LV_EVENT_VALUE_CHANGED, NULL);
    field_help(b);
    return ta;
}

static char (*s_locs)[64];
static void location_picked(int i, const char *text, void *user) { lv_textarea_set_text(F->ta_location, text); }

static void location_cb(lv_event_t *e)
{
    if (!s_locs) s_locs = heap_caps_malloc(32 * 64, MALLOC_CAP_SPIRAM);
    int n = s_locs ? spoolman_locations(s_locs, 32) : 0;
    const char *items[32];
    int sel = -1;
    for (int i = 0; i < n; i++) {
        items[i] = s_locs[i];
        if (!strcmp(s_locs[i], lv_textarea_get_text(F->ta_location))) sel = i;
    }
    ui_pick(ICON_HOME "  Location", items, n, sel, "New location", location_picked, NULL);
}

/* ── extra fields ────────────────────────────────────────────────────── */
static void xchoice_label(int i)
{
    sm_xfield_t *x = &F->xf[i];
    char t[200] = "";
    char buf[256];
    strlcpy(buf, x->choices, sizeof(buf));
    int k = 0;
    for (char *c = strtok(buf, "\n"); c; c = strtok(NULL, "\n"), k++) {
        bool on = x->multi ? (k < 16 && F->xsel[i][k]) : F->xchoice[i] == k;
        if (!on) continue;
        if (t[0]) strlcat(t, ", ", sizeof(t));
        strlcat(t, c, sizeof(t));
    }
    lv_label_set_text(F->xw[i], t);
}

static int choice_split(const char *choices, char *buf, size_t cap, const char **items, int max)
{
    strlcpy(buf, choices, cap);
    int n = 0;
    for (char *c = strtok(buf, "\n"); c && n < max; c = strtok(NULL, "\n")) items[n++] = c;
    return n;
}

static void xchoice_picked(int k, const char *text, void *user)
{
    int i = (int)(intptr_t)user;
    F->xchoice[i] = k;
    xchoice_label(i);
}
static void xmulti_done(void *user) { xchoice_label((int)(intptr_t)user); }

static void xchoice_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sm_xfield_t *x = &F->xf[i];
    char buf[256];
    const char *items[16];
    int n = choice_split(x->choices, buf, sizeof(buf), items, 16);
    if (x->multi) ui_pick_multi(x->name, items, n, F->xsel[i], xmulti_done, (void *)(intptr_t)i);
    else ui_pick(x->name, items, n, F->xchoice[i], NULL, xchoice_picked, (void *)(intptr_t)i);
}

static void xdate_picked(time_t t, void *user)
{
    int i = (int)(intptr_t)user;
    F->xdate[i] = t;
    date_label(F->xw[i], t);
}
static void xdate_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ui_date_pick(F->xf[i].name, F->xdate[i], xdate_picked, (void *)(intptr_t)i);
}

static void build_extras(void)
{
    F->nxf = spoolman_xfields(F->kind, F->xf, SM_XF_MAX);
    if (!F->nxf) return;
    lv_obj_t *h = plain(F->grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(h, col_w(true));
    ui_label(h, ui_font_bold(24), COL_TEXT, "Extra Fields");
    for (int i = 0; i < F->nxf; i++) {
        sm_xfield_t *x = &F->xf[i];
        F->xchoice[i] = -1;
        lv_obj_t *b = field(x->name, false, NULL, false);
        switch (x->type) {
        case XF_TEXT: F->xw[i] = text_input(b, 250, false); break;
        case XF_INT: case XF_FLOAT: F->xw[i] = num_input(b, x->unit, true); break;
        case XF_INT_RANGE: case XF_FLOAT_RANGE: {
            lv_obj_t *r = plain(b, LV_FLEX_FLOW_ROW_WRAP);
            F->xw[i] = num_input(r, NULL, true);
            ui_label(r, FONT_M, COL_MUTED, "to");
            F->xw2[i] = num_input(r, x->unit, true);
            lv_obj_set_width(F->xw[i], 150);
            lv_obj_set_width(F->xw2[i], 150);
            break;
        }
        case XF_DATETIME:
            F->xw[i] = pick_button(b, xdate_cb, (void *)(intptr_t)i);
            lv_obj_set_width(lv_obj_get_parent(F->xw[i]), 300);
            date_label(F->xw[i], 0);
            break;
        case XF_BOOL:
            F->xw[i] = lv_switch_create(b);
            lv_obj_set_size(F->xw[i], 80, 44);
            break;
        case XF_CHOICE: F->xw[i] = pick_button(b, xchoice_cb, (void *)(intptr_t)i); break;
        }
    }
}

/* Spoolman stores each extra value as a JSON text ("\"abc\"", "5", "[1, 2]", "true"). */
static void extras_fill(const cJSON *extra, bool defaults)
{
    for (int i = 0; i < F->nxf; i++) {
        sm_xfield_t *x = &F->xf[i];
        const char *raw = NULL;
        const cJSON *v = extra ? cJSON_GetObjectItem(extra, x->key) : NULL;
        if (cJSON_IsString(v)) raw = v->valuestring;
        else if (defaults && x->deflt[0]) raw = x->deflt;
        cJSON *j = raw ? cJSON_Parse(raw) : NULL;
        switch (x->type) {
        case XF_TEXT: set_text(F->xw[i], cJSON_GetStringValue(j)); break;
        case XF_INT: case XF_FLOAT:
            set_num(F->xw[i], cJSON_IsNumber(j) ? j->valuedouble : NAN, x->type == XF_INT ? 0 : 3);
            break;
        case XF_INT_RANGE: case XF_FLOAT_RANGE: {
            const cJSON *a = cJSON_GetArrayItem(j, 0), *b = cJSON_GetArrayItem(j, 1);
            int d = x->type == XF_INT_RANGE ? 0 : 3;
            set_num(F->xw[i], cJSON_IsNumber(a) ? a->valuedouble : NAN, d);
            set_num(F->xw2[i], cJSON_IsNumber(b) ? b->valuedouble : NAN, d);
            break;
        }
        case XF_DATETIME:
            F->xdate[i] = parse_iso(cJSON_GetStringValue(j));
            date_label(F->xw[i], F->xdate[i]);
            break;
        case XF_BOOL:
            if (cJSON_IsTrue(j)) lv_obj_add_state(F->xw[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(F->xw[i], LV_STATE_CHECKED);
            break;
        case XF_CHOICE: {
            char buf[256];
            const char *items[16];
            int n = choice_split(x->choices, buf, sizeof(buf), items, 16);
            memset(F->xsel[i], 0, sizeof(F->xsel[i]));
            F->xchoice[i] = -1;
            for (int k = 0; k < n; k++) {
                bool on = false;
                if (cJSON_IsString(j)) on = !strcmp(j->valuestring, items[k]);
                const cJSON *c, *arr = cJSON_IsArray(j) ? j : NULL;
                cJSON_ArrayForEach(c, arr) if (cJSON_IsString(c) && !strcmp(c->valuestring, items[k])) on = true;
                if (on) { F->xsel[i][k] = true; if (F->xchoice[i] < 0) F->xchoice[i] = k; }
            }
            xchoice_label(i);
            break;
        }
        }
        cJSON_Delete(j);
    }
}

static void extras_json(cJSON *obj)
{
    if (!F->nxf) return;
    cJSON *ex = cJSON_AddObjectToObject(obj, "extra");
    for (int i = 0; i < F->nxf; i++) {
        sm_xfield_t *x = &F->xf[i];
        cJSON *v = NULL;
        switch (x->type) {
        case XF_TEXT: {
            const char *t = lv_textarea_get_text(F->xw[i]);
            if (t[0]) v = cJSON_CreateString(t);
            break;
        }
        case XF_INT: case XF_FLOAT: {
            double d = get_num(F->xw[i]);
            if (!isnan(d)) v = cJSON_CreateNumber(x->type == XF_INT ? round(d) : d);
            break;
        }
        case XF_INT_RANGE: case XF_FLOAT_RANGE: {
            double a = get_num(F->xw[i]), b = get_num(F->xw2[i]);
            if (!isnan(a) || !isnan(b)) {
                v = cJSON_CreateArray();
                cJSON_AddItemToArray(v, isnan(a) ? cJSON_CreateNull() : cJSON_CreateNumber(x->type == XF_INT_RANGE ? round(a) : a));
                cJSON_AddItemToArray(v, isnan(b) ? cJSON_CreateNull() : cJSON_CreateNumber(x->type == XF_INT_RANGE ? round(b) : b));
            }
            break;
        }
        case XF_DATETIME:
            if (F->xdate[i]) { char t[32]; fmt_iso(F->xdate[i], t, sizeof(t)); v = cJSON_CreateString(t); }
            break;
        case XF_BOOL: v = cJSON_CreateBool(lv_obj_has_state(F->xw[i], LV_STATE_CHECKED)); break;
        case XF_CHOICE: {
            char buf[256];
            const char *items[16];
            int n = choice_split(x->choices, buf, sizeof(buf), items, 16);
            if (x->multi) {
                v = cJSON_CreateArray();
                for (int k = 0; k < n; k++) if (F->xsel[i][k]) cJSON_AddItemToArray(v, cJSON_CreateString(items[k]));
            } else if (F->xchoice[i] >= 0 && F->xchoice[i] < n) v = cJSON_CreateString(items[F->xchoice[i]]);
            break;
        }
        }
        if (!v) continue;   /* empty: left out (Spoolman then keeps no value / its default) */
        char *t = cJSON_PrintUnformatted(v);
        cJSON_Delete(v);
        if (t) { cJSON_AddStringToObject(ex, x->key, t); cJSON_free(t); }
    }
}

/* ── external database search (new filaments) ───────────────────────── */
static void ext_close(lv_event_t *e)
{
    if (F->ext_ov) ui_modal_close(F->ext_ov);
    F->ext_ov = NULL;
}

static void ext_apply(const sm_ext_t *x)
{
    set_text(F->ta_name, x->name);
    /* manufacturer: an existing one with that name, else created on save (as Spoolman's import does) */
    list_load(SM_VENDOR);
    F->vendor_id = -1;
    F->vendor_new[0] = 0;
    for (int i = 0; i < s_list_n; i++) if (!strcasecmp(s_list[i].name, x->manufacturer)) F->vendor_id = s_list[i].id;
    if (F->vendor_id < 0 && x->manufacturer[0]) strlcpy(F->vendor_new, x->manufacturer, sizeof(F->vendor_new));
    vendor_show();
    set_text(F->ta_material, x->material);
    set_num(F->ta_density, x->density > 0 ? x->density : NAN, 3);
    set_num(F->ta_diameter, x->diameter > 0 ? x->diameter : NAN, 2);
    set_num(F->ta_weight, x->weight > 0 ? x->weight : NAN, 1);
    set_num(F->ta_spoolw, x->spool_weight >= 0 ? x->spool_weight : NAN, 1);
    set_num(F->ta_ext, x->ext_temp ? x->ext_temp : NAN, 0);
    set_num(F->ta_bed, x->bed_temp ? x->bed_temp : NAN, 0);
    F->nmcol = 0;
    F->color = -1;
    if (x->color_hexes[0]) {
        char buf[64];
        strlcpy(buf, x->color_hexes, sizeof(buf));
        for (char *c = strtok(buf, ","); c && F->nmcol < NMULTI; c = strtok(NULL, ",")) F->mcol[F->nmcol++] = strtol(c, NULL, 16);
        F->multi = true;
        strlcpy(F->direction, x->direction[0] ? x->direction : "coaxial", sizeof(F->direction));
    } else {
        F->multi = false;
        if (x->color_hex[0]) F->color = strtol(x->color_hex, NULL, 16);
    }
    lv_buttonmatrix_set_button_ctrl(F->color_tabs, F->multi ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    color_build();
    strlcpy(F->external_id, x->ext_id, sizeof(F->external_id));
}

static void ext_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= F->ext_n) return;
    ext_apply(&F->ext_hits[i]);
    ext_close(NULL);
    ui_toast(ICON_CHECK "  Filled in from the database - check and save");
}

#define EXT_SHOW 40

static void ext_search(void)
{
    if (!F->ext_ov) return;
    if (spoolman_ext_state(NULL) != SM_OK) { F->ext_pending = true; return; }
    F->ext_pending = false;
    if (!F->ext_hits) F->ext_hits = heap_caps_malloc(EXT_SHOW * sizeof(sm_ext_t), MALLOC_CAP_SPIRAM);
    if (!F->ext_hits) return;
    int total = 0;
    F->ext_n = spoolman_ext_search(lv_textarea_get_text(F->ext_ta), F->ext_hits, EXT_SHOW, &total);
    lv_obj_clean(F->ext_list);
    for (int i = 0; i < F->ext_n; i++) {
        const sm_ext_t *x = &F->ext_hits[i];
        lv_obj_t *r = lv_obj_create(F->ext_list);
        ui_list_row(r, 70);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_bg_color(r, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_pad_hor(r, 12, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, ext_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *s = lv_obj_create(r);
        lv_obj_set_size(s, 40, 40);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s, 2, 0);
        lv_obj_set_style_border_color(s, COL_BORDER, 0);
        const char *hex = x->color_hex[0] ? x->color_hex : x->color_hexes;
        lv_obj_set_style_bg_color(s, hex[0] ? lv_color_hex(strtol(hex, NULL, 16)) : COL_CARD, 0);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        char t[128], sub[96];
        snprintf(t, sizeof(t), "%s  %s", x->manufacturer, x->name);
        snprintf(sub, sizeof(sub), "%s, %.2f mm%s%.0f g", x->material, x->diameter, x->weight > 0 ? ", " : "",
                 x->weight > 0 ? x->weight : 0);
        if (x->weight <= 0) sub[strlen(sub) - 2] = 0;   /* drop " 0 g" */
        ui_two_line(r, FONT_M, COL_TEXT, t, sub);
    }
    char st[80];
    if (!total) snprintf(st, sizeof(st), "No matches");
    else if (total > F->ext_n) snprintf(st, sizeof(st), "%d matches, showing %d - type more to narrow it down", total, F->ext_n);
    else snprintf(st, sizeof(st), "%d match%s", total, total == 1 ? "" : "es");
    lv_label_set_text(F->ext_status, st);
    lv_obj_scroll_to_y(F->ext_list, 0, LV_ANIM_OFF);
}

static void ext_search_cb(lv_event_t *e) { ui_keyboard_close(); ext_search(); }

static void ext_ov_deleted(lv_event_t *e) { if (F && F->ext_ov == lv_event_get_target(e)) F->ext_ov = NULL; }

static void ext_open_cb(lv_event_t *e)
{
    spoolman_ext_load();
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    F->ext_ov = ov;
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, ext_ov_deleted, LV_EVENT_DELETE, NULL);
    ui_modal_track(ov);
    lv_obj_t *b = ui_card(ov);
    lv_obj_set_size(b, ui_is_portrait() ? LV_PCT(94) : 900, ui_height() + 40 > 660 ? 660 : ui_height() + 40);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, 18, 0);
    lv_obj_set_style_pad_gap(b, 10, 0);
    lv_obj_t *hr = plain(b, LV_FLEX_FLOW_ROW);
    lv_obj_t *t = ui_label(hr, FONT_L, COL_TEXT, ICON_SEARCH "  Spoolman filament database");
    lv_obj_set_flex_grow(t, 1);
    ui_button(hr, ICON_CLOSE, "Close", COL_CARD2, ext_close, NULL);
    lv_obj_t *sr = plain(b, LV_FLEX_FLOW_ROW);
    F->ext_ta = lv_textarea_create(sr);
    lv_textarea_set_one_line(F->ext_ta, true);
    lv_textarea_set_max_length(F->ext_ta, 60);
    lv_textarea_set_placeholder_text(F->ext_ta, "e.g. esun petg black");
    lv_obj_set_width(F->ext_ta, 1);
    lv_obj_set_flex_grow(F->ext_ta, 1);
    lv_obj_set_style_text_font(F->ext_ta, FONT_L, 0);
    ui_attach_keyboard(F->ext_ta, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(F->ext_ta, ext_search_cb, LV_EVENT_READY, NULL);
    ui_button(sr, ICON_SEARCH, "Search", COL_ACCENT, ext_search_cb, NULL);
    F->ext_status = ui_label(b, FONT_S, COL_MUTED, "Downloading the database...");
    F->ext_list = lv_obj_create(b);
    lv_obj_set_width(F->ext_list, LV_PCT(100));
    lv_obj_set_flex_grow(F->ext_list, 1);
    lv_obj_set_style_bg_opa(F->ext_list, 0, 0);
    lv_obj_set_style_border_width(F->ext_list, 0, 0);
    lv_obj_set_style_pad_all(F->ext_list, 0, 0);
    lv_obj_set_style_pad_gap(F->ext_list, 8, 0);
    lv_obj_set_flex_flow(F->ext_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(F->ext_list, LV_DIR_VER);
    F->ext_pending = false;
    if (spoolman_ext_state(NULL) == SM_OK) lv_label_set_text(F->ext_status, "Type a manufacturer, name, material or colour.");
    lv_obj_send_event(F->ext_ta, LV_EVENT_FOCUSED, NULL);
}

/* ── building the forms ──────────────────────────────────────────────── */
static void save_cb(lv_event_t *e);
static void cancel_cb(lv_event_t *e) { ui_nav_back(); }

static void top_bar(lv_obj_t *parent, bool top)
{
    lv_obj_t *r = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(r, 12, 0);
    if (top && F->kind == SM_FILAMENT && F->mode != FM_EDIT)
        ui_button(r, ICON_SEARCH, ui_is_portrait() ? "Database" : "Fill in from Spoolman's database", COL_CARD2, ext_open_cb, NULL);
    if (top) {
        F->status = ui_label(r, FONT_S, COL_MUTED, "");
        lv_obj_set_flex_grow(F->status, 1);
    } else {
        lv_obj_t *sp = plain(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(sp, 1);
        lv_obj_set_flex_grow(sp, 1);
    }
    ui_button(r, ICON_CLOSE, "Cancel", COL_CARD2, cancel_cb, NULL);
    lv_obj_t *s = ui_button(r, ICON_SAVE, F->mode == FM_EDIT ? "Save" : "Create", COL_OK, save_cb, NULL);
    if (top) F->save_top = s;
    else F->save_bot = s;
}

static void build_vendor(void)
{
    F->ta_name = add_text("Name", true, NULL, 64, false);
    F->ta_empty = add_num("Empty Weight", false, "The weight of an empty spool from this manufacturer.", "g");
    lv_obj_t *b = field("Comment", false, NULL, true);
    F->ta_comment = text_input(b, 1024, true);
}

static void build_filament(void)
{
    F->ta_name = add_text("Name", false,
                          "Filament name, to distinguish this filament type among others from the same manufacturer. "
                          "Should contain the color for example.", 64, false);
    lv_obj_t *b = field("Manufacturer", false, NULL, false);
    F->vendor_lbl = pick_button(b, vendor_cb, NULL);

    b = field("Color", false, NULL, true);
    static const char *const CMAP[] = { "Single", "Multi", "" };
    F->color_tabs = segmented(b, CMAP, F->multi, 260, color_tab_cb);
    F->color_box = plain(b, LV_FLEX_FLOW_COLUMN);
    color_build();

    F->ta_material = text_with_pick("Material", "E.g. PLA, ABS, PETG, etc.", 64, material_cb);
    char cur[8], unit[12];
    spoolman_currency(cur, sizeof(cur));
    snprintf(unit, sizeof(unit), "%s", cur);
    F->ta_price = add_num("Price", false, "Price of a full spool.", unit);
    F->ta_density = add_num("Density", true, NULL, "g/cm³");
    F->ta_diameter = add_num("Diameter", true, NULL, "mm");
    F->ta_weight = add_num("Weight", false,
                           "The filament weight of a full spool (net weight). This should not include the weight of the "
                           "spool itself, only the filament. It is what is usually written on the packaging.", "g");
    F->ta_spoolw = add_num("Spool Weight", false, "The weight of an empty spool. Used to determine measured weight of a spool.", "g");
    F->ta_ext = add_num("Extruder Temp", false, NULL, "°C");
    F->ta_bed = add_num("Bed Temp", false, NULL, "°C");
    F->ta_article = add_text("Article Number", false, "E.g. EAN, UPC, etc.", 64, false);
    b = field("Comment", false, NULL, true);
    F->ta_comment = text_input(b, 1024, true);
}

static void build_spool(void)
{
    F->first_lbl = date_field("First Used", 0);
    F->last_lbl = date_field("Last Used", 1);
    date_label(F->first_lbl, 0);
    date_label(F->last_lbl, 0);
    lv_obj_t *b = field("Filament", true, NULL, true);
    F->filament_lbl = pick_button(b, filament_cb, NULL);

    char cur[8];
    spoolman_currency(cur, sizeof(cur));
    F->ta_price = add_num("Price", false, "The price of this spool. Leave empty to use the value from the filament.", cur);
    F->ta_initial = add_num("Initial Weight", false,
                            "The initial weight of the filament on the spool, excluding the spool itself. Leave empty to use "
                            "the value from the filament.", "g");
    F->ta_sweight = add_num("Empty Weight", false,
                            "The weight of the spool when it is empty. Leave empty to use the value from the filament or the "
                            "manufacturer.", "g");
    lv_obj_add_event_cb(F->ta_initial, weight_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(F->ta_sweight, weight_changed, LV_EVENT_VALUE_CHANGED, NULL);

    b = field("Weight", false,
              "Select what weight value to enter. Measured weight is only available if the spool weight is set for the "
              "selected filament.", true);
    static const char *const WMAP[] = { "Used Weight", "Remaining Weight", "Measured Weight", "" };
    F->wtabs = segmented(b, WMAP, F->wmode, ui_is_portrait() ? col_w(true) : 640, wtab_cb);
    field_help(b);
    F->ta_used = weight_input("Used Weight", "How much filament has been used from the spool. A new spool should have 0g used.");
    F->ta_rem = weight_input("Remaining Weight", "How much filament is left on the spool. For a new spool this should match the spool weight.");
    F->ta_meas = weight_input("Measured Weight", "How much the filament and spool weigh.");

    F->ta_location = text_with_pick("Location", "Where the spool is located if you have multiple locations where you store your spools.",
                                    64, location_cb);
    F->ta_lot = add_text("Lot Nr", false,
                         "Manufacturer's lot number. Can be used to ensure a print has consistent color if multiple spools are used.",
                         64, false);
    b = field("Comment", false, NULL, true);
    F->ta_comment = text_input(b, 1024, true);
    if (F->mode == FM_EDIT) {
        b = field("Archived", false, "Archived spools are hidden from the list unless you show them.", false);
        F->sw_archived = lv_switch_create(b);
        lv_obj_set_size(F->sw_archived, 80, 44);
        field_help(b);
    } else {
        F->ta_qty = add_num("Quantity", false, "How many identical spools to create.", NULL);
        lv_textarea_set_accepted_chars(F->ta_qty, "0123456789");
        lv_textarea_set_text(F->ta_qty, "1");
    }
}

/* ── filling from an existing item ───────────────────────────────────── */
static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}
static const char *jstr(const cJSON *o, const char *k) { return cJSON_GetStringValue(cJSON_GetObjectItem(o, k)); }

static void fill(const cJSON *o)
{
    const cJSON *extra = cJSON_GetObjectItem(o, "extra");
    if (F->kind == SM_VENDOR) {
        set_text(F->ta_name, jstr(o, "name"));
        set_num(F->ta_empty, jnum(o, "empty_spool_weight"), 1);
        set_text(F->ta_comment, jstr(o, "comment"));
    } else if (F->kind == SM_FILAMENT) {
        set_text(F->ta_name, jstr(o, "name"));
        const cJSON *v = cJSON_GetObjectItem(o, "vendor");
        F->vendor_id = v ? (int)jnum(v, "id") : -1;
        vendor_show();
        const char *hex = jstr(o, "color_hex"), *mh = jstr(o, "multi_color_hexes");
        F->color = hex && hex[0] ? strtol(hex[0] == '#' ? hex + 1 : hex, NULL, 16) : -1;
        F->nmcol = 0;
        if (mh && mh[0]) {
            char buf[96];
            strlcpy(buf, mh, sizeof(buf));
            for (char *c = strtok(buf, ","); c && F->nmcol < NMULTI; c = strtok(NULL, ","))
                F->mcol[F->nmcol++] = strtol(c[0] == '#' ? c + 1 : c, NULL, 16);
            F->multi = true;
        }
        strlcpy(F->direction, jstr(o, "multi_color_direction") ?: "coaxial", sizeof(F->direction));
        lv_buttonmatrix_set_button_ctrl(F->color_tabs, F->multi ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
        color_build();
        set_text(F->ta_material, jstr(o, "material"));
        set_num(F->ta_price, jnum(o, "price"), 2);
        set_num(F->ta_density, jnum(o, "density"), 3);
        set_num(F->ta_diameter, jnum(o, "diameter"), 2);
        set_num(F->ta_weight, jnum(o, "weight"), 1);
        set_num(F->ta_spoolw, jnum(o, "spool_weight"), 1);
        set_num(F->ta_ext, jnum(o, "settings_extruder_temp"), 0);
        set_num(F->ta_bed, jnum(o, "settings_bed_temp"), 0);
        set_text(F->ta_article, jstr(o, "article_number"));
        set_text(F->ta_comment, jstr(o, "comment"));
        strlcpy(F->external_id, jstr(o, "external_id") ?: "", sizeof(F->external_id));
    } else {
        F->first_used = parse_iso(jstr(o, "first_used"));
        F->last_used = parse_iso(jstr(o, "last_used"));
        date_label(F->first_lbl, F->first_used);
        date_label(F->last_lbl, F->last_used);
        const cJSON *f = cJSON_GetObjectItem(o, "filament");
        F->filament_id = f ? (int)jnum(f, "id") : -1;
        filament_show();
        set_num(F->ta_price, jnum(o, "price"), 2);
        set_num(F->ta_initial, jnum(o, "initial_weight"), 1);
        set_num(F->ta_sweight, jnum(o, "spool_weight"), 1);
        F->wmode = 0;
        lv_buttonmatrix_set_button_ctrl(F->wtabs, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
        set_num(F->ta_used, jnum(o, "used_weight"), 1);
        set_text(F->ta_location, jstr(o, "location"));
        set_text(F->ta_lot, jstr(o, "lot_nr"));
        set_text(F->ta_comment, jstr(o, "comment"));
        if (F->sw_archived) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(o, "archived"))) lv_obj_add_state(F->sw_archived, LV_STATE_CHECKED);
            else lv_obj_remove_state(F->sw_archived, LV_STATE_CHECKED);
        }
        weight_recompute();
    }
    extras_fill(extra, false);
}

static void fill_defaults(void)
{
    extras_fill(NULL, true);
    if (F->kind == SM_FILAMENT) set_num(F->ta_diameter, 1.75, 2);
    if (F->kind == SM_SPOOL) {
        set_num(F->ta_used, 0, 0);
        filament_show();
        weight_recompute();
    }
}

/* ── saving ──────────────────────────────────────────────────────────── */
static void j_str(cJSON *o, const char *k, lv_obj_t *ta, bool edit)
{
    const char *t = ta ? lv_textarea_get_text(ta) : "";
    if (t[0]) cJSON_AddStringToObject(o, k, t);
    else if (edit) cJSON_AddNullToObject(o, k);
}

static void j_num(cJSON *o, const char *k, lv_obj_t *ta, bool integer, bool edit)
{
    double v = get_num(ta);
    if (!isnan(v)) cJSON_AddNumberToObject(o, k, integer ? round(v) : v);
    else if (edit) cJSON_AddNullToObject(o, k);
}

static void j_date(cJSON *o, const char *k, time_t t, bool edit)
{
    if (t) { char s[32]; fmt_iso(t, s, sizeof(s)); cJSON_AddStringToObject(o, k, s); }
    else if (edit) cJSON_AddNullToObject(o, k);
}

static void set_busy(bool busy, const char *msg)
{
    lv_obj_t *b[2] = { F->save_top, F->save_bot };
    for (int i = 0; i < 2; i++) if (b[i]) {
        if (busy) lv_obj_add_state(b[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(b[i], LV_STATE_DISABLED);
    }
    if (F->status) lv_label_set_text(F->status, msg ? msg : "");
}

/* Builds the item's JSON. Returns NULL (with a toast) if a required field is missing. */
static char *item_json(void)
{
    bool edit = F->mode == FM_EDIT;
    cJSON *o = cJSON_CreateObject();
    if (F->kind == SM_VENDOR) {
        if (!lv_textarea_get_text(F->ta_name)[0]) { ui_toast(ICON_ALERT "  Enter a name"); cJSON_Delete(o); return NULL; }
        j_str(o, "name", F->ta_name, false);
        j_num(o, "empty_spool_weight", F->ta_empty, false, edit);
        j_str(o, "comment", F->ta_comment, edit);
    } else if (F->kind == SM_FILAMENT) {
        if (!(get_num(F->ta_density) > 0)) { ui_toast(ICON_ALERT "  Density is required"); cJSON_Delete(o); return NULL; }
        if (!(get_num(F->ta_diameter) > 0)) { ui_toast(ICON_ALERT "  Diameter is required"); cJSON_Delete(o); return NULL; }
        j_str(o, "name", F->ta_name, edit);
        if (F->vendor_id >= 0) cJSON_AddNumberToObject(o, "vendor_id", F->vendor_id);
        else if (edit) cJSON_AddNullToObject(o, "vendor_id");
        j_str(o, "material", F->ta_material, edit);
        j_num(o, "price", F->ta_price, false, edit);
        j_num(o, "density", F->ta_density, false, false);
        j_num(o, "diameter", F->ta_diameter, false, false);
        j_num(o, "weight", F->ta_weight, false, edit);
        j_num(o, "spool_weight", F->ta_spoolw, false, edit);
        j_num(o, "settings_extruder_temp", F->ta_ext, true, edit);
        j_num(o, "settings_bed_temp", F->ta_bed, true, edit);
        j_str(o, "article_number", F->ta_article, edit);
        j_str(o, "comment", F->ta_comment, edit);
        char hex[8];
        if (F->multi && F->nmcol) {
            char m[NMULTI * 7 + 1] = "";
            for (int i = 0; i < F->nmcol; i++) {
                snprintf(hex, sizeof(hex), "%06lx", (unsigned long)F->mcol[i]);
                if (i) strlcat(m, ",", sizeof(m));
                strlcat(m, hex, sizeof(m));
            }
            cJSON_AddStringToObject(o, "multi_color_hexes", m);
            cJSON_AddStringToObject(o, "multi_color_direction", F->direction[0] ? F->direction : "coaxial");
            if (edit) cJSON_AddNullToObject(o, "color_hex");
        } else {
            if (F->color >= 0) { snprintf(hex, sizeof(hex), "%06lx", (unsigned long)F->color); cJSON_AddStringToObject(o, "color_hex", hex); }
            else if (edit) cJSON_AddNullToObject(o, "color_hex");
            if (edit) { cJSON_AddNullToObject(o, "multi_color_hexes"); cJSON_AddNullToObject(o, "multi_color_direction"); }
        }
        if (F->external_id[0]) cJSON_AddStringToObject(o, "external_id", F->external_id);
    } else {
        if (F->filament_id < 0) { ui_toast(ICON_ALERT "  Choose a filament"); cJSON_Delete(o); return NULL; }
        j_date(o, "first_used", F->first_used, edit);
        j_date(o, "last_used", F->last_used, edit);
        cJSON_AddNumberToObject(o, "filament_id", F->filament_id);
        j_num(o, "price", F->ta_price, false, edit);
        j_num(o, "initial_weight", F->ta_initial, false, edit);
        j_num(o, "spool_weight", F->ta_sweight, false, edit);
        double init, sw;
        spool_basis(&init, &sw);
        if (F->wmode == 0) {
            double u = get_num(F->ta_used);
            cJSON_AddNumberToObject(o, "used_weight", isnan(u) ? 0 : u);
        } else {
            double r = F->wmode == 1 ? get_num(F->ta_rem) : get_num(F->ta_meas) - sw;
            if (isnan(r)) { ui_toast(ICON_ALERT "  Enter the weight"); cJSON_Delete(o); return NULL; }
            if (r < 0) r = 0;
            cJSON_AddNumberToObject(o, "remaining_weight", r);
        }
        j_str(o, "location", F->ta_location, edit);
        j_str(o, "lot_nr", F->ta_lot, edit);
        j_str(o, "comment", F->ta_comment, edit);
        if (F->sw_archived) cJSON_AddBoolToObject(o, "archived", lv_obj_has_state(F->sw_archived, LV_STATE_CHECKED));
    }
    extras_json(o);
    char *t = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return t;
}

static void start_item_save(void)
{
    char *body = item_json();
    if (!body) { set_busy(false, NULL); F->step = 0; return; }
    char path[48];
    int repeat = 1;
    if (F->mode == FM_EDIT) snprintf(path, sizeof(path), "%s/%d", KIND_PATH[F->kind], F->id);
    else {
        strlcpy(path, KIND_PATH[F->kind], sizeof(path));
        if (F->ta_qty) {
            double q = get_num(F->ta_qty);
            repeat = isnan(q) || q < 1 ? 1 : q > 50 ? 50 : (int)q;
        }
    }
    bool ok = spoolman_job(F->mode == FM_EDIT ? "PATCH" : "POST", path, body, repeat);
    cJSON_free(body);
    if (!ok) { set_busy(false, "Busy - try again"); F->step = 0; return; }
    F->step = 2;
    set_busy(true, "Saving...");
}

static void save_cb(lv_event_t *e)
{
    if (F->step || F->loading) return;
    ui_keyboard_close();
    if (F->kind == SM_FILAMENT && F->vendor_new[0]) {
        /* create the new manufacturer first */
        cJSON *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "name", F->vendor_new);
        char *body = cJSON_PrintUnformatted(v);
        cJSON_Delete(v);
        if (!body) return;
        /* check the filament fields before creating anything */
        char *probe = item_json();
        if (!probe) { cJSON_free(body); return; }
        cJSON_free(probe);
        bool ok = spoolman_job("POST", "/v1/vendor", body, 1);
        cJSON_free(body);
        if (!ok) { ui_toast("Busy - try again"); return; }
        F->step = 1;
        set_busy(true, "Creating the manufacturer...");
        return;
    }
    start_item_save();
}

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int arg)
{
    F = heap_caps_calloc(1, sizeof(form_t), MALLOC_CAP_SPIRAM);
    if (!F) { ui_label(parent, FONT_M, COL_ERR, "Out of memory"); return; }
    F->kind = arg & 3;
    F->mode = (arg >> 2) & 3;
    F->id = arg >> 4;
    if (F->kind >= SM_KINDS) F->kind = SM_SPOOL;
    F->vendor_id = F->filament_id = -1;
    F->color = -1;
    strcpy(F->direction, "coaxial");
    if (F->mode == FM_SPOOL_OF) { F->filament_id = F->id; F->kind = SM_SPOOL; }

    static const char *const NAMES[SM_KINDS] = { "spool", "filament", "manufacturer" };
    char t[64];
    snprintf(t, sizeof(t), "New %s", NAMES[F->kind]);
    if (F->mode == FM_EDIT) snprintf(t, sizeof(t), "Edit %s #%d", NAMES[F->kind], F->id);
    ui_set_title(t);

    lv_obj_set_style_pad_gap(parent, 14, 0);
    top_bar(parent, true);
    F->grid = plain(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(F->grid, 16, 0);
    lv_obj_set_style_pad_column(F->grid, 24, 0);
    if (F->kind == SM_VENDOR) build_vendor();
    else if (F->kind == SM_FILAMENT) build_filament();
    else build_spool();
    build_extras();
    top_bar(parent, false);

    if (F->mode == FM_EDIT || F->mode == FM_COPY) {
        char path[48];
        snprintf(path, sizeof(path), "%s/%d", KIND_PATH[F->kind], F->id);
        spoolman_job_clear();
        if (spoolman_job("GET", path, NULL, 1)) {
            F->loading = true;
            set_busy(true, "Loading...");
        } else set_busy(false, ICON_ALERT "  Couldn't load the item");
    } else fill_defaults();
}

static void refresh(void)
{
    if (!F) return;
    if (F->fresh && !lv_obj_has_state(F->fresh, LV_STATE_FOCUSED)) F->fresh = NULL;
    if (F->ext_ov) {
        int kb;
        sm_state_t es = spoolman_ext_state(&kb);
        if (es == SM_LOADING) {
            char st[64];
            snprintf(st, sizeof(st), "Downloading the database...  %d KB", kb);
            lv_label_set_text(F->ext_status, st);
        } else if (es == SM_ERROR) lv_label_set_text(F->ext_status, ICON_ALERT "  Couldn't download Spoolman's filament database");
        else if (F->ext_pending) ext_search();
        else if (!strncmp(lv_label_get_text(F->ext_status), "Downloading", 11))
            lv_label_set_text(F->ext_status, "Type a manufacturer, name, material or colour.");
    }
    char msg[96];
    int id;
    sm_state_t js = spoolman_job_state(msg, sizeof(msg), &id);
    if (js == SM_LOADING) return;
    if (F->loading) {
        F->loading = false;
        char *resp = js == SM_OK ? spoolman_job_response() : NULL;
        cJSON *o = resp ? cJSON_Parse(resp) : NULL;
        free(resp);
        if (o) {
            fill(o);
            cJSON_Delete(o);
            set_busy(false, F->mode == FM_COPY ? "Copy - change what you need and create" : NULL);
        } else set_busy(true, ICON_ALERT "  Couldn't load the item");
        spoolman_job_clear();
        return;
    }
    if (F->step == 1) {
        if (js == SM_OK && id >= 0) {
            F->vendor_id = id;
            F->vendor_new[0] = 0;
            spoolman_job_clear();
            start_item_save();
        } else {
            F->step = 0;
            set_busy(false, NULL);
            char t[128];
            snprintf(t, sizeof(t), ICON_ALERT "  %s", msg[0] ? msg : "Couldn't create the manufacturer");
            ui_toast(t);
            spoolman_job_clear();
        }
        return;
    }
    if (F->step == 2) {
        F->step = 0;
        if (js == SM_OK) {
            static const char *const N[SM_KINDS] = { "Spool", "Filament", "Manufacturer" };
            char t[80];
            snprintf(t, sizeof(t), ICON_CHECK "  %s #%d %s", N[F->kind], F->mode == FM_EDIT ? F->id : id,
                     F->mode == FM_EDIT ? "saved" : "created");
            spoolman_set_focus(F->kind, F->mode == FM_EDIT ? F->id : id);
            spoolman_job_clear();
            ui_toast(t);
            ui_nav_back();
            return;
        }
        set_busy(false, NULL);
        char t[128];
        snprintf(t, sizeof(t), ICON_ALERT "  %s", msg[0] ? msg : "Couldn't save");
        ui_toast(t);
        spoolman_job_clear();
    }
}

static void destroy(void)
{
    int k, i;
    if (F && !spoolman_take_focus(&k, &i)) {
        /* closed without saving (Cancel / back): the list returns to the same tab and item */
        k = F->mode == FM_SPOOL_OF ? SM_FILAMENT : F->kind;
        i = F->mode == FM_NEW ? -1 : F->id;
    }
    if (F) spoolman_set_focus(k, i);
    if (F && F->ext_ov) { lv_obj_delete(F->ext_ov); }
    if (F) heap_caps_free(F->ext_hits);
    heap_caps_free(F);
    F = NULL;
    heap_caps_free(s_list);
    s_list = NULL;
    heap_caps_free(s_mats);
    s_mats = NULL;
    heap_caps_free(s_locs);
    s_locs = NULL;
    heap_caps_free(s_fil_ids);
    s_fil_ids = NULL;
    spoolman_free();   /* the list page loads everything again */
}

const ui_page_t page_spool_form = { "Spoolman", create, refresh, destroy };
