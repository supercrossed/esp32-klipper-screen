/* Home: one tile per printer + "add printer" tile. */
#include "ui.h"
#include "icons.h"
#include "ui_thumb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "printers.h"
#include "wifi_mgr.h"

typedef struct {
    lv_obj_t *card, *name, *chip, *chip_lbl, *host, *thumb, *file, *bar, *prog, *temps_row, *badges;
    lv_obj_t *temp[3];
    ui_thumb_t th;
    uint32_t seq;
    int status;
} tile_t;

static tile_t s_tiles[MAX_PRINTERS];
static int s_ntiles;
static lv_obj_t *s_grid, *s_empty;
static printer_state_t s_st;   /* scratch copy (static: keeps it off the LVGL task stack) */
static int s_tick;

static bool s_long;   /* a long-press also produces CLICKED on release: swallow it */
static lv_obj_t *s_spool_btn;

static void tile_click(lv_event_t *e)
{
    if (s_long) { s_long = false; return; }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui_nav_push(PAGE_PRINTER, idx);
}

static void rename_done(const char *text, void *user)
{
    int idx = (int)(intptr_t)user;
    printer_cfg_t c;
    if (!printers_get_cfg(idx, &c)) return;
    strlcpy(c.name, text, sizeof(c.name));
    printers_update(idx, &c);
    char nm[48];
    snprintf(nm, sizeof(nm), ICON_PRINTER "  %s", c.name);
    if (idx < s_ntiles) ui_set_text(s_tiles[idx].name, nm);
}

static void tile_long(lv_event_t *e)
{
    s_long = true;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    printer_cfg_t c;
    if (printers_get_cfg(idx, &c)) ui_prompt(ICON_EDIT "  Nickname for this printer", c.name, rename_done, (void *)(intptr_t)idx);
}

static void spool_click(lv_event_t *e)
{
    for (int i = 0; i < printers_count(); i++)
        if (printers_get_state(i, &s_st) && s_st.has_spoolman) { ui_nav_push(PAGE_SPOOLMAN, i); return; }
}

static void add_click(lv_event_t *e) { ui_nav_push(PAGE_ADD_PRINTER, -1); }

