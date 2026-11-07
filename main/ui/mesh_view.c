/*
 * Bed mesh viewer. See mesh_view.h.
 *
 * 3D: orthographic projection of the bed (grey plane + grid) with the probed
 * mesh on it as a shaded, colour-mapped surface (Catmull-Rom smoothed),
 * probe-grid wireframe on top. Height is exaggerated so a 0.05 mm mesh is
 * visible. Quads are sorted back to front (painter's algorithm). While
 * dragging a coarse version is drawn for speed; the full one on release.
 * Top: one tile per probe point with its value, front of the bed at the bottom.
 *
 * Draw tasks are flushed every BATCH primitives: LVGL's dispatcher checks each
 * task against all queued ones, which gets slow with thousands queued.
 */
#include "mesh_view.h"
#include "ui.h"
#include "icons.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define LEG_W       76      /* colour scale strip on the left */
#define BATCH       160
#define MAX_QUADS   2400
#define DRAG_MS     70

typedef struct {
    lv_obj_t *root, *canvas, *tip, *modes;
    lv_draw_buf_t *buf;
    int w, h;
    mesh_t m;
    bool has;
    float lo, hi;
    int sel_r, sel_c;                       /* selected probe point, -1 = none */
    int16_t px[MESH_MAX][MESH_MAX], py[MESH_MAX][MESH_MAX];   /* where probe points were drawn */
    lv_point_t press;
    bool moved, pending;
    float yaw0, pitch0;
    int64_t last;
    lv_timer_t *timer;
} mv_t;

/* view survives tab switches */
static bool  s_top;
static float s_yaw = -0.55f, s_pitch = 0.95f;   /* radians; pitch 0 = straight down */

/* ── colours ─────────────────────────────────────────────────────────── */
/* blue (low) -> light blue -> pale yellow -> orange -> red (high), like Mainsail */
static const uint32_t STOPS[] = { 0x313695, 0x4575B4, 0x74ADD1, 0xE0F3F8, 0xFFFFBF, 0xFDAE61, 0xF46D43, 0xA50026 };
#define NSTOPS (sizeof(STOPS) / sizeof(STOPS[0]))

static lv_color_t ramp(float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float f = t * (NSTOPS - 1);
    int i = (int)f;
    if (i >= (int)NSTOPS - 1) return lv_color_hex(STOPS[NSTOPS - 1]);
    return lv_color_mix(lv_color_hex(STOPS[i + 1]), lv_color_hex(STOPS[i]), (uint8_t)((f - i) * 255));
}

static lv_color_t zcol(const mv_t *v, float z)
{
    return ramp(v->hi > v->lo ? (z - v->lo) / (v->hi - v->lo) : 0.5f);
}

/* ── geometry ────────────────────────────────────────────────────────── */
static float cr(float p0, float p1, float p2, float p3, float t)   /* Catmull-Rom */
{
    return 0.5f * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (3 * p1 - p0 - 3 * p2 + p3) * t * t * t);
}

static float at(const mesh_t *m, int r, int c)
{
    if (r < 0) r = 0;
    if (r >= m->rows) r = m->rows - 1;
    if (c < 0) c = 0;
    if (c >= m->cols) c = m->cols - 1;
    return m->z[r][c];
}

/* u in [0, cols-1], v in [0, rows-1] */
static float sample(const mesh_t *m, float u, float v)
{
    int c = (int)u, r = (int)v;
    if (c >= m->cols - 1) c = m->cols - 2;
    if (r >= m->rows - 1) r = m->rows - 2;
    float tu = u - c, tv = v - r, row[4];
    for (int k = 0; k < 4; k++)
        row[k] = cr(at(m, r - 1 + k, c - 1), at(m, r - 1 + k, c), at(m, r - 1 + k, c + 1), at(m, r - 1 + k, c + 2), tu);
    return cr(row[0], row[1], row[2], row[3], tv);
}

typedef struct {
    float cx, cy, zc, k, s, ox, oy, cyaw, syaw, cp, sp;
} proj_t;

typedef struct { float x, y, d; } pt_t;

