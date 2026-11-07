/* Files: browse a printer's G-code files (newest first) and start a print. */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "printers.h"
#include "files.h"

static int s_idx;
static int s_sel = -1;
static uint32_t s_ver;
static gfile_t *s_list;
static int s_n;
static gmeta_t s_meta;              /* details shown on the right (thumb owned here) */
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_rows, *s_detail, *s_status, *s_thumb;
static char s_start_path[128];
static printer_state_t s_st;

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

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void fmt_size(uint32_t b, char *out, size_t cap)
{
    if (b >= 1048576) snprintf(out, cap, "%.1f MB", b / 1048576.0f);
    else snprintf(out, cap, "%u KB", (unsigned)((b + 1023) / 1024));
}

static void fmt_date(double t, char *out, size_t cap)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, cap, "%d %b %Y, %H:%M", &tm);
}

static void free_thumb(void)
{
    if (s_thumb) lv_image_set_src(s_thumb, NULL);
    if (s_meta.thumb) {
        lv_image_cache_drop(&s_dsc);
        heap_caps_free(s_meta.thumb);
    }
    s_meta.thumb = NULL;
    s_meta.thumb_len = 0;
}

/* ── detail ──────────────────────────────────────────────────────────── */
static void do_start(void *u)
{
    printers_command(s_idx, CMD_PRINT_START, s_start_path);
    ui_nav_back();   /* back to the printer page to watch it start */
}

static void start_cb(lv_event_t *e)
{
    if (s_sel < 0 || s_sel >= s_n) return;
    printers_get_state(s_idx, &s_st);
    if (s_st.status == PS_PRINTING || s_st.status == PS_PAUSED) { ui_toast("The printer is already printing"); return; }
    if (s_st.status == PS_OFFLINE || s_st.status == PS_KLIPPY_DOWN) { ui_toast("The printer isn't ready"); return; }
    strlcpy(s_start_path, s_list[s_sel].path, sizeof(s_start_path));
    char t[200];
    snprintf(t, sizeof(t), "Start printing %s?", base_name(s_start_path));
    ui_confirm("Start print?", t, "Start print", COL_OK, do_start, NULL);
}

static void detail_line(lv_obj_t *p, const char *icon, const char *k, const char *v)
{
    char t[160];
    snprintf(t, sizeof(t), "%s  %s:  %s", icon, k, v);
    lv_obj_t *l = ui_label(p, FONT_M, COL_TEXT, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
}

static void build_detail(void)
{
    free_thumb();
    s_thumb = NULL;
    lv_obj_clean(s_detail);
    if (!s_list || s_sel < 0 || s_sel >= s_n) {
        ui_label(s_detail, FONT_M, COL_MUTED, ICON_INFO "  Select a file to see details.");
        return;
    }
    const gfile_t *f = &s_list[s_sel];
    lv_obj_t *media = lv_obj_create(s_detail);
    lv_obj_set_size(media, LV_PCT(100), 220);
    lv_obj_set_style_bg_color(media, COL_CARD2, 0);
    lv_obj_set_style_border_width(media, 0, 0);
    lv_obj_set_style_radius(media, 12, 0);
    lv_obj_remove_flag(media, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ph = ui_label(media, FONT_XXL, COL_BORDER, ICON_PRINTER);
    lv_obj_center(ph);
    s_thumb = lv_image_create(media);
    lv_obj_set_size(s_thumb, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(s_thumb, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(s_thumb);

    lv_obj_t *nm = ui_label(s_detail, FONT_L, COL_TEXT, base_name(f->path));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nm, LV_PCT(100));

    gmeta_t m;
    bool have = files_meta(f->path, &m);
    char v[64];
    if (have && m.ok) {
        if (m.thumb) {
            lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN);
            s_meta = m;
            memset(&s_dsc, 0, sizeof(s_dsc));
            s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_dsc.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
            s_dsc.header.w = (m.thumb[16] << 24) | (m.thumb[17] << 16) | (m.thumb[18] << 8) | m.thumb[19];
            s_dsc.header.h = (m.thumb[20] << 24) | (m.thumb[21] << 16) | (m.thumb[22] << 8) | m.thumb[23];
            s_dsc.data = m.thumb;
            s_dsc.data_size = m.thumb_len;
            lv_image_set_src(s_thumb, &s_dsc);
        }
        if (m.est_time > 0) { ui_fmt_duration(v, sizeof(v), m.est_time); detail_line(s_detail, ICON_TIMER, "Print time", v); }
        if (m.filament_mm > 0) {
            if (m.filament_g > 0) snprintf(v, sizeof(v), "%.2f m  (%.0f g)", m.filament_mm / 1000, m.filament_g);
            else snprintf(v, sizeof(v), "%.2f m", m.filament_mm / 1000);
            detail_line(s_detail, ICON_SPOOL, "Filament", v);
        }
        if (m.layer_h > 0) { snprintf(v, sizeof(v), "%.2f mm", m.layer_h); detail_line(s_detail, ICON_LAYERS, "Layer height", v); }
        if (m.slicer[0]) detail_line(s_detail, ICON_INFO, "Slicer", m.slicer);
    } else if (have) {
        ui_label(s_detail, FONT_S, COL_MUTED, "Details not available for this file.");   /* failed once: don't retry in a loop */
    } else {
        ui_label(s_detail, FONT_S, COL_MUTED, "Loading details...");
        files_fetch_meta(s_idx, f->path);
    }
    fmt_date(f->modified, v, sizeof(v));
    detail_line(s_detail, ICON_CLOCK, "Modified", v);
    fmt_size(f->size, v, sizeof(v));
    detail_line(s_detail, ICON_INFO, "Size", v);

    lv_obj_t *b = ui_tile_button(plain(s_detail, LV_FLEX_FLOW_ROW), ICON_PLAY, "Start print", COL_OK, start_cb, NULL);
    lv_obj_set_height(b, 84);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 1), FONT_L, 0);
}