static lv_obj_t *flat_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(r, 0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_gap(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}

static int tile_width(void)
{
    int cols = ui_is_portrait() ? 2 : 3;
    int w = ui_width() - 32;              /* content padding */
    return (w - (cols - 1) * 16) / cols;
}

static void build_tile(int i)
{
    tile_t *t = &s_tiles[i];
    memset(t, 0, sizeof(*t));
    t->status = -1;
    t->card = ui_card(s_grid);
    lv_obj_set_size(t->card, tile_width(), 300);
    lv_obj_set_flex_flow(t->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(t->card, 8, 0);
    lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t->card, COL_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(t->card, tile_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(t->card, tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    lv_obj_t *r1 = flat_row(t->card);
    t->name = ui_label(r1, FONT_L, COL_TEXT, "");
    lv_label_set_long_mode(t->name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t->name, 1);
    t->chip = lv_obj_create(r1);
    lv_obj_set_size(t->chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(t->chip, 20, 0);
    lv_obj_set_style_border_width(t->chip, 0, 0);
    lv_obj_set_style_pad_hor(t->chip, 12, 0);
    lv_obj_set_style_pad_ver(t->chip, 4, 0);
    lv_obj_remove_flag(t->chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t->chip, LV_OBJ_FLAG_EVENT_BUBBLE);
    t->chip_lbl = ui_label(t->chip, FONT_S, lv_color_black(), "");

    t->host = ui_label(t->card, FONT_S, COL_MUTED, "");

    lv_obj_t *mid = flat_row(t->card);
    lv_obj_set_flex_grow(mid, 1);
    t->thumb = lv_image_create(mid);
    lv_obj_add_flag(t->thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(t->thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t->thumb, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *col = lv_obj_create(mid);
    lv_obj_set_style_bg_opa(col, 0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_gap(col, 8, 0);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
    t->file = ui_label(col, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(t->file, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t->file, LV_PCT(100));
    t->bar = lv_bar_create(col);
    lv_obj_set_size(t->bar, LV_PCT(100), 16);
    lv_bar_set_range(t->bar, 0, 1000);
    lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t->bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    t->prog = ui_label(col, FONT_S, COL_MUTED, "");
    lv_label_set_long_mode(t->prog, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t->prog, LV_PCT(100));

    /* temperatures: one label per heater (hotend, bed, chamber) + badges on the right */
    t->temps_row = flat_row(t->card);
    lv_obj_set_style_pad_gap(t->temps_row, 18, 0);
    lv_obj_set_style_pad_top(t->temps_row, 8, 0);
    lv_obj_set_style_border_side(t->temps_row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(t->temps_row, 1, 0);
    lv_obj_set_style_border_color(t->temps_row, COL_BORDER, 0);
    for (int k = 0; k < 3; k++) {
        t->temp[k] = ui_label(t->temps_row, FONT_M, COL_TEXT, "");
        lv_obj_add_flag(t->temp[k], LV_OBJ_FLAG_HIDDEN);
    }
    t->badges = ui_label(t->temps_row, FONT_L, COL_MUTED, "");
    lv_obj_set_flex_grow(t->badges, 1);
    lv_obj_set_style_text_align(t->badges, LV_TEXT_ALIGN_RIGHT, 0);

    printer_cfg_t cfg;
    if (printers_get_cfg(i, &cfg)) {
        char nm[48];
        snprintf(nm, sizeof(nm), ICON_PRINTER "  %s", cfg.name);
        ui_set_text(t->name, nm);
        char h[80];
        snprintf(h, sizeof(h), "%s", cfg.host);
        ui_set_text(t->host, h);
    }
}

static void update_tile(int i)
{
    tile_t *t = &s_tiles[i];
    if (!printers_get_state(i, &s_st)) return;
    bool thumb = ui_thumb_update(&t->th, t->thumb, i, 110);
    if (s_st.seq == t->seq && (int)s_st.status == t->status) return;   /* nothing new */
    t->seq = s_st.seq;

    if ((int)s_st.status != t->status) {
        t->status = s_st.status;
        lv_obj_set_style_bg_color(t->chip, ui_status_color(s_st.status), 0);
        ui_set_text(t->chip_lbl, printer_status_str(s_st.status));
        lv_obj_set_style_opa(t->card, s_st.status == PS_OFFLINE ? LV_OPA_60 : LV_OPA_COVER, 0);
    }

    char b[160];
    bool job = s_st.status == PS_PRINTING || s_st.status == PS_PAUSED;
    if (s_st.status == PS_OFFLINE || s_st.status == PS_KLIPPY_DOWN) {
        ui_set_text(t->file, s_st.message);
        lv_obj_add_flag(t->bar, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(t->prog, "");
    } else if (s_st.filename[0] && (job || s_st.status == PS_COMPLETE)) {
        const char *fn = strrchr(s_st.filename, '/');
        ui_set_text(t->file, fn ? fn + 1 : s_st.filename);
        lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(t->bar, (int)(s_st.progress * 1000), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(t->bar, ui_status_color(s_st.status), LV_PART_INDICATOR);
        if (job) {
            char left[24], eta[16] = "";
            ui_fmt_duration(left, sizeof(left), s_st.eta_s);
            if (s_st.eta_s >= 0 && wifi_mgr_time_valid()) {
                time_t end = time(NULL) + (time_t)s_st.eta_s;
                struct tm tm;
                localtime_r(&end, &tm);
                strftime(eta, sizeof(eta), " - ETA %H:%M", &tm);
            }
            snprintf(b, sizeof(b), "%d%%  -  %s left%s", (int)(s_st.progress * 100), left, eta);
        } else {
            char d[24];
            ui_fmt_duration(d, sizeof(d), s_st.print_duration);
            snprintf(b, sizeof(b), "Finished in %s", d);
        }
        ui_set_text(t->prog, b);
    } else {
        ui_set_text(t->file, "Ready");
        lv_obj_add_flag(t->bar, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(t->prog, "");
    }
    if (!thumb && !job) lv_obj_add_flag(t->thumb, LV_OBJ_FLAG_HIDDEN);

    /* subtitle: Moonraker hostname + address */
    printer_cfg_t cfg;
    if (printers_get_cfg(i, &cfg)) {
        if (s_st.hostname[0] && strcasecmp(s_st.hostname, cfg.host)) snprintf(b, sizeof(b), "%s  -  %s", s_st.hostname, cfg.host);
        else snprintf(b, sizeof(b), "%s", cfg.host);
        ui_set_text(t->host, b);
    }

    int shown = 0;
    bool live = s_st.online && s_st.status != PS_KLIPPY_DOWN;
    for (int h = 0; h < s_st.nheaters && live && shown < 3; h++) {
        const heater_t *ht = &s_st.heaters[h];
        if (ht->kind > 2) continue;
        if (ht->kind == 0 && strcmp(ht->obj, "extruder")) continue;   /* tiles show the first hotend only */
        const char *ic = ht->kind == 0 ? ICON_HOTEND : ht->kind == 1 ? ICON_BED : ICON_CHAMBER;
        if (ht->settable && ht->target > 0) snprintf(b, sizeof(b), "%s %.0f/%.0f°", ic, ht->temp, ht->target);
        else snprintf(b, sizeof(b), "%s %.0f°", ic, ht->temp);
        ui_set_text(t->temp[shown], b);
        lv_obj_set_style_text_color(t->temp[shown], ht->target > 0 ? COL_HOT : COL_TEXT, 0);
        lv_obj_remove_flag(t->temp[shown], LV_OBJ_FLAG_HIDDEN);
        shown++;
    }
    for (int k = shown; k < 3; k++) lv_obj_add_flag(t->temp[k], LV_OBJ_FLAG_HIDDEN);
    b[0] = 0;
    if (s_st.mmu.type != MMU_NONE) strlcat(b, ICON_MMU, sizeof(b));
    if (s_st.has_spoolman) strlcat(b, "  " ICON_SPOOL, sizeof(b));
    if (s_st.nwebcams) strlcat(b, "  " ICON_WEBCAM, sizeof(b));
    ui_set_text(t->badges, b);
}

static void build(void)
{
    lv_obj_clean(s_grid);
    for (int i = 0; i < s_ntiles; i++) ui_thumb_free(&s_tiles[i].th);
    s_ntiles = printers_count();
    for (int i = 0; i < s_ntiles; i++) build_tile(i);

    lv_obj_t *add = ui_card(s_grid);
    lv_obj_set_size(add, tile_width(), 300);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(add, COL_ACCENT, 0);
    lv_obj_set_style_border_opa(add, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(add, COL_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(add, add_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(add, FONT_L, COL_ACCENT, ICON_PLUS "  Add printer");
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);

    if (s_empty) {
        if (s_ntiles) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < s_ntiles; i++) update_tile(i);
}

static void update_spool_btn(void)
{
    bool any = false;
    for (int i = 0; i < printers_count() && !any; i++)
        if (printers_get_state(i, &s_st) && s_st.has_spoolman) any = true;
    if (any) lv_obj_remove_flag(s_spool_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_spool_btn, LV_OBJ_FLAG_HIDDEN);
}

static void create(lv_obj_t *parent, int arg)
{
    printers_set_focus(-1);   /* nothing in the foreground: all printers poll at the slow rate */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    s_empty = ui_label(parent, FONT_M, COL_MUTED,
                       "No printers yet. Tap \"Add printer\" to search the network or enter an IP address.");
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, LV_PCT(100));
    s_long = false;
    s_spool_btn = ui_button(parent, ICON_SPOOL, "Spoolman  -  spools & filaments", COL_CARD, spool_click, NULL);
    lv_obj_add_flag(s_spool_btn, LV_OBJ_FLAG_HIDDEN);
    s_grid = lv_obj_create(parent);
    lv_obj_set_size(s_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_grid, 0, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_style_pad_gap(s_grid, 16, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    s_ntiles = 0;
    build();
    update_spool_btn();
    lv_obj_t *hint = ui_label(parent, FONT_S, COL_MUTED, ICON_INFO "  Long-press a printer to give it a nickname.");
    (void)hint;
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
    s_grid = s_empty = NULL;
}

const ui_page_t page_home = { "Printers", create, refresh, destroy };
