/*
 * System services (GET /machine/system_info) and console history
 * (GET /server/gcode_store) for the open printer. One PSRAM-stack task polls
 * while the System or Console tab is visible; nothing runs otherwise.
 */
#include "machine.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "http_util.h"
#include "printers.h"

static const char *TAG = "machine";

typedef enum { MODE_NONE = 0, MODE_SERVICES, MODE_CONSOLE, MODE_HISTORY, MODE_MESH } mode_t_;

static SemaphoreHandle_t s_mtx;
static volatile mode_t_ s_mode;
static volatile int s_idx = -1;
static volatile uint32_t s_gen;        /* bumps on every start/stop: old tasks exit */
static TaskHandle_t s_task;
static volatile uint32_t s_ver;

static svc_t *s_svc;
static int s_nsvc = -1;
static con_line_t *s_con;              /* ring, oldest first after copy */
static int s_ncon;
static char s_err[96];
static hist_job_t *s_hist;
static int s_nhist = -1;
static hist_totals_t s_tot;
static mesh_t *s_mesh;
static bool s_mesh_ok;

static void lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }

/* Keep text drawable by Montserrat: ASCII, degree sign and bullet. NBSP becomes a
 * space, box-drawing becomes - | +, anything else is dropped (no tofu boxes). */
static void sanitize(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 4 < cap) {
        uint32_t cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0x10000; len = 4; }
        else { p++; continue; }
        if (cp == '\n' || cp == '\r' || cp == '\t') out[o++] = ' ';
        else if (cp < 0x80) out[o++] = (char)cp;
        else if (cp == 0xA0 || cp == 0x2007 || cp == 0x202F) out[o++] = ' ';
        else if (cp == 0xB0 || cp == 0x2022) { memcpy(out + o, p, len); o += len; }
        else if (cp >= 0x2500 && cp <= 0x257F) {
            bool vert = cp == 0x2502 || cp == 0x2503 || cp == 0x2551;
            bool horiz = cp == 0x2500 || cp == 0x2501 || cp == 0x2550;
            out[o++] = vert ? '|' : horiz ? '-' : '+';
        } else if (cp >= 0x2580 && cp <= 0x259F) out[o++] = '#';   /* block elements (progress bars) */
        p += len;
    }
    out[o] = 0;
}
static void unlock(void) { xSemaphoreGive(s_mtx); }

static void fetch_services(esp_http_client_handle_t *conn, http_buf_t *b, const printer_cfg_t *pc, uint32_t gen)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/machine/system_info", pc->host, pc->port);
    int st = http_request(conn, HTTP_METHOD_GET, url, pc->api_key, NULL, NULL, 0, b, 8000);
    if (st != 200 || !b->data) {
        lock();
        snprintf(s_err, sizeof(s_err), st < 0 ? "No response from Moonraker" : "Moonraker HTTP %d", st);
        unlock();
        return;
    }
    cJSON *j = cJSON_ParseWithLength((char *)b->data, b->len);
    const cJSON *si = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "system_info");
    const cJSON *avail = cJSON_GetObjectItem(si, "available_services");
    const cJSON *state = cJSON_GetObjectItem(si, "service_state");
    svc_t tmp[SVC_MAX];
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, avail) {
        if (n >= SVC_MAX || !cJSON_IsString(a)) continue;
        svc_t *v = &tmp[n++];
        memset(v, 0, sizeof(*v));
        strlcpy(v->name, a->valuestring, sizeof(v->name));
        const cJSON *ss = cJSON_GetObjectItem(state, a->valuestring);
        const cJSON *act = cJSON_GetObjectItem(ss, "active_state");
        const cJSON *sub = cJSON_GetObjectItem(ss, "sub_state");
        if (cJSON_IsString(act)) strlcpy(v->active, act->valuestring, sizeof(v->active));
        if (cJSON_IsString(sub)) strlcpy(v->sub, sub->valuestring, sizeof(v->sub));
    }
    cJSON_Delete(j);
    lock();
    if (gen != s_gen) { unlock(); return; }   /* a newer session started: drop stale data */
    memcpy(s_svc, tmp, n * sizeof(svc_t));
    s_nsvc = n;
    s_err[0] = 0;
    s_ver++;
    unlock();
}

