/*
 * Spoolman labels for the BLE label printer (label_printer.c): drawing the
 * label, the printer chooser, and "print, then report" for the pages.
 *
 * The label (50 x 30 mm at 203 dpi = 400 x 240 dots) is drawn with the UI's
 * own fonts on an off-screen L8 canvas, the QR code is set module by module,
 * and the result goes to the printer as one TSPL BITMAP (1 = white).
 * The content keeps inside 2..46 mm across and 2..27 mm down: that is what this
 * printer's 48 mm head and feed actually reach on a 50 x 30 mm label.
 */
#include "ui.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "label_printer.h"
#include "ui_label.h"

#define LW 400
#define LH 240
#define DOTS_MM 8

/* ── drawing ─────────────────────────────────────────────────────────── */
static int text_h(const char *t, const lv_font_t *f, int w)
{
    lv_point_t p;
    lv_text_get_size(&p, t, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    return p.y;
}

static int draw_text(lv_layer_t *layer, const char *t, const lv_font_t *f, int x, int y, int w)
{
    if (!t || !t[0]) return 0;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_black();
    d.text = t;
    d.text_local = 1;   /* drawn at lv_canvas_finish_layer(): copy, the caller reuses its buffer */
    int h = text_h(t, f, w);
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    lv_draw_label(layer, &d, &a);
    return h;
}

uint8_t *ui_label_tspl(const ui_label_t *in, size_t *len_out)
{
    label_cfg_t cfg;
    label_cfg_get(&cfg);
    *len_out = 0;
    lv_draw_buf_t *db = lv_draw_buf_create(LW, LH, LV_COLOR_FORMAT_L8, 0);
    if (!db) return NULL;
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, db);
    lv_canvas_fill_bg(cv, lv_color_white(), LV_OPA_COVER);

    /* safe area, moved by the user's nudge */
    int dx = cfg.dx_mm * DOTS_MM, dy = cfg.dy_mm * DOTS_MM;
    int x0 = 16 + dx, y0 = 16 + dy, x1 = 368 + dx, y1 = 216 + dy;

    /* QR code: as large as fits in 152 dots, whole dots per module */
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    bool have_qr = in->qr[0] && qrcodegen_encodeText(in->qr, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
    int qn = have_qr ? qrcodegen_getSize(qr) : 0;
    int mod = qn ? 152 / qn : 0;
    int qs = qn * mod;
    int qy = y0 + (y1 - y0 - qs) / 2;
    int tx = have_qr ? x0 + qs + 14 : x0;
    int tw = x1 - tx;

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    /* sub line ("PLA  -  Sunlu") with the small note ("#A99388") after it, or under it if it doesn't fit */
    lv_point_t ps, pn;
    lv_text_get_size(&ps, in->sub, FONT_M, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&pn, in->note, FONT_S, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    bool note_inline = !in->note[0] || !in->sub[0] || ps.x + 10 + pn.x <= tw;
    int lm = lv_font_get_line_height(FONT_M), ls = lv_font_get_line_height(FONT_S);
    int sub_h = (in->sub[0] || in->note[0] ? lm : 0) + (note_inline ? 0 : ls);
    const lv_font_t *fb = ui_font_bold(22);
    int temps_h = (in->ext_temp > 0) * (lv_font_get_line_height(fb) + 2) + (in->bed_temp > 0) * (lv_font_get_line_height(fb) + 4);
    int rest = 2 + sub_h + 6 + 8 + temps_h + (in->foot[0] ? lm : 0);

    /* name: the biggest size whose (at most two) lines leave room for the rest above the bottom edge */
    static const int SIZES[] = { 30, 26, 22 };
    const lv_font_t *fn = NULL;
    int name_h = 0;
    for (int i = 0; i < 3; i++) {
        fn = ui_font_bold(SIZES[i]);
        int lh = lv_font_get_line_height(fn), h = text_h(in->name, fn, tw);
        name_h = h < 2 * lh ? h : 2 * lh;
        if (h <= 2 * lh && 2 + name_h + rest <= y1 - y0) break;
        if (i == 2 && 2 + name_h + rest > y1 - y0) name_h = lh;   /* still too tall: one line */
    }
    int y = y0 + 2;
    char t[96];
    lv_area_t clip = layer._clip_area;
    layer._clip_area.y2 = y + name_h - 1;   /* cut the name at name_h */
    draw_text(&layer, in->name, fn, tx, y, tw);
    layer._clip_area = clip;
    y += name_h + 2;
    if (in->sub[0]) draw_text(&layer, in->sub, FONT_M, tx, y, tw);
    if (in->note[0]) {
        if (note_inline) draw_text(&layer, in->note, FONT_S, tx + (in->sub[0] ? ps.x + 10 : 0), y + lm - ls - 1, tw);
        else draw_text(&layer, in->note, FONT_S, tx, y + lm, tw);
    }
    y += sub_h + 6;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_black();
    lv_area_t ra = { tx, y, x1, y + 1 };
    lv_draw_rect(&layer, &r, &ra);
    y += 8;
    if (in->ext_temp > 0) { snprintf(t, sizeof(t), "Nozzle %d°C", in->ext_temp); y += draw_text(&layer, t, fb, tx, y, tw) + 2; }
    if (in->bed_temp > 0) { snprintf(t, sizeof(t), "Bed %d°C", in->bed_temp); y += draw_text(&layer, t, fb, tx, y, tw) + 4; }
    draw_text(&layer, in->foot, FONT_M, tx, y, tw);
    lv_canvas_finish_layer(cv, &layer);

    /* 1 bit per dot (TSPL BITMAP: 1 = white), QR modules set directly */
    int wb = LW / 8;
    static const char HEAD[] = "SIZE 50 mm,30 mm\r\nGAP 2 mm,0 mm\r\nDENSITY %u\r\nDIRECTION 1\r\nCLS\r\nBITMAP 0,0,%d,%d,0,";
    char head[128];
    int hl = snprintf(head, sizeof(head), HEAD, cfg.density, wb, LH);
    static const char TAIL[] = "\r\nPRINT 1,1\r\n";
    size_t len = hl + wb * LH + sizeof(TAIL) - 1;
    uint8_t *out = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out) {
        memcpy(out, head, hl);
        uint8_t *bits = out + hl;
        for (int yy = 0; yy < LH; yy++) {
            const uint8_t *row = (const uint8_t *)db->data + yy * db->header.stride;
            for (int xb = 0; xb < wb; xb++) {
                uint8_t b = 0;
                for (int k = 0; k < 8; k++) {
                    int xx = xb * 8 + k;
                    bool black = row[xx] < 128;
                    if (have_qr && xx >= x0 && xx < x0 + qs && yy >= qy && yy < qy + qs)
                        black = qrcodegen_getModule(qr, (xx - x0) / mod, (yy - qy) / mod);
                    if (!black) b |= 0x80 >> k;
                }
                bits[yy * wb + xb] = b;
            }
        }
        memcpy(bits + wb * LH, TAIL, sizeof(TAIL) - 1);
        *len_out = len;
    }
    lv_obj_delete(cv);
    lv_draw_buf_destroy(db);
    return out;
}

/* ── printing with feedback ──────────────────────────────────────────── */
static lv_timer_t *s_watch;
static uint32_t s_watch_seq;
static ui_label_t s_pending;     /* waits here while a printer is being chosen */

static void watch_cb(lv_timer_t *t)
{
    if (label_seq() == s_watch_seq) return;
    char m[100], toast[128];
    label_state_t st = label_state(m, sizeof(m));
    snprintf(toast, sizeof(toast), "%s  %s", st == LABEL_DONE ? ICON_CHECK : ICON_ALERT, m);
    ui_toast(toast);
    lv_timer_delete(t);
    s_watch = NULL;
}

static void print_now(const ui_label_t *l)
{
    size_t len;
    uint8_t *d = ui_label_tspl(l, &len);
    if (!d) { ui_toast(ICON_ALERT "  Out of memory"); return; }
    s_watch_seq = label_seq();
    bool ok = label_print(d, len);
    heap_caps_free(d);
    if (!ok) { ui_toast(ICON_ALERT "  A label is already printing"); return; }
    label_cfg_t c;
    label_cfg_get(&c);
    char m[80];
    snprintf(m, sizeof(m), ICON_SPOOL "  Printing on %s...", c.name);
    ui_toast(m);
    if (!s_watch) s_watch = lv_timer_create(watch_cb, 300, NULL);
}

static void chosen_then_print(void *user) { print_now(&s_pending); }

void ui_label_print(const ui_label_t *l)
{
    label_cfg_t c;
    label_cfg_get(&c);
    if (c.have) { print_now(l); return; }
    s_pending = *l;
    ui_label_choose(chosen_then_print, NULL);
}

/* ── printer chooser ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *ov, *list, *status;
    lv_timer_t *tm;
    label_dev_t dev[LABEL_SCAN_MAX];
    int n, shown;
    void (*done)(void *);
    void *user;
} chooser_t;

static chooser_t *s_ch;

static void ch_deleted(lv_event_t *e)
{
    chooser_t *c = lv_event_get_user_data(e);
    if (c->tm) lv_timer_delete(c->tm);
    lv_free(c);
    if (s_ch == c) s_ch = NULL;
}

static void ch_close(lv_event_t *e) { if (s_ch) ui_modal_close(s_ch->ov); }

static void ch_pick(lv_event_t *e)
{
    chooser_t *c = s_ch;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!c || i < 0 || i >= c->n) return;
    label_cfg_t cfg;
    label_cfg_get(&cfg);
    cfg.have = true;
    strlcpy(cfg.name, c->dev[i].name, sizeof(cfg.name));
    memcpy(cfg.addr, c->dev[i].addr, 6);
    cfg.addr_type = c->dev[i].addr_type;
    label_cfg_set(&cfg);
    char m[80];
    snprintf(m, sizeof(m), ICON_CHECK "  Label printer: %s", cfg.name);
    ui_toast(m);
    void (*done)(void *) = c->done;
    void *user = c->user;
    ui_modal_close(c->ov);
    if (done) done(user);
}

static void ch_rescan(lv_event_t *e) { label_scan(6); if (s_ch) s_ch->shown = -1; }

static void ch_tick(lv_timer_t *t)
{
    chooser_t *c = s_ch;
    if (!c) return;
    char m[100];
    label_state_t st = label_state(m, sizeof(m));
    c->n = label_scan_results(c->dev, LABEL_SCAN_MAX);
    if (st == LABEL_SCANNING || st == LABEL_STARTING) {
        char s[120];
        snprintf(s, sizeof(s), "%s  %d found so far. Turn the printer on and keep it close.", m, c->n);
        lv_label_set_text(c->status, s);
    } else lv_label_set_text(c->status, st == LABEL_ERROR ? m : c->n ? "Tap your printer." : "Nothing found. Is the printer on? Tap Search again.");
    /* redraw when the list or its order changes */
    uint32_t sig = c->n;
    for (int i = 0; i < c->n; i++) sig = sig * 31 + c->dev[i].addr[0] + (c->dev[i].addr[1] << 8);
    if ((int)sig == c->shown) return;
    c->shown = (int)sig;
    lv_obj_clean(c->list);
    for (int i = 0; i < c->n; i++) {
        lv_obj_t *r = lv_obj_create(c->list);
        ui_list_row(r, 64);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_bg_color(r, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, ch_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char sub[48];
        const uint8_t *a = c->dev[i].addr;
        snprintf(sub, sizeof(sub), "%02X:%02X:%02X:%02X:%02X:%02X  -  signal %d dBm", a[5], a[4], a[3], a[2], a[1], a[0],
                 c->dev[i].rssi);
        bool lp = label_looks_like_printer(c->dev[i].name);
        char title[64];
        snprintf(title, sizeof(title), "%s%s", c->dev[i].name, lp ? "   " ICON_CHECK " label printer" : "");
        ui_two_line(r, FONT_M, lp ? COL_ACCENT : COL_TEXT, title, sub);
    }
}

void ui_label_choose(void (*done)(void *user), void *user)
{
    if (s_ch) return;
    chooser_t *c = lv_malloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->done = done;
    c->user = user;
    c->shown = -1;
    s_ch = c;
    c->ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(c->ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(c->ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(c->ov, 0, 0);
    lv_obj_set_style_radius(c->ov, 0, 0);
    lv_obj_remove_flag(c->ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c->ov, ch_deleted, LV_EVENT_DELETE, c);
    ui_modal_track(c->ov);
    lv_obj_t *b = ui_card(c->ov);
    int h = ui_height() + 40;
    lv_obj_set_size(b, ui_is_portrait() ? LV_PCT(94) : 720, h > 600 ? 600 : h);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, 18, 0);
    lv_obj_set_style_pad_gap(b, 12, 0);
    lv_obj_t *hr = lv_obj_create(b);
    lv_obj_set_size(hr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hr, 0, 0);
    lv_obj_set_style_border_width(hr, 0, 0);
    lv_obj_set_style_pad_all(hr, 0, 0);
    lv_obj_set_style_pad_gap(hr, 12, 0);
    lv_obj_set_flex_flow(hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = ui_label(hr, FONT_L, COL_TEXT, ICON_SPOOL "  Choose the label printer");
    lv_obj_set_flex_grow(tl, 1);
    ui_button(hr, ICON_SEARCH, "Search", COL_CARD2, ch_rescan, NULL);
    ui_button(hr, ICON_CLOSE, "Cancel", COL_CARD2, ch_close, NULL);
    c->status = ui_label(b, FONT_S, COL_MUTED, "Starting Bluetooth...");
    lv_label_set_long_mode(c->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c->status, LV_PCT(100));
    c->list = lv_obj_create(b);
    lv_obj_set_width(c->list, LV_PCT(100));
    lv_obj_set_flex_grow(c->list, 1);
    lv_obj_set_style_bg_opa(c->list, 0, 0);
    lv_obj_set_style_border_width(c->list, 0, 0);
    lv_obj_set_style_pad_all(c->list, 0, 0);
    lv_obj_set_style_pad_gap(c->list, 8, 0);
    lv_obj_set_flex_flow(c->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(c->list, LV_DIR_VER);
    c->tm = lv_timer_create(ch_tick, 400, NULL);
    label_scan(6);
}

/* Settings' and the console's test label */
void ui_label_test(void)
{
    ui_label_t l = {0};
    strcpy(l.qr, "web+spoolman:s-0");
    strcpy(l.name, "Test label");
    strcpy(l.sub, "PLA  -  Klipper Screen");
    l.ext_temp = 210;
    l.bed_temp = 60;
    strcpy(l.note, "#FF6600");
    strcpy(l.foot, "Spool #0");
    ui_label_print(&l);
}
