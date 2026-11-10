/*
 * Printer registry + Moonraker pollers.
 *
 * One FreeRTOS task per printer polls Moonraker over plain HTTP (keep-alive).
 * The UI never touches the network: it copies printer_state_t snapshots under
 * a mutex. Commands run on short-lived tasks so a slow G28 never delays an
 * emergency stop.
 */
#include "printers.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* task stacks in PSRAM: keeps internal SRAM for Wi-Fi/SDIO DMA */
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include "http_util.h"
#include "app_settings.h"
#include "wifi_mgr.h"

static const char *TAG = "printers";

typedef struct {
    bool            used;
    volatile bool   stop;
    volatile bool   alive;
    printer_cfg_t   cfg;
    printer_state_t st;
    TaskHandle_t    task;
} slot_t;

static slot_t s_slots[MAX_PRINTERS];
static int s_order[MAX_PRINTERS];   /* visible index -> slot */
static int s_count;
static SemaphoreHandle_t s_mtx;
static volatile int s_focus_slot = -1;

static char s_result[160];
static volatile bool s_result_new;

#define ALERT_Q 8
static job_alert_t s_alerts[ALERT_Q];
static int s_alert_head, s_alert_n;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

/* Thumbnail versions are unique across all printers (the UI caches decoded
 * thumbnails by version). 0 means "never had one". Take it with LOCK held. */
static uint32_t s_thumb_gen;

/* ── helpers ─────────────────────────────────────────────────────────── */

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static double jnum(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool jbool(const cJSON *o, const char *k, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valueint != 0;
    return dflt;
}

static void jstr(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(dst, v->valuestring, cap);
}

static bool jhas(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k) != NULL;
}

/* Parse "#RRGGBB", "RRGGBB", "RRGGBBAA" or a few colour names. */
static bool parse_color(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    if (*s == '#') s++;
    size_t n = strlen(s);
    if (n == 6 || n == 8) {
        bool hex = true;
        for (size_t i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) hex = false;
        if (hex) {
            char t[7];
            memcpy(t, s, 6);
            t[6] = 0;
            *out = strtoul(t, NULL, 16);
            return true;
        }
    }
    static const struct { const char *n; uint32_t c; } names[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xE53935}, {"green", 0x43A047},
        {"blue", 0x1E88E5}, {"yellow", 0xFDD835}, {"orange", 0xFB8C00}, {"purple", 0x8E24AA},
        {"grey", 0x9E9E9E}, {"gray", 0x9E9E9E}, {"pink", 0xEC407A}, {"brown", 0x6D4C41},
        {"cyan", 0x00ACC1}, {"magenta", 0xD81B60}, {"silver", 0xBDBDBD}, {"gold", 0xFFC107},
        {"natural", 0xEFE6D2}, {"transparent", 0xDDDDDD},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcasecmp(s, names[i].n)) { *out = names[i].c; return true; }
    return false;
}

const char *printer_status_str(printer_status_t s)
{
    switch (s) {
    case PS_OFFLINE:     return "Offline";
    case PS_KLIPPY_DOWN: return "Klipper not ready";
    case PS_STANDBY:     return "Idle";
    case PS_PRINTING:    return "Printing";
    case PS_PAUSED:      return "Paused";
    case PS_COMPLETE:    return "Complete";
    case PS_CANCELLED:   return "Cancelled";
    case PS_ERROR:       return "Error";
    }
    return "?";
}

/* ── persistence ─────────────────────────────────────────────────────── */

static void save_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_count; i++) {
        printer_cfg_t *c = &s_slots[s_order[i]].cfg;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "n", c->name);
        cJSON_AddStringToObject(o, "h", c->host);
        cJSON_AddNumberToObject(o, "p", c->port);
        if (c->api_key[0]) cJSON_AddStringToObject(o, "k", c->api_key);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    nvs_handle_t h;
    if (txt && nvs_open("printers", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "list", txt);
        nvs_commit(h);
        nvs_close(h);
    }
    cJSON_free(txt);
}

/* ── Moonraker poller ────────────────────────────────────────────────── */

typedef struct {
    slot_t *slot;
    esp_http_client_handle_t conn;
    http_buf_t buf;
    char base[96];
    char *query;            /* full objects query (focused printer), built from /printer/objects/list */
    char *query_lite;       /* home-tile query: status, progress, temps only */
    bool have_objects;
    char last_file[160];
    /* discovered objects */
    char afc_lanes[MAX_LANES][48];
    int  nafc_lanes;
    mmu_type_t mmu;
    heater_t heaters[MAX_HEATERS];
    int nheaters;
    char macros[MAX_MACROS][40];
    int nmacros;
    bool has_qgl, has_ztilt, has_bed_mesh, has_fan;
    printer_status_t last_status;   /* for job-end alerts */
    bool status_known;
} poll_ctx_t;

static int get_json(poll_ctx_t *c, const char *path, cJSON **out, int timeout_ms)
{
    char url[1024 + 128];
    printer_cfg_t cfg;
    LOCK();
    cfg = c->slot->cfg;
    UNLOCK();
    snprintf(url, sizeof(url), "%s%s", c->base, path);
    int st = http_request(&c->conn, HTTP_METHOD_GET, url, cfg.api_key, NULL, NULL, 0, &c->buf, timeout_ms);
    *out = NULL;
    if (st > 0 && c->buf.data) *out = cJSON_ParseWithLength((char *)c->buf.data, c->buf.len);
    return st;
}

static const char *heater_kind_label(const char *obj, uint8_t *kind, char *label, size_t cap)
{
    const char *sp = strchr(obj, ' ');
    const char *nm = sp ? sp + 1 : obj;
    if (!strncmp(obj, "extruder", 8)) {
        *kind = 0;
        if (!strcmp(obj, "extruder")) strlcpy(label, "Hotend", cap);
        else snprintf(label, cap, "Hotend %d", atoi(obj + 8) + 1);
    } else if (!strcmp(obj, "heater_bed")) {
        *kind = 1;
        strlcpy(label, "Bed", cap);
    } else {
        *kind = strstr(obj, "chamber") ? 2 : 3;
        if (*kind == 2) strlcpy(label, strncmp(obj, "temperature_sensor ", 19) ? "Chamber heater" : "Chamber", cap);
        else strlcpy(label, nm, cap);
        if (label[0] >= 'a' && label[0] <= 'z') label[0] -= 32;
    }
    return label;
}