static void fetch_console(esp_http_client_handle_t *conn, http_buf_t *b, const printer_cfg_t *pc, uint32_t gen)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/server/gcode_store?count=%d", pc->host, pc->port, CONSOLE_MAX);
    int st = http_request(conn, HTTP_METHOD_GET, url, pc->api_key, NULL, NULL, 0, b, 6000);
    if (st != 200 || !b->data) {
        lock();
        snprintf(s_err, sizeof(s_err), st < 0 ? "No response from Moonraker" : "Moonraker HTTP %d", st);
        unlock();
        return;
    }
    cJSON *j = cJSON_ParseWithLength((char *)b->data, b->len);
    const cJSON *arr = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "gcode_store");
    int total = cJSON_GetArraySize(arr);
    int start = total > CONSOLE_MAX ? total - CONSOLE_MAX : 0;
    /* Only republish when the newest entry changed (cheap change detection). */
    double newest = 0;
    const cJSON *last = cJSON_GetArrayItem(arr, total - 1);
    if (last) newest = cJSON_GetNumberValue(cJSON_GetObjectItem(last, "time"));
    lock();
    bool changed = total != s_ncon || (s_ncon && s_con[s_ncon - 1].time != newest);
    unlock();
    if (changed) {
        con_line_t *tmp = heap_caps_malloc(CONSOLE_MAX * sizeof(con_line_t), MALLOC_CAP_SPIRAM);
        int n = 0;
        for (int i = start; tmp && i < total; i++) {
            const cJSON *e = cJSON_GetArrayItem(arr, i);
            const cJSON *m = cJSON_GetObjectItem(e, "message");
            const cJSON *t = cJSON_GetObjectItem(e, "type");
            if (!cJSON_IsString(m)) continue;
            con_line_t *l = &tmp[n++];
            l->time = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "time"));
            l->type = (cJSON_IsString(t) && !strcmp(t->valuestring, "command")) ? 'c' : 'r';
            sanitize(m->valuestring, l->msg, sizeof(l->msg));
        }
        if (tmp) {
            lock();
            if (gen == s_gen) memcpy(s_con, tmp, n * sizeof(con_line_t));
            else n = s_ncon;   /* stale task: keep the current data */
            s_ncon = n;
            s_err[0] = 0;
            s_ver++;
            unlock();
            heap_caps_free(tmp);
        }
    }
    cJSON_Delete(j);
}

static int get(esp_http_client_handle_t *conn, http_buf_t *b, const printer_cfg_t *pc, const char *path, cJSON **j)
{
    char url[200];
    snprintf(url, sizeof(url), "http://%s:%u%s", pc->host, pc->port, path);
    int st = http_request(conn, HTTP_METHOD_GET, url, pc->api_key, NULL, NULL, 0, b, 8000);
    *j = NULL;
    if (st == 200 && b->data) *j = cJSON_ParseWithLength((char *)b->data, b->len);
    else {
        lock();
        snprintf(s_err, sizeof(s_err), st < 0 ? "No response from Moonraker" : "Moonraker HTTP %d", st);
        unlock();
    }
    return st;
}

static float fnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : 0;
}

#define STR_(x) #x
#define STR(x) STR_(x)
#define HIST_LIMIT_STR STR(HIST_MAX)

