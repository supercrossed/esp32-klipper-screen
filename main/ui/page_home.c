/*
 * Home: one tile per printer, readable from across a desk.
 *
 * The grid adapts to the number of printers. In landscape, 1-4 printers use
 * two columns and 5 or more use three; in portrait, 1-2 printers use one
 * column and more use two. The tiles share the height that's left, and the
 * text size follows the tile size (three presets below). "Add printer" is a
 * tile only while there are no printers, then a button in the bottom bar.
 *
 * Every tile uses the same template so they line up regardless of state:
 *   name ............ [status]
 *   host / hostname ... [mmu][spool][cam]
 *   [thumb or placeholder]  file / progress / time
 *   hotend | bed | chamber  (three equal columns, "--" when absent)
 */
#include "ui.h"
#include "icons.h"
#include "ui_thumb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "printers.h"
#include "wifi_mgr.h"

#define GAP       16
#define BOTTOM_H  56

enum { BADGE_MMU = 0, BADGE_SPOOL, BADGE_CAM, BADGE_N };

/* text sizes for one tile size */
typedef struct {
    int name, chip, file, prog, temp;              /* Inter Bold px */
    const lv_font_t **host, **sub, **tgt;           /* regular */
    int thumb, bar, pill_w, pill_h, gap;
    bool sub_line;                                  /* "Finishes at 14:32" under the progress */
    bool temp_inline;                               /* "215° /215°" on one line, else target below */
} preset_t;

static const preset_t P_BIG    = { 44, 24, 32, 28, 40, &g_font_24, &g_font_24, &g_font_24, 220, 18, 46, 38, 10, true,  false };
static const preset_t P_WIDE   = { 30, 20, 24, 22, 28, &g_font_20, &g_font_20, &g_font_20, 104, 12, 38, 30,  5, true,  true  };
static const preset_t P_NARROW = { 26, 18, 22, 20, 24, &g_font_16, &g_font_16, &g_font_16,  80, 10, 34, 28,  6, false, false };

typedef struct {
    lv_obj_t *card, *name, *chip, *chip_lbl, *host;
    lv_obj_t *badge[BADGE_N];
    lv_obj_t *thumb, *placeholder, *file, *bar, *prog, *sub;
    lv_obj_t *temp[3], *tgt[3];
    ui_thumb_t th;
    uint32_t seq;
    int status;
} tile_t;

static tile_t s_tiles[MAX_PRINTERS];
static int s_ntiles;
static lv_obj_t *s_grid, *s_empty, *s_spool_btn, *s_add_btn;
static printer_state_t s_st;   /* scratch copy (static: keeps it off the LVGL task stack) */
static int s_tick;
static bool s_long;            /* a long-press also produces CLICKED on release: swallow it */
static const preset_t *s_p = &P_WIDE;
static int s_tile_w, s_tile_h;

/* ── events ──────────────────────────────────────────────────────────── */
static void tile_pressed(lv_event_t *e) { s_long = false; }   /* each touch starts fresh */

static void tile_click(lv_event_t *e)
{
    if (s_long) { s_long = false; return; }
    ui_nav_push(PAGE_PRINTER, (int)(intptr_t)lv_event_get_user_data(e));
}

static void rename_done(const char *text, void *user)
{
    int idx = (int)(intptr_t)user;
    printer_cfg_t c;
    if (!printers_get_cfg(idx, &c)) return;
    strlcpy(c.name, text, sizeof(c.name));
    printers_update(idx, &c);
    if (idx < s_ntiles) ui_set_text(s_tiles[idx].name, c.name);
}

static void tile_long(lv_event_t *e)
{
    s_long = true;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    printer_cfg_t c;
    if (printers_get_cfg(idx, &c)) ui_prompt(ICON_EDIT "  Nickname for this printer", c.name, rename_done, (void *)(intptr_t)idx);
}

static void add_click(lv_event_t *e) { ui_nav_push(PAGE_ADD_PRINTER, -1); }

static void spool_click(lv_event_t *e)
{
    for (int i = 0; i < printers_count(); i++)
        if (printers_get_state(i, &s_st) && s_st.has_spoolman) { ui_nav_push(PAGE_SPOOLMAN, i); return; }
}