static pt_t project(const proj_t *p, float x, float y, float z)
{
    float X = x - p->cx, Y = y - p->cy, Z = z * p->k;
    float xr = X * p->cyaw - Y * p->syaw;
    float yr = X * p->syaw + Y * p->cyaw;
    pt_t o = {
        .x = p->ox + xr * p->s,
        .y = p->oy - (yr * p->cp + (Z - p->zc) * p->sp) * p->s,
        .d = -yr * p->sp + Z * p->cp,
    };
    return o;
}

/* ── drawing helpers ─────────────────────────────────────────────────── */
typedef struct {
    mv_t *v;
    lv_layer_t layer;
    int n;
} pen_t;

static void pen_open(pen_t *pn, mv_t *v) { pn->v = v; pn->n = 0; lv_canvas_init_layer(v->canvas, &pn->layer); }
static void pen_flush(pen_t *pn) { lv_canvas_finish_layer(pn->v->canvas, &pn->layer); lv_canvas_init_layer(pn->v->canvas, &pn->layer); pn->n = 0; }
static void pen_close(pen_t *pn) { lv_canvas_finish_layer(pn->v->canvas, &pn->layer); }
static void pen_tick(pen_t *pn) { if (++pn->n >= BATCH) pen_flush(pn); }

static void tri(pen_t *pn, pt_t a, pt_t b, pt_t c, lv_color_t col, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = (int32_t)lroundf(a.x); d.p[0].y = (int32_t)lroundf(a.y);
    d.p[1].x = (int32_t)lroundf(b.x); d.p[1].y = (int32_t)lroundf(b.y);
    d.p[2].x = (int32_t)lroundf(c.x); d.p[2].y = (int32_t)lroundf(c.y);
    d.color = col;
    d.opa = opa;
    lv_draw_triangle(&pn->layer, &d);
    pen_tick(pn);
}

/* quad, grown 0.6 px around its centre so neighbours overlap (no hairline seams) */
static void quad(pen_t *pn, pt_t a, pt_t b, pt_t c, pt_t d, lv_color_t col, lv_opa_t opa)
{
    float mx = (a.x + b.x + c.x + d.x) / 4, my = (a.y + b.y + c.y + d.y) / 4;
    pt_t *q[4] = { &a, &b, &c, &d };
    for (int i = 0; i < 4; i++) {
        float dx = q[i]->x - mx, dy = q[i]->y - my, l = sqrtf(dx * dx + dy * dy);
        if (l > 0.01f) { q[i]->x += dx / l * 0.6f; q[i]->y += dy / l * 0.6f; }
    }
    tri(pn, a, b, c, col, opa);
    tri(pn, a, c, d, col, opa);
}

static void line(pen_t *pn, pt_t a, pt_t b, lv_color_t col, lv_opa_t opa, int w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = a.x; d.p1.y = a.y;
    d.p2.x = b.x; d.p2.y = b.y;
    d.color = col;
    d.opa = opa;
    d.width = w;
    lv_draw_line(&pn->layer, &d);
    pen_tick(pn);
}

static void text(pen_t *pn, int x, int y, int w, lv_text_align_t al, lv_color_t col, const lv_font_t *f, const char *t)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;   /* copied: callers pass stack buffers */
    d.font = f;
    d.color = col;
    d.align = al;
    lv_area_t a = { x, y, x + w - 1, y + lv_font_get_line_height(f) };
    lv_draw_label(&pn->layer, &d, &a);
    pen_tick(pn);
}

static void rect(pen_t *pn, int x1, int y1, int x2, int y2, lv_color_t col, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(&pn->layer, &d, &a);
    pen_tick(pn);
}

static void dot(pen_t *pn, int x, int y)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_white();
    d.radius = LV_RADIUS_CIRCLE;
    d.border_color = lv_color_black();
    d.border_width = 2;
    lv_area_t a = { x - 7, y - 7, x + 7, y + 7 };
    lv_draw_rect(&pn->layer, &d, &a);
    pen_tick(pn);
}