static void add_heater(poll_ctx_t *c, const char *obj, bool settable)
{
    if (c->nheaters >= MAX_HEATERS) return;
    for (int i = 0; i < c->nheaters; i++) if (!strcmp(c->heaters[i].obj, obj)) return;
    heater_t *h = &c->heaters[c->nheaters++];
    memset(h, 0, sizeof(*h));
    strlcpy(h->obj, obj, sizeof(h->obj));
    heater_kind_label(obj, &h->kind, h->label, sizeof(h->label));
    h->settable = settable;
    h->power = -1;
}

static void q_append(char **q, size_t *cap, const char *obj, const char *fields)
{
    char enc[128];
    url_encode(obj, enc, sizeof(enc), 0);
    size_t need = strlen(*q) + strlen(enc) + (fields ? strlen(fields) : 0) + 4;
    if (need > *cap) {
        *cap = need * 2;
        *q = realloc(*q, *cap);
    }
    if ((*q)[0]) strcat(*q, "&");
    strcat(*q, enc);
    if (fields) {
        strcat(*q, "=");
        strcat(*q, fields);
    }
}

static int heater_cmp(const void *a, const void *b)
{
    const heater_t *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return strcmp(x->obj, y->obj);
}

static bool discover_objects(poll_ctx_t *c)
{
    cJSON *j;
    int st = get_json(c, "/printer/objects/list", &j, 5000);
    if (st != 200 || !j) { cJSON_Delete(j); return false; }
    const cJSON *objs = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "objects");
    if (!cJSON_IsArray(objs)) { cJSON_Delete(j); return false; }

    size_t cap = 512, lcap = 256;
    char *q = calloc(1, cap);
    char *ql = calloc(1, lcap);
    q_append(&ql, &lcap, "webhooks", "state,state_message");
    q_append(&ql, &lcap, "print_stats", "state,filename,print_duration,total_duration,filament_used,message,info");
    q_append(&ql, &lcap, "virtual_sdcard", "progress,file_position");
    q_append(&ql, &lcap, "display_status", "progress");
    q_append(&q, &cap, "webhooks", "state,state_message");
    q_append(&q, &cap, "print_stats", "state,filename,print_duration,total_duration,filament_used,message,info");
    q_append(&q, &cap, "virtual_sdcard", "progress,is_active,file_position");
    q_append(&q, &cap, "display_status", "progress,message");
    q_append(&q, &cap, "toolhead", "homed_axes,position,extruder");
    q_append(&q, &cap, "gcode_move", "speed_factor,extrude_factor,homing_origin");
    q_append(&q, &cap, "idle_timeout", "state");

    c->nheaters = 0;
    c->nmacros = 0;
    c->has_qgl = c->has_ztilt = c->has_bed_mesh = false;
    c->has_fan = false;
    c->nafc_lanes = 0;
    c->mmu = MMU_NONE;
    bool has_fan = false;
    const cJSON *it;
    cJSON_ArrayForEach(it, objs) {
        const char *o = cJSON_GetStringValue(it);
        if (!o) continue;
        bool ext = !strncmp(o, "extruder", 8) && (o[8] == 0 || isdigit((unsigned char)o[8]));
        if (ext || !strcmp(o, "heater_bed")) {
            add_heater(c, o, true);
            q_append(&q, &cap, o, "temperature,target,power");
            q_append(&ql, &lcap, o, "temperature,target");
        } else if (strstr(o, "chamber") &&
                   (!strncmp(o, "temperature_sensor ", 19) || !strncmp(o, "heater_generic ", 15) ||
                    !strncmp(o, "temperature_fan ", 16))) {
            bool settable = strncmp(o, "temperature_sensor ", 19) != 0;
            add_heater(c, o, settable);
            q_append(&q, &cap, o, settable ? "temperature,target,power,speed" : "temperature");
            q_append(&ql, &lcap, o, settable ? "temperature,target" : "temperature");
        } else if (!strncmp(o, "gcode_macro ", 12)) {
            const char *m = o + 12;
            /* never truncate: a cut-off name would run the wrong (or no) macro */
            if (m[0] != '_' && c->nmacros < MAX_MACROS && strlen(m) < sizeof(c->macros[0]))
                strlcpy(c->macros[c->nmacros++], m, sizeof(c->macros[0]));
        } else if (!strcmp(o, "quad_gantry_level")) {
            c->has_qgl = true;
        } else if (!strcmp(o, "z_tilt")) {
            c->has_ztilt = true;
        } else if (!strcmp(o, "bed_mesh")) {
            c->has_bed_mesh = true;
        } else if (!strcmp(o, "fan")) {
            has_fan = true;
            q_append(&q, &cap, o, "speed");
        } else if (!strcmp(o, "AFC")) {
            c->mmu = MMU_AFC;
            q_append(&q, &cap, o, NULL);
        } else if ((!strncmp(o, "AFC_stepper ", 12) || !strncmp(o, "AFC_lane ", 9)) && c->nafc_lanes < MAX_LANES) {
            strlcpy(c->afc_lanes[c->nafc_lanes++], o, sizeof(c->afc_lanes[0]));
            q_append(&q, &cap, o, NULL);
        } else if (!strcmp(o, "mmu")) {
            c->mmu = MMU_HAPPY_HARE;
            q_append(&q, &cap, o, "enabled,num_gates,gate,tool,filament,filament_pos,action,"
                                  "gate_status,gate_material,gate_color,gate_filament_name,ttg_map,unit,is_homed");
        }
    }
    c->has_fan = has_fan;
    qsort(c->heaters, c->nheaters, sizeof(heater_t), heater_cmp);
    qsort(c->macros, c->nmacros, sizeof(c->macros[0]), (int (*)(const void *, const void *))strcasecmp);
    cJSON_Delete(j);
    if (c->mmu != MMU_AFC) c->nafc_lanes = 0;
    free(c->query);
    free(c->query_lite);
    c->query = q;
    c->query_lite = ql;
    ESP_LOGI(TAG, "[%s] %d heaters, mmu=%d, query %u bytes", c->slot->cfg.name, c->nheaters, c->mmu, (unsigned)strlen(q));
    return true;
}