/* ── list ────────────────────────────────────────────────────────────── */
static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == s_sel) return;
    s_sel = i;
    for (int k = 0; k < (int)lv_obj_get_child_count(s_rows); k++)
        lv_obj_set_style_border_color(lv_obj_get_child(s_rows, k), k == i ? COL_ACCENT : COL_CARD2, 0);
    build_detail();
}

static void build_rows(void)
{
    lv_obj_clean(s_rows);
    heap_caps_free(s_list);
    s_list = heap_caps_malloc(FILES_MAX * sizeof(gfile_t), MALLOC_CAP_SPIRAM);
    s_n = s_list ? files_list(s_list, FILES_MAX) : 0;
    if (s_sel >= s_n) s_sel = -1;
    for (int i = 0; i < s_n; i++) {
        const gfile_t *f = &s_list[i];
        lv_obj_t *r = lv_obj_create(s_rows);
        lv_obj_set_size(r, LV_PCT(100), 78);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_bg_color(r, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, i == s_sel ? COL_ACCENT : COL_CARD2, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l1 = ui_label(r, FONT_M, COL_TEXT, base_name(f->path));
        lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l1, LV_PCT(100));
        lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 4);
        char d[48], sz[16], t[120];
        fmt_date(f->modified, d, sizeof(d));
        fmt_size(f->size, sz, sizeof(sz));
        const char *slash = strrchr(f->path, '/');
        if (slash) {
            char dir[64];
            size_t n = slash - f->path < (int)sizeof(dir) - 1 ? (size_t)(slash - f->path) : sizeof(dir) - 1;
            memcpy(dir, f->path, n);
            dir[n] = 0;
            snprintf(t, sizeof(t), "%s  -  %s  -  %s", dir, d, sz);
        } else snprintf(t, sizeof(t), "%s  -  %s", d, sz);
        lv_obj_t *l2 = ui_label(r, FONT_S, COL_MUTED, t);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l2, LV_PCT(100));
        lv_obj_align(l2, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    }
    if (!s_n) ui_label(s_rows, FONT_M, COL_MUTED, "No G-code files on this printer.");
}

static void reload_cb(lv_event_t *e)
{
    s_sel = -1;
    lv_obj_clean(s_rows);   /* rows index into s_list: remove them before freeing it */
    heap_caps_free(s_list);
    s_list = NULL;          /* rebuilt when the new list arrives */
    s_n = 0;
    build_detail();
    files_fetch(s_idx);
}

/* ── page ────────────────────────────────────────────────────────────── */
static void create(lv_obj_t *parent, int idx)
{
    s_idx = idx;
    s_sel = -1;
    s_list = NULL;
    s_n = 0;
    memset(&s_meta, 0, sizeof(s_meta));
    s_thumb = NULL;
    printer_cfg_t pc;
    char t[64];
    if (printers_get_cfg(idx, &pc)) snprintf(t, sizeof(t), "Files  -  %s", pc.name);
    else strcpy(t, "Files");
    ui_set_title(t);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *top = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_status = ui_label(top, FONT_M, COL_MUTED, "Loading files...");
    lv_obj_set_flex_grow(s_status, 1);
    ui_button(top, ICON_RESTART, "Reload", COL_CARD2, reload_cb, NULL);

    lv_obj_t *body = plain(parent, ui_is_portrait() ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(body, 16, 0);
    s_rows = lv_obj_create(body);
    lv_obj_set_style_bg_opa(s_rows, 0, 0);
    lv_obj_set_style_border_width(s_rows, 0, 0);
    lv_obj_set_style_pad_all(s_rows, 0, 0);
    lv_obj_set_style_pad_gap(s_rows, 8, 0);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_rows, LV_DIR_VER);
    s_detail = ui_card(body);
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_detail, 10, 0);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);
    int h = ui_height() - 32 - 60 - 16;
    if (ui_is_portrait()) {
        lv_obj_set_size(s_rows, LV_PCT(100), h / 2);
        lv_obj_set_size(s_detail, LV_PCT(100), h / 2);
    } else {
        int w = ui_width() - 32 - 16;
        lv_obj_set_size(s_rows, w * 55 / 100, h);
        lv_obj_set_size(s_detail, w - w * 55 / 100, h);
    }
    s_ver = files_version();
    files_fetch(idx);
    build_detail();
}

static void refresh(void)
{
    if (files_version() != s_ver) {
        s_ver = files_version();
        char err[96];
        fl_state_t st = files_state(err, sizeof(err));
        if (st == FL_OK && !s_list) build_rows();
        else if (st == FL_OK && s_sel < 0) build_rows();
        if (s_sel >= 0 && !s_meta.thumb) build_detail();   /* details for the selected file arrived */
        char t[96];
        if (st == FL_ERROR) snprintf(t, sizeof(t), ICON_ALERT "  %s", err);
        else snprintf(t, sizeof(t), "%d file%s  -  newest first", s_n, s_n == 1 ? "" : "s");
        ui_set_text(s_status, t);
    }
}

static void destroy(void)
{
    free_thumb();
    heap_caps_free(s_list);
    s_list = NULL;
    s_n = 0;
    files_free();
}

const ui_page_t page_files = { "Files", create, refresh, destroy };
