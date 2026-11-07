/*
 * Single-printer view with a left sidebar:
 *   Status (job, temperatures, quick controls) | Move | MMU | Macros | Camera
 * Only the selected tab's widgets exist; switching tabs deletes the old ones.
 */
#include "ui.h"
#include "icons.h"
#include "ui_thumb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "printers.h"
#include "wifi_mgr.h"

typedef enum { TAB_STATUS = 0, TAB_MOVE, TAB_EXTRUDE, TAB_MMU, TAB_MACROS, TAB_COUNT } tab_t;

#define SIDEBAR_W 104
#define PATH_N 5

/* ── state ───────────────────────────────────────────────────────────── */
static int s_idx;
static printer_state_t s_st;
static uint32_t s_seq;
static int s_tick;
static tab_t s_tab;
static lv_obj_t *s_panel;
static lv_obj_t *s_side_btn[TAB_COUNT], *s_cam_btn, *s_spool_btn;
static int s_step_i = 2, s_ext_i = 1;     /* remembered while the page is open */

/* Per-tab widgets. Zeroed whenever the tab is rebuilt. */
typedef struct { lv_obj_t *row, *name, *cur, *tgt, *bar; } heater_row_t;
typedef struct { lv_obj_t *box, *swatch, *name, *tool, *mat, *status; } lane_w_t;

static struct {
    /* status tab */
    lv_obj_t *chip, *chip_lbl, *file, *thumb, *pct, *bar, *msg, *job_box;
    lv_obj_t *stat[6];
    lv_obj_t *btn_pause, *btn_pause_lbl, *btn_cancel, *btn_restart, *btn_fwrestart;
    lv_obj_t *heat_card;
    heater_row_t hrows[MAX_HEATERS];
    int nhrows;
    lv_obj_t *mmu_summary;
    ui_thumb_t th;
    /* move tab */
    lv_obj_t *pos[3], *homed, *ext_temp, *ext_hint;
    /* mmu tab */
    lv_obj_t *mmu_title, *mmu_info, *mmu_grid;
    lv_obj_t *path_node[PATH_N], *path_lbl[PATH_N], *path_link[PATH_N - 1];
    lane_w_t lanes[MAX_LANES];
    int nlanes_shown;
    /* macros tab */
    lv_obj_t *macro_grid;
    int nmacros_shown;
} W;

static const char *stat_names[6] = { ICON_CLOCK "  Elapsed", ICON_TIMER "  Remaining", ICON_ETA "  ETA",
                                      ICON_LAYERS "  Layer", ICON_SPOOL "  Filament", ICON_SPEED "  Speed" };
static const float STEPS[] = { 0.1f, 1, 10, 50 };
static const int EXTS[] = { 1, 5, 10, 25, 50 };
static const int ESPEEDS[] = { 1, 2, 5, 10 };   /* extrude speed, mm/s */
static int s_espd_i = 1;

/* ── helpers ─────────────────────────────────────────────────────────── */
static lv_obj_t *plain(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_gap(o, 12, 0);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *c = ui_card(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    if (title) ui_section_title(c, title);
    return c;
}

static lv_obj_t *grow_btn(lv_obj_t *parent, const char *sym, const char *txt, lv_color_t c, lv_event_cb_t cb, void *u)
{
    lv_obj_t *b = ui_button(parent, sym, txt, c, cb, u);
    lv_obj_set_flex_grow(b, 1);
    return b;
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else if (!on && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static int panel_width(void) { return ui_width() - 32 - SIDEBAR_W - 16; }
static bool busy_printing(void) { return s_st.status == PS_PRINTING; }

/* ── actions ─────────────────────────────────────────────────────────── */
static void do_cmd(void *user) { printers_command(s_idx, (printer_cmd_t)(intptr_t)user, NULL); }
static void do_gcode(void *user) { printers_command(s_idx, CMD_GCODE, (const char *)user); }
static void gcode_now(const char *g) { printers_command(s_idx, CMD_GCODE, g); }

static void pause_cb(lv_event_t *e)
{
    if (s_st.status == PS_PAUSED) printers_command(s_idx, CMD_RESUME, NULL);
    else ui_confirm("Pause print?", "The print will pause at the next safe point.", "Pause", COL_WARN, do_cmd, (void *)CMD_PAUSE);
}
static void cancel_cb(lv_event_t *e)
{
    ui_confirm("Cancel print?", "This stops the current job and cannot be undone.", "Cancel print", COL_ERR, do_cmd, (void *)CMD_CANCEL);
}
static void estop_cb(lv_event_t *e) { printers_command(s_idx, CMD_ESTOP, NULL); ui_toast("Emergency stop sent"); }
static void restart_cb(lv_event_t *e) { printers_command(s_idx, CMD_KLIPPER_RESTART, NULL); }
static void fwrestart_cb(lv_event_t *e) { printers_command(s_idx, CMD_FIRMWARE_RESTART, NULL); }
static void heaters_off_cb(lv_event_t *e)
{
    ui_confirm("Turn off all heaters?", "Sets every heater target to 0.", "Turn off", COL_WARN, do_gcode, "TURN_OFF_HEATERS");
}
static void motors_cb(lv_event_t *e)
{
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    ui_confirm("Disable motors?", "Steppers will be released and the printer will need homing again.", "Disable", COL_WARN, do_gcode, "M84");
}
static void home_cb(lv_event_t *e)
{
    const char *g = lv_event_get_user_data(e);
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    gcode_now(g);
    ui_toast("Homing...");
}
static void level_cb(lv_event_t *e)
{
    const char *g = lv_event_get_user_data(e);
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    ui_confirm("Run leveling?", g, "Run", COL_ACCENT, do_gcode, (void *)g);
}

static void set_temp(float v, void *user)
{
    int h = (int)(intptr_t)user;
    if (h >= s_st.nheaters) return;
    const heater_t *ht = &s_st.heaters[h];
    char g[128];
    const char *sp = strchr(ht->obj, ' ');
    if (!strncmp(ht->obj, "temperature_fan ", 16))
        snprintf(g, sizeof(g), "SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=%s TARGET=%.0f", sp + 1, v);
    else
        snprintf(g, sizeof(g), "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%.0f", sp ? sp + 1 : ht->obj, v);
    gcode_now(g);
}

static void heater_click(lv_event_t *e)
{
    int h = (int)(intptr_t)lv_event_get_user_data(e);
    if (h >= s_st.nheaters || !s_st.heaters[h].settable) return;
    static const int hot[] = { 0, 190, 210, 230, 250, 270 };
    static const int bed[] = { 0, 55, 60, 70, 90, 110 };
    static const int cham[] = { 0, 35, 45, 55, 60 };
    const heater_t *ht = &s_st.heaters[h];
    char title[48];
    snprintf(title, sizeof(title), "%s target (°C)", ht->label);
    const int *p = ht->kind == 0 ? hot : ht->kind == 1 ? bed : cham;
    int n = ht->kind == 2 ? 5 : 6;
    ui_numpad(title, ht->target, p, n, set_temp, (void *)(intptr_t)h);
}

/* jog: user data = axis char | (sign << 8) */
static void jog_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    char axis = (char)(v & 0xFF);
    int sign = (v >> 8) ? -1 : 1;
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    char g[96];
    float d = STEPS[s_step_i] * sign;
    int f = axis == 'Z' ? 600 : 6000;
    snprintf(g, sizeof(g), "G91\nG1 %c%.2f F%d\nG90", axis, d, f);
    gcode_now(g);
}

static void extrude_cb(lv_event_t *e)
{
    int sign = (int)(intptr_t)lv_event_get_user_data(e);
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    float t = 0;
    for (int i = 0; i < s_st.nheaters; i++) if (!strcmp(s_st.heaters[i].obj, "extruder")) t = s_st.heaters[i].temp;
    if (t < 170) { ui_toast("Hotend is below 170°C - heat it first"); return; }
    char g[64];
    snprintf(g, sizeof(g), "M83\nG1 E%d F%d", sign * EXTS[s_ext_i], ESPEEDS[s_espd_i] * 60);
    gcode_now(g);
}

static void step_cb(lv_event_t *e) { s_step_i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)); }
static void ext_cb(lv_event_t *e) { s_ext_i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)); }