/* colour scale with max / mid / min labels */
static void legend(pen_t *pn, mv_t *v)
{
    int top = 34, bot = v->h - 34, x1 = 14, x2 = 30, n = 48;
    for (int i = 0; i < n; i++) {
        int y1 = top + (bot - top) * i / n, y2 = top + (bot - top) * (i + 1) / n;
        rect(pn, x1, y1, x2, y2, ramp(1.0f - (i + 0.5f) / n), 0, LV_OPA_COVER);
    }
    char b[16];
    snprintf(b, sizeof(b), "%+.3f", v->hi);
    text(pn, 4, top - 26, LEG_W, LV_TEXT_ALIGN_LEFT, COL_TEXT, FONT_S, b);
    snprintf(b, sizeof(b), "%+.3f", v->lo);
    text(pn, 4, bot + 6, LEG_W, LV_TEXT_ALIGN_LEFT, COL_TEXT, FONT_S, b);
    snprintf(b, sizeof(b), "%+.3f", (v->hi + v->lo) / 2);
    text(pn, x2 + 4, (top + bot) / 2 - 9, LEG_W - x2 - 4, LV_TEXT_ALIGN_LEFT, COL_MUTED, &lv_font_montserrat_14, b);
}

/* ── renderers ───────────────────────────────────────────────────────── */
typedef struct { float d; uint16_t i, j; } qd_t;

static int qd_cmp(const void *a, const void *b)
{
    float x = ((const qd_t *)a)->d, y = ((const qd_t *)b)->d;
    return x < y ? -1 : x > y;
}

static void bed_bounds(const mesh_t *m, float *x0, float *y0, float *x1, float *y1)
{
    if (m->has_bed && m->bed_max_x > m->bed_min_x && m->bed_max_y > m->bed_min_y) {
        *x0 = m->bed_min_x; *y0 = m->bed_min_y; *x1 = m->bed_max_x; *y1 = m->bed_max_y;
        /* the mesh may lie partly outside the travel limits: include it */
        if (m->min_x < *x0) *x0 = m->min_x;
        if (m->min_y < *y0) *y0 = m->min_y;
        if (m->max_x > *x1) *x1 = m->max_x;
        if (m->max_y > *y1) *y1 = m->max_y;
    } else {
        float mx = (m->max_x - m->min_x) * 0.1f, my = (m->max_y - m->min_y) * 0.1f;
        *x0 = m->min_x - mx; *y0 = m->min_y - my; *x1 = m->max_x + mx; *y1 = m->max_y + my;
    }
}