static void fetch_webcams(poll_ctx_t *c, printer_state_t *s)
{
    cJSON *j;
    s->nwebcams = 0;
    if (get_json(c, "/server/webcams/list", &j, 5000) != 200 || !j) { cJSON_Delete(j); return; }
    const cJSON *cams = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "webcams");
    const cJSON *w;
    cJSON_ArrayForEach(w, cams) {
        if (s->nwebcams >= MAX_WEBCAMS) break;
        if (cJSON_IsFalse(cJSON_GetObjectItem(w, "enabled"))) continue;
        webcam_t *cam = &s->webcams[s->nwebcams];
        memset(cam, 0, sizeof(*cam));
        jstr(w, "name", cam->name, sizeof(cam->name));
        jstr(w, "snapshot_url", cam->snapshot_url, sizeof(cam->snapshot_url));
        jstr(w, "stream_url", cam->stream_url, sizeof(cam->stream_url));
        cam->flip_h = jbool(w, "flip_horizontal", false);
        cam->flip_v = jbool(w, "flip_vertical", false);
        cam->rotation = (int)jnum(w, "rotation", 0);
        if (cam->snapshot_url[0] || cam->stream_url[0]) s->nwebcams++;
    }
    cJSON_Delete(j);
}

static void parse_afc(poll_ctx_t *c, const cJSON *status, mmu_info_t *m)
{
    const cJSON *afc = cJSON_GetObjectItem(status, "AFC");
    m->type = MMU_AFC;
    m->nlanes = 0;
    m->action[0] = 0;
    char cur[32] = "";
    if (afc) {
        jstr(afc, "current_load", cur, sizeof(cur));
        if (!cur[0]) jstr(afc, "current_lane", cur, sizeof(cur));
        jstr(afc, "current_state", m->action, sizeof(m->action));
        const cJSON *units = cJSON_GetObjectItem(afc, "units");
        m->unit[0] = 0;
        const cJSON *u;
        cJSON_ArrayForEach(u, units) {
            const char *s = cJSON_GetStringValue(u);
            if (!s) continue;
            if (m->unit[0]) strlcat(m->unit, ", ", sizeof(m->unit));
            strlcat(m->unit, s, sizeof(m->unit));
        }
    }
    strlcpy(m->current, cur[0] ? cur : "None", sizeof(m->current));
    bool any_loaded = false;
    for (int i = 0; i < c->nafc_lanes && m->nlanes < MAX_LANES; i++) {
        const cJSON *l = cJSON_GetObjectItem(status, c->afc_lanes[i]);
        if (!l) continue;
        mmu_lane_t *ln = &m->lanes[m->nlanes++];
        memset(ln, 0, sizeof(*ln));
        const char *sp = strchr(c->afc_lanes[i], ' ');
        strlcpy(ln->name, sp ? sp + 1 : c->afc_lanes[i], sizeof(ln->name));
        jstr(l, "name", ln->name, sizeof(ln->name));
        jstr(l, "map", ln->tool, sizeof(ln->tool));
        jstr(l, "material", ln->material, sizeof(ln->material));
        char col[24] = "";
        jstr(l, "color", col, sizeof(col));
        ln->has_color = parse_color(col, &ln->color);
        const cJSON *sid = cJSON_GetObjectItem(l, "spool_id");
        if (cJSON_IsNumber(sid)) snprintf(ln->spool, sizeof(ln->spool), "Spool #%d", sid->valueint);
        else if (cJSON_IsString(sid) && sid->valuestring[0]) snprintf(ln->spool, sizeof(ln->spool), "Spool #%s", sid->valuestring);
        ln->weight_g = (int)jnum(l, "weight", -1);
        ln->present = jbool(l, "load", false) || jbool(l, "prep", false);
        ln->loaded = jbool(l, "tool_loaded", false);
        ln->at_hub = jbool(l, "loaded_to_hub", false);
        jstr(l, "status", ln->status, sizeof(ln->status));
        if (!ln->status[0]) jstr(l, "filament_status", ln->status, sizeof(ln->status));
        ln->active = cur[0] && !strcmp(cur, ln->name);
        if (ln->loaded) any_loaded = true;
    }
    /* Path graphic: the lane in the toolhead wins, else the current lane, else one sitting at the hub. */
    strcpy(m->hub_name, "Hub");
    m->active_lane = -1;
    m->stage = -1;
    for (int i = 0; i < m->nlanes; i++) if (m->lanes[i].loaded) { m->active_lane = i; m->stage = 4; }
    if (m->active_lane < 0)
        for (int i = 0; i < m->nlanes; i++) if (m->lanes[i].active) { m->active_lane = i; m->stage = m->lanes[i].at_hub ? 2 : 0; }
    if (m->active_lane < 0)
        for (int i = 0; i < m->nlanes; i++) if (m->lanes[i].at_hub) { m->active_lane = i; m->stage = 1; }
    strlcpy(m->filament, any_loaded ? "Loaded" : "Unloaded", sizeof(m->filament));
}

