/*
 * Single-printer view with a left sidebar:
 *   Status (job, temperatures, quick controls) | Move | MMU | Macros | Camera
 * Only the selected tab's widgets exist; switching tabs deletes the old ones.
 */
#include "ui.h"
#include "mesh_view.h"
#include "icons.h"
#include "ui_thumb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "printers.h"
#include "wifi_mgr.h"
#include "machine.h"
#include "esp_timer.h"
#include <math.h>
#include "esp_heap_caps.h"

typedef enum { TAB_STATUS = 0, TAB_TUNE, TAB_MOVE, TAB_EXTRUDE, TAB_MMU, TAB_MACROS,
               TAB_CONSOLE, TAB_SYSTEM, TAB_HISTORY, TAB_MESH, TAB_COUNT } tab_t;

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
    /* console tab */
    lv_obj_t *con_ta, *con_log, *con_status;
    /* system tab */
    lv_obj_t *svc_list, *svc_status;
    uint32_t mver;
    /* tune tab */
    lv_obj_t *z_lbl, *sl[3], *sl_lbl[3], *fan_card;
    /* history tab */
    lv_obj_t *hist_tot, *hist_list;
    /* mesh tab */
    lv_obj_t *mesh_info, *mesh_grid, *mesh_prof;
} W;
static int64_t s_tune_touch;   /* last slider interaction: don't overwrite it from polls */
static int s_zstep_i = 1;

static const char *stat_names[6] = { ICON_CLOCK "  Elapsed", ICON_TIMER "  Left", ICON_ETA "  ETA",
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

static int panel_width(void) { return ui_width() - 32 - 2 * (SIDEBAR_W + 16); }   /* two side bars */
static bool busy_printing(void) { return s_st.status == PS_PRINTING; }

/* ── actions ─────────────────────────────────────────────────────────── */
static void do_cmd(void *user) { printers_command(s_idx, (printer_cmd_t)(intptr_t)user, NULL); }
static void do_gcode(void *user) { printers_command(s_idx, CMD_GCODE, (const char *)user); }
static void do_motors_off(void *u) { printers_gcode(s_idx, "M84", "Motors off", false); }
static void do_heaters_off(void *u) { printers_gcode(s_idx, "TURN_OFF_HEATERS", "Heaters off", false); }

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
    ui_confirm("Turn off all heaters?", "Sets every heater target to 0.", "Turn off", COL_WARN, do_heaters_off, NULL);
}
static void motors_cb(lv_event_t *e)
{
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    ui_confirm("Disable motors?", "Steppers will be released and the printer will need homing again.", "Disable", COL_WARN, do_motors_off, NULL);
}
static void home_cb(lv_event_t *e)
{
    const char *g = lv_event_get_user_data(e);
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    printers_gcode(s_idx, g, "Homing", false);
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
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "%s temperature", ht->label);
    printers_gcode(s_idx, g, lbl, true);
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
    char g[160];
    float d = STEPS[s_step_i] * sign;
    int f = axis == 'Z' ? 600 : 6000;
    snprintf(g, sizeof(g), "SAVE_GCODE_STATE NAME=_ks_jog\nG91\nG1 %c%.2f F%d\nRESTORE_GCODE_STATE NAME=_ks_jog", axis, d, f);
    printers_gcode(s_idx, g, "Move", true);
}

static void extrude_cb(lv_event_t *e)
{
    int sign = (int)(intptr_t)lv_event_get_user_data(e);
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    float t = 0;
    for (int i = 0; i < s_st.nheaters; i++) if (!strcmp(s_st.heaters[i].obj, "extruder")) t = s_st.heaters[i].temp;
    if (t < 170) { ui_toast("Hotend is below 170°C - heat it first"); return; }
    char g[160];
    snprintf(g, sizeof(g), "SAVE_GCODE_STATE NAME=_ks_ext\nM83\nG1 E%d F%d\nRESTORE_GCODE_STATE NAME=_ks_ext",
             sign * EXTS[s_ext_i], ESPEEDS[s_espd_i] * 60);
    printers_gcode(s_idx, g, sign > 0 ? "Extrude" : "Retract", true);
}

static void step_cb(lv_event_t *e) { s_step_i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)); }
static void ext_cb(lv_event_t *e) { s_ext_i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)); }