static void render_3d(mv_t *v, pen_t *pn, bool coarse)
{
    const mesh_t *m = &v->m;
    float bx0, by0, bx1, by1;
    bed_bounds(m, &bx0, &by0, &bx1, &by1);
    float size = fmaxf(bx1 - bx0, by1 - by0), diag = size * 1.4142f;
    float zr = fmaxf(v->hi - v->lo, 0.02f);

    proj_t p = {
        .cx = (bx0 + bx1) / 2, .cy = (by0 + by1) / 2,
        .k = 0.22f * size / zr,                        /* height exaggeration */
        .cyaw = cosf(s_yaw), .syaw = sinf(s_yaw), .cp = cosf(s_pitch), .sp = sinf(s_pitch),
    };
    float zlo = fminf(v->lo, 0) * p.k, zhi = fmaxf(v->hi, 0) * p.k;
    p.zc = (zlo + zhi) / 2;
    int pw = v->w - LEG_W, ph = v->h;
    /* fixed scale for every yaw so rotating doesn't zoom */
    float sw = pw * 0.94f / diag, sh = ph * 0.86f / (diag * p.cp + (zhi - zlo) * p.sp + 1);
    p.s = fminf(sw, sh);
    p.ox = LEG_W + pw / 2.0f;
    p.oy = ph / 2.0f;

    /* bed plane + grid */
    lv_color_t bed = lv_color_mix(COL_TEXT, COL_CARD, 40);
    quad(pn, project(&p, bx0, by0, 0), project(&p, bx1, by0, 0), project(&p, bx1, by1, 0), project(&p, bx0, by1, 0), bed, LV_OPA_80);
    float step = size > 400 ? 100 : 50;
    lv_color_t gl = lv_color_mix(COL_TEXT, COL_CARD, 90);
    for (float x = ceilf(bx0 / step) * step; x <= bx1 + 0.01f; x += step)
        line(pn, project(&p, x, by0, 0), project(&p, x, by1, 0), gl, LV_OPA_70, 1);
    for (float y = ceilf(by0 / step) * step; y <= by1 + 0.01f; y += step)
        line(pn, project(&p, bx0, y, 0), project(&p, bx1, y, 0), gl, LV_OPA_70, 1);
    pt_t c0 = project(&p, bx0, by0, 0), c1 = project(&p, bx1, by0, 0), c2 = project(&p, bx1, by1, 0), c3 = project(&p, bx0, by1, 0);
    line(pn, c0, c1, COL_ACCENT, LV_OPA_COVER, 3);   /* front edge highlighted */
    line(pn, c1, c2, gl, LV_OPA_COVER, 2);
    line(pn, c2, c3, gl, LV_OPA_COVER, 2);
    line(pn, c3, c0, gl, LV_OPA_COVER, 2);

    /* surface */
    int S = coarse ? 1 : (int)sqrtf((float)MAX_QUADS / ((m->cols - 1) * (m->rows - 1)));
    if (S < 1) S = 1;
    if (S > 6) S = 6;
    int nx = (m->cols - 1) * S + 1, ny = (m->rows - 1) * S + 1;
    pt_t *vp = heap_caps_malloc(nx * ny * sizeof(pt_t), MALLOC_CAP_SPIRAM);
    float *vz = heap_caps_malloc(nx * ny * sizeof(float), MALLOC_CAP_SPIRAM);
    qd_t *qs = heap_caps_malloc((nx - 1) * (ny - 1) * sizeof(qd_t), MALLOC_CAP_SPIRAM);
    if (vp && vz && qs) {
        float dxm = (m->max_x - m->min_x) / (nx - 1), dym = (m->max_y - m->min_y) / (ny - 1);
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                float z = (S == 1) ? m->z[j][i] : sample(m, (float)i / S, (float)j / S);
                vz[j * nx + i] = z;
                vp[j * nx + i] = project(&p, m->min_x + i * dxm, m->min_y + j * dym, z);
            }
        int nq = 0;
        for (int j = 0; j < ny - 1; j++)
            for (int i = 0; i < nx - 1; i++) {
                qs[nq].d = (vp[j * nx + i].d + vp[j * nx + i + 1].d + vp[(j + 1) * nx + i].d + vp[(j + 1) * nx + i + 1].d) / 4;
                qs[nq].i = i;
                qs[nq].j = j;
                nq++;
            }
        qsort(qs, nq, sizeof(qd_t), qd_cmp);
        /* light from the front-left, above */
        const float L[3] = { -0.35f, -0.45f, 0.82f };
        lv_color_t wire = lv_color_black();
        for (int q = 0; q < nq; q++) {
            int i = qs[q].i, j = qs[q].j;
            int a = j * nx + i, b = a + 1, c = a + nx + 1, d = a + nx;
            float zav = (vz[a] + vz[b] + vz[c] + vz[d]) / 4;
            /* normal of the exaggerated surface */
            float gx = ((vz[b] + vz[c]) - (vz[a] + vz[d])) / 2 * p.k / dxm;
            float gy = ((vz[c] + vz[d]) - (vz[a] + vz[b])) / 2 * p.k / dym;
            float inv = 1.0f / sqrtf(gx * gx + gy * gy + 1);
            float dotl = (-gx * L[0] - gy * L[1] + L[2]) * inv;
            float shade = 0.62f + 0.38f * fmaxf(dotl, 0);
            lv_color_t col = lv_color_mix(zcol(v, zav), lv_color_black(), (uint8_t)(shade * 255));
            quad(pn, vp[a], vp[b], vp[c], vp[d], col, LV_OPA_COVER);
            /* probe-grid wireframe, drawn with its quad so hidden parts stay hidden */
            if (i % S == 0) line(pn, vp[a], vp[d], wire, LV_OPA_40, 1);
            if (j % S == 0) line(pn, vp[a], vp[b], wire, LV_OPA_40, 1);
            if (i == nx - 2) line(pn, vp[b], vp[c], wire, LV_OPA_40, 1);
            if (j == ny - 2) line(pn, vp[d], vp[c], wire, LV_OPA_40, 1);
        }
        for (int r = 0; r < m->rows; r++)
            for (int c = 0; c < m->cols; c++) {
                pt_t q = vp[(r * S) * nx + c * S];
                v->px[r][c] = (int16_t)q.x;
                v->py[r][c] = (int16_t)q.y;
            }
    }
    heap_caps_free(vp);
    heap_caps_free(vz);
    heap_caps_free(qs);

    /* axis labels: X along the front edge, Y along the right edge */
    pt_t xl = project(&p, (bx0 + bx1) / 2, by0 - size * 0.09f, 0);
    pt_t yl = project(&p, bx1 + size * 0.09f, (by0 + by1) / 2, 0);
    text(pn, (int)xl.x - 60, (int)xl.y - 10, 120, LV_TEXT_ALIGN_CENTER, COL_ACCENT, FONT_S, "Front (X)");
    text(pn, (int)yl.x - 20, (int)yl.y - 10, 40, LV_TEXT_ALIGN_CENTER, COL_MUTED, FONT_S, "Y");
}