static void parse_hh(const cJSON *status, mmu_info_t *m)
{
    const cJSON *mmu = cJSON_GetObjectItem(status, "mmu");
    m->type = MMU_HAPPY_HARE;
    m->nlanes = 0;
    m->action[0] = 0;
    m->filament[0] = 0;
    if (!mmu) return;
    int gate = (int)jnum(mmu, "gate", -1);
    int tool = (int)jnum(mmu, "tool", -1);
    if (tool == -2) strlcpy(m->current, "Bypass", sizeof(m->current));
    else if (tool >= 0) snprintf(m->current, sizeof(m->current), "T%d (gate %d)", tool, gate);
    else strlcpy(m->current, "None", sizeof(m->current));
    jstr(mmu, "filament", m->filament, sizeof(m->filament));
    strcpy(m->hub_name, "Selector");
    m->active_lane = gate >= 0 && gate < MAX_LANES ? gate : -1;
    /* HH filament_pos: 0 unloaded, 1 homed at gate, 2 start bowden, 3 in bowden,
     * 4 end bowden, 5 homed extruder, 6 extruder entry, 7 in extruder, 8 loaded */
    int fp = (int)jnum(mmu, "filament_pos", -1);
    m->stage = fp < 0 ? -1 : fp == 0 ? 0 : fp == 1 ? 1 : fp <= 4 ? 2 : fp <= 7 ? 3 : 4;
    jstr(mmu, "action", m->action, sizeof(m->action));
    const cJSON *unit = cJSON_GetObjectItem(mmu, "unit");
    if (cJSON_IsString(unit)) strlcpy(m->unit, unit->valuestring, sizeof(m->unit));
    else strlcpy(m->unit, "Happy Hare", sizeof(m->unit));

    const cJSON *gs = cJSON_GetObjectItem(mmu, "gate_status");
    const cJSON *gm = cJSON_GetObjectItem(mmu, "gate_material");
    const cJSON *gc = cJSON_GetObjectItem(mmu, "gate_color");
    const cJSON *gn = cJSON_GetObjectItem(mmu, "gate_filament_name");
    const cJSON *ttg = cJSON_GetObjectItem(mmu, "ttg_map");
    int n = (int)jnum(mmu, "num_gates", cJSON_GetArraySize(gs));
    bool loaded = m->filament[0] && !strcasecmp(m->filament, "Loaded");
    for (int g = 0; g < n && m->nlanes < MAX_LANES; g++) {
        mmu_lane_t *ln = &m->lanes[m->nlanes++];
        memset(ln, 0, sizeof(*ln));
        snprintf(ln->name, sizeof(ln->name), "Gate %d", g);
        int t = -1;
        for (int k = 0; k < cJSON_GetArraySize(ttg); k++)
            if (cJSON_GetArrayItem(ttg, k)->valueint == g) { t = k; break; }
        if (t >= 0) snprintf(ln->tool, sizeof(ln->tool), "T%d", t);
        const char *mat = cJSON_GetStringValue(cJSON_GetArrayItem(gm, g));
        if (mat) strlcpy(ln->material, mat, sizeof(ln->material));
        const char *col = cJSON_GetStringValue(cJSON_GetArrayItem(gc, g));
        ln->has_color = parse_color(col, &ln->color);
        const char *fn = cJSON_GetStringValue(cJSON_GetArrayItem(gn, g));
        if (fn) strlcpy(ln->spool, fn, sizeof(ln->spool));
        const cJSON *sv = cJSON_GetArrayItem(gs, g);
        int st = cJSON_IsNumber(sv) ? sv->valueint : -1;
        ln->present = st > 0;
        strlcpy(ln->status, st > 0 ? (st == 2 ? "Buffered" : "Available") : st == 0 ? "Empty" : "Unknown", sizeof(ln->status));
        ln->active = g == gate;
        ln->loaded = ln->active && loaded;
        ln->weight_g = -1;
    }
}

