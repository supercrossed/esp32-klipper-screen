/*
 * Spoolman through Moonraker's proxy (POST /server/spoolman/proxy), so the
 * display never needs Spoolman's own address. Fetches run on a short-lived
 * task with a PSRAM stack; parsed results live in PSRAM until the page closes.
 */
#include "spoolman.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
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

static const char *TAG = "spoolman";

static SemaphoreHandle_t s_mtx;
static spool_item_t *s_items[2];
static int s_count[2];
static volatile sm_state_t s_state;
static char s_err[96];
static volatile uint32_t s_ver;
static volatile int s_active = -1;
static volatile bool s_busy;

static void lock(void) { if (!s_mtx) s_mtx = xSemaphoreCreateMutex(); xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static bool hex_color(const char *s, uint32_t *out)
{
    if (!s) return false;
    if (*s == '#') s++;
    if (strlen(s) < 6) return false;
    for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) return false;
    char t[7];
    memcpy(t, s, 6);
    t[6] = 0;
    *out = strtoul(t, NULL, 16);
    return true;
}

static void str(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v)) strlcpy(dst, v->valuestring, cap);
}

static double num(const cJSON *o, const char *k, double d)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : d;
}

static void parse_filament(const cJSON *f, spool_item_t *it)
{
    str(f, "name", it->name, sizeof(it->name));
    str(f, "material", it->material, sizeof(it->material));
    str(cJSON_GetObjectItem(f, "vendor"), "name", it->vendor, sizeof(it->vendor));
    const cJSON *c = cJSON_GetObjectItem(f, "color_hex");
    it->has_color = hex_color(cJSON_GetStringValue(c), &it->color);
    if (!it->has_color) {   /* multi-colour filaments: take the first */
        const cJSON *mc = cJSON_GetObjectItem(f, "multi_color_hexes");
        if (cJSON_IsString(mc)) it->has_color = hex_color(mc->valuestring, &it->color);
    }
    it->ext_temp = (int)num(f, "settings_extruder_temp", 0);
    it->bed_temp = (int)num(f, "settings_bed_temp", 0);
}

/* POST to the proxy; returns the Spoolman payload (array/object) or NULL. Caller frees root. */
static const cJSON *proxy_get(esp_http_client_handle_t *conn, const printer_cfg_t *pc, const char *path,
                              const char *query, cJSON **root)
{
    char url[160], body[256];
    snprintf(url, sizeof(url), "http://%s:%u/server/spoolman/proxy", pc->host, pc->port);
    snprintf(body, sizeof(body), "{\"request_method\":\"GET\",\"path\":\"%s\",\"query\":\"%s\",\"use_v2_response\":true}",
             path, query ? query : "");
    http_buf_t b = { .max = 1024 * 1024 };
    int st = http_request(conn, HTTP_METHOD_POST, url, pc->api_key, "application/json", body, strlen(body), &b, 10000);
    *root = NULL;
    if (st != 200 || !b.data) {
        snprintf(s_err, sizeof(s_err), st < 0 ? "No response from Moonraker" : "Spoolman proxy error (HTTP %d)", st);
        http_buf_free(&b);
        return NULL;
    }
    *root = cJSON_ParseWithLength((char *)b.data, b.len);
    http_buf_free(&b);
    const cJSON *res = cJSON_GetObjectItem(*root, "result");
    /* v2: {"result":{"response":..., "error":...}}; older Moonraker: {"result": payload} */
    const cJSON *resp = cJSON_GetObjectItem(res, "response");
    const cJSON *err = cJSON_GetObjectItem(res, "error");
    if (err && !cJSON_IsNull(err)) {
        const cJSON *m = cJSON_GetObjectItem(err, "message");
        snprintf(s_err, sizeof(s_err), "Spoolman: %s", cJSON_IsString(m) ? m->valuestring : "error");
        return NULL;
    }
    return resp ? resp : res;
}

static int parse_list(const cJSON *arr, int kind, spool_item_t **out)
{
    int n = cJSON_GetArraySize(arr);
    if (n > SPOOL_MAX) n = SPOOL_MAX;
    spool_item_t *items = heap_caps_calloc(n ? n : 1, sizeof(spool_item_t), MALLOC_CAP_SPIRAM);
    if (!items) return 0;
    int c = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (c >= n) break;
        spool_item_t *it = &items[c];
        it->id = (int)num(o, "id", 0);
        it->remaining_g = -1;
        it->total_g = -1;
        if (kind == 0) {
            const cJSON *f = cJSON_GetObjectItem(o, "filament");
            parse_filament(f, it);
            it->remaining_g = num(o, "remaining_weight", -1);
            it->total_g = num(o, "initial_weight", num(f, "weight", -1));
            str(o, "location", it->location, sizeof(it->location));
            str(o, "lot_nr", it->lot, sizeof(it->lot));
            it->archived = cJSON_IsTrue(cJSON_GetObjectItem(o, "archived"));
            if (it->archived) continue;
        } else {
            parse_filament(o, it);
            it->total_g = num(o, "weight", -1);
        }
        c++;
    }
    *out = items;
    return c;
}

