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

typedef enum { MODE_NONE = 0, MODE_SERVICES, MODE_CONSOLE } mode_t_;

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
        else break;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(m == MODE_CONSOLE ? 1000 : 5000));
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
    }
    machine_stop();
    lock();
    s_idx = idx;
    s_mode = m;
    s_nsvc = -1;
    s_ncon = 0;
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