static void fetch_metadata(poll_ctx_t *c, printer_state_t *s)
{
    char enc[400], path[512];
    s->est_slicer = 0;
    s->filament_total = 0;
    s->gcode_start = s->gcode_end = 0;
    url_encode(s->filename, enc, sizeof(enc), 1);
    snprintf(path, sizeof(path), "/server/files/metadata?filename=%s", enc);
    cJSON *j;
    if (get_json(c, path, &j, 5000) != 200 || !j) { cJSON_Delete(j); return; }
    const cJSON *r = cJSON_GetObjectItem(j, "result");
    s->est_slicer = jnum(r, "estimated_time", 0);
    s->filament_total = jnum(r, "filament_total", 0);
    double gs = jnum(r, "gcode_start_byte", 0), ge = jnum(r, "gcode_end_byte", 0);
    if (gs >= 0 && ge > gs) { s->gcode_start = (uint32_t)gs; s->gcode_end = (uint32_t)ge; }
    if (s->layer_total < 0) {
        const cJSON *lc = cJSON_GetObjectItem(r, "layer_count");
        if (cJSON_IsNumber(lc)) s->layer_total = lc->valueint;
    }
    /* Pick the largest thumbnail no wider than 400 px. */
    const cJSON *thumbs = cJSON_GetObjectItem(r, "thumbnails");
    const cJSON *t, *best = NULL;
    int bw = 0;
    cJSON_ArrayForEach(t, thumbs) {
        int w = (int)jnum(t, "width", 0);
        if (w <= 400 && w > bw) { bw = w; best = t; }
    }
    char rel[200] = "";
    if (best) jstr(best, "relative_path", rel, sizeof(rel));
    cJSON_Delete(j);
    if (!rel[0]) return;

    /* Thumbnail path is relative to the gcode's directory. */
    char dir[160] = "";
    strlcpy(dir, s->filename, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    char full[400];
    snprintf(full, sizeof(full), "%s%s", dir, rel);
    url_encode(full, enc, sizeof(enc), 1);
    char url[600];
    snprintf(url, sizeof(url), "%s/server/files/gcodes/%s", c->base, enc);
    printer_cfg_t cfg;
    LOCK();
    cfg = c->slot->cfg;
    UNLOCK();
    http_buf_t tb = { .max = 512 * 1024 };
    int st = http_request(&c->conn, HTTP_METHOD_GET, url, cfg.api_key, NULL, NULL, 0, &tb, 8000);
    if (st == 200 && tb.len > 8 && tb.data[0] == 0x89 && tb.data[1] == 'P') {
        LOCK();
        if (c->slot->st.thumb) heap_caps_free(c->slot->st.thumb);
        c->slot->st.thumb = tb.data;
        c->slot->st.thumb_len = tb.len;
        c->slot->st.thumb_ver = ++s_thumb_gen;
        UNLOCK();
        ESP_LOGI(TAG, "[%s] thumbnail %ux? %u bytes", cfg.name, bw, (unsigned)tb.len);
        return;
    }
    http_buf_free(&tb);
}

static void clear_thumb(slot_t *sl)
{
    LOCK();
    if (sl->st.thumb) heap_caps_free(sl->st.thumb);
    sl->st.thumb = NULL;
    sl->st.thumb_len = 0;
    sl->st.thumb_ver = ++s_thumb_gen;
    UNLOCK();
}

static printer_status_t map_print_state(const char *s)
{
    if (!strcmp(s, "printing")) return PS_PRINTING;
    if (!strcmp(s, "paused")) return PS_PAUSED;
    if (!strcmp(s, "complete")) return PS_COMPLETE;
    if (!strcmp(s, "cancelled")) return PS_CANCELLED;
    if (!strcmp(s, "error")) return PS_ERROR;
    return PS_STANDBY;
}

static void set_offline(poll_ctx_t *c, printer_status_t st, const char *msg)
{
    LOCK();
    c->slot->st.status = st;
    c->slot->st.online = st != PS_OFFLINE;
    if (msg) strlcpy(c->slot->st.message, msg, sizeof(c->slot->st.message));
    UNLOCK();
}

static void poll_once(poll_ctx_t *c)
{
    printer_state_t *s = heap_caps_malloc(sizeof(printer_state_t), MALLOC_CAP_SPIRAM);
    if (!s) return;
    LOCK();
    *s = c->slot->st;
    UNLOCK();
    cJSON *j = NULL;

    if (!c->have_objects) {
        int st = get_json(c, "/server/info", &j, 4000);
        if (st != 200 || !j) {
            cJSON_Delete(j);
            set_offline(c, PS_OFFLINE, st < 0 ? "Unreachable" : "Moonraker error");
            free(s);
            return;
        }
        const cJSON *r = cJSON_GetObjectItem(j, "result");
        char ks[16] = "";
        jstr(r, "klippy_state", ks, sizeof(ks));
        cJSON_Delete(j);
        j = NULL;
        LOCK();
        strlcpy(c->slot->st.klippy_state, ks, sizeof(c->slot->st.klippy_state));
        UNLOCK();
        /* printer/info carries the state message (and hostname) even when not ready */
        char msg[160] = "", host[48] = "", ver[40] = "";
        if (get_json(c, "/printer/info", &j, 4000) == 200 && j) {
            const cJSON *pi = cJSON_GetObjectItem(j, "result");
            jstr(pi, "state_message", msg, sizeof(msg));
            jstr(pi, "hostname", host, sizeof(host));
            jstr(pi, "software_version", ver, sizeof(ver));
        }
        cJSON_Delete(j);
        j = NULL;
        LOCK();
        if (host[0]) strlcpy(c->slot->st.hostname, host, sizeof(c->slot->st.hostname));
        if (ver[0]) strlcpy(c->slot->st.sw_version, ver, sizeof(c->slot->st.sw_version));
        UNLOCK();
        if (strcmp(ks, "ready") != 0) {
            for (char *p = msg; *p; p++) if (*p == '\n') *p = ' ';
            set_offline(c, PS_KLIPPY_DOWN, msg[0] ? msg : ks);
            free(s);
            return;
        }
        if (!discover_objects(c)) { set_offline(c, PS_KLIPPY_DOWN, "Object list failed"); free(s); return; }
        fetch_webcams(c, s);
        /* Spoolman (404 when Moonraker has no [spoolman] section) */
        s->has_spoolman = false;
        s->active_spool = -1;
        if (get_json(c, "/server/spoolman/status", &j, 4000) == 200 && j) {
            const cJSON *r = cJSON_GetObjectItem(j, "result");
            if (jhas(r, "spoolman_connected")) {
                s->has_spoolman = true;
                s->spoolman_connected = jbool(r, "spoolman_connected", false);
                s->active_spool = (int)jnum(r, "spool_id", -1);
            }
        }
        cJSON_Delete(j);
        j = NULL;
        s->nmacros = c->nmacros;
        memcpy(s->macros, c->macros, sizeof(s->macros));
        s->has_qgl = c->has_qgl;
        s->has_ztilt = c->has_ztilt;
        s->has_bed_mesh = c->has_bed_mesh;
        s->has_fan = c->has_fan;
        LOCK();
        c->slot->st.nwebcams = s->nwebcams;
        memcpy(c->slot->st.webcams, s->webcams, sizeof(s->webcams));
        UNLOCK();
        c->have_objects = true;
        c->last_file[0] = 0;
    }

    /* Unfocused printers (home tiles) get the light query: less JSON, less parsing. */
    bool full = s_focus_slot == (int)(c->slot - s_slots);
    const char *qs = full ? c->query : c->query_lite;
    char *path = malloc(strlen(qs) + 32);
    sprintf(path, "/printer/objects/query?%s", qs);
    int st = get_json(c, path, &j, 4000);
    free(path);
    if (st != 200 || !j) {
        cJSON_Delete(j);
        c->have_objects = false;   /* re-check server/klippy state next time */
        set_offline(c, st < 0 ? PS_OFFLINE : PS_KLIPPY_DOWN, st < 0 ? "Unreachable" : "Klipper not ready");
        free(s);
        return;
    }
    const cJSON *status = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "status");

    const cJSON *wh = cJSON_GetObjectItem(status, "webhooks");
    jstr(wh, "state", s->klippy_state, sizeof(s->klippy_state));
    s->message[0] = 0;
    jstr(wh, "state_message", s->message, sizeof(s->message));

    const cJSON *ps = cJSON_GetObjectItem(status, "print_stats");
    char pstate[16] = "standby";
    jstr(ps, "state", pstate, sizeof(pstate));
    s->filename[0] = 0;
    jstr(ps, "filename", s->filename, sizeof(s->filename));
    s->print_duration = jnum(ps, "print_duration", 0);
    s->total_duration = jnum(ps, "total_duration", 0);
    s->filament_used = jnum(ps, "filament_used", 0);
    char pmsg[120] = "";
    jstr(ps, "message", pmsg, sizeof(pmsg));
    const cJSON *info = cJSON_GetObjectItem(ps, "info");
    s->layer = (int)jnum(info, "current_layer", -1);
    int lt = (int)jnum(info, "total_layer", -1);
    if (lt > 0) s->layer_total = lt;

    if (strcmp(s->klippy_state, "ready") != 0) {
        s->status = PS_KLIPPY_DOWN;
        c->have_objects = false;
    } else {
        s->status = map_print_state(pstate);
        if (s->status == PS_ERROR && pmsg[0]) strlcpy(s->message, pmsg, sizeof(s->message));
    }
    s->online = true;

    const cJSON *vsd = cJSON_GetObjectItem(status, "virtual_sdcard");
    const cJSON *ds = cJSON_GetObjectItem(status, "display_status");
    float p = jnum(vsd, "progress", 0);
    float pd = jnum(ds, "progress", 0);
    s->progress = p > 0 ? p : pd;
    double fpos = jnum(vsd, "file_position", -1);

    const cJSON *th = cJSON_GetObjectItem(status, "toolhead");
    if (full) {
    jstr(th, "homed_axes", s->homed, sizeof(s->homed));
    const cJSON *pos = cJSON_GetObjectItem(th, "position");
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetArrayItem(pos, i);
        s->pos[i] = cJSON_IsNumber(v) ? v->valuedouble : 0;
    }
    const cJSON *gm = cJSON_GetObjectItem(status, "gcode_move");
    s->speed_factor = jnum(gm, "speed_factor", 1);
    s->extrude_factor = jnum(gm, "extrude_factor", 1);
    const cJSON *ho = cJSON_GetObjectItem(gm, "homing_origin");
    const cJSON *hz = cJSON_GetArrayItem(ho, 2);
    if (cJSON_IsNumber(hz)) s->z_offset = hz->valuedouble;
    const cJSON *fan = cJSON_GetObjectItem(status, "fan");
    s->fan_speed = fan ? jnum(fan, "speed", 0) : -1;
    }

    s->nheaters = c->nheaters;
    for (int i = 0; i < c->nheaters; i++) {
        heater_t *h = &s->heaters[i];
        *h = c->heaters[i];
        const cJSON *o = cJSON_GetObjectItem(status, h->obj);
        h->temp = jnum(o, "temperature", 0);
        h->target = jnum(o, "target", 0);
        h->power = jhas(o, "power") ? jnum(o, "power", 0) : jhas(o, "speed") ? jnum(o, "speed", 0) : -1;
    }

    if (!full) s->mmu.type = c->mmu;   /* tiles only need the type; lanes come with the full query */
    else if (c->mmu == MMU_AFC) parse_afc(c, status, &s->mmu);
    else if (c->mmu == MMU_HAPPY_HARE) parse_hh(status, &s->mmu);
    else s->mmu.type = MMU_NONE;
    cJSON_Delete(j);

    /* New job: fetch slicer estimate + thumbnail once. */
    bool file_changed = strcmp(s->filename, c->last_file) != 0;
    if (file_changed) {
        strlcpy(c->last_file, s->filename, sizeof(c->last_file));
        s->layer_total = lt > 0 ? lt : -1;
        clear_thumb(c->slot);
        s->est_slicer = 0;
        s->filament_total = 0;
        s->gcode_start = s->gcode_end = 0;
        if (s->filename[0]) fetch_metadata(c, s);
    }

    /* Progress like Mainsail ("file relative"): position within the G-code itself,
     * so the slicer's thumbnails at the top and config block at the end don't count.
     * Raw virtual_sdcard progress lags behind and made the time left too long. */
    if (s->gcode_end > s->gcode_start && fpos >= 0) {
        if (fpos <= s->gcode_start) s->progress = 0;
        else if (fpos >= s->gcode_end) s->progress = 1;
        else s->progress = (float)((fpos - s->gcode_start) / (double)(s->gcode_end - s->gcode_start));
    }

    /* Time left, like Mainsail's default: the average of the file-progress and the
     * filament-used estimates. Early on (under 5%) those swing wildly, so the
     * slicer's own estimate is used while there is one. */
    s->eta_s = -1;
    if (s->status == PS_PRINTING || s->status == PS_PAUSED) {
        float pdur = s->print_duration;
        float file_eta = (s->progress > 0.001f && pdur > 0) ? pdur / s->progress - pdur : -1;
        float fil = (s->filament_total > 0 && s->filament_used > 0) ? s->filament_used / s->filament_total : 0;
        float fil_eta = (fil > 0.001f && pdur > 0) ? pdur / fil - pdur : -1;
        float slicer_eta = s->est_slicer > 0 ? s->est_slicer - pdur : -1;
        float sum = 0;
        int n = 0;
        if (file_eta > 0) { sum += file_eta; n++; }
        if (fil_eta > 0) { sum += fil_eta; n++; }
        if (s->progress < 0.05f && slicer_eta > 0) s->eta_s = slicer_eta;
        else if (n) s->eta_s = sum / n;
        else if (slicer_eta > 0) s->eta_s = slicer_eta;
        if (s->eta_s < 0 && s->progress > 0) s->eta_s = 0;
    }

    s->last_ok_us = esp_timer_get_time();

    /* Job finished / failed / cancelled: queue an alert (only on a real transition). */
    bool was_job = c->status_known && (c->last_status == PS_PRINTING || c->last_status == PS_PAUSED);
    bool ended = s->status == PS_COMPLETE || s->status == PS_ERROR || s->status == PS_CANCELLED ||
                 s->status == PS_KLIPPY_DOWN;
    if (was_job && ended) {
        LOCK();
        job_alert_t *a = &s_alerts[(s_alert_head + s_alert_n) % ALERT_Q];
        if (s_alert_n < ALERT_Q) s_alert_n++;
        else s_alert_head = (s_alert_head + 1) % ALERT_Q;   /* full: drop the oldest */
        memset(a, 0, sizeof(*a));
        a->printer = -1;
        for (int i = 0; i < s_count; i++) if (&s_slots[s_order[i]] == c->slot) a->printer = i;
        strlcpy(a->name, c->slot->cfg.name, sizeof(a->name));
        const char *fn = strrchr(s->filename, '/');
        strlcpy(a->file, fn ? fn + 1 : s->filename, sizeof(a->file));
        a->status = s->status;
        a->duration_s = s->print_duration;
        strlcpy(a->message, s->message, sizeof(a->message));
        UNLOCK();
        ESP_LOGI(TAG, "[%s] job ended: %s", c->slot->cfg.name, printer_status_str(s->status));
    }
    c->last_status = s->status;
    c->status_known = true;

    LOCK();
    /* keep fields the poller does not own */
    s->thumb = c->slot->st.thumb;
    s->thumb_len = c->slot->st.thumb_len;
    s->thumb_ver = c->slot->st.thumb_ver;
    s->seq = c->slot->st.seq + 1;
    c->slot->st = *s;
    UNLOCK();
    free(s);
}