static void render_top(mv_t *v, pen_t *pn)
{
    const mesh_t *m = &v->m;
    int pw = v->w - LEG_W - 8, ph = v->h - 30, gap = 4;
    int cell = (pw - (m->cols - 1) * gap) / m->cols;
    int ch = (ph - (m->rows - 1) * gap) / m->rows;
    if (ch < cell) cell = ch;
    if (cell > 72) cell = 72;
    int gw = m->cols * cell + (m->cols - 1) * gap, gh = m->rows * cell + (m->rows - 1) * gap;
    int x0 = LEG_W + (pw - gw) / 2, y0 = (ph - gh) / 2;
    char b[12];
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            int x = x0 + c * (cell + gap), y = y0 + (m->rows - 1 - r) * (cell + gap);   /* front at the bottom */
            lv_color_t col = zcol(v, m->z[r][c]);
            rect(pn, x, y, x + cell - 1, y + cell - 1, col, 6, LV_OPA_COVER);
            if (cell >= 40) {
                snprintf(b, sizeof(b), "%.3f", m->z[r][c]);
                const lv_font_t *f = cell >= 60 ? FONT_S : &lv_font_montserrat_14;
                text(pn, x, y + cell / 2 - lv_font_get_line_height(f) / 2, cell, LV_TEXT_ALIGN_CENTER, ui_contrast(col), f, b);
            }
            v->px[r][c] = x + cell / 2;
            v->py[r][c] = y + cell / 2;
        }
    text(pn, x0, y0 + gh + 6, gw, LV_TEXT_ALIGN_CENTER, COL_ACCENT, FONT_S, "Front");
}

