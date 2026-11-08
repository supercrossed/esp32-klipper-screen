#pragma once
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdbool.h>

/* ── pages ───────────────────────────────────────────────────────────── */
typedef enum {
    PAGE_HOME = 0,
    PAGE_PRINTER,
    PAGE_WEBCAM,
    PAGE_ADD_PRINTER,     /* arg: -1 add, >=0 edit printer idx */
    PAGE_SETTINGS,
    PAGE_WIFI,
    PAGE_SPOOLMAN,        /* arg: printer idx */
    PAGE_FILES,           /* arg: printer idx */
    PAGE_COUNT
} page_id_t;

typedef struct {
    const char *title;
    void (*create)(lv_obj_t *parent, int arg);
    void (*refresh)(void);    /* ~4 Hz, LVGL lock held */
    void (*destroy)(void);    /* free everything the page owns (called before widgets are deleted) */
} ui_page_t;

extern const ui_page_t page_home, page_printer, page_webcam, page_add_printer, page_settings, page_wifi, page_spoolman, page_files;

void ui_init(lv_display_t *disp);
void ui_nav_push(page_id_t id, int arg);
void ui_nav_replace(page_id_t id, int arg);
void ui_nav_back(void);
void ui_nav_home(void);
void ui_set_title(const char *t);

/* ── theme ───────────────────────────────────────────────────────────── */
/* Runtime palette (dark / light), switched by ui_set_light_mode(). */
typedef struct {
    lv_color_t bg, header, card, card2, border, text, muted, accent, ok, warn, err, hot;
} ui_palette_t;
extern ui_palette_t g_pal;
#define COL_BG        (g_pal.bg)
#define COL_HEADER    (g_pal.header)
#define COL_CARD      (g_pal.card)
#define COL_CARD2     (g_pal.card2)
#define COL_BORDER    (g_pal.border)
#define COL_TEXT      (g_pal.text)
#define COL_MUTED     (g_pal.muted)
#define COL_ACCENT    (g_pal.accent)
#define COL_OK        (g_pal.ok)
#define COL_WARN      (g_pal.warn)
#define COL_ERR       (g_pal.err)
#define COL_HOT       (g_pal.hot)

void       ui_set_light_mode(bool light);          /* re-themes and rebuilds the current screen */
void       ui_set_theme(int mode, int accent);     /* mode 0 dark / 1 light / 2 auto, -1 = keep; accent ACCENT_THEME = theme's own */
void       ui_set_palette(int theme);              /* full theme; also resets the accent to the theme's */
int        ui_theme_count(void);
const char *ui_theme_name(int i);
void       ui_theme_preview(int i, lv_color_t *bg, lv_color_t *card, lv_color_t *accent, lv_color_t *text);   /* in the current mode */
int        ui_accent_count(void);
lv_color_t ui_accent_color(int i);               /* in the current mode; outside 0..count-1 = the theme's accent */
const char *ui_accent_name(int i);               /* "Theme" for ACCENT_THEME */
lv_color_t ui_contrast(lv_color_t bg);             /* black or white text for a background */

/* Montserrat copies whose fallback is the matching Material Design Icons size,
 * so ICON_* (icons.h) and text mix freely in any label. Built by ui_init(). */
/* Inter (regular) with MDI icons; see fonts_init() in ui.c */
extern const lv_font_t *g_font_16, *g_font_20, *g_font_24, *g_font_32, *g_font_48;
#define FONT_S   (g_font_16)
#define FONT_M   (g_font_20)
#define FONT_L   (g_font_24)
#define FONT_XL  (g_font_32)
#define FONT_XXL (g_font_48)
const lv_font_t *ui_font_bold(int px);   /* Inter Bold (nearest pre-rendered size), with icons */
const lv_font_t *ui_font_icon(int px);   /* MDI icons only, any size */

/* ── helpers ─────────────────────────────────────────────────────────── */
int        ui_width(void);            /* logical content width (after rotation) */
int        ui_height(void);           /* content area height (below header) */
bool       ui_is_portrait(void);
lv_obj_t  *ui_card(lv_obj_t *parent);
lv_obj_t  *ui_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, const char *txt);
/* List rows: make `row` a flex row (min height, grows with its text), and add a
 * title-over-subtitle column that takes the leftover width and ends long text
 * with "..." instead of running under icons or buttons beside it. */
void       ui_list_row(lv_obj_t *row, int min_h);
lv_obj_t  *ui_two_line(lv_obj_t *row, const lv_font_t *f, lv_color_t c, const char *title, const char *sub);
lv_obj_t  *ui_button(lv_obj_t *parent, const char *symbol, const char *txt, lv_color_t bg, lv_event_cb_t cb, void *user);
lv_obj_t  *ui_section_title(lv_obj_t *parent, const char *txt);
lv_obj_t  *ui_tile_button(lv_obj_t *parent, const char *icon, const char *txt, lv_color_t bg, lv_event_cb_t cb, void *user);
void       ui_toast(const char *msg);
void       ui_attach_keyboard(lv_obj_t *textarea, lv_keyboard_mode_t mode);

typedef void (*ui_confirm_cb_t)(void *user);
void ui_confirm(const char *title, const char *text, const char *ok, lv_color_t ok_col, ui_confirm_cb_t cb, void *user);

typedef void (*ui_numpad_cb_t)(float value, void *user);
void ui_numpad(const char *title, float initial, const int *presets, int npresets, ui_numpad_cb_t cb, void *user);

typedef void (*ui_prompt_cb_t)(const char *text, void *user);
void ui_prompt(const char *title, const char *initial, ui_prompt_cb_t cb, void *user);

lv_color_t  ui_status_color(int printer_status);
void        ui_fmt_duration(char *out, size_t cap, float seconds);
void        ui_apply_brightness(void);
void        ui_set_text(lv_obj_t *lbl, const char *txt);   /* only redraws on change */
void        ui_alert_tick(void);                           /* job-ended banners (ui_alert.c) */
void        ui_update_dialog(void);                        /* release notes + confirm (when an update is available) */
void        ui_update_tick(void);                          /* opens the progress view when an install starts by itself */
void        ui_keyboard_setup(lv_obj_t *kb);               /* custom layouts (USER_1 = IP numpad) */