static void poll_task(void *arg)
{
    slot_t *sl = arg;
    poll_ctx_t *c = calloc(1, sizeof(poll_ctx_t));
    c->slot = sl;
    c->buf.max = 256 * 1024;
    LOCK();
    snprintf(c->base, sizeof(c->base), "http://%s:%u", sl->cfg.host, sl->cfg.port);
    UNLOCK();

    while (!sl->stop) {
        if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
            set_offline(c, PS_OFFLINE, "No Wi-Fi");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        LOCK();
        char nb[96];
        snprintf(nb, sizeof(nb), "http://%s:%u", sl->cfg.host, sl->cfg.port);
        UNLOCK();
        if (strcmp(nb, c->base)) {   /* host edited */
            strlcpy(c->base, nb, sizeof(c->base));
            http_conn_close(&c->conn);
            c->have_objects = false;
        }
        poll_once(c);
        bool focused = s_focus_slot == (int)(sl - s_slots);
        int ms = focused ? g_settings.poll_ms : g_settings.poll_bg_ms;
        if (!sl->st.online) ms = focused ? 3000 : 8000;
        if (!c->have_objects && sl->st.online) ms = focused ? 2000 : 5000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
    }
    http_conn_close(&c->conn);
    http_buf_free(&c->buf);
    free(c->query);
    free(c->query_lite);
    free(c);
    LOCK();
    if (sl->st.thumb) heap_caps_free(sl->st.thumb);
    sl->st.thumb = NULL;
    sl->alive = false;
    sl->used = false;
    UNLOCK();
    vTaskDeleteWithCaps(NULL);
}