static void place_tip(mv_t *v)
{
    if (v->sel_r < 0 || !v->has || v->sel_r >= v->m.rows || v->sel_c >= v->m.cols) {
        lv_obj_add_flag(v->tip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const mesh_t *m = &v->m;
    float x = m->min_x + v->sel_c * (m->max_x - m->min_x) / (m->cols - 1);
    float y = m->min_y + v->sel_r * (m->max_y - m->min_y) / (m->rows - 1);
    char b[80];
    snprintf(b, sizeof(b), "X %.1f   Y %.1f\nZ %+.3f mm", x, y, m->z[v->sel_r][v->sel_c]);
    lv_label_set_text(v->tip, b);
    lv_obj_remove_flag(v->tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(v->tip);
    int tw = lv_obj_get_width(v->tip), th = lv_obj_get_height(v->tip);
    int tx = v->px[v->sel_r][v->sel_c] + 14, ty = v->py[v->sel_r][v->sel_c] - th - 10;
    if (tx + tw > v->w) tx = v->px[v->sel_r][v->sel_c] - tw - 14;
    if (ty < 0) ty = v->py[v->sel_r][v->sel_c] + 14;
    lv_obj_set_pos(v->tip, tx, ty + lv_obj_get_y(v->canvas));
}

static void render(mv_t *v, bool coarse)
{
    v->last = esp_timer_get_time();
    v->pending = false;
    lv_canvas_fill_bg(v->canvas, COL_CARD, LV_OPA_COVER);
    pen_t pn;
    pen_open(&pn, v);
    if (!v->has || v->m.rows < 2 || v->m.cols < 2) {
        text(&pn, 0, v->h / 2 - 12, v->w, LV_TEXT_ALIGN_CENTER, COL_MUTED, FONT_M, "No mesh loaded");
        pen_close(&pn);
        lv_obj_add_flag(v->tip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    legend(&pn, v);
    if (s_top) render_top(v, &pn);
    else render_3d(v, &pn, coarse);
    if (v->sel_r >= 0 && !coarse) dot(&pn, v->px[v->sel_r][v->sel_c], v->py[v->sel_r][v->sel_c]);
    pen_close(&pn);
    if (coarse) lv_obj_add_flag(v->tip, LV_OBJ_FLAG_HIDDEN);
    else place_tip(v);
}

/* ── input ───────────────────────────────────────────────────────────── */
static void canvas_cb(lv_event_t *e)
{
    mv_t *v = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || !v->has) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    lv_area_t a;
    lv_obj_get_coords(v->canvas, &a);
    pt.x -= a.x1;
    pt.y -= a.y1;
    if (code == LV_EVENT_PRESSED) {
        v->press = pt;
        v->moved = false;
        v->yaw0 = s_yaw;
        v->pitch0 = s_pitch;
    } else if (code == LV_EVENT_PRESSING) {
        int dx = pt.x - v->press.x, dy = pt.y - v->press.y;
        if (!v->moved && abs(dx) + abs(dy) > 10) v->moved = true;
        if (!v->moved || s_top) return;
        s_yaw = v->yaw0 + dx * 0.009f;
        s_pitch = v->pitch0 - dy * 0.007f;
        if (s_pitch < 0.0f) s_pitch = 0.0f;
        if (s_pitch > 1.40f) s_pitch = 1.40f;
        if (esp_timer_get_time() - v->last > DRAG_MS * 1000) render(v, true);
        else v->pending = true;
    } else if (code == LV_EVENT_RELEASED) {
        if (!v->moved) {   /* tap: nearest probe point */
            int best = 60 * 60, br = -1, bc = -1;
            for (int r = 0; r < v->m.rows; r++)
                for (int c = 0; c < v->m.cols; c++) {
                    int dx = v->px[r][c] - pt.x, dy = v->py[r][c] - pt.y;
                    if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; br = r; bc = c; }
                }
            v->sel_r = (br == v->sel_r && bc == v->sel_c) ? -1 : br;   /* tap again to hide */
            v->sel_c = bc;
        }
        render(v, false);
    }
}

static void timer_cb(lv_timer_t *t)
{
    mv_t *v = lv_timer_get_user_data(t);
    if (v->pending) render(v, true);
}

static void mode_cb(lv_event_t *e)
{
    mv_t *v = lv_event_get_user_data(e);
    s_top = lv_buttonmatrix_get_selected_button(v->modes) == 1;
    render(v, false);
}

static void reset_cb(lv_event_t *e)
{
    mv_t *v = lv_event_get_user_data(e);
    s_yaw = -0.55f;
    s_pitch = 0.95f;
    v->sel_r = -1;
    render(v, false);
}

static void delete_cb(lv_event_t *e)
{
    mv_t *v = lv_event_get_user_data(e);
    if (v->timer) lv_timer_delete(v->timer);
    if (v->buf) lv_draw_buf_destroy(v->buf);
    heap_caps_free(v);
}

static const char *const MODES[] = { ICON_CUBE "  3D", ICON_MESH "  Top", "" };

lv_obj_t *mesh_view_create(lv_obj_t *parent, int height)
{
    mv_t *v = heap_caps_calloc(1, sizeof(mv_t), MALLOC_CAP_SPIRAM);
    if (!v) return NULL;
    v->sel_r = v->sel_c = -1;
    lv_obj_update_layout(parent);
    v->w = lv_obj_get_content_width(parent);
    v->h = height;

    v->root = lv_obj_create(parent);
    lv_obj_set_size(v->root, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(v->root, 0, 0);
    lv_obj_set_style_border_width(v->root, 0, 0);
    lv_obj_set_style_pad_all(v->root, 0, 0);
    lv_obj_set_style_pad_gap(v->root, 8, 0);
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(v->root, v);
    lv_obj_add_event_cb(v->root, delete_cb, LV_EVENT_DELETE, v);

    lv_obj_t *bar = lv_obj_create(v->root);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_gap(bar, 10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    v->modes = lv_buttonmatrix_create(bar);
    lv_buttonmatrix_set_map(v->modes, MODES);
    lv_buttonmatrix_set_button_ctrl_all(v->modes, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(v->modes, true);
    lv_buttonmatrix_set_button_ctrl(v->modes, s_top ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(v->modes, 250, 56);
    lv_obj_set_style_pad_all(v->modes, 4, 0);
    lv_obj_set_style_text_font(v->modes, FONT_M, 0);
    lv_obj_set_style_bg_color(v->modes, COL_CARD2, 0);
    lv_obj_set_style_border_width(v->modes, 0, 0);
    lv_obj_set_style_bg_color(v->modes, COL_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v->modes, COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(v->modes, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_color(v->modes, ui_contrast(COL_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(v->modes, mode_cb, LV_EVENT_VALUE_CHANGED, v);
    lv_obj_t *rb = ui_button(bar, ICON_RESTART, "Reset view", COL_CARD2, reset_cb, v);
    lv_obj_set_style_min_height(rb, 56, 0);
    lv_obj_t *hint = ui_label(bar, FONT_S, COL_MUTED, "Drag to turn, tap a point");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(hint, 1);

    v->buf = lv_draw_buf_create(v->w, v->h, LV_COLOR_FORMAT_RGB565, 0);
    v->canvas = lv_canvas_create(v->root);
    if (v->buf) lv_canvas_set_draw_buf(v->canvas, v->buf);
    lv_obj_add_flag(v->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v->canvas, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);   /* drags turn the mesh, not the page */
    lv_obj_add_event_cb(v->canvas, canvas_cb, LV_EVENT_PRESSED, v);
    lv_obj_add_event_cb(v->canvas, canvas_cb, LV_EVENT_PRESSING, v);
    lv_obj_add_event_cb(v->canvas, canvas_cb, LV_EVENT_RELEASED, v);

    v->tip = ui_label(v->root, FONT_S, lv_color_white(), "");
    lv_obj_add_flag(v->tip, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(v->tip, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(v->tip, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(v->tip, 8, 0);
    lv_obj_set_style_radius(v->tip, 8, 0);

    v->timer = lv_timer_create(timer_cb, DRAG_MS, v);
    if (v->buf) render(v, false);
    return v->root;
}

void mesh_view_set(lv_obj_t *view, const mesh_t *m)
{
    if (!view) return;
    mv_t *v = lv_obj_get_user_data(view);
    if (!v || !v->buf) return;
    if (v->has && !memcmp(&v->m, m, sizeof(*m))) return;   /* unchanged: keep the picture (and the view) */
    bool shape = !v->has || v->m.rows != m->rows || v->m.cols != m->cols;
    v->m = *m;
    v->has = true;
    v->lo = v->hi = m->rows ? m->z[0][0] : 0;
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            if (m->z[r][c] < v->lo) v->lo = m->z[r][c];
            if (m->z[r][c] > v->hi) v->hi = m->z[r][c];
        }
    if (shape) v->sel_r = -1;
    render(v, false);
}