static void fetch_history(esp_http_client_handle_t *conn, http_buf_t *b, const printer_cfg_t *pc, uint32_t gen)
{
    cJSON *j;
    get(conn, b, pc, "/server/history/list?limit=" HIST_LIMIT_STR "&order=desc", &j);
    if (!j) return;
    hist_job_t *tmp = heap_caps_calloc(HIST_MAX, sizeof(hist_job_t), MALLOC_CAP_SPIRAM);
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "jobs")) {
        if (!tmp || n >= HIST_MAX) break;
        hist_job_t *h = &tmp[n++];
        const cJSON *fn = cJSON_GetObjectItem(e, "filename"), *st = cJSON_GetObjectItem(e, "status");
        if (cJSON_IsString(fn)) {
            const char *sl = strrchr(fn->valuestring, '/');
            strlcpy(h->file, sl ? sl + 1 : fn->valuestring, sizeof(h->file));
        }
        if (cJSON_IsString(st)) strlcpy(h->status, st->valuestring, sizeof(h->status));
        const cJSON *t0 = cJSON_GetObjectItem(e, "start_time");
        h->start = cJSON_IsNumber(t0) ? t0->valuedouble : 0;
        h->print_s = fnum(e, "print_duration");
        h->total_s = fnum(e, "total_duration");
        h->filament_mm = fnum(e, "filament_used");
    }
    cJSON_Delete(j);
    hist_totals_t tot = { 0 };
    if (get(conn, b, pc, "/server/history/totals", &j) == 200 && j) {
        const cJSON *t = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "job_totals");
        tot.jobs = (int)fnum(t, "total_jobs");
        tot.total_s = fnum(t, "total_time");
        tot.print_s = fnum(t, "total_print_time");
        tot.filament_mm = fnum(t, "total_filament_used");
    }
    cJSON_Delete(j);
    lock();
    if (gen == s_gen && tmp) {
        /* Only announce new data when something changed: the History tab
         * rebuilds its whole list on every new version. */
        bool changed = n != s_nhist || s_err[0] || memcmp(s_hist, tmp, n * sizeof(hist_job_t)) ||
                       memcmp(&s_tot, &tot, sizeof(tot));
        memcpy(s_hist, tmp, n * sizeof(hist_job_t));
        s_nhist = n;
        s_tot = tot;
        s_err[0] = 0;
        if (changed) s_ver++;
    }
    unlock();
    heap_caps_free(tmp);
}

static void fetch_mesh(esp_http_client_handle_t *conn, http_buf_t *b, const printer_cfg_t *pc, uint32_t gen)
{
    cJSON *j;
    get(conn, b, pc, "/printer/objects/query?bed_mesh&toolhead=axis_minimum,axis_maximum", &j);
    if (!j) return;
    const cJSON *stt = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "status");
    const cJSON *bm = cJSON_GetObjectItem(stt, "bed_mesh");
    mesh_t *m = heap_caps_calloc(1, sizeof(mesh_t), MALLOC_CAP_SPIRAM);
    if (m) {   /* bed size for the 3D view: the X/Y travel limits */
        const cJSON *th = cJSON_GetObjectItem(stt, "toolhead");
        const cJSON *amin = cJSON_GetObjectItem(th, "axis_minimum"), *amax = cJSON_GetObjectItem(th, "axis_maximum");
        if (cJSON_GetArraySize(amin) >= 2 && cJSON_GetArraySize(amax) >= 2) {
            m->bed_min_x = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(amin, 0));
            m->bed_min_y = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(amin, 1));
            m->bed_max_x = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(amax, 0));
            m->bed_max_y = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(amax, 1));
            m->has_bed = m->bed_max_x > m->bed_min_x && m->bed_max_y > m->bed_min_y;
        }
    }
    if (m && bm) {
        const cJSON *pn = cJSON_GetObjectItem(bm, "profile_name");
        if (cJSON_IsString(pn)) strlcpy(m->profile, pn->valuestring, sizeof(m->profile));
        const cJSON *mn = cJSON_GetObjectItem(bm, "mesh_min"), *mx = cJSON_GetObjectItem(bm, "mesh_max");
        m->min_x = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(mn, 0));
        m->min_y = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(mn, 1));
        m->max_x = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(mx, 0));
        m->max_y = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(mx, 1));
        const cJSON *mat = cJSON_GetObjectItem(bm, "probed_matrix");
        int rows = cJSON_GetArraySize(mat);
        int cols = rows ? cJSON_GetArraySize(cJSON_GetArrayItem(mat, 0)) : 0;
        /* Klipper reports [[0.0]] when no mesh is loaded */
        if (rows > 1 && cols > 1 && rows <= MESH_MAX && cols <= MESH_MAX) {
            m->rows = rows;
            m->cols = cols;
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    const cJSON *v = cJSON_GetArrayItem(cJSON_GetArrayItem(mat, r), c);
                    m->z[r][c] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0;
                }
        }
        const cJSON *pr;
        cJSON_ArrayForEach(pr, cJSON_GetObjectItem(bm, "profiles")) {
            if (m->nprofiles >= 8 || !pr->string) continue;
            strlcpy(m->profiles[m->nprofiles++], pr->string, sizeof(m->profiles[0]));
        }
    }
    cJSON_Delete(j);
    lock();
    if (gen == s_gen && m) {
        *s_mesh = *m;
        s_mesh_ok = true;
        s_err[0] = 0;
        s_ver++;
    }
    unlock();
    heap_caps_free(m);
}

