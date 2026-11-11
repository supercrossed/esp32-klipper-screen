/*
 * Modal pickers for forms: a list (search box when long, optional "new..."
 * row, single or multi select), a colour picker and a date picker.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"

static lv_obj_t *overlay(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_60, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    ui_modal_track(o);   /* closed by a page change */
    return o;
}

static lv_obj_t *box(lv_obj_t *ov, int w, int h)
{
    lv_obj_t *b = ui_card(ov);
    lv_obj_set_size(b, ui_is_portrait() ? LV_PCT(94) : w, h);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, 18, 0);
    lv_obj_set_style_pad_gap(b, 12, 0);
    return b;
}

static lv_obj_t *row(lv_obj_t *p)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(r, 0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_gap(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* ── list picker ─────────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *ov, *list, *search;
    ui_pick_cb_t cb;
    ui_pick_done_cb_t done;
    void *user;
    const char *new_label;
    char new_title[64];
    int n;
    bool multi;
    bool *sel;          /* multi: caller's array, updated in place */
    char **items;
} pick_t;

static void pick_free(lv_event_t *e)
{
    pick_t *p = lv_event_get_user_data(e);
    for (int i = 0; i < p->n; i++) heap_caps_free(p->items[i]);
    heap_caps_free(p->items);
    lv_free(p);
}

static void pick_close(lv_event_t *e)
{
    pick_t *p = lv_event_get_user_data(e);
    ui_pick_done_cb_t done = p->multi ? p->done : NULL;
    void *user = p->user;
    ui_modal_close(p->ov);
    if (done) done(user);
}

static void mark_row(lv_obj_t *r, bool on)
{
    lv_obj_set_style_border_color(r, on ? COL_ACCENT : COL_CARD2, 0);
    lv_obj_t *l = lv_obj_get_child(r, 0);
    if (l) lv_obj_set_style_text_color(l, on ? COL_ACCENT : COL_TEXT, 0);
}

static void pick_row_cb(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_current_target(e);
    pick_t *p = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(r);
    if (p->multi) {
        p->sel[i] = !p->sel[i];
        mark_row(r, p->sel[i]);
        return;
    }
    ui_pick_cb_t cb = p->cb;
    void *user = p->user;
    char txt[96];
    strlcpy(txt, p->items[i], sizeof(txt));
    ui_modal_close(p->ov);
    if (cb) cb(i, txt, user);
}

static void pick_new_done(const char *text, void *user)
{
    pick_t *p = user;
    ui_pick_cb_t cb = p->cb;
    void *u = p->user;
    ui_modal_close(p->ov);
    if (cb) cb(-1, text, u);
}

static void pick_new_cb(lv_event_t *e)
{
    pick_t *p = lv_event_get_user_data(e);
    const char *typed = p->search ? lv_textarea_get_text(p->search) : "";
    ui_prompt(p->new_title, typed, pick_new_done, p);
}

static void pick_filter(lv_event_t *e)
{
    pick_t *p = lv_event_get_user_data(e);
    const char *q = lv_textarea_get_text(p->search);
    for (int k = 0; k < (int)lv_obj_get_child_count(p->list); k++) {
        lv_obj_t *r = lv_obj_get_child(p->list, k);
        int i = (int)(intptr_t)lv_obj_get_user_data(r);
        if (i < 0) continue;   /* the "new" row stays */
        bool m = !q[0] || strcasestr(p->items[i], q);
        if (m) lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *pick_row(pick_t *p, const char *txt, int i, bool on)
{
    lv_obj_t *r = lv_obj_create(p->list);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 60, 0);
    lv_obj_set_style_bg_color(r, COL_CARD2, 0);
    lv_obj_set_style_bg_color(r, COL_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(r, 3, 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_pad_hor(r, 14, 0);
    lv_obj_set_style_pad_ver(r, 10, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    lv_obj_t *l = ui_label(r, FONT_M, COL_TEXT, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    mark_row(r, on);
    return r;
}

static void pick_open(const char *title, const char *const *items, int n, int selected, bool *sel,
                      const char *new_label, ui_pick_cb_t cb, ui_pick_done_cb_t done, void *user)
{
    pick_t *p = lv_malloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->done = done;
    p->user = user;
    p->multi = sel != NULL;
    p->sel = sel;
    p->new_label = new_label;
    if (new_label) snprintf(p->new_title, sizeof(p->new_title), ICON_PLUS "  %s", new_label);
    p->items = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < n && p->items; i++) {
        p->items[i] = heap_caps_malloc(strlen(items[i]) + 1, MALLOC_CAP_SPIRAM);
        if (p->items[i]) strcpy(p->items[i], items[i]);
        else { n = i; break; }
    }
    p->n = p->items ? n : 0;
    p->ov = overlay();
    lv_obj_add_event_cb(p->ov, pick_free, LV_EVENT_DELETE, p);

    int h = ui_height() + 40;
    lv_obj_t *b = box(p->ov, 720, h > 640 ? 640 : h);
    lv_obj_t *hr = row(b);
    lv_obj_t *t = ui_label(hr, FONT_L, COL_TEXT, title);
    lv_obj_set_flex_grow(t, 1);
    ui_button(hr, p->multi ? ICON_CHECK : ICON_CLOSE, p->multi ? "Done" : "Cancel", p->multi ? COL_ACCENT : COL_CARD2,
              pick_close, p);
    if (p->n > 8) {
        p->search = lv_textarea_create(b);
        lv_textarea_set_one_line(p->search, true);
        lv_textarea_set_placeholder_text(p->search, "Search...");
        lv_obj_set_width(p->search, LV_PCT(100));
        lv_obj_set_style_text_font(p->search, FONT_L, 0);
        ui_attach_keyboard(p->search, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_add_event_cb(p->search, pick_filter, LV_EVENT_VALUE_CHANGED, p);
    }
    p->list = lv_obj_create(b);
    lv_obj_set_width(p->list, LV_PCT(100));
    lv_obj_set_flex_grow(p->list, 1);
    lv_obj_set_style_bg_opa(p->list, 0, 0);
    lv_obj_set_style_border_width(p->list, 0, 0);
    lv_obj_set_style_pad_all(p->list, 0, 0);
    lv_obj_set_style_pad_gap(p->list, 8, 0);
    lv_obj_set_flex_flow(p->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
    if (new_label) {
        char nl[80];
        snprintf(nl, sizeof(nl), ICON_PLUS "  %s", new_label);
        lv_obj_t *r = pick_row(p, nl, -1, false);
        lv_obj_remove_event_cb(r, pick_row_cb);
        lv_obj_add_event_cb(r, pick_new_cb, LV_EVENT_CLICKED, p);
    }
    lv_obj_t *sel_row = NULL;
    for (int i = 0; i < p->n; i++) {
        bool on = p->multi ? sel[i] : i == selected;
        lv_obj_t *r = pick_row(p, p->items[i], i, on);
        lv_obj_add_event_cb(r, pick_row_cb, LV_EVENT_CLICKED, p);
        if (on && !sel_row) sel_row = r;
    }
    if (!p->n && !new_label) ui_label(p->list, FONT_M, COL_MUTED, "Nothing to choose from.");
    if (sel_row) {
        lv_obj_update_layout(p->list);
        lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF);
    }
}

void ui_pick(const char *title, const char *const *items, int n, int selected, const char *new_label, ui_pick_cb_t cb, void *user)
{
    pick_open(title, items, n, selected, NULL, new_label, cb, NULL, user);
}

void ui_pick_multi(const char *title, const char *const *items, int n, bool *sel, ui_pick_done_cb_t done, void *user)
{
    pick_open(title, items, n, -1, sel, NULL, NULL, done, user);
}

/* ── colour picker ───────────────────────────────────────────────────── */
static const uint32_t PALETTE[] = {
    0x000000, 0x404040, 0x808080, 0xC0C0C0, 0xFFFFFF, 0xF5F0E1, 0xA99388, 0x8B5A2B,
    0xFF0000, 0xB00020, 0xFF6600, 0xFFA500, 0xFFD700, 0xFFFF00, 0x9ACD32, 0x00C000,
    0x006400, 0x00CED1, 0x00BFFF, 0x0047AB, 0x000080, 0x6A0DAD, 0xBA55D3, 0xFF69B4,
    0xFFC0CB, 0x800000, 0xD2B48C, 0xCD7F32, 0xB87333, 0xD4AF37, 0x39FF14, 0xFF1493,
};

typedef struct {
    lv_obj_t *ov, *ta, *prev, *prev_lbl;
    ui_color_cb_t cb;
    void *user;
    int32_t cur;
} colorp_t;

static void colorp_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

static void colorp_show(colorp_t *c)
{
    if (c->cur >= 0) {
        lv_obj_set_style_bg_color(c->prev, lv_color_hex(c->cur), 0);
        lv_obj_set_style_bg_opa(c->prev, LV_OPA_COVER, 0);
        char t[16];
        snprintf(t, sizeof(t), "#%06lX", (unsigned long)c->cur);
        lv_label_set_text(c->prev_lbl, t);
    } else {
        lv_obj_set_style_bg_opa(c->prev, 0, 0);
        lv_label_set_text(c->prev_lbl, "No colour");
    }
}

static void colorp_swatch(lv_event_t *e)
{
    colorp_t *c = lv_event_get_user_data(e);
    c->cur = (int32_t)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    char t[8];
    snprintf(t, sizeof(t), "%06lX", (unsigned long)c->cur);
    lv_textarea_set_text(c->ta, t);   /* fires VALUE_CHANGED -> colorp_hex */
    colorp_show(c);
}

static void colorp_hex(lv_event_t *e)
{
    colorp_t *c = lv_event_get_user_data(e);
    const char *t = lv_textarea_get_text(c->ta);
    if (t[0] == '#') t++;
    if (strlen(t) == 6) {
        char *end;
        long v = strtol(t, &end, 16);
        if (!*end) { c->cur = (int32_t)v; colorp_show(c); }
    } else if (!t[0]) { c->cur = -1; colorp_show(c); }
}

static void colorp_btn(lv_event_t *e)
{
    colorp_t *c = lv_event_get_user_data(e);
    int what = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    ui_color_cb_t cb = c->cb;
    void *user = c->user;
    int32_t v = what == 2 ? -1 : c->cur;
    ui_modal_close(c->ov);
    if (what && cb) cb(v, user);
}

void ui_color_pick(const char *title, int32_t initial, ui_color_cb_t cb, void *user)
{
    colorp_t *c = lv_malloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->cb = cb;
    c->user = user;
    c->cur = initial;
    c->ov = overlay();
    lv_obj_add_event_cb(c->ov, colorp_free, LV_EVENT_DELETE, c);
    lv_obj_t *b = box(c->ov, 720, LV_SIZE_CONTENT);
    ui_label(b, FONT_L, COL_TEXT, title);

    lv_obj_t *grid = lv_obj_create(b);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_gap(grid, 10, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < sizeof(PALETTE) / sizeof(PALETTE[0]); i++) {
        lv_obj_t *s = lv_obj_create(grid);
        lv_obj_set_size(s, 64, 64);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s, 2, 0);
        lv_obj_set_style_border_color(s, COL_BORDER, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(PALETTE[i]), 0);
        lv_obj_set_style_outline_width(s, 3, LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(s, COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(s, (void *)(intptr_t)PALETTE[i]);
        lv_obj_add_event_cb(s, colorp_swatch, LV_EVENT_CLICKED, c);
    }

    lv_obj_t *r = row(b);
    c->prev = lv_obj_create(r);
    lv_obj_set_size(c->prev, 56, 56);
    lv_obj_set_style_radius(c->prev, 12, 0);
    lv_obj_set_style_border_width(c->prev, 2, 0);
    lv_obj_set_style_border_color(c->prev, COL_BORDER, 0);
    lv_obj_remove_flag(c->prev, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui_label(r, FONT_M, COL_MUTED, "Hex  #");
    c->ta = lv_textarea_create(r);
    lv_textarea_set_one_line(c->ta, true);
    lv_textarea_set_max_length(c->ta, 6);
    lv_textarea_set_accepted_chars(c->ta, "0123456789abcdefABCDEF");
    lv_obj_set_width(c->ta, 160);
    lv_obj_set_style_text_font(c->ta, FONT_L, 0);
    char t[8] = "";
    if (initial >= 0) snprintf(t, sizeof(t), "%06lX", (unsigned long)initial);
    lv_textarea_set_text(c->ta, t);
    ui_attach_keyboard(c->ta, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_event_cb(c->ta, colorp_hex, LV_EVENT_VALUE_CHANGED, c);
    c->prev_lbl = ui_label(r, FONT_M, COL_TEXT, "");
    colorp_show(c);

    lv_obj_t *br = row(b);
    static const char *const L[] = { "Cancel", "Set", "No colour" };
    static const char *const I[] = { ICON_CLOSE, ICON_CHECK, ICON_DELETE };
    const int order[] = { 0, 2, 1 };
    for (int k = 0; k < 3; k++) {
        int i = order[k];
        lv_obj_t *bt = ui_button(br, I[i], L[i], i == 1 ? COL_ACCENT : COL_CARD2, colorp_btn, c);
        lv_obj_set_user_data(bt, (void *)(intptr_t)i);
        lv_obj_set_flex_grow(bt, 1);
    }
}

/* ── date picker ─────────────────────────────────────────────────────── */
#define YEAR0 2015
#define NYEARS 21

typedef struct {
    lv_obj_t *ov, *d, *m, *y;
    ui_date_cb_t cb;
    void *user;
} datep_t;

static void datep_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

static void datep_set(datep_t *c, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    lv_roller_set_selected(c->d, tm.tm_mday - 1, LV_ANIM_OFF);
    lv_roller_set_selected(c->m, tm.tm_mon, LV_ANIM_OFF);
    int yi = tm.tm_year + 1900 - YEAR0;
    lv_roller_set_selected(c->y, yi < 0 ? 0 : yi >= NYEARS ? NYEARS - 1 : yi, LV_ANIM_OFF);
}

static void datep_btn(lv_event_t *e)
{
    datep_t *c = lv_event_get_user_data(e);
    int what = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (what == 3) { datep_set(c, time(NULL)); return; }   /* Today */
    time_t out = 0;
    if (what == 1) {
        struct tm tm = {0};
        tm.tm_mday = lv_roller_get_selected(c->d) + 1;
        tm.tm_mon = lv_roller_get_selected(c->m);
        tm.tm_year = lv_roller_get_selected(c->y) + YEAR0 - 1900;
        tm.tm_isdst = -1;
        time_t now = time(NULL);
        struct tm n;
        localtime_r(&now, &n);
        if (tm.tm_mday == n.tm_mday && tm.tm_mon == n.tm_mon && tm.tm_year == n.tm_year) out = now;   /* today: now */
        else { tm.tm_hour = 12; out = mktime(&tm); }   /* other days: midday, so no time zone moves the date */
    }
    ui_date_cb_t cb = c->cb;
    void *user = c->user;
    ui_modal_close(c->ov);
    if (what && cb) cb(out, user);
}

static lv_obj_t *roller(lv_obj_t *p, const char *opts, int w)
{
    lv_obj_t *r = lv_roller_create(p);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 4);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, FONT_L, 0);
    lv_obj_set_style_bg_color(r, COL_CARD2, 0);
    lv_obj_set_style_text_color(r, COL_TEXT, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, COL_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, ui_contrast(COL_ACCENT), LV_PART_SELECTED);
    return r;
}

void ui_date_pick(const char *title, time_t initial, ui_date_cb_t cb, void *user)
{
    datep_t *c = lv_malloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->cb = cb;
    c->user = user;
    c->ov = overlay();
    lv_obj_add_event_cb(c->ov, datep_free, LV_EVENT_DELETE, c);
    lv_obj_t *b = box(c->ov, 640, LV_SIZE_CONTENT);
    ui_label(b, FONT_L, COL_TEXT, title);
    lv_obj_t *r = row(b);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char days[31 * 3 + 1] = "";
    for (int i = 1; i <= 31; i++) {
        char t[4];
        snprintf(t, sizeof(t), i < 31 ? "%d\n" : "%d", i);
        strlcat(days, t, sizeof(days));
    }
    char years[NYEARS * 5 + 1] = "";
    for (int i = 0; i < NYEARS; i++) {
        char t[6];
        snprintf(t, sizeof(t), i < NYEARS - 1 ? "%d\n" : "%d", YEAR0 + i);
        strlcat(years, t, sizeof(years));
    }
    c->d = roller(r, days, 110);
    c->m = roller(r, "January\nFebruary\nMarch\nApril\nMay\nJune\nJuly\nAugust\nSeptember\nOctober\nNovember\nDecember", 220);
    c->y = roller(r, years, 140);
    datep_set(c, initial ? initial : time(NULL));

    lv_obj_t *br = row(b);
    static const char *const L[] = { "Cancel", "Set", "Clear", "Today" };
    static const char *const I[] = { ICON_CLOSE, ICON_CHECK, ICON_DELETE, ICON_CLOCK };
    const int order[] = { 0, 2, 3, 1 };
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        lv_obj_t *bt = ui_button(br, I[i], L[i], i == 1 ? COL_ACCENT : COL_CARD2, datep_btn, c);
        lv_obj_set_user_data(bt, (void *)(intptr_t)i);
        lv_obj_set_flex_grow(bt, 1);
    }
}