static void fetch_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    printer_cfg_t pc;
    esp_http_client_handle_t conn = NULL;
    spool_item_t *sp = NULL, *fi = NULL;
    int nsp = 0, nfi = 0;
    bool ok = false;
    if (!printers_get_cfg(idx, &pc)) { strcpy(s_err, "No such printer"); goto done; }

    /* active spool + connection */
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/server/spoolman/status", pc.host, pc.port);
    http_buf_t b = { .max = 16 * 1024 };
    if (http_request(&conn, HTTP_METHOD_GET, url, pc.api_key, NULL, NULL, 0, &b, 5000) == 200 && b.data) {
        cJSON *j = cJSON_ParseWithLength((char *)b.data, b.len);
        const cJSON *r = cJSON_GetObjectItem(j, "result");
        s_active = (int)num(r, "spool_id", -1);
        bool conn_ok = cJSON_IsTrue(cJSON_GetObjectItem(r, "spoolman_connected"));
        cJSON_Delete(j);
        if (!conn_ok) {
            strcpy(s_err, "Moonraker is not connected to Spoolman");
            http_buf_free(&b);
            goto done;
        }
    }
    http_buf_free(&b);

    cJSON *root;
    const cJSON *arr = proxy_get(&conn, &pc, "/v1/spool", "allow_archived=false", &root);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); if (!s_err[0]) strcpy(s_err, "Unexpected spool list"); goto done; }
    nsp = parse_list(arr, 0, &sp);
    cJSON_Delete(root);
    arr = proxy_get(&conn, &pc, "/v1/filament", "", &root);
    if (cJSON_IsArray(arr)) nfi = parse_list(arr, 1, &fi);
    cJSON_Delete(root);
    ok = true;
    ESP_LOGI(TAG, "%d spools, %d filaments, active %d", nsp, nfi, s_active);

done:
    http_conn_close(&conn);
    lock();
    if (ok) {
        heap_caps_free(s_items[0]);
        heap_caps_free(s_items[1]);
        s_items[0] = sp;
        s_items[1] = fi;
        s_count[0] = nsp;
        s_count[1] = nfi;
        s_state = SM_OK;
    } else {
        heap_caps_free(sp);
        heap_caps_free(fi);
        s_state = SM_ERROR;
    }
    s_ver++;
    s_busy = false;
    unlock();
    vTaskDeleteWithCaps(NULL);
}

void spoolman_fetch(int idx)
{
    if (s_busy) return;
    s_busy = true;
    s_err[0] = 0;
    s_state = SM_LOADING;
    if (xTaskCreateWithCaps(fetch_task, "spoolman", 8192, (void *)(intptr_t)idx, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy = false;
        s_state = SM_ERROR;
        strcpy(s_err, "Out of memory");
    }
}

sm_state_t spoolman_state(char *err, int cap)
{
    if (err) strlcpy(err, s_err, cap);
    return s_state;
}

uint32_t spoolman_version(void) { return s_ver; }
int spoolman_active_spool(void) { return s_active; }

int spoolman_items(int kind, spool_item_t *out, int max)
{
    lock();
    int n = s_count[kind & 1] < max ? s_count[kind & 1] : max;
    if (n > 0) memcpy(out, s_items[kind & 1], n * sizeof(spool_item_t));
    unlock();
    return n;
}

typedef struct { int idx, id; } set_job_t;

static void set_task(void *arg)
{
    set_job_t *j = arg;
    printer_cfg_t pc;
    if (printers_get_cfg(j->idx, &pc)) {
        char url[160], body[48];
        esp_http_client_handle_t conn = NULL;
        snprintf(url, sizeof(url), "http://%s:%u/server/spoolman/spool_id", pc.host, pc.port);
        if (j->id >= 0) snprintf(body, sizeof(body), "{\"spool_id\":%d}", j->id);
        else strcpy(body, "{}");
        int st = http_request(&conn, HTTP_METHOD_POST, url, pc.api_key, "application/json", body, strlen(body), NULL, 8000);
        http_conn_close(&conn);
        if (st == 200) s_active = j->id;
        s_ver++;
        ESP_LOGI(TAG, "set active spool %d -> HTTP %d", j->id, st);
    }
    free(j);
    vTaskDeleteWithCaps(NULL);
}

void spoolman_set_active(int idx, int id)
{
    set_job_t *j = malloc(sizeof(*j));
    if (!j) return;
    j->idx = idx;
    j->id = id;
    if (xTaskCreateWithCaps(set_task, "spool_set", 6144, j, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) free(j);
}

void spoolman_free(void)
{
    lock();
    heap_caps_free(s_items[0]);
    heap_caps_free(s_items[1]);
    s_items[0] = s_items[1] = NULL;
    s_count[0] = s_count[1] = 0;
    s_state = SM_IDLE;
    unlock();
}
