/*
 * G-code file browser data: GET /server/files/list?root=gcodes (sorted newest
 * first), and per-file GET /server/files/metadata + the largest thumbnail up to
 * 400 px, fetched only for the file being looked at. Freed when the page closes.
 */
#include "files.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "http_util.h"
#include "printers.h"

static const char *TAG = "files";

static SemaphoreHandle_t s_mtx;
static gfile_t *s_files;
static int s_nfiles;
static gmeta_t s_meta;              /* one file's details at a time */
static volatile fl_state_t s_state;
static volatile uint32_t s_ver;
static char s_err[96];
static volatile bool s_busy_list, s_busy_meta;

static void lock(void) { if (!s_mtx) s_mtx = xSemaphoreCreateMutex(); xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static int cmp_newest(const void *a, const void *b)
{
    double x = ((const gfile_t *)a)->modified, y = ((const gfile_t *)b)->modified;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void list_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    printer_cfg_t pc;
    esp_http_client_handle_t conn = NULL;
    http_buf_t b = { .max = 1024 * 1024 };
    gfile_t *tmp = NULL;
    int n = 0;
    bool ok = false;
    if (!printers_get_cfg(idx, &pc)) { strcpy(s_err, "No such printer"); goto done; }
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/server/files/list?root=gcodes", pc.host, pc.port);
    int st = http_request(&conn, HTTP_METHOD_GET, url, pc.api_key, NULL, NULL, 0, &b, 10000);
    if (st != 200 || !b.data) {
        snprintf(s_err, sizeof(s_err), st < 0 ? "No response from the printer" : "Moonraker HTTP %d", st);
        goto done;
    }
    cJSON *j = cJSON_ParseWithLength((char *)b.data, b.len);
    const cJSON *arr = cJSON_GetObjectItem(j, "result");
    int total = cJSON_GetArraySize(arr);
    tmp = heap_caps_calloc(total > 0 ? total : 1, sizeof(gfile_t), MALLOC_CAP_SPIRAM);
    const cJSON *f;
    cJSON_ArrayForEach(f, arr) {
        if (!tmp) break;
        const cJSON *p = cJSON_GetObjectItem(f, "path");
        if (!cJSON_IsString(p) || strlen(p->valuestring) >= sizeof(tmp[0].path)) continue;
        const char *ext = strrchr(p->valuestring, '.');
        if (!ext || (strcasecmp(ext, ".gcode") && strcasecmp(ext, ".gco") && strcasecmp(ext, ".g") &&
                     strcasecmp(ext, ".ufp"))) continue;
        gfile_t *g = &tmp[n++];
        strlcpy(g->path, p->valuestring, sizeof(g->path));
        const cJSON *mo = cJSON_GetObjectItem(f, "modified"), *sz = cJSON_GetObjectItem(f, "size");
        g->modified = cJSON_IsNumber(mo) ? mo->valuedouble : 0;
        g->size = cJSON_IsNumber(sz) && sz->valuedouble > 0 ? (uint32_t)sz->valuedouble : 0;
    }
    cJSON_Delete(j);
    if (tmp) qsort(tmp, n, sizeof(gfile_t), cmp_newest);
    if (n > FILES_MAX) n = FILES_MAX;
    ok = tmp != NULL;
done:
    http_conn_close(&conn);
    http_buf_free(&b);
    lock();
    if (ok) {
        heap_caps_free(s_files);
        s_files = tmp;
        s_nfiles = n;
        s_state = FL_OK;
    } else {
        heap_caps_free(tmp);
        s_state = FL_ERROR;
    }
    s_ver++;
    s_busy_list = false;
    unlock();
    vTaskDeleteWithCaps(NULL);
}

typedef struct { int idx; char path[128]; } meta_job_t;

static void meta_task(void *arg)
{
    meta_job_t *job = arg;
    printer_cfg_t pc;
    esp_http_client_handle_t conn = NULL;
    http_buf_t b = { .max = 256 * 1024 };
    gmeta_t m = { 0 };
    strlcpy(m.path, job->path, sizeof(m.path));
    if (!printers_get_cfg(job->idx, &pc)) goto done;
    char enc[400], url[600];
    url_encode(job->path, enc, sizeof(enc), 1);
    snprintf(url, sizeof(url), "http://%s:%u/server/files/metadata?filename=%s", pc.host, pc.port, enc);
    if (http_request(&conn, HTTP_METHOD_GET, url, pc.api_key, NULL, NULL, 0, &b, 8000) != 200 || !b.data) goto done;
    cJSON *j = cJSON_ParseWithLength((char *)b.data, b.len);
    const cJSON *r = cJSON_GetObjectItem(j, "result");
    m.est_time = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "estimated_time"));
    m.filament_mm = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "filament_total"));
    m.filament_g = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "filament_weight_total"));
    m.layer_h = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "layer_height"));
    if (m.est_time != m.est_time) m.est_time = 0;   /* NaN -> 0 when the key is missing */
    if (m.filament_mm != m.filament_mm) m.filament_mm = 0;
    if (m.filament_g != m.filament_g) m.filament_g = 0;
    if (m.layer_h != m.layer_h) m.layer_h = 0;
    const cJSON *sl = cJSON_GetObjectItem(r, "slicer");
    if (cJSON_IsString(sl)) strlcpy(m.slicer, sl->valuestring, sizeof(m.slicer));
    const cJSON *t, *best = NULL;
    int bw = 0;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(r, "thumbnails")) {
        const cJSON *wj = cJSON_GetObjectItem(t, "width");
        int w = cJSON_IsNumber(wj) ? wj->valueint : 0;
        if (w <= 400 && w > bw) { bw = w; best = t; }
    }
    char rel[200] = "";
    const cJSON *rp = best ? cJSON_GetObjectItem(best, "relative_path") : NULL;
    if (cJSON_IsString(rp)) strlcpy(rel, rp->valuestring, sizeof(rel));
    cJSON_Delete(j);
    m.ok = true;
    if (rel[0]) {
        char dir[128];
        strlcpy(dir, job->path, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
        char full[400];
        snprintf(full, sizeof(full), "%s%s", dir, rel);
        url_encode(full, enc, sizeof(enc), 1);
        snprintf(url, sizeof(url), "http://%s:%u/server/files/gcodes/%s", pc.host, pc.port, enc);
        http_buf_t tb = { .max = 512 * 1024 };
        if (http_request(&conn, HTTP_METHOD_GET, url, pc.api_key, NULL, NULL, 0, &tb, 8000) == 200 &&
            tb.len > 8 && tb.data[0] == 0x89 && tb.data[1] == 'P') {
            m.thumb = tb.data;
            m.thumb_len = tb.len;
        } else http_buf_free(&tb);
    }
done:
    http_conn_close(&conn);
    http_buf_free(&b);
    lock();
    heap_caps_free(s_meta.thumb);
    s_meta = m;
    s_ver++;
    s_busy_meta = false;
    unlock();
    ESP_LOGI(TAG, "metadata %s: %s, thumb %u bytes", job->path, m.ok ? "ok" : "failed", (unsigned)m.thumb_len);
    free(job);
    vTaskDeleteWithCaps(NULL);
}