static void macro_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_st.nmacros) return;
    static char g[32];
    strlcpy(g, s_st.macros[i], sizeof(g));
    char t[96];
    snprintf(t, sizeof(t), "Run %s on this printer?", g);
    ui_confirm("Run macro?", t, "Run", COL_ACCENT, do_gcode, g);
}

/* MMU lane actions */
static char s_lane_gcode[96];
static void lane_click(lv_event_t *e)
{
    int l = (int)(intptr_t)lv_event_get_user_data(e);
    if (l >= s_st.mmu.nlanes) return;
    if (busy_printing()) { ui_toast("Lane changes are disabled while printing"); return; }
    const mmu_lane_t *ln = &s_st.mmu.lanes[l];
    char txt[160];
    if (s_st.mmu.type == MMU_AFC) {
        if (ln->loaded) {
            strcpy(s_lane_gcode, "TOOL_UNLOAD");
            snprintf(txt, sizeof(txt), "%s is loaded in the toolhead. Unload it?", ln->name);
            ui_confirm(ln->name, txt, "Unload", COL_WARN, do_gcode, s_lane_gcode);
        } else {
            snprintf(s_lane_gcode, sizeof(s_lane_gcode), "CHANGE_TOOL LANE=%s", ln->name);
            snprintf(txt, sizeof(txt), "Load %s (%s %s) to the toolhead?", ln->name, ln->material, ln->tool);
            ui_confirm(ln->name, txt, "Load", COL_ACCENT, do_gcode, s_lane_gcode);
        }
    } else {
        if (ln->loaded) {
            strcpy(s_lane_gcode, "MMU_UNLOAD");
            snprintf(txt, sizeof(txt), "%s is loaded. Unload filament?", ln->name);
            ui_confirm(ln->name, txt, "Unload", COL_WARN, do_gcode, s_lane_gcode);
        } else {
            if (ln->tool[0]) snprintf(s_lane_gcode, sizeof(s_lane_gcode), "MMU_CHANGE_TOOL TOOL=%s", ln->tool + 1);
            else snprintf(s_lane_gcode, sizeof(s_lane_gcode), "MMU_SELECT GATE=%d", l);
            snprintf(txt, sizeof(txt), "Load %s (%s)?", ln->name, ln->material[0] ? ln->material : "unknown material");
            ui_confirm(ln->name, txt, "Load", COL_ACCENT, do_gcode, s_lane_gcode);
        }
    }
}

static void mmu_unload_cb(lv_event_t *e)
{
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    ui_confirm("Unload toolhead?", "Unloads the current filament back to its lane.", "Unload", COL_WARN, do_gcode,
               s_st.mmu.type == MMU_AFC ? "TOOL_UNLOAD" : "MMU_UNLOAD");
}
static void mmu_home_cb(lv_event_t *e)
{
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    ui_confirm("Home MMU?", "Runs MMU_HOME.", "Home", COL_ACCENT, do_gcode, "MMU_HOME");
}