static void task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    esp_http_client_handle_t conn = NULL;
    http_buf_t b = { .max = 256 * 1024 };
    while (gen == s_gen) {
        printer_cfg_t pc;
        if (!printers_get_cfg(s_idx, &pc)) break;
        mode_t_ m = s_mode;
        if (m == MODE_SERVICES) fetch_services(&conn, &b, &pc, gen);
        else if (m == MODE_CONSOLE) fetch_console(&conn, &b, &pc, gen);
        else if (m == MODE_HISTORY) fetch_history(&conn, &b, &pc, gen);
        else if (m == MODE_MESH) fetch_mesh(&conn, &b, &pc, gen);
        else break;
        int wait = m == MODE_CONSOLE ? 1000 : m == MODE_SERVICES ? 5000 : m == MODE_MESH ? 10000 : 15000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
    http_conn_close(&conn);
    http_buf_free(&b);
    lock();
    if (s_task == xTaskGetCurrentTaskHandle()) s_task = NULL;
    unlock();
    vTaskDeleteWithCaps(NULL);
}

static void start(int idx, mode_t_ m)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        s_svc = heap_caps_calloc(SVC_MAX, sizeof(svc_t), MALLOC_CAP_SPIRAM);
        s_con = heap_caps_calloc(CONSOLE_MAX, sizeof(con_line_t), MALLOC_CAP_SPIRAM);
        s_hist = heap_caps_calloc(HIST_MAX, sizeof(hist_job_t), MALLOC_CAP_SPIRAM);
        s_mesh = heap_caps_calloc(1, sizeof(mesh_t), MALLOC_CAP_SPIRAM);
    }
    machine_stop();
    lock();
    s_idx = idx;
    s_mode = m;
    s_nsvc = -1;
    s_ncon = 0;
    s_nhist = -1;
    s_mesh_ok = false;
    s_err[0] = 0;
    s_ver++;
    uint32_t gen = ++s_gen;
    TaskHandle_t h = NULL;
    if (xTaskCreateWithCaps(task, "machine", 6144, (void *)(uintptr_t)gen, 3, &h,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS)
        s_task = h;
    else
        strcpy(s_err, "Out of memory");
    unlock();
}

void machine_start_services(int idx) { start(idx, MODE_SERVICES); }
void machine_start_console(int idx) { start(idx, MODE_CONSOLE); }
void machine_start_history(int idx) { start(idx, MODE_HISTORY); }
void machine_start_mesh(int idx) { start(idx, MODE_MESH); }

int machine_history(hist_job_t *out, int max, hist_totals_t *tot)
{
    if (!s_mtx) return -1;
    lock();
    int n = s_nhist;
    if (n > max) n = max;
    if (n > 0) memcpy(out, s_hist, n * sizeof(hist_job_t));
    if (tot) *tot = s_tot;
    unlock();
    return n;
}

bool machine_mesh(mesh_t *out)
{
    if (!s_mtx) return false;
    lock();
    bool ok = s_mesh_ok;
    if (ok) *out = *s_mesh;
    unlock();
    return ok;
}

void machine_stop(void)
{
    if (!s_mtx) return;
    lock();
    s_gen++;              /* the running task notices and exits on its own */
    s_mode = MODE_NONE;
    if (s_task) xTaskNotifyGive(s_task);
    unlock();
}

void machine_refresh_now(void)
{
    if (!s_mtx) return;
    lock();
    if (s_task) xTaskNotifyGive(s_task);
    unlock();
}

uint32_t machine_version(void) { return s_ver; }

int machine_services(svc_t *out, int max)
{
    if (!s_mtx) return -1;
    lock();
    int n = s_nsvc;
    if (n > max) n = max;
    if (n > 0) memcpy(out, s_svc, n * sizeof(svc_t));
    unlock();
    return n;
}

int machine_console(con_line_t *out, int max)
{
    if (!s_mtx) return 0;
    lock();
    int n = s_ncon < max ? s_ncon : max;
    if (n > 0) memcpy(out, s_con + (s_ncon - n), n * sizeof(con_line_t));
    unlock();
    return n;
}

bool machine_error(char *out, int cap)
{
    if (!s_mtx) return false;
    lock();
    bool e = s_err[0] != 0;
    if (e) strlcpy(out, s_err, cap);
    unlock();
    (void)TAG;
    return e;
}