static void macro_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_st.nmacros) return;
    static char g[48];
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
        lv_obj_set_flex_flow(h->row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(h->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(h->row, 10, 0);
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
        /* [ name + power bar | current | target ] as a flex row: nothing can overlap */
        lv_obj_t *nc = plain(h->row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(nc, 1);
        lv_obj_set_flex_grow(nc, 1);
        lv_obj_set_style_pad_gap(nc, 6, 0);
        lv_obj_remove_flag(nc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(nc, LV_OBJ_FLAG_EVENT_BUBBLE);
        h->name = ui_label(nc, FONT_M, COL_MUTED, nm);
        lv_label_set_long_mode(h->name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(h->name, LV_PCT(100));
        h->bar = lv_bar_create(nc);
        lv_obj_set_size(h->bar, LV_PCT(80), 6);
        lv_bar_set_range(h->bar, 0, 100);
        h->cur = ui_label(h->row, FONT_XL, COL_TEXT, "--");
        h->tgt = ui_label(h->row, FONT_M, COL_MUTED, "");
        lv_obj_set_width(h->tgt, 72);
        lv_obj_set_style_text_align(h->tgt, LV_TEXT_ALIGN_RIGHT, 0);
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
        lv_obj_set_width(cell, LV_PCT(32));
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


    W.heat_card = card(right, NULL);
    build_heaters();

    memset(&W.th, 0, sizeof(W.th));
    W.th.ver = UINT32_MAX;
}

static void refresh_status(bool fresh)
{
    ui_thumb_update(&W.th, W.thumb, s_idx, 120);
    if (!fresh) return;
    char b[200];
    lv_obj_set_style_bg_color(W.chip, ui_status_color(s_st.status), 0);
    lv_obj_set_style_text_color(W.chip_lbl, ui_contrast(ui_status_color(s_st.status)), 0);
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
    printers_gcode(s_idx, g, "Hotend temperature", true);
    char m[48];
    snprintf(m, sizeof(m), t ? ICON_HOTEND "  Heating hotend to %d°" : ICON_HEAT_OFF "  Hotend off", t);
    ui_toast(m);
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

/* ── CONSOLE tab ─────────────────────────────────────────────────────── */
static con_line_t *s_con;   /* PSRAM scratch while the console is open */

static void con_send(const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    printers_gcode(s_idx, cmd, "Console", true);   /* responses show up in the log itself */
    machine_refresh_now();
}

static void con_send_cb(lv_event_t *e)
{
    char cmd[200];
    strlcpy(cmd, lv_textarea_get_text(W.con_ta), sizeof(cmd));
    con_send(cmd);
    lv_textarea_set_text(W.con_ta, "");
}

static void con_quick_cb(lv_event_t *e) { con_send((const char *)lv_event_get_user_data(e)); }

static void build_console(lv_obj_t *p)
{
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *c = card(p, NULL);
    lv_obj_t *r = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    W.con_ta = lv_textarea_create(r);
    lv_textarea_set_one_line(W.con_ta, true);
    lv_textarea_set_placeholder_text(W.con_ta, "Type a command, e.g. G28");
    lv_textarea_set_max_length(W.con_ta, 190);
    lv_obj_set_flex_grow(W.con_ta, 1);
    lv_obj_set_style_text_font(W.con_ta, FONT_L, 0);
    ui_attach_keyboard(W.con_ta, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_event_cb(W.con_ta, con_send_cb, LV_EVENT_READY, NULL);
    ui_button(r, ICON_SEND, "Send", COL_ACCENT, con_send_cb, NULL);
    lv_obj_t *q = plain(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(q, 8, 0);
    static const char *quick[] = { "QUERY_ENDSTOPS", "GET_POSITION", "M115", "HELP" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_button(q, NULL, quick[i], COL_CARD2, con_quick_cb, (void *)quick[i]);
        lv_obj_set_style_min_height(b, 48, 0);
        lv_obj_set_style_pad_ver(b, 6, 0);
    }

    lv_obj_t *lc = card(p, NULL);
    W.con_status = ui_label(lc, FONT_S, COL_MUTED, "Loading console...");
    W.con_log = plain(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(W.con_log, 4, 0);
    if (!s_con) s_con = heap_caps_malloc(CONSOLE_MAX * sizeof(con_line_t), MALLOC_CAP_SPIRAM);
    W.mver = UINT32_MAX;
    machine_start_console(s_idx);
}

static void refresh_console(bool fresh)
{
    char err[96];
    bool failed = machine_error(err, sizeof(err));
    if (machine_version() == W.mver && !failed) return;
    W.mver = machine_version();
    if (failed) { ui_set_text(W.con_status, err); return; }
    if (!s_con) return;
    int n = machine_console(s_con, CONSOLE_MAX);
    lv_obj_clean(W.con_log);
    for (int i = n - 1; i >= 0; i--) {   /* newest first, right under the input */
        const con_line_t *l = &s_con[i];
        char t[200], ts[12] = "";
        time_t tt = (time_t)l->time;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
        bool err_line = !strncmp(l->msg, "!!", 2);
        snprintf(t, sizeof(t), "%s  %s%s", ts, l->type == 'c' ? "> " : "", l->msg);
        lv_obj_t *lb = ui_label(W.con_log, FONT_S,
                                err_line ? COL_ERR : l->type == 'c' ? COL_ACCENT : COL_TEXT, t);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lb, LV_PCT(100));
    }
    char st[48];
    snprintf(st, sizeof(st), "%d recent lines  -  newest first", n);
    ui_set_text(W.con_status, n ? st : "Console is empty");
}

/* ── SYSTEM tab ──────────────────────────────────────────────────────── */
static svc_t s_svc[SVC_MAX];
static struct { printer_cmd_t cmd; char name[32]; } s_pending;

static void do_pending(void *u) { printers_command(s_idx, s_pending.cmd, s_pending.name[0] ? s_pending.name : NULL); }

static void sys_cmd_cb(lv_event_t *e)
{
    printer_cmd_t cmd = (printer_cmd_t)(intptr_t)lv_event_get_user_data(e);
    s_pending.cmd = cmd;
    s_pending.name[0] = 0;
    const char *title = "", *text = "", *ok = "Confirm";
    lv_color_t col = COL_WARN;
    switch (cmd) {
    case CMD_KLIPPER_RESTART:  title = "Restart Klipper?"; text = "Reloads printer.cfg. Any print in progress stops."; ok = "Restart"; break;
    case CMD_FIRMWARE_RESTART: title = "Firmware restart?"; text = "Restarts Klipper and the printer's MCUs. Any print in progress stops."; ok = "Restart"; break;
    case CMD_HOST_REBOOT:      title = "Reboot the host?"; text = "The printer's computer restarts. It will be offline for a minute or two."; ok = "Reboot"; break;
    case CMD_HOST_SHUTDOWN:    title = "Shut down the host?"; text = "The printer's computer powers off. You'll need to switch it back on by hand."; ok = "Shut down"; col = COL_ERR; break;
    default: return;
    }
    ui_confirm(title, text, ok, col, do_pending, NULL);
}

static void svc_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int i = v & 0xFF;
    printer_cmd_t cmd = (printer_cmd_t)(v >> 8);
    if (i >= SVC_MAX) return;
    s_pending.cmd = cmd;
    strlcpy(s_pending.name, s_svc[i].name, sizeof(s_pending.name));
    if (cmd == CMD_SERVICE_START) { do_pending(NULL); return; }
    char t[120];
    snprintf(t, sizeof(t), "%s the %s service?", cmd == CMD_SERVICE_STOP ? "Stop" : "Restart", s_svc[i].name);
    ui_confirm(cmd == CMD_SERVICE_STOP ? "Stop service?" : "Restart service?", t,
               cmd == CMD_SERVICE_STOP ? "Stop" : "Restart", cmd == CMD_SERVICE_STOP ? COL_ERR : COL_WARN, do_pending, NULL);
}

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_event_cb_t cb, intptr_t u)
{
    lv_obj_t *b = ui_button(parent, icon, NULL, bg, cb, (void *)u);
    lv_obj_set_size(b, 64, 52);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_min_height(b, 52, 0);
    return b;
}

static void build_svc_rows(int n)
{
    lv_obj_clean(W.svc_list);
    for (int i = 0; i < n; i++) {
        const svc_t *v = &s_svc[i];
        lv_obj_t *r = lv_obj_create(W.svc_list);
        lv_obj_set_size(r, LV_PCT(100), 68);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_set_style_pad_ver(r, 0, 0);
        lv_obj_set_style_pad_gap(r, 10, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *nm = ui_label(r, FONT_M, COL_TEXT, v->name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(nm, 1);
        bool running = !strcmp(v->active, "active");
        bool failed = !strcmp(v->active, "failed");
        lv_obj_t *chip = lv_obj_create(r);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(chip, 20, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_pad_hor(chip, 12, 0);
        lv_obj_set_style_pad_ver(chip, 4, 0);
        lv_color_t cc = running ? COL_OK : failed ? COL_ERR : COL_BORDER;
        lv_obj_set_style_bg_color(chip, cc, 0);
        ui_label(chip, FONT_S, ui_contrast(cc), running ? "Running" : failed ? "Failed" : v->active[0] ? "Stopped" : "Unknown");
        icon_btn(r, ICON_RESTART, COL_CARD, svc_cb, (CMD_SERVICE_RESTART << 8) | i);
        if (running) icon_btn(r, ICON_CANCEL, COL_CARD, svc_cb, (CMD_SERVICE_STOP << 8) | i);
        else icon_btn(r, ICON_PLAY, COL_CARD, svc_cb, (CMD_SERVICE_START << 8) | i);
    }
}

static void build_system(lv_obj_t *p)
{
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *kc = card(p, "KLIPPER");
    lv_obj_t *kr = plain(kc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(kr, 12, 0);
    ui_tile_button(kr, ICON_RESTART, "Restart", COL_CARD2, sys_cmd_cb, (void *)CMD_KLIPPER_RESTART);
    ui_tile_button(kr, ICON_RESTART, "Firmware restart", COL_CARD2, sys_cmd_cb, (void *)CMD_FIRMWARE_RESTART);

    lv_obj_t *sc = card(p, "SERVICES");
    W.svc_status = ui_label(sc, FONT_S, COL_MUTED, "Loading services...");
    W.svc_list = plain(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(W.svc_list, 8, 0);

    lv_obj_t *hc = card(p, "HOST");
    lv_obj_t *hr = plain(hc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(hr, 12, 0);
    ui_tile_button(hr, ICON_RESTART, "Reboot", COL_CARD2, sys_cmd_cb, (void *)CMD_HOST_REBOOT);
    ui_tile_button(hr, ICON_POWER, "Shut down", COL_ERR, sys_cmd_cb, (void *)CMD_HOST_SHUTDOWN);

    W.mver = UINT32_MAX;
    machine_start_services(s_idx);
}

static void refresh_system(bool fresh)
{
    char err[96];
    bool failed = machine_error(err, sizeof(err));
    if (machine_version() == W.mver && !failed) return;
    W.mver = machine_version();
    if (failed) { ui_set_text(W.svc_status, err); return; }
    int n = machine_services(s_svc, SVC_MAX);
    if (n < 0) return;
    build_svc_rows(n);
    ui_set_text(W.svc_status, n ? "Services managed by Moonraker" : "Moonraker lists no services");
}

/* ── TUNE tab ────────────────────────────────────────────────────────── */
static const float ZSTEPS[] = { 0.005f, 0.01f, 0.025f, 0.05f };
enum { SL_SPEED = 0, SL_FLOW, SL_FAN };

static void zstep_cb(lv_event_t *e) { s_zstep_i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)); }

static void z_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    char g[96];
    bool homed_z = strchr(s_st.homed, 'z') != NULL;
    if (dir == 0) snprintf(g, sizeof(g), "SET_GCODE_OFFSET Z=0%s", homed_z ? " MOVE=1" : "");
    else snprintf(g, sizeof(g), "SET_GCODE_OFFSET Z_ADJUST=%+.3f%s", dir * ZSTEPS[s_zstep_i], homed_z ? " MOVE=1" : "");
    printers_gcode(s_idx, g, "Z offset", true);
}

static void slider_label(int k, int v)
{
    static const char *names[3] = { ICON_SPEED "  Speed", ICON_EXTRUDE "  Flow", ICON_FAN "  Part fan" };
    char t[48];
    snprintf(t, sizeof(t), "%s   %d%%", names[k], v);
    ui_set_text(W.sl_lbl[k], t);
}

static void slider_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(W.sl[k]);
    s_tune_touch = esp_timer_get_time();
    slider_label(k, v);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    char g[48];
    if (k == SL_SPEED) snprintf(g, sizeof(g), "M220 S%d", v);
    else if (k == SL_FLOW) snprintf(g, sizeof(g), "M221 S%d", v);
    else snprintf(g, sizeof(g), "M106 S%d", v * 255 / 100);
    printers_gcode(s_idx, g, k == SL_SPEED ? "Speed" : k == SL_FLOW ? "Flow" : "Fan", true);
}

static void slider_reset_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int v = k == SL_FAN ? 0 : 100;
    lv_slider_set_value(W.sl[k], v, LV_ANIM_OFF);
    slider_label(k, v);
    s_tune_touch = esp_timer_get_time();
    printers_gcode(s_idx, k == SL_SPEED ? "M220 S100" : k == SL_FLOW ? "M221 S100" : "M107",
                   k == SL_SPEED ? "Speed" : k == SL_FLOW ? "Flow" : "Fan", true);
}

static lv_obj_t *slider_card(lv_obj_t *parent, int k, int min, int max, const char *reset)
{
    lv_obj_t *c = card(parent, NULL);
    lv_obj_t *r = plain(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    W.sl_lbl[k] = ui_label(r, FONT_L, COL_TEXT, "");
    lv_obj_t *rb = ui_button(r, NULL, reset, COL_CARD2, slider_reset_cb, (void *)(intptr_t)k);
    lv_obj_set_style_min_height(rb, 48, 0);
    lv_obj_set_style_pad_ver(rb, 6, 0);
    W.sl[k] = lv_slider_create(c);
    lv_obj_set_width(W.sl[k], LV_PCT(94));
    lv_obj_set_height(W.sl[k], 22);
    lv_obj_set_style_pad_ver(W.sl[k], 18, 0);
    lv_obj_set_style_margin_left(W.sl[k], 12, 0);
    lv_slider_set_range(W.sl[k], min, max);
    lv_obj_add_event_cb(W.sl[k], slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
    lv_obj_add_event_cb(W.sl[k], slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)k);
    return c;
}

static void build_tune(lv_obj_t *p)
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
    lv_obj_t *zc = card(left, "Z OFFSET  (babystep)");
    W.z_lbl = ui_label(zc, FONT_XXL, COL_TEXT, "--");
    static const char *zmap[] = { "0.005", "0.01", "0.025", "0.05 mm", "" };
    selector(zc, zmap, s_zstep_i, zstep_cb);
    lv_obj_t *zr = plain(zc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(zr, 12, 0);
    ui_tile_button(zr, ICON_DOWN, "Lower (closer)", COL_CARD2, z_cb, (void *)(intptr_t)-1);
    ui_tile_button(zr, ICON_UP, "Raise", COL_CARD2, z_cb, (void *)(intptr_t)1);
    lv_obj_t *rr = plain(zc, LV_FLEX_FLOW_ROW);
    lv_obj_t *rz = ui_button(rr, ICON_RESTART, "Reset to 0", COL_CARD2, z_cb, (void *)(intptr_t)0);
    lv_obj_set_flex_grow(rz, 1);
    lv_obj_t *hint = ui_label(zc, FONT_S, COL_MUTED, "Babystepping is temporary. Save it in your printer's config to keep it.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    slider_card(right, SL_SPEED, 10, 300, "100%");
    slider_card(right, SL_FLOW, 50, 150, "100%");
    W.fan_card = slider_card(right, SL_FAN, 0, 100, "Off");
    s_tune_touch = 0;
}

static void refresh_tune(bool fresh)
{
    if (!fresh) return;
    char b[48];
    bool down = s_st.status == PS_OFFLINE || s_st.status == PS_KLIPPY_DOWN;
    snprintf(b, sizeof(b), down ? "--" : "%+.3f mm", s_st.z_offset);
    ui_set_text(W.z_lbl, b);
    show(W.fan_card, s_st.has_fan);
    if (esp_timer_get_time() - s_tune_touch < 3000000) return;   /* user is adjusting */
    int v[3] = { (int)(s_st.speed_factor * 100 + 0.5f), (int)(s_st.extrude_factor * 100 + 0.5f),
                 (int)(s_st.fan_speed * 100 + 0.5f) };
    if (v[2] < 0) v[2] = 0;
    for (int k = 0; k < 3; k++) {
        if (lv_obj_has_state(W.sl[k], LV_STATE_PRESSED)) continue;
        lv_slider_set_value(W.sl[k], v[k], LV_ANIM_OFF);
        slider_label(k, v[k]);
    }
}

/* ── HISTORY tab ─────────────────────────────────────────────────────── */
static hist_job_t *s_hist;

static void build_history(lv_obj_t *p)
{
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *tc = card(p, "TOTALS");
    W.hist_tot = ui_label(tc, FONT_L, COL_TEXT, "Loading history...");
    lv_obj_t *lc = card(p, "RECENT JOBS");
    W.hist_list = plain(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(W.hist_list, 8, 0);
    if (!s_hist) s_hist = heap_caps_malloc(HIST_MAX * sizeof(hist_job_t), MALLOC_CAP_SPIRAM);
    W.mver = UINT32_MAX;
    machine_start_history(s_idx);
}

static void refresh_history(bool fresh)
{
    char err[96];
    bool failed = machine_error(err, sizeof(err));
    if (machine_version() == W.mver && !failed) return;
    W.mver = machine_version();
    if (failed) { ui_set_text(W.hist_tot, err); return; }
    hist_totals_t tot;
    int n = s_hist ? machine_history(s_hist, HIST_MAX, &tot) : -1;
    if (n < 0) return;
    char a[24], b[160];
    ui_fmt_duration(a, sizeof(a), tot.print_s);
    snprintf(b, sizeof(b), ICON_PRINTER " %d jobs     " ICON_CLOCK " %s printing     " ICON_SPOOL " %.1f m filament",
             tot.jobs, a, tot.filament_mm / 1000);
    ui_set_text(W.hist_tot, b);
    lv_obj_clean(W.hist_list);
    for (int i = 0; i < n; i++) {
        const hist_job_t *h = &s_hist[i];
        bool ok = !strcmp(h->status, "completed");
        bool cancel = !strcmp(h->status, "cancelled");
        bool running = !strcmp(h->status, "in_progress");
        lv_color_t col = ok ? COL_OK : cancel ? COL_MUTED : running ? COL_ACCENT : COL_ERR;
        lv_obj_t *r = lv_obj_create(W.hist_list);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(r, 68, 0);
        lv_obj_set_style_bg_color(r, COL_CARD2, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_set_style_pad_ver(r, 10, 0);
        lv_obj_set_style_pad_gap(r, 14, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        ui_label(r, FONT_XL, col, ok ? ICON_CHECK : cancel ? ICON_CANCEL : running ? ICON_PLAY : ICON_ALERT);
        lv_obj_t *tc = plain(r, LV_FLEX_FLOW_COLUMN);   /* file name above details */
        lv_obj_set_width(tc, 1);
        lv_obj_set_flex_grow(tc, 1);
        lv_obj_set_style_pad_gap(tc, 4, 0);
        lv_obj_t *l1 = ui_label(tc, FONT_M, COL_TEXT, h->file);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l1, LV_PCT(100));
        char d[32], dur[24];
        time_t t = (time_t)h->start;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(d, sizeof(d), "%d %b %Y, %H:%M", &tm);
        ui_fmt_duration(dur, sizeof(dur), h->print_s);
        snprintf(b, sizeof(b), "%s  -  %s  -  %.2f m  -  %s", d, dur, h->filament_mm / 1000,
                 ok ? "Completed" : cancel ? "Cancelled" : running ? "Printing" : h->status);
        lv_obj_t *l2 = ui_label(tc, FONT_S, COL_MUTED, b);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l2, LV_PCT(100));
    }
    if (!n) ui_label(W.hist_list, FONT_M, COL_MUTED, "No print history yet.");
}

/* ── MESH tab ────────────────────────────────────────────────────────── */
static mesh_t *s_mesh;
static char s_mesh_cmd[64];

static void do_mesh_cmd(void *u) { printers_gcode(s_idx, s_mesh_cmd, "Bed mesh", false); machine_refresh_now(); }

static void mesh_load_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_mesh || i >= s_mesh->nprofiles) return;
    snprintf(s_mesh_cmd, sizeof(s_mesh_cmd), "BED_MESH_PROFILE LOAD=%s", s_mesh->profiles[i]);
    do_mesh_cmd(NULL);
}

static void mesh_cal_cb(lv_event_t *e)
{
    if (busy_printing()) { ui_toast("Not while printing"); return; }
    bool homed = strchr(s_st.homed, 'x') && strchr(s_st.homed, 'y') && strchr(s_st.homed, 'z');
    strlcpy(s_mesh_cmd, homed ? "BED_MESH_CALIBRATE" : "G28\nBED_MESH_CALIBRATE", sizeof(s_mesh_cmd));
    ui_confirm("Calibrate bed mesh?", homed ? "The printer probes the bed. This takes a few minutes."
                                            : "The printer homes, then probes the bed. This takes a few minutes.",
               "Calibrate", COL_ACCENT, do_mesh_cmd, NULL);
}

static void mesh_clear_cb(lv_event_t *e)
{
    strlcpy(s_mesh_cmd, "BED_MESH_CLEAR", sizeof(s_mesh_cmd));
    do_mesh_cmd(NULL);
}

static void build_mesh(lv_obj_t *p)
{
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    int w = panel_width() - 16;
    lv_obj_t *mc = card(p, "BED MESH");
    lv_obj_set_width(mc, ui_is_portrait() ? LV_PCT(100) : w * 60 / 100);
    W.mesh_info = ui_label(mc, FONT_M, COL_TEXT, "Loading mesh...");
    lv_label_set_long_mode(W.mesh_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(W.mesh_info, LV_PCT(100));
    /* canvas height: what's left of the panel below the info line and toolbar */
    int vh = ui_is_portrait() ? 420 : ui_height() - 230;
    if (vh < 280) vh = 280;
    if (vh > 470) vh = 470;
    W.mesh_grid = mesh_view_create(mc, vh);

    lv_obj_t *ac = card(p, "PROFILES");
    lv_obj_set_width(ac, ui_is_portrait() ? LV_PCT(100) : w - w * 60 / 100);
    W.mesh_prof = plain(ac, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(W.mesh_prof, 8, 0);
    lv_obj_t *br = plain(ac, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(br, 12, 0);
    ui_tile_button(br, ICON_MESH, "Calibrate", COL_ACCENT, mesh_cal_cb, NULL);
    ui_tile_button(br, ICON_CLOSE, "Clear", COL_CARD2, mesh_clear_cb, NULL);
    if (!s_mesh) s_mesh = heap_caps_malloc(sizeof(mesh_t), MALLOC_CAP_SPIRAM);
    W.mver = UINT32_MAX;
    machine_start_mesh(s_idx);
}

static void refresh_mesh(bool fresh)
{
    char err[96];
    bool failed = machine_error(err, sizeof(err));
    if (machine_version() == W.mver && !failed) return;
    W.mver = machine_version();
    if (failed) { ui_set_text(W.mesh_info, err); return; }
    if (!s_mesh || !machine_mesh(s_mesh)) return;
    mesh_t *m = s_mesh;
    char b[160];
    if (m->rows < 2) {
        ui_set_text(W.mesh_info, ICON_INFO "  No mesh loaded. Load a profile or calibrate.");
    } else {
        float lo = m->z[0][0], hi = lo;
        for (int r = 0; r < m->rows; r++)
            for (int c = 0; c < m->cols; c++) { if (m->z[r][c] < lo) lo = m->z[r][c]; if (m->z[r][c] > hi) hi = m->z[r][c]; }
        snprintf(b, sizeof(b), "Profile: %s   -   %dx%d points   -   range %.3f mm (%.3f to %+.3f)",
                 m->profile[0] ? m->profile : "(unsaved)", m->cols, m->rows, hi - lo, lo, hi);
        ui_set_text(W.mesh_info, b);
    }
    mesh_view_set(W.mesh_grid, m);
    lv_obj_clean(W.mesh_prof);
    for (int i = 0; i < m->nprofiles; i++) {
        bool active = !strcmp(m->profiles[i], m->profile);
        snprintf(b, sizeof(b), "%s%s", m->profiles[i], active ? "   (loaded)" : "");
        lv_obj_t *bt = ui_button(W.mesh_prof, ICON_MESH, b, active ? COL_OK : COL_CARD2, mesh_load_cb, (void *)(intptr_t)i);
        lv_obj_set_width(bt, LV_PCT(100));
    }
    if (!m->nprofiles) ui_label(W.mesh_prof, FONT_S, COL_MUTED, "No saved profiles.");
}

/* ── tabs / sidebar ──────────────────────────────────────────────────── */
static void set_tab(tab_t t)
{
    if (s_tab == TAB_STATUS && W.thumb) {   /* leaving status: drop the thumbnail */
        lv_image_set_src(W.thumb, NULL);
        ui_thumb_free(&W.th);
    }
    machine_stop();   /* console/system polling only while their tab is open */
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
    case TAB_CONSOLE: build_console(s_panel); break;
    case TAB_SYSTEM: build_system(s_panel); break;
    case TAB_TUNE: build_tune(s_panel); break;
    case TAB_HISTORY: build_history(s_panel); break;
    case TAB_MESH: build_mesh(s_panel); break;
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

static lv_obj_t *s_cam_menu;
static void cam_pick_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_cam_menu) { lv_obj_delete_async(s_cam_menu); s_cam_menu = NULL; }
    ui_nav_push(PAGE_WEBCAM, s_idx * 16 + c);
}
static void cam_menu_close(lv_event_t *e) { if (s_cam_menu) { lv_obj_delete_async(s_cam_menu); s_cam_menu = NULL; } }
static void cam_cb(lv_event_t *e)
{
    if (s_st.nwebcams <= 1) { ui_nav_push(PAGE_WEBCAM, s_idx * 16); return; }
    /* several cameras: let the user pick */
    s_cam_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_cam_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_cam_menu, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cam_menu, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_cam_menu, 0, 0);
    lv_obj_set_style_radius(s_cam_menu, 0, 0);
    lv_obj_add_event_cb(s_cam_menu, cam_menu_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *box = ui_card(s_cam_menu);
    lv_obj_set_size(box, 480, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_pad_gap(box, 12, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   /* taps inside don't close it */
    ui_label(box, FONT_L, COL_TEXT, ICON_WEBCAM "  Choose a camera");
    for (int i = 0; i < s_st.nwebcams; i++) {
        lv_obj_t *b = ui_button(box, ICON_WEBCAM, s_st.webcams[i].name[0] ? s_st.webcams[i].name : "Camera",
                                COL_CARD2, cam_pick_cb, (void *)(intptr_t)i);
        lv_obj_set_width(b, LV_PCT(100));
    }
}
static void files_cb(lv_event_t *e) { ui_nav_push(PAGE_FILES, s_idx); }
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
    s_side_btn[TAB_TUNE]   = side_btn(bar, ICON_TUNE, "Tune", side_cb, (void *)TAB_TUNE);
    s_side_btn[TAB_MOVE]   = side_btn(bar, ICON_MOVE, "Move", side_cb, (void *)TAB_MOVE);
    s_side_btn[TAB_EXTRUDE] = side_btn(bar, ICON_EXTRUDE, "Extrude", side_cb, (void *)TAB_EXTRUDE);
    s_side_btn[TAB_MMU]    = side_btn(bar, ICON_MMU, "MMU", side_cb, (void *)TAB_MMU);
    s_side_btn[TAB_MACROS] = side_btn(bar, ICON_MACRO, "Macros", side_cb, (void *)TAB_MACROS);
    side_btn(bar, ICON_FILE, "Files", files_cb, NULL);
    show(s_side_btn[TAB_MMU], s_st.mmu.type != MMU_NONE);
    show(s_side_btn[TAB_MACROS], s_st.nmacros > 0);

    s_panel = lv_obj_create(parent);
    lv_obj_set_height(s_panel, LV_PCT(100));
    lv_obj_set_flex_grow(s_panel, 1);
    lv_obj_set_style_bg_opa(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_pad_gap(s_panel, 16, 0);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);

    /* Right bar: extra tools (room for more later). */
    lv_obj_t *rbar = lv_obj_create(parent);
    lv_obj_set_size(rbar, SIDEBAR_W, LV_PCT(100));
    lv_obj_set_style_bg_opa(rbar, 0, 0);
    lv_obj_set_style_border_width(rbar, 0, 0);
    lv_obj_set_style_pad_all(rbar, 0, 0);
    lv_obj_set_style_pad_gap(rbar, 6, 0);
    lv_obj_set_flex_flow(rbar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(rbar, LV_DIR_VER);
    s_cam_btn = side_btn(rbar, ICON_WEBCAM, "Camera", cam_cb, NULL);
    s_spool_btn = side_btn(rbar, ICON_SPOOL, "Spools", spool_cb, NULL);
    s_side_btn[TAB_CONSOLE] = side_btn(rbar, ICON_CONSOLE, "Console", side_cb, (void *)TAB_CONSOLE);
    s_side_btn[TAB_SYSTEM] = side_btn(rbar, ICON_SERVER, "System", side_cb, (void *)TAB_SYSTEM);
    s_side_btn[TAB_HISTORY] = side_btn(rbar, ICON_HISTORY, "History", side_cb, (void *)TAB_HISTORY);
    s_side_btn[TAB_MESH] = side_btn(rbar, ICON_MESH, "Mesh", side_cb, (void *)TAB_MESH);
    show(s_side_btn[TAB_MESH], s_st.has_bed_mesh);
    show(s_spool_btn, s_st.has_spoolman);
    show(s_cam_btn, s_st.nwebcams > 0);

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
    show(s_side_btn[TAB_MESH], s_st.has_bed_mesh);

    switch (s_tab) {
    case TAB_STATUS: refresh_status(fresh); break;
    case TAB_MOVE:   refresh_move(fresh); break;
    case TAB_EXTRUDE: refresh_extrude(fresh); break;
    case TAB_MMU:    refresh_mmu(fresh); break;
    case TAB_MACROS: refresh_macros(fresh); break;
    case TAB_CONSOLE: refresh_console(fresh); break;
    case TAB_SYSTEM: refresh_system(fresh); break;
    case TAB_TUNE: refresh_tune(fresh); break;
    case TAB_HISTORY: refresh_history(fresh); break;
    case TAB_MESH: refresh_mesh(fresh); break;
    default: break;
    }
}

static void destroy(void)
{
    machine_stop();
    if (s_con) { heap_caps_free(s_con); s_con = NULL; }
    if (s_hist) { heap_caps_free(s_hist); s_hist = NULL; }
    if (s_mesh) { heap_caps_free(s_mesh); s_mesh = NULL; }
    if (s_cam_menu) { lv_obj_delete(s_cam_menu); s_cam_menu = NULL; }
    if (s_tab == TAB_STATUS && W.thumb) lv_image_set_src(W.thumb, NULL);
    ui_thumb_free(&W.th);
    printers_set_focus(-1);   /* back to slow, light polling */
}

const ui_page_t page_printer = { "Printer", create, refresh, destroy };