/* ── STATUS tab ──────────────────────────────────────────────────────── */
static void build_heaters(void)
{
    lv_obj_clean(W.heat_card);
    W.nhrows = 0;
    lv_obj_t *t = plain(W.heat_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_section_title(t, "TEMPERATURES  (tap to set)");
    for (int i = 0; i < s_st.nheaters; i++) {
        heater_row_t *h = &W.hrows[W.nhrows++];
        h->row = lv_obj_create(W.heat_card);
        lv_obj_set_size(h->row, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(h->row, COL_CARD2, 0);
        lv_obj_set_style_bg_color(h->row, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(h->row, 0, 0);
        lv_obj_set_style_radius(h->row, 10, 0);
        lv_obj_set_style_pad_hor(h->row, 14, 0);
        lv_obj_set_style_pad_ver(h->row, 6, 0);
        lv_obj_remove_flag(h->row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(h->row, heater_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char nm[48];
        uint8_t k = s_st.heaters[i].kind;
        snprintf(nm, sizeof(nm), "%s  %s", k == 0 ? ICON_HOTEND : k == 1 ? ICON_BED : k == 2 ? ICON_CHAMBER : ICON_THERMO,
                 s_st.heaters[i].label);
        h->name = ui_label(h->row, FONT_M, COL_MUTED, nm);
        lv_obj_align(h->name, LV_ALIGN_LEFT_MID, 0, -8);
        h->cur = ui_label(h->row, FONT_XL, COL_TEXT, "--");
        lv_obj_align(h->cur, LV_ALIGN_RIGHT_MID, -130, 0);
        h->tgt = ui_label(h->row, FONT_M, COL_MUTED, "");
        lv_obj_set_width(h->tgt, 120);
        lv_obj_set_style_text_align(h->tgt, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(h->tgt, LV_ALIGN_RIGHT_MID, 0, 0);
        h->bar = lv_bar_create(h->row);
        lv_obj_set_size(h->bar, 120, 6);
        lv_bar_set_range(h->bar, 0, 100);
        lv_obj_align(h->bar, LV_ALIGN_LEFT_MID, 0, 18);
        lv_obj_set_style_bg_color(h->bar, COL_HOT, LV_PART_INDICATOR);
        lv_obj_remove_flag(h->bar, LV_OBJ_FLAG_CLICKABLE);
        if (!s_st.heaters[i].settable) lv_obj_add_flag(h->bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_status(lv_obj_t *p)
{
    bool portrait = ui_is_portrait();
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_t *left = plain(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *right = plain(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(left, 16, 0);
    lv_obj_set_style_pad_gap(right, 16, 0);
    if (!portrait) {
        int w = panel_width() - 16;
        lv_obj_set_width(left, w * 55 / 100);
        lv_obj_set_width(right, w - w * 55 / 100);
    }

    lv_obj_t *c = card(left, NULL);
    lv_obj_t *r = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    W.chip = lv_obj_create(r);
    lv_obj_set_size(W.chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(W.chip, 20, 0);
    lv_obj_set_style_border_width(W.chip, 0, 0);
    lv_obj_set_style_pad_hor(W.chip, 14, 0);
    lv_obj_set_style_pad_ver(W.chip, 6, 0);
    W.chip_lbl = ui_label(W.chip, FONT_M, lv_color_black(), "");
    W.file = ui_label(r, FONT_M, COL_TEXT, "");
    lv_label_set_long_mode(W.file, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(W.file, 1);

    W.msg = ui_label(c, FONT_S, COL_ERR, "");
    lv_label_set_long_mode(W.msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(W.msg, LV_PCT(100));

    W.job_box = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(W.job_box, 16, 0);
    W.thumb = lv_image_create(W.job_box);
    lv_obj_add_flag(W.thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *jc = plain(W.job_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(jc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(jc, 1);
    W.pct = ui_label(jc, FONT_XXL, COL_TEXT, "0%");
    W.bar = lv_bar_create(jc);
    lv_obj_set_size(W.bar, LV_PCT(100), 20);
    lv_bar_set_range(W.bar, 0, 1000);
    lv_obj_t *grid = plain(jc, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 8, 0);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *cell = plain(grid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(cell, LV_PCT(31));
        lv_obj_set_style_pad_gap(cell, 2, 0);
        ui_label(cell, FONT_S, COL_MUTED, stat_names[i]);
        W.stat[i] = ui_label(cell, FONT_L, COL_TEXT, "--");
    }
    lv_obj_t *br = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(br, 14, 0);
    W.btn_pause = grow_btn(br, ICON_PAUSE, "Pause", COL_WARN, pause_cb, NULL);
    W.btn_pause_lbl = lv_obj_get_child(W.btn_pause, 0);
    W.btn_cancel = grow_btn(br, ICON_CANCEL, "Cancel", COL_ERR, cancel_cb, NULL);
    W.btn_restart = grow_btn(br, ICON_RESTART, "Restart", COL_CARD2, restart_cb, NULL);
    W.btn_fwrestart = grow_btn(br, ICON_RESTART, "Firmware restart", COL_WARN, fwrestart_cb, NULL);

    W.mmu_summary = ui_label(left, FONT_S, COL_MUTED, "");
    lv_label_set_long_mode(W.mmu_summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(W.mmu_summary, LV_PCT(100));

    W.heat_card = card(right, NULL);
    build_heaters();

    lv_obj_t *cc = card(right, "QUICK CONTROLS");
    lv_obj_set_style_pad_gap(cc, 14, 0);
    lv_obj_t *r1 = plain(cc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(r1, 14, 0);
    ui_tile_button(r1, ICON_HOME, "Home all", COL_ACCENT, home_cb, "G28");
    ui_tile_button(r1, ICON_MOTOR_OFF, "Motors off", COL_CARD2, motors_cb, NULL);
    ui_tile_button(r1, ICON_HEAT_OFF, "Heaters off", COL_CARD2, heaters_off_cb, NULL);
    lv_obj_t *r2 = plain(cc, LV_FLEX_FLOW_ROW);
    lv_obj_t *es = ui_tile_button(r2, ICON_ESTOP, "EMERGENCY STOP", COL_ERR, estop_cb, NULL);
    lv_obj_set_flex_flow(es, LV_FLEX_FLOW_ROW);   /* wide bar: icon beside the text */
    lv_obj_set_style_pad_gap(es, 16, 0);
    lv_obj_set_height(es, 84);
    lv_obj_set_style_text_font(lv_obj_get_child(es, 1), FONT_L, 0);

    memset(&W.th, 0, sizeof(W.th));
    W.th.ver = UINT32_MAX;
}

static void refresh_status(bool fresh)
{
    ui_thumb_update(&W.th, W.thumb, s_idx, ui_is_portrait() ? 200 : 160);
    if (!fresh) return;
    char b[200];
    lv_obj_set_style_bg_color(W.chip, ui_status_color(s_st.status), 0);
    ui_set_text(W.chip_lbl, printer_status_str(s_st.status));
    bool job = s_st.status == PS_PRINTING || s_st.status == PS_PAUSED;
    bool down = s_st.status == PS_OFFLINE || s_st.status == PS_KLIPPY_DOWN;
    const char *fn = strrchr(s_st.filename, '/');
    ui_set_text(W.file, s_st.filename[0] ? (fn ? fn + 1 : s_st.filename) : (down ? "" : "No active job"));
    bool err = down || s_st.status == PS_ERROR;
    ui_set_text(W.msg, err ? s_st.message : "");
    show(W.msg, err && s_st.message[0]);

    show(W.job_box, !down && (job || s_st.filename[0]));
    snprintf(b, sizeof(b), "%d%%", (int)(s_st.progress * 100));
    ui_set_text(W.pct, b);
    lv_bar_set_value(W.bar, (int)(s_st.progress * 1000), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(W.bar, ui_status_color(s_st.status), LV_PART_INDICATOR);
    ui_fmt_duration(b, sizeof(b), s_st.print_duration);  ui_set_text(W.stat[0], b);
    ui_fmt_duration(b, sizeof(b), job ? s_st.eta_s : -1); ui_set_text(W.stat[1], b);
    if (job && s_st.eta_s >= 0 && wifi_mgr_time_valid()) {
        time_t end = time(NULL) + (time_t)s_st.eta_s;
        struct tm tm;
        localtime_r(&end, &tm);
        strftime(b, sizeof(b), "%H:%M", &tm);
    } else strcpy(b, "--");
    ui_set_text(W.stat[2], b);
    if (s_st.layer >= 0 && s_st.layer_total > 0) snprintf(b, sizeof(b), "%d / %d", s_st.layer, s_st.layer_total);
    else if (s_st.layer >= 0) snprintf(b, sizeof(b), "%d", s_st.layer);
    else strcpy(b, "--");
    ui_set_text(W.stat[3], b);
    snprintf(b, sizeof(b), "%.2f m", s_st.filament_used / 1000.0f);
    ui_set_text(W.stat[4], b);
    snprintf(b, sizeof(b), "%d%%", (int)(s_st.speed_factor * 100 + 0.5f));
    ui_set_text(W.stat[5], b);

    show(W.btn_pause, job);
    show(W.btn_cancel, job);
    ui_set_text(W.btn_pause_lbl, s_st.status == PS_PAUSED ? ICON_PLAY "  Resume" : ICON_PAUSE "  Pause");
    lv_obj_set_style_bg_color(W.btn_pause, s_st.status == PS_PAUSED ? COL_OK : COL_WARN, 0);
    show(W.btn_restart, s_st.status == PS_KLIPPY_DOWN);
    show(W.btn_fwrestart, s_st.status == PS_KLIPPY_DOWN);

    if (s_st.nheaters != W.nhrows) build_heaters();
    for (int i = 0; i < W.nhrows; i++) {
        const heater_t *h = &s_st.heaters[i];
        heater_row_t *r = &W.hrows[i];
        snprintf(b, sizeof(b), down ? "--" : "%.1f°", h->temp);
        ui_set_text(r->cur, b);
        if (h->settable) {
            if (h->target > 0) snprintf(b, sizeof(b), "/ %.0f°", h->target);
            else strcpy(b, "off");
            ui_set_text(r->tgt, b);
            lv_bar_set_value(r->bar, h->power >= 0 ? (int)(h->power * 100) : 0, LV_ANIM_OFF);
            lv_obj_set_style_text_color(r->cur, h->target > 0 ? COL_HOT : COL_TEXT, 0);
        } else ui_set_text(r->tgt, "");
    }

    const mmu_info_t *m = &s_st.mmu;
    if (m->type != MMU_NONE && m->nlanes) {
        snprintf(b, sizeof(b), ICON_MMU "  %s: %s loaded  -  see the MMU tab",
                 m->type == MMU_AFC ? "AFC" : "Happy Hare", m->current);
        ui_set_text(W.mmu_summary, b);
    } else ui_set_text(W.mmu_summary, "");
}

/* ── MOVE tab ────────────────────────────────────────────────────────── */
static lv_obj_t *jog_btn(lv_obj_t *parent, const char *txt, char axis, int neg, int w)
{
    lv_obj_t *b = ui_button(parent, NULL, txt, COL_CARD2, jog_cb, (void *)(intptr_t)(axis | (neg << 8)));
    lv_obj_set_size(b, w, 84);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), FONT_L, 0);
    return b;
}

static lv_obj_t *selector(lv_obj_t *parent, const char **map, int sel, lv_event_cb_t cb)
{
    lv_obj_t *bm = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(bm, map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_buttonmatrix_set_button_ctrl(bm, sel, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, LV_PCT(100), 64);
    lv_obj_set_style_text_font(bm, FONT_M, 0);
    lv_obj_set_style_bg_opa(bm, 0, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return bm;
}

static void build_move(lv_obj_t *p)
{
    bool portrait = ui_is_portrait();
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_t *left = plain(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *right = plain(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(left, 16, 0);
    lv_obj_set_style_pad_gap(right, 16, 0);
    if (!portrait) {
        int w = panel_width() - 16;
        lv_obj_set_width(left, w * 55 / 100);
        lv_obj_set_width(right, w - w * 55 / 100);
    }

    /* jog pad */
    lv_obj_t *jc = card(left, "JOG  (relative moves)");
    static const char *step_map[] = { "0.1", "1", "10", "50 mm", "" };
    selector(jc, step_map, s_step_i, step_cb);
    lv_obj_t *pad = plain(jc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *xy = lv_obj_create(pad);
    lv_obj_set_size(xy, 3 * 96 + 2 * 8, 3 * 84 + 2 * 8);
    lv_obj_set_style_bg_opa(xy, 0, 0);
    lv_obj_set_style_border_width(xy, 0, 0);
    lv_obj_set_style_pad_all(xy, 0, 0);
    lv_obj_remove_flag(xy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b;
    b = jog_btn(xy, "Y+", 'Y', 0, 96); lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 0);
    b = jog_btn(xy, "X-", 'X', 1, 96); lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
    b = jog_btn(xy, "X+", 'X', 0, 96); lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    b = jog_btn(xy, "Y-", 'Y', 1, 96); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
    b = ui_button(xy, ICON_HOME, "XY", COL_ACCENT, home_cb, "G28 X Y");
    lv_obj_set_size(b, 96, 84);
    lv_obj_center(b);
    lv_obj_t *zc = plain(pad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(zc, 96);
    lv_obj_set_style_pad_gap(zc, 8, 0);
    jog_btn(zc, "Z+", 'Z', 0, 96);
    b = ui_button(zc, ICON_HOME, "Z", COL_ACCENT, home_cb, "G28 Z");
    lv_obj_set_size(b, 96, 84);
    jog_btn(zc, "Z-", 'Z', 1, 96);

    /* position + homing */
    lv_obj_t *pc = card(right, "POSITION");
    lv_obj_t *pr = plain(pc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pr, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *ax[3] = { "X", "Y", "Z" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *col = plain(pr, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(col, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(col, 0, 0);
        ui_label(col, FONT_S, COL_MUTED, ax[i]);
        W.pos[i] = ui_label(col, FONT_XL, COL_TEXT, "--");
    }
    W.homed = ui_label(pc, FONT_S, COL_MUTED, "");
    lv_obj_t *hr = plain(pc, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(hr, 8, 0);
    lv_obj_set_style_pad_gap(hr, 12, 0);
    ui_tile_button(hr, ICON_HOME, "Home all", COL_ACCENT, home_cb, "G28");
    ui_tile_button(hr, ICON_HOME_AXIS, "X", COL_CARD2, home_cb, "G28 X");
    ui_tile_button(hr, ICON_HOME_AXIS, "Y", COL_CARD2, home_cb, "G28 Y");
    ui_tile_button(hr, ICON_HOME_AXIS, "Z", COL_CARD2, home_cb, "G28 Z");
    lv_obj_t *lr = plain(pc, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(lr, 8, 0);
    lv_obj_set_style_pad_gap(lr, 12, 0);
    if (s_st.has_qgl) ui_tile_button(lr, ICON_LEVEL, "Gantry level", COL_CARD2, level_cb, "QUAD_GANTRY_LEVEL");
    if (s_st.has_ztilt) ui_tile_button(lr, ICON_LEVEL, "Z tilt", COL_CARD2, level_cb, "Z_TILT_ADJUST");
    ui_tile_button(lr, ICON_MOTOR_OFF, "Motors off", COL_CARD2, motors_cb, NULL);
}

static void refresh_move(bool fresh)
{
    if (!fresh) return;
    char b[64];
    bool down = s_st.status == PS_OFFLINE || s_st.status == PS_KLIPPY_DOWN;
    for (int i = 0; i < 3; i++) {
        snprintf(b, sizeof(b), down ? "--" : (i == 2 ? "%.2f" : "%.1f"), s_st.pos[i]);
        ui_set_text(W.pos[i], b);
        char a = "xyz"[i];
        bool h = strchr(s_st.homed, a) != NULL;
        lv_obj_set_style_text_color(W.pos[i], h ? COL_TEXT : COL_WARN, 0);
    }
    snprintf(b, sizeof(b), "Homed: %s", s_st.homed[0] ? s_st.homed : "none (orange = not homed)");
    ui_set_text(W.homed, b);
}

/* ── EXTRUDE tab ─────────────────────────────────────────────────────── */
static void espd_cb(lv_event_t *e) { s_espd_i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)); }

static void preheat_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    char g[80];
    snprintf(g, sizeof(g), "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=%d", t);
    gcode_now(g);
}

static const char *find_macro(const char *name)
{
    for (int i = 0; i < s_st.nmacros; i++) if (!strcasecmp(s_st.macros[i], name)) return s_st.macros[i];
    return NULL;
}

static void filament_macro_cb(lv_event_t *e)
{
    const char *m = lv_event_get_user_data(e);
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    static char g[32];
    strlcpy(g, m, sizeof(g));
    char t[80];
    snprintf(t, sizeof(t), "Run %s?", g);
    ui_confirm(g, t, "Run", COL_ACCENT, do_gcode, g);
}

static void build_extrude(lv_obj_t *p)
{
    bool portrait = ui_is_portrait();
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_t *left = plain(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *right = plain(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(left, 16, 0);
    lv_obj_set_style_pad_gap(right, 16, 0);
    if (!portrait) {
        int w = panel_width() - 16;
        lv_obj_set_width(left, w / 2);
        lv_obj_set_width(right, w - w / 2);
    }

    /* hotend */
    lv_obj_t *hc = card(left, "HOTEND");
    W.ext_temp = ui_label(hc, FONT_XXL, COL_TEXT, "--");
    W.ext_hint = ui_label(hc, FONT_S, COL_MUTED, "");
    lv_obj_t *pr = plain(hc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(pr, 12, 0);
    ui_tile_button(pr, ICON_HOTEND, "PLA 210°", COL_CARD2, preheat_cb, (void *)(intptr_t)210);
    ui_tile_button(pr, ICON_HOTEND, "PETG 240°", COL_CARD2, preheat_cb, (void *)(intptr_t)240);
    ui_tile_button(pr, ICON_HOTEND, "ABS 255°", COL_CARD2, preheat_cb, (void *)(intptr_t)255);
    ui_tile_button(pr, ICON_HEAT_OFF, "Off", COL_CARD2, preheat_cb, (void *)(intptr_t)0);

    /* load / unload macros if the printer has them */
    const char *lm = find_macro("LOAD_FILAMENT"), *um = find_macro("UNLOAD_FILAMENT");
    if (lm || um) {
        lv_obj_t *mc = card(left, "FILAMENT");
        lv_obj_t *mr = plain(mc, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(mr, 12, 0);
        if (lm) ui_tile_button(mr, ICON_LOAD, "Load filament", COL_CARD2, filament_macro_cb, (void *)lm);
        if (um) ui_tile_button(mr, ICON_UNLOAD, "Unload filament", COL_CARD2, filament_macro_cb, (void *)um);
    }

    /* manual extrude */
    lv_obj_t *ec = card(right, "EXTRUDE / RETRACT");
    ui_label(ec, FONT_S, COL_MUTED, "Length");
    static const char *ext_map[] = { "1", "5", "10", "25", "50 mm", "" };
    selector(ec, ext_map, s_ext_i, ext_cb);
    ui_label(ec, FONT_S, COL_MUTED, "Speed");
    static const char *spd_map[] = { "1", "2", "5", "10 mm/s", "" };
    selector(ec, spd_map, s_espd_i, espd_cb);
    lv_obj_t *er = plain(ec, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(er, 12, 0);
    ui_tile_button(er, ICON_RETRACT, "Retract", COL_CARD2, extrude_cb, (void *)(intptr_t)-1);
    ui_tile_button(er, ICON_EXTRUDE, "Extrude", COL_ACCENT, extrude_cb, (void *)(intptr_t)1);
}

static void refresh_extrude(bool fresh)
{
    if (!fresh) return;
    char b[96];
    for (int i = 0; i < s_st.nheaters; i++)
        if (!strcmp(s_st.heaters[i].obj, "extruder")) {
            const heater_t *h = &s_st.heaters[i];
            if (h->target > 0) snprintf(b, sizeof(b), ICON_HOTEND " %.0f° / %.0f°", h->temp, h->target);
            else snprintf(b, sizeof(b), ICON_HOTEND " %.0f°", h->temp);
            ui_set_text(W.ext_temp, b);
            lv_obj_set_style_text_color(W.ext_temp, h->target > 0 ? COL_HOT : COL_TEXT, 0);
            ui_set_text(W.ext_hint, h->temp < 170 ? ICON_ALERT "  Heat to at least 170° to extrude" : ICON_CHECK "  Hot enough to extrude");
        }
}

/* ── MMU tab ─────────────────────────────────────────────────────────── */
static void build_mmu(lv_obj_t *p)
{
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *c = card(p, NULL);
    lv_obj_t *tr = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    W.mmu_title = ui_label(tr, FONT_L, COL_TEXT, "Multi-material");
    lv_obj_t *ar = plain(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(ar, LV_SIZE_CONTENT);
    if (s_st.mmu.type == MMU_HAPPY_HARE) ui_button(ar, ICON_HOME_AXIS, "Home MMU", COL_CARD2, mmu_home_cb, NULL);
    ui_button(ar, ICON_UNLOAD, "Unload", COL_WARN, mmu_unload_cb, NULL);
    W.mmu_info = ui_label(c, FONT_M, COL_MUTED, "");
    lv_label_set_long_mode(W.mmu_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(W.mmu_info, LV_PCT(100));

    /* filament path: Lane -> Hub/Selector -> Bowden -> Extruder -> Nozzle */
    static const char *names[PATH_N] = { "Lane", "Hub", "Bowden", "Extruder", "Nozzle" };
    lv_obj_t *row = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 12, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int i = 0; i < PATH_N; i++) {
        lv_obj_t *cell = plain(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(cell, 110);
        lv_obj_set_style_pad_gap(cell, 6, 0);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        W.path_node[i] = lv_obj_create(cell);
        lv_obj_set_size(W.path_node[i], 52, 52);
        lv_obj_set_style_radius(W.path_node[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(W.path_node[i], 3, 0);
        lv_obj_set_style_border_color(W.path_node[i], COL_BORDER, 0);
        lv_obj_set_style_bg_color(W.path_node[i], COL_CARD2, 0);
        lv_obj_remove_flag(W.path_node[i], LV_OBJ_FLAG_SCROLLABLE);
        W.path_lbl[i] = ui_label(cell, FONT_S, COL_MUTED, names[i]);
        lv_label_set_long_mode(W.path_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(W.path_lbl[i], 110, 0);
        if (i < PATH_N - 1) {
            W.path_link[i] = lv_obj_create(row);
            lv_obj_set_height(W.path_link[i], 8);
            lv_obj_set_flex_grow(W.path_link[i], 1);
            lv_obj_set_style_radius(W.path_link[i], 4, 0);
            lv_obj_set_style_border_width(W.path_link[i], 0, 0);
            lv_obj_set_style_bg_color(W.path_link[i], COL_BORDER, 0);
            lv_obj_set_style_margin_top(W.path_link[i], 22, 0);
        }
    }

    lv_obj_t *lc = card(p, "LANES  (tap to load / unload)");
    W.mmu_grid = plain(lc, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(W.mmu_grid, 10, 0);
    W.nlanes_shown = -1;
}

static void build_lanes(void)
{
    lv_obj_clean(W.mmu_grid);
    W.nlanes_shown = s_st.mmu.nlanes;
    for (int i = 0; i < W.nlanes_shown; i++) {
        lane_w_t *w = &W.lanes[i];
        w->box = lv_obj_create(W.mmu_grid);
        lv_obj_set_size(w->box, 150, 168);
        lv_obj_set_style_bg_color(w->box, COL_CARD2, 0);
        lv_obj_set_style_bg_color(w->box, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(w->box, 3, 0);
        lv_obj_set_style_border_color(w->box, COL_CARD2, 0);
        lv_obj_set_style_radius(w->box, 12, 0);
        lv_obj_set_style_pad_all(w->box, 8, 0);
        lv_obj_set_style_pad_gap(w->box, 2, 0);
        lv_obj_set_flex_flow(w->box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(w->box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(w->box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(w->box, lane_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        w->swatch = lv_obj_create(w->box);
        lv_obj_set_size(w->swatch, 52, 52);
        lv_obj_set_style_radius(w->swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(w->swatch, 2, 0);
        lv_obj_set_style_border_color(w->swatch, COL_MUTED, 0);
        lv_obj_remove_flag(w->swatch, LV_OBJ_FLAG_CLICKABLE);
        w->name = ui_label(w->box, FONT_M, COL_TEXT, "");
        w->tool = ui_label(w->box, FONT_S, COL_ACCENT, "");
        w->mat = ui_label(w->box, FONT_S, COL_MUTED, "");
        w->status = ui_label(w->box, &lv_font_montserrat_14, COL_MUTED, "");
        lv_label_set_long_mode(w->mat, LV_LABEL_LONG_DOT);
        lv_obj_set_width(w->mat, LV_PCT(100));
        lv_obj_set_style_text_align(w->mat, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void refresh_mmu(bool fresh)
{
    if (!fresh) return;
    const mmu_info_t *m = &s_st.mmu;
    char b[200];
    const char *sys = m->type == MMU_AFC ? "AFC" : m->type == MMU_HAPPY_HARE ? "Happy Hare" : "No MMU";
    bool same = !m->unit[0] || !strcasecmp(m->unit, sys);
    snprintf(b, sizeof(b), "%s%s%s", sys, same ? "" : "  -  ", same ? "" : m->unit);
    ui_set_text(W.mmu_title, b);
    static const char *stage_txt[] = { "parked in lane", "at hub/selector", "in bowden", "in extruder", "loaded to nozzle" };
    snprintf(b, sizeof(b), "Current: %s   -   Filament %s%s%s", m->current,
             m->stage >= 0 && m->stage <= 4 ? stage_txt[m->stage] : (m->filament[0] ? m->filament : "unknown"),
             m->action[0] ? "   -   " : "", m->action);
    ui_set_text(W.mmu_info, b);

    lv_color_t fc = COL_ACCENT;
    bool act = m->active_lane >= 0 && m->active_lane < m->nlanes;
    if (act && m->lanes[m->active_lane].has_color) fc = lv_color_hex(m->lanes[m->active_lane].color);
    ui_set_text(W.path_lbl[0], act ? m->lanes[m->active_lane].name : "Lane");
    ui_set_text(W.path_lbl[1], m->hub_name[0] ? m->hub_name : "Hub");
    for (int i = 0; i < PATH_N; i++) {
        bool lit = act && m->stage >= i;
        lv_obj_set_style_bg_color(W.path_node[i], lit ? fc : COL_CARD2, 0);
        lv_obj_set_style_border_color(W.path_node[i], lit ? lv_color_white() : COL_BORDER, 0);
        lv_obj_set_style_text_color(W.path_lbl[i], lit ? COL_TEXT : COL_MUTED, 0);
        if (i < PATH_N - 1) lv_obj_set_style_bg_color(W.path_link[i], act && m->stage >= i + 1 ? fc : COL_BORDER, 0);
    }

    if (m->nlanes != W.nlanes_shown) build_lanes();
    for (int i = 0; i < W.nlanes_shown; i++) {
        const mmu_lane_t *ln = &m->lanes[i];
        lane_w_t *w = &W.lanes[i];
        ui_set_text(w->name, ln->name);
        ui_set_text(w->tool, ln->tool[0] ? ln->tool : "-");
        snprintf(b, sizeof(b), "%s", ln->material[0] ? ln->material : (ln->present ? "?" : "empty"));
        if (ln->weight_g > 0) {
            char wt[16];
            snprintf(wt, sizeof(wt), "  %dg", ln->weight_g);
            strlcat(b, wt, sizeof(b));
        }
        ui_set_text(w->mat, b);
        ui_set_text(w->status, ln->loaded ? "IN TOOLHEAD" : ln->status);
        lv_obj_set_style_text_color(w->status, ln->loaded ? COL_OK : COL_MUTED, 0);
        if (ln->present && ln->has_color) {
            lv_obj_set_style_bg_color(w->swatch, lv_color_hex(ln->color), 0);
            lv_obj_set_style_bg_opa(w->swatch, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(w->swatch, ln->present ? LV_OPA_50 : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(w->swatch, COL_MUTED, 0);
        }
        lv_obj_set_style_border_color(w->box, ln->loaded ? COL_OK : ln->active ? COL_ACCENT : COL_CARD2, 0);
        lv_obj_set_style_opa(w->box, ln->present ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

/* ── MACROS tab ──────────────────────────────────────────────────────── */
static void build_macro_buttons(void)
{
    lv_obj_clean(W.macro_grid);
    W.nmacros_shown = s_st.nmacros;
    int cols = ui_is_portrait() ? 2 : 3;
    int bw = (panel_width() - 32 - (cols - 1) * 10) / cols;
    for (int i = 0; i < s_st.nmacros; i++) {
        lv_obj_t *b = ui_button(W.macro_grid, NULL, s_st.macros[i], COL_CARD2, macro_cb, (void *)(intptr_t)i);
        lv_obj_set_width(b, bw);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!s_st.nmacros) ui_label(W.macro_grid, FONT_M, COL_MUTED, "No user macros found.");
}

static void build_macros(lv_obj_t *p)
{
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *c = card(p, "MACROS  (macros starting with _ are hidden)");
    W.macro_grid = plain(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(W.macro_grid, 10, 0);
    build_macro_buttons();
}

static void refresh_macros(bool fresh)
{
    if (fresh && s_st.nmacros != W.nmacros_shown) build_macro_buttons();
}

/* ── tabs / sidebar ──────────────────────────────────────────────────── */
static void set_tab(tab_t t)
{
    if (s_tab == TAB_STATUS && W.thumb) {   /* leaving status: drop the thumbnail */
        lv_image_set_src(W.thumb, NULL);
        ui_thumb_free(&W.th);
    }
    s_tab = t;
    lv_obj_clean(s_panel);
    memset(&W, 0, sizeof(W));
    lv_obj_scroll_to_y(s_panel, 0, LV_ANIM_OFF);
    switch (t) {
    case TAB_STATUS: build_status(s_panel); break;
    case TAB_MOVE:   build_move(s_panel); break;
    case TAB_EXTRUDE: build_extrude(s_panel); break;
    case TAB_MMU:    build_mmu(s_panel); break;
    case TAB_MACROS: build_macros(s_panel); break;
    default: break;
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t bg = i == (int)t ? COL_ACCENT : COL_CARD;
        lv_obj_set_style_bg_color(s_side_btn[i], bg, 0);
        lv_obj_set_style_text_color(s_side_btn[i], ui_contrast(bg), 0);
    }
    s_seq = 0;   /* force a full refresh */
}

static void side_cb(lv_event_t *e)
{
    tab_t t = (tab_t)(intptr_t)lv_event_get_user_data(e);
    if (t != s_tab) set_tab(t);
}

static void cam_cb(lv_event_t *e) { ui_nav_push(PAGE_WEBCAM, s_idx * 16); }
static void spool_cb(lv_event_t *e) { ui_nav_push(PAGE_SPOOLMAN, s_idx); }

static lv_obj_t *side_btn(lv_obj_t *bar, const char *sym, const char *txt, lv_event_cb_t cb, void *u)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_size(b, SIDEBAR_W, 80);
    lv_obj_set_style_bg_color(b, COL_CARD, 0);
    lv_obj_set_style_text_color(b, COL_TEXT, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(b, 4, 0);
    lv_obj_t *i = lv_label_create(b);       /* colours inherit from the button (contrast-aware) */
    lv_obj_set_style_text_font(i, FONT_XL, 0);
    lv_label_set_text(i, sym);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, FONT_S, 0);
    lv_label_set_text(l, txt);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, u);
    return b;
}

static void create(lv_obj_t *parent, int idx)
{
    s_idx = idx;
    s_seq = 0;
    s_tick = 0;
    memset(&W, 0, sizeof(W));
    printers_set_focus(idx);   /* this printer now polls fast, with the full query */
    printer_cfg_t cfg;
    if (printers_get_cfg(idx, &cfg)) ui_set_title(cfg.name);
    printers_get_state(idx, &s_st);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(parent, 16, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);   /* the panel scrolls, the sidebar stays */

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SIDEBAR_W, LV_PCT(100));
    lv_obj_set_style_bg_opa(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_gap(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(bar, LV_DIR_VER);
    s_side_btn[TAB_STATUS] = side_btn(bar, ICON_STATUS, "Status", side_cb, (void *)TAB_STATUS);
    s_side_btn[TAB_MOVE]   = side_btn(bar, ICON_MOVE, "Move", side_cb, (void *)TAB_MOVE);
    s_side_btn[TAB_EXTRUDE] = side_btn(bar, ICON_EXTRUDE, "Extrude", side_cb, (void *)TAB_EXTRUDE);
    s_side_btn[TAB_MMU]    = side_btn(bar, ICON_MMU, "MMU", side_cb, (void *)TAB_MMU);
    s_side_btn[TAB_MACROS] = side_btn(bar, ICON_MACRO, "Macros", side_cb, (void *)TAB_MACROS);
    s_spool_btn = side_btn(bar, ICON_SPOOL, "Spools", spool_cb, NULL);
    s_cam_btn = side_btn(bar, ICON_WEBCAM, "Camera", cam_cb, NULL);
    show(s_spool_btn, s_st.has_spoolman);
    show(s_side_btn[TAB_MMU], s_st.mmu.type != MMU_NONE);
    show(s_side_btn[TAB_MACROS], s_st.nmacros > 0);
    show(s_cam_btn, s_st.nwebcams > 0);

    s_panel = lv_obj_create(parent);
    lv_obj_set_height(s_panel, LV_PCT(100));
    lv_obj_set_flex_grow(s_panel, 1);
    lv_obj_set_style_bg_opa(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_pad_gap(s_panel, 16, 0);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);

    s_tab = TAB_STATUS;
    set_tab(TAB_STATUS);
}

static void refresh(void)
{
    if (!printers_get_state(s_idx, &s_st)) { ui_nav_home(); return; }
    bool fresh = s_st.seq != s_seq || s_seq == 0 || (++s_tick % 8) == 0;
    s_seq = s_st.seq;

    show(s_side_btn[TAB_MMU], s_st.mmu.type != MMU_NONE);
    show(s_side_btn[TAB_MACROS], s_st.nmacros > 0);
    show(s_cam_btn, s_st.nwebcams > 0);
    show(s_spool_btn, s_st.has_spoolman);

    switch (s_tab) {
    case TAB_STATUS: refresh_status(fresh); break;
    case TAB_MOVE:   refresh_move(fresh); break;
    case TAB_EXTRUDE: refresh_extrude(fresh); break;
    case TAB_MMU:    refresh_mmu(fresh); break;
    case TAB_MACROS: refresh_macros(fresh); break;
    default: break;
    }
}

static void destroy(void)
{
    if (s_tab == TAB_STATUS && W.thumb) lv_image_set_src(W.thumb, NULL);
    ui_thumb_free(&W.th);
    printers_set_focus(-1);   /* back to slow, light polling */
}

const ui_page_t page_printer = { "Printer", create, refresh, destroy };