/* ── layout ──────────────────────────────────────────────────────────── */
static void compute_layout(int n)
{
    bool portrait = ui_is_portrait();
    int cols = portrait ? (n <= 2 ? 1 : 2) : (n <= 4 ? 2 : 3);
    int rows = n < 1 ? 1 : (n + cols - 1) / cols;
    int avail = ui_height() - 2 * 16 - BOTTOM_H - GAP;   /* content padding, bottom bar */
    s_tile_w = (ui_width() - 32 - (cols - 1) * GAP) / cols;
    s_tile_h = (avail - (rows - 1) * GAP) / rows;
    if (s_tile_h > 600) s_tile_h = 600;
    if (s_tile_w >= 560 && s_tile_h >= 420) s_p = &P_BIG;
    else if (s_tile_w >= 560) s_p = &P_WIDE;
    else s_p = &P_NARROW;
    int min_h = 258;
    if (s_tile_h < min_h) s_tile_h = min_h;   /* more rows than fit: the page scrolls */
    if (rows > 2 && s_tile_h > 300) s_tile_h = 300;
    lv_obj_set_flex_align(s_grid, n < cols ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

/* ── building blocks ─────────────────────────────────────────────────── */
static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(r, 0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_gap(r, 10, 0);
    lv_obj_set_flex_flow(r, flow);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *icon)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, s_p->pill_w, s_p->pill_h);
    lv_obj_set_style_radius(p, 8, 0);
    lv_obj_set_style_bg_color(p, COL_CARD2, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *l = ui_label(p, *s_p->host, COL_MUTED, icon);
    lv_obj_center(l);
    return p;
}

static lv_obj_t *dot_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = ui_label(parent, f, c, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

static void build_tile(int i)
{
    const preset_t *p = s_p;
    tile_t *t = &s_tiles[i];
    memset(t, 0, sizeof(*t));
    t->status = -1;
    t->card = ui_card(s_grid);
    lv_obj_set_size(t->card, s_tile_w, s_tile_h);
    lv_obj_set_flex_flow(t->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(t->card, p == &P_BIG ? 22 : 16, 0);
    lv_obj_set_style_pad_gap(t->card, p->gap, 0);
    lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t->card, COL_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(t->card, tile_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(t->card, tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(t->card, tile_pressed, LV_EVENT_PRESSED, NULL);

    /* 1: name + status */
    lv_obj_t *r1 = box(t->card, LV_FLEX_FLOW_ROW);
    t->name = ui_label(r1, ui_font_bold(p->name), COL_TEXT, "");
    lv_label_set_long_mode(t->name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t->name, 1);
    t->chip = lv_obj_create(r1);
    lv_obj_set_size(t->chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(t->chip, 40, 0);
    lv_obj_set_style_border_width(t->chip, 0, 0);
    lv_obj_set_style_pad_hor(t->chip, p->chip / 2 + 4, 0);
    lv_obj_set_style_pad_ver(t->chip, p->chip / 5, 0);
    lv_obj_remove_flag(t->chip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t->chip, LV_OBJ_FLAG_EVENT_BUBBLE);
    t->chip_lbl = ui_label(t->chip, ui_font_bold(p->chip), lv_color_black(), "");

    /* 2: host + feature pills */
    lv_obj_t *r2 = box(t->card, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(r2, 6, 0);
    t->host = ui_label(r2, *p->host, COL_MUTED, "");
    lv_label_set_long_mode(t->host, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t->host, 1);
    t->badge[BADGE_MMU] = pill(r2, ICON_MMU);
    t->badge[BADGE_SPOOL] = pill(r2, ICON_SPOOL);
    t->badge[BADGE_CAM] = pill(r2, ICON_WEBCAM);

    /* 3: media + job */
    lv_obj_t *mid = box(t->card, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(mid, p == &P_BIG ? 22 : 14, 0);
    lv_obj_set_style_margin_top(mid, 4, 0);
    lv_obj_t *media = lv_obj_create(mid);
    lv_obj_set_size(media, p->thumb, p->thumb);
    lv_obj_set_style_radius(media, 12, 0);
    lv_obj_set_style_bg_color(media, COL_CARD2, 0);
    lv_obj_set_style_border_width(media, 0, 0);
    lv_obj_set_style_pad_all(media, 0, 0);
    lv_obj_set_style_clip_corner(media, true, 0);
    lv_obj_remove_flag(media, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(media, LV_OBJ_FLAG_EVENT_BUBBLE);
    t->placeholder = ui_label(media, p == &P_BIG ? ui_font_bold(96) : FONT_XXL, COL_BORDER, ICON_PRINTER);
    lv_obj_center(t->placeholder);
    t->thumb = lv_image_create(media);
    lv_obj_center(t->thumb);
    lv_obj_add_flag(t->thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(t->thumb, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *col = box(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_gap(col, p == &P_BIG ? 12 : 6, 0);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    t->file = dot_label(col, ui_font_bold(p->file), COL_TEXT);
    t->bar = lv_bar_create(col);
    lv_obj_set_size(t->bar, LV_PCT(100), p->bar);
    lv_bar_set_range(t->bar, 0, 1000);
    lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t->bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    t->prog = dot_label(col, ui_font_bold(p->prog), COL_TEXT);
    if (p->sub_line) t->sub = dot_label(col, *p->sub, COL_MUTED);

    /* 4: temperatures, three equal columns at the bottom */
    lv_obj_t *r4 = box(t->card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(r4, 1);
    lv_obj_set_style_pad_gap(r4, 0, 0);
    lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_border_side(r4, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r4, 1, 0);
    lv_obj_set_style_border_color(r4, COL_BORDER, 0);
    lv_obj_set_style_pad_top(r4, 10, 0);
    for (int k = 0; k < 3; k++) {
        lv_obj_t *cell = box(r4, p->temp_inline ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(cell, 1);
        lv_obj_set_width(cell, 1);
        lv_obj_set_style_pad_gap(cell, p->temp_inline ? 4 : 0, 0);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, p->temp_inline ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        t->temp[k] = ui_label(cell, ui_font_bold(p->temp), COL_TEXT, "");
        lv_label_set_long_mode(t->temp[k], LV_LABEL_LONG_CLIP);
        t->tgt[k] = ui_label(cell, *p->tgt, COL_MUTED, "");
        if (p->temp_inline) lv_obj_set_style_pad_bottom(t->tgt[k], p->temp / 10, 0);   /* sit on the big text's baseline */
    }

    printer_cfg_t cfg;
    if (printers_get_cfg(i, &cfg)) {
        ui_set_text(t->name, cfg.name);
        ui_set_text(t->host, cfg.host);
    }
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_sub(tile_t *t, const char *txt) { if (t->sub) ui_set_text(t->sub, txt); }

static void update_tile(int i)
{
    tile_t *t = &s_tiles[i];
    if (!printers_get_state(i, &s_st)) return;
    bool has_thumb = ui_thumb_update(&t->th, t->thumb, i, s_p->thumb);
    show(t->placeholder, !has_thumb);
    if (s_st.seq == t->seq && (int)s_st.status == t->status) return;   /* nothing new */
    t->seq = s_st.seq;

    if ((int)s_st.status != t->status) {
        t->status = s_st.status;
        lv_obj_set_style_bg_color(t->chip, ui_status_color(s_st.status), 0);
        lv_obj_set_style_text_color(t->chip_lbl, ui_contrast(ui_status_color(s_st.status)), 0);
        ui_set_text(t->chip_lbl, printer_status_str(s_st.status));
        lv_obj_set_style_opa(t->card, s_st.status == PS_OFFLINE ? LV_OPA_60 : LV_OPA_COVER, 0);
    }

    char b[160];
    printer_cfg_t cfg;
    if (printers_get_cfg(i, &cfg)) {
        if (s_st.hostname[0] && strcasecmp(s_st.hostname, cfg.host)) snprintf(b, sizeof(b), "%s  -  %s", s_st.hostname, cfg.host);
        else snprintf(b, sizeof(b), "%s", cfg.host);
        ui_set_text(t->host, b);
    }
    show(t->badge[BADGE_MMU], s_st.mmu.type != MMU_NONE);
    show(t->badge[BADGE_SPOOL], s_st.has_spoolman);
    show(t->badge[BADGE_CAM], s_st.nwebcams > 0);

    bool job = s_st.status == PS_PRINTING || s_st.status == PS_PAUSED;
    bool down = s_st.status == PS_OFFLINE || s_st.status == PS_KLIPPY_DOWN;
    lv_color_t prog_col = COL_TEXT;
    if (down) {
        ui_set_text(t->file, s_st.status == PS_OFFLINE ? "Not reachable" : "Klipper not ready");
        show(t->bar, false);
        ui_set_text(t->prog, s_st.message);
        prog_col = COL_MUTED;
        set_sub(t, "");
    } else if (s_st.filename[0] && (job || s_st.status == PS_COMPLETE)) {
        const char *fn = strrchr(s_st.filename, '/');
        ui_set_text(t->file, fn ? fn + 1 : s_st.filename);
        show(t->bar, true);
        lv_bar_set_value(t->bar, (int)(s_st.progress * 1000), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(t->bar, ui_status_color(s_st.status), LV_PART_INDICATOR);
        if (job) {
            char left[24], eta[32] = "";
            ui_fmt_duration(left, sizeof(left), s_st.eta_s);
            if (s_st.eta_s >= 0 && wifi_mgr_time_valid()) {
                time_t end = time(NULL) + (time_t)s_st.eta_s;
                struct tm tm;
                localtime_r(&end, &tm);
                strftime(eta, sizeof(eta), "Finishes at %H:%M", &tm);
            }
            snprintf(b, sizeof(b), "%d%%  -  %s left", (int)(s_st.progress * 100), left);
            set_sub(t, eta);
        } else {
            char d[24];
            ui_fmt_duration(d, sizeof(d), s_st.print_duration);
            snprintf(b, sizeof(b), "Finished in %s", d);
            set_sub(t, "");
        }
        ui_set_text(t->prog, b);
    } else {
        ui_set_text(t->file, "Ready");
        show(t->bar, false);
        ui_set_text(t->prog, "Tap to open");
        prog_col = COL_MUTED;
        set_sub(t, "");
    }
    lv_obj_set_style_text_color(t->prog, prog_col, 0);

    /* temperatures: hotend / bed / chamber, always three columns */
    const heater_t *slot[3] = { NULL, NULL, NULL };
    for (int h = 0; h < s_st.nheaters; h++) {
        const heater_t *ht = &s_st.heaters[h];
        if (ht->kind == 0 && !strcmp(ht->obj, "extruder") && !slot[0]) slot[0] = ht;
        else if (ht->kind == 1 && !slot[1]) slot[1] = ht;
        else if (ht->kind == 2 && (!slot[2] || (ht->settable && !slot[2]->settable))) slot[2] = ht;
    }
    static const char *ic[3] = { ICON_HOTEND, ICON_BED, ICON_CHAMBER };
    for (int k = 0; k < 3; k++) {
        const heater_t *ht = slot[k];
        bool heating = ht && !down && ht->settable && ht->target > 0;
        if (!ht || down) snprintf(b, sizeof(b), "%s --", ic[k]);
        else snprintf(b, sizeof(b), "%s %.0f°", ic[k], ht->temp);
        ui_set_text(t->temp[k], b);
        lv_obj_set_style_text_color(t->temp[k], !ht || down ? COL_MUTED : heating ? COL_HOT : COL_TEXT, 0);
        if (heating) snprintf(b, sizeof(b), "/%.0f°", ht->target);
        else b[0] = 0;
        ui_set_text(t->tgt[k], b);
    }
}

static void build(void)
{
    lv_obj_clean(s_grid);
    for (int i = 0; i < s_ntiles; i++) ui_thumb_free(&s_tiles[i].th);
    s_ntiles = printers_count();
    compute_layout(s_ntiles);
    for (int i = 0; i < s_ntiles; i++) build_tile(i);

    if (s_ntiles == 0) {   /* first run: a big "Add printer" tile */
        lv_obj_t *add = ui_card(s_grid);
        lv_obj_set_size(add, s_tile_w, s_tile_h);
        lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(add, COL_ACCENT, 0);
        lv_obj_set_style_border_opa(add, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(add, COL_CARD2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(add, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(add, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(add, add_click, LV_EVENT_CLICKED, NULL);
        ui_label(add, FONT_XXL, COL_ACCENT, ICON_PLUS);
        ui_label(add, ui_font_bold(28), COL_ACCENT, "Add printer");
    }
    show(s_empty, s_ntiles == 0);
    show(s_add_btn, s_ntiles > 0);
    for (int i = 0; i < s_ntiles; i++) update_tile(i);
}

static void update_spool_btn(void)
{
    bool any = false;
    for (int i = 0; i < printers_count() && !any; i++)
        if (printers_get_state(i, &s_st) && s_st.has_spoolman) any = true;
    show(s_spool_btn, any);
}

static lv_obj_t *bar_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(parent, icon, txt, COL_CARD, cb, NULL);
    lv_obj_set_style_min_height(b, BOTTOM_H, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int arg)
{
    printers_set_focus(-1);   /* nothing in the foreground: all printers poll at the slow rate */
    s_long = false;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(parent, GAP, 0);
    s_empty = ui_label(parent, FONT_L, COL_MUTED,
                       "No printers yet. Tap \"Add printer\" to search your network or enter an address.");
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, LV_PCT(100));

    s_grid = lv_obj_create(parent);
    lv_obj_set_size(s_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_grid, 0, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_style_pad_gap(s_grid, GAP, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bottom = lv_obj_create(parent);
    lv_obj_set_size(bottom, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bottom, 0, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_set_style_pad_gap(bottom, 12, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hint = ui_label(bottom, FONT_S, COL_MUTED, ICON_INFO "  Long-press a printer to give it a nickname.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(hint, 1);
    s_spool_btn = bar_button(bottom, ICON_SPOOL, "Spoolman", spool_click);
    s_add_btn = bar_button(bottom, ICON_PLUS, "Add printer", add_click);

    s_ntiles = 0;
    build();
    update_spool_btn();
}

static void refresh(void)
{
    if (++s_tick % 2) return;   /* 2 Hz is plenty for tiles */
    if (printers_count() != s_ntiles) { build(); return; }
    for (int i = 0; i < s_ntiles; i++) update_tile(i);
    if (s_tick % 8 == 0) update_spool_btn();
}

static void destroy(void)
{
    for (int i = 0; i < s_ntiles; i++) ui_thumb_free(&s_tiles[i].th);
    s_ntiles = 0;
    s_grid = s_empty = s_spool_btn = s_add_btn = NULL;
}

const ui_page_t page_home = { "Printers", create, refresh, destroy };