void files_fetch(int idx)
{
    if (s_busy_list) return;
    s_busy_list = true;
    s_err[0] = 0;
    s_state = FL_LOADING;
    if (xTaskCreateWithCaps(list_task, "files", 8192, (void *)(intptr_t)idx, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy_list = false;
        s_state = FL_ERROR;
        strcpy(s_err, "Out of memory");
    }
}

void files_fetch_meta(int idx, const char *path)
{
    if (s_busy_meta) return;   /* the page re-asks once the current one lands */
    meta_job_t *j = malloc(sizeof(*j));
    if (!j) return;
    j->idx = idx;
    strlcpy(j->path, path, sizeof(j->path));
    s_busy_meta = true;
    if (xTaskCreateWithCaps(meta_task, "fmeta", 8192, j, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy_meta = false;
        free(j);
    }
}

fl_state_t files_state(char *err, int cap)
{
    if (err) strlcpy(err, s_err, cap);
    return s_state;
}

uint32_t files_version(void) { return s_ver; }

int files_list(gfile_t *out, int max)
{
    lock();
    int n = s_nfiles < max ? s_nfiles : max;
    if (n > 0) memcpy(out, s_files, n * sizeof(gfile_t));
    unlock();
    return n;
}

bool files_meta(const char *path, gmeta_t *out)
{
    bool r = false;
    lock();
    if (s_meta.path[0] && !strcmp(s_meta.path, path)) {
        *out = s_meta;
        out->thumb = NULL;
        if (s_meta.thumb && s_meta.thumb_len) {
            out->thumb = heap_caps_malloc(s_meta.thumb_len, MALLOC_CAP_SPIRAM);
            if (out->thumb) memcpy(out->thumb, s_meta.thumb, s_meta.thumb_len);
            else out->thumb_len = 0;
        }
        r = true;
    }
    unlock();
    return r;
}

void files_free(void)
{
    lock();
    heap_caps_free(s_files);
    s_files = NULL;
    s_nfiles = 0;
    heap_caps_free(s_meta.thumb);
    memset(&s_meta, 0, sizeof(s_meta));
    s_state = FL_IDLE;
    unlock();
}