static void start_slot(int si)
{
    slot_t *sl = &s_slots[si];
    memset(&sl->st, 0, sizeof(sl->st));
    sl->st.status = PS_OFFLINE;
    sl->st.eta_s = -1;
    sl->st.layer = sl->st.layer_total = -1;
    strcpy(sl->st.message, "Connecting");
    sl->stop = false;
    sl->alive = true;
    sl->used = true;
    xTaskCreateWithCaps(poll_task, "poll", 8192, sl, 4, &sl->task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_PRINTERS; i++)
        if (!s_slots[i].used && !s_slots[i].alive) return i;
    return -1;
}

/* ── public API ──────────────────────────────────────────────────────── */

void printers_init(void)
{
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    s_mtx = xSemaphoreCreateMutex();

    nvs_handle_t h;
    if (nvs_open("printers", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_str(h, "list", NULL, &len) == ESP_OK && len > 1) {
        char *txt = malloc(len);
        nvs_get_str(h, "list", txt, &len);
        cJSON *arr = cJSON_Parse(txt);
        free(txt);
        const cJSON *o;
        cJSON_ArrayForEach(o, arr) {
            printer_cfg_t c = {0};
            jstr(o, "n", c.name, sizeof(c.name));
            jstr(o, "h", c.host, sizeof(c.host));
            c.port = (uint16_t)jnum(o, "p", 7125);
            jstr(o, "k", c.api_key, sizeof(c.api_key));
            if (!c.host[0]) continue;
            int si = free_slot();
            if (si < 0) break;
            s_slots[si].cfg = c;
            s_order[s_count++] = si;
            start_slot(si);
        }
        cJSON_Delete(arr);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d printers loaded", s_count);
}

int printers_count(void) { return s_count; }

bool printers_get_cfg(int idx, printer_cfg_t *out)
{
    if (idx < 0 || idx >= s_count) return false;
    LOCK();
    *out = s_slots[s_order[idx]].cfg;
    UNLOCK();
    return true;
}

int printers_add(const printer_cfg_t *cfg)
{
    LOCK();
    int si = free_slot();
    if (si < 0 || s_count >= MAX_PRINTERS) { UNLOCK(); return -1; }
    s_slots[si].cfg = *cfg;
    if (!s_slots[si].cfg.port) s_slots[si].cfg.port = 7125;
    s_order[s_count++] = si;
    start_slot(si);
    save_list();
    int idx = s_count - 1;
    UNLOCK();
    return idx;
}

bool printers_update(int idx, const printer_cfg_t *cfg)
{
    if (idx < 0 || idx >= s_count) return false;
    LOCK();
    slot_t *sl = &s_slots[s_order[idx]];
    sl->cfg = *cfg;
    save_list();
    UNLOCK();
    if (sl->task) xTaskNotifyGive(sl->task);
    return true;
}

bool printers_remove(int idx)
{
    if (idx < 0 || idx >= s_count) return false;
    LOCK();
    int si = s_order[idx];
    s_slots[si].stop = true;
    TaskHandle_t t = s_slots[si].task;
    for (int i = idx; i < s_count - 1; i++) s_order[i] = s_order[i + 1];
    s_count--;
    if (s_focus_slot == si) s_focus_slot = -1;
    save_list();
    UNLOCK();
    if (t) xTaskNotifyGive(t);
    return true;
}

int printers_find_host(const char *host, uint16_t port)
{
    int r = -1;
    LOCK();
    for (int i = 0; i < s_count; i++) {
        printer_cfg_t *c = &s_slots[s_order[i]].cfg;
        if (!strcasecmp(c->host, host) && (port == 0 || c->port == port)) { r = i; break; }
    }
    UNLOCK();
    return r;
}

bool printers_get_state(int idx, printer_state_t *out)
{
    if (idx < 0 || idx >= s_count) return false;
    LOCK();
    *out = s_slots[s_order[idx]].st;
    out->thumb = NULL;
    UNLOCK();
    return true;
}

uint8_t *printers_copy_thumb(int idx, uint32_t *ver, size_t *len)
{
    if (idx < 0 || idx >= s_count) return NULL;
    uint8_t *r = NULL;
    LOCK();
    printer_state_t *st = &s_slots[s_order[idx]].st;
    if (st->thumb_ver != *ver) {
        *ver = st->thumb_ver;
        *len = 0;
        if (st->thumb && st->thumb_len) {
            r = heap_caps_malloc(st->thumb_len, MALLOC_CAP_SPIRAM);
            if (r) {
                memcpy(r, st->thumb, st->thumb_len);
                *len = st->thumb_len;
            }
        }
    }
    UNLOCK();
    return r;
}

void printers_set_focus(int idx)
{
    int si = (idx >= 0 && idx < s_count) ? s_order[idx] : -1;
    s_focus_slot = si;
    if (si >= 0 && s_slots[si].task) xTaskNotifyGive(s_slots[si].task);   /* refresh now */
}

/* ── commands ────────────────────────────────────────────────────────── */

typedef struct {
    printer_cfg_t cfg;
    printer_cmd_t cmd;
    char gcode[256];
    char label[40];     /* what the toast calls it */
    bool quiet;         /* only report failures */
    int slot;
} cmd_job_t;

static void cmd_task(void *arg)
{
    cmd_job_t *job = arg;
    char path[900], enc[800];
    const char *what = "Command";
    int timeout = 10000;
    switch (job->cmd) {
    case CMD_GCODE:
        url_encode(job->gcode, enc, sizeof(enc), 0);
        snprintf(path, sizeof(path), "/printer/gcode/script?script=%s", enc);
        what = job->label;
        timeout = 180000;   /* G28/QGL/etc. block until finished */
        break;
    case CMD_PAUSE:  strcpy(path, "/printer/print/pause");  what = "Pause"; break;
    case CMD_RESUME: strcpy(path, "/printer/print/resume"); what = "Resume"; break;
    case CMD_CANCEL: strcpy(path, "/printer/print/cancel"); what = "Cancel"; break;
    case CMD_ESTOP:  strcpy(path, "/printer/emergency_stop"); what = "Emergency stop"; break;
    case CMD_FIRMWARE_RESTART: strcpy(path, "/printer/firmware_restart"); what = "Firmware restart"; break;
    case CMD_KLIPPER_RESTART:  strcpy(path, "/printer/restart"); what = "Restart"; break;
    case CMD_SERVICE_RESTART:
    case CMD_SERVICE_STOP:
    case CMD_SERVICE_START:
        url_encode(job->gcode, enc, sizeof(enc), 0);
        snprintf(path, sizeof(path), "/machine/services/%s?service=%s",
                 job->cmd == CMD_SERVICE_RESTART ? "restart" : job->cmd == CMD_SERVICE_STOP ? "stop" : "start", enc);
        snprintf(job->label, sizeof(job->label), "%s %s",
                 job->cmd == CMD_SERVICE_RESTART ? "Restart" : job->cmd == CMD_SERVICE_STOP ? "Stop" : "Start", job->gcode);
        what = job->label;
        timeout = 30000;
        break;
    case CMD_PRINT_START:
        url_encode(job->gcode, enc, sizeof(enc), 1);
        snprintf(path, sizeof(path), "/printer/print/start?filename=%s", enc);
        what = "Start print";
        break;
    case CMD_HOST_REBOOT:   strcpy(path, "/machine/reboot"); what = "Host reboot"; break;
    case CMD_HOST_SHUTDOWN: strcpy(path, "/machine/shutdown"); what = "Host shutdown"; break;
    }
    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%u%s", job->cfg.host, job->cfg.port, path);
    esp_http_client_handle_t conn = NULL;
    http_buf_t buf = { .max = 32 * 1024 };
    int st = http_request(&conn, HTTP_METHOD_POST, url, job->cfg.api_key, NULL, NULL, 0, &buf, timeout);
    http_conn_close(&conn);

    char res[160];
    bool report = true;
    if (st == 200) {
        snprintf(res, sizeof(res), "%s: %s done", job->cfg.name, what);
        report = !job->quiet;
    } else if (st < 0 && (job->cmd == CMD_HOST_REBOOT || job->cmd == CMD_HOST_SHUTDOWN ||
                          (job->cmd >= CMD_SERVICE_RESTART && job->cmd <= CMD_SERVICE_START &&
                           !strcasecmp(job->gcode, "moonraker")))) {
        /* these take the connection down with them: no reply is the expected outcome */
        snprintf(res, sizeof(res), "%s: %s sent", job->cfg.name, what);
    } else if (st < 0) {
        snprintf(res, sizeof(res), "%s: no response from %s", what, job->cfg.name);
    } else {
        char msg[120] = "";
        cJSON *j = buf.data ? cJSON_ParseWithLength((char *)buf.data, buf.len) : NULL;
        jstr(cJSON_GetObjectItem(j, "error"), "message", msg, sizeof(msg));
        cJSON_Delete(j);
        snprintf(res, sizeof(res), "%s: %s failed - %s", job->cfg.name, what, msg[0] ? msg : "HTTP error");
    }
    http_buf_free(&buf);
    ESP_LOGI(TAG, "%s", res);
    LOCK();
    if (report) {
        strlcpy(s_result, res, sizeof(s_result));
        s_result_new = true;
    }
    if (job->slot >= 0 && s_slots[job->slot].task) xTaskNotifyGive(s_slots[job->slot].task);
    UNLOCK();
    free(job);
    vTaskDeleteWithCaps(NULL);
}

static void submit(int idx, printer_cmd_t cmd, const char *gcode, const char *label, bool quiet)
{
    if (idx < 0 || idx >= s_count) return;
    cmd_job_t *job = calloc(1, sizeof(*job));
    if (!job) return;
    LOCK();
    job->slot = s_order[idx];
    job->cfg = s_slots[job->slot].cfg;
    UNLOCK();
    job->cmd = cmd;
    job->quiet = quiet;
    if (gcode) strlcpy(job->gcode, gcode, sizeof(job->gcode));
    if (label && label[0]) strlcpy(job->label, label, sizeof(job->label));
    else if (gcode) {   /* default label: first word of the G-code */
        size_t n = strcspn(gcode, " \n");
        if (n >= sizeof(job->label)) n = sizeof(job->label) - 1;
        memcpy(job->label, gcode, n);
        job->label[n] = 0;
    }
    /* E-stop gets top priority. */
    if (xTaskCreateWithCaps(cmd_task, "cmd", 6144, job, cmd == CMD_ESTOP ? 8 : 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) free(job);
}

void printers_command(int idx, printer_cmd_t cmd, const char *gcode) { submit(idx, cmd, gcode, NULL, false); }

void printers_gcode(int idx, const char *gcode, const char *label, bool quiet)
{
    submit(idx, CMD_GCODE, gcode, label, quiet);
}

bool printers_pop_alert(job_alert_t *out)
{
    bool r = false;
    LOCK();
    if (s_alert_n) {
        *out = s_alerts[s_alert_head];
        s_alert_head = (s_alert_head + 1) % ALERT_Q;
        s_alert_n--;
        r = true;
    }
    UNLOCK();
    return r;
}

bool printers_last_result(char *out, size_t cap)
{
    if (!s_result_new) return false;
    LOCK();
    strlcpy(out, s_result, cap);
    s_result_new = false;
    UNLOCK();
    return true;
}
