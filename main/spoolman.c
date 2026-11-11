/*
 * Spoolman through Moonraker's proxy (POST /server/spoolman/proxy), so the
 * display never needs Spoolman's own address. Fetches and writes run on
 * short-lived tasks with PSRAM stacks; parsed results live in PSRAM until the
 * Spoolman pages close.
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

#define LOC_MAX 32
#define MAT_MAX 64

static SemaphoreHandle_t s_mtx;
static spool_item_t *s_items[SM_KINDS];
static int s_count[SM_KINDS];
static volatile sm_state_t s_state;
static char s_err[96];
static volatile uint32_t s_ver;
static volatile int s_active = -1;
static volatile bool s_busy;
static volatile int s_printer = -1;

/* metadata */
static sm_xfield_t *s_xf[SM_KINDS];
static int s_nxf[SM_KINDS];
static char (*s_loc)[64];
static int s_nloc;
static sm_material_t *s_mat;
static int s_nmat;
static char s_currency[8] = "";
static char s_web[96];

static void lock(void) { if (!s_mtx) s_mtx = xSemaphoreCreateMutex(); xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static void *ps_calloc(size_t n, size_t sz) { return heap_caps_calloc(n ? n : 1, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

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
    const cJSON *v = cJSON_GetObjectItem(f, "vendor");
    str(v, "name", it->vendor, sizeof(it->vendor));
    it->vendor_id = v ? (int)num(v, "id", -1) : -1;
    const cJSON *c = cJSON_GetObjectItem(f, "color_hex");
    it->has_color = hex_color(cJSON_GetStringValue(c), &it->color);
    if (!it->has_color) {   /* multi-colour filaments: take the first */
        const cJSON *mc = cJSON_GetObjectItem(f, "multi_color_hexes");
        if (cJSON_IsString(mc)) it->has_color = hex_color(mc->valuestring, &it->color);
    }
    it->ext_temp = (int)num(f, "settings_extruder_temp", 0);
    it->bed_temp = (int)num(f, "settings_bed_temp", 0);
    it->density = num(f, "density", 0);
    it->diameter = num(f, "diameter", 0);
    it->total_g = num(f, "weight", -1);
    it->spool_g = num(f, "spool_weight", v ? num(v, "empty_spool_weight", -1) : -1);
}

/* Spoolman's error text from a Moonraker v2 error object. */
static void error_text(const cJSON *err, int http, char *e, size_t ecap)
{
    const cJSON *m = cJSON_GetObjectItem(err, "message");
    const char *msg = cJSON_IsString(m) ? m->valuestring : NULL;
    /* the message is often Spoolman's own JSON body: {"message": ...} or {"detail": [{"msg": ...}]} */
    cJSON *j = msg ? cJSON_Parse(msg) : NULL;
    if (j) {
        const cJSON *d = cJSON_GetObjectItem(j, "detail");
        const cJSON *mm = cJSON_GetObjectItem(j, "message");
        if (cJSON_IsArray(d)) {
            const cJSON *d0 = cJSON_GetArrayItem(d, 0);
            const cJSON *loc = cJSON_GetObjectItem(d0, "loc");
            const cJSON *last = loc ? cJSON_GetArrayItem(loc, cJSON_GetArraySize(loc) - 1) : NULL;
            snprintf(e, ecap, "Spoolman: %s%s%s", cJSON_IsString(last) ? last->valuestring : "",
                     cJSON_IsString(last) ? ": " : "", cJSON_GetStringValue(cJSON_GetObjectItem(d0, "msg")) ?: "invalid");
        } else if (cJSON_IsString(d)) snprintf(e, ecap, "Spoolman: %s", d->valuestring);
        else if (cJSON_IsString(mm)) snprintf(e, ecap, "Spoolman: %s", mm->valuestring);
        else snprintf(e, ecap, "Spoolman error (HTTP %d)", http);
        cJSON_Delete(j);
    } else snprintf(e, ecap, "Spoolman: %s", msg ? msg : "error");
}

/* One request through the proxy. Returns Spoolman's payload (inside *root, which the
 * caller frees) or NULL with s_err set. body: JSON text or NULL. */
static const cJSON *proxy(esp_http_client_handle_t *conn, const printer_cfg_t *pc, const char *method,
                          const char *path, const char *query, const char *body, size_t max, cJSON **root,
                          char *e, size_t ecap)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/server/spoolman/proxy", pc->host, pc->port);
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "request_method", method);
    cJSON_AddStringToObject(rq, "path", path);
    if (query && query[0]) cJSON_AddStringToObject(rq, "query", query);
    if (body) {
        cJSON *b = cJSON_Parse(body);
        if (b) cJSON_AddItemToObject(rq, "body", b);
    }
    cJSON_AddTrueToObject(rq, "use_v2_response");
    char *txt = cJSON_PrintUnformatted(rq);
    cJSON_Delete(rq);
    *root = NULL;
    if (!txt) { strlcpy(e, "Out of memory", ecap); return NULL; }
    http_buf_t b = { .max = max };
    int st = http_request(conn, HTTP_METHOD_POST, url, pc->api_key, "application/json", txt, strlen(txt), &b, 20000);
    cJSON_free(txt);
    if (st != 200 || !b.data) {
        snprintf(e, ecap, st < 0 ? "No response from Moonraker" : "Spoolman proxy error (HTTP %d)", st);
        http_buf_free(&b);
        return NULL;
    }
    *root = cJSON_ParseWithLength((char *)b.data, b.len);
    http_buf_free(&b);
    if (!*root) { strlcpy(e, "Bad reply from Moonraker", ecap); return NULL; }
    const cJSON *res = cJSON_GetObjectItem(*root, "result");
    /* v2: {"result":{"response":..., "error":...}}; older Moonraker: {"result": payload} */
    const cJSON *resp = cJSON_GetObjectItem(res, "response");
    const cJSON *err = cJSON_GetObjectItem(res, "error");
    if (err && !cJSON_IsNull(err)) {
        error_text(err, (int)num(err, "status_code", 0), e, ecap);
        return NULL;
    }
    return resp ? resp : res;
}

static int parse_list(const cJSON *arr, int kind, spool_item_t **out)
{
    int n = cJSON_GetArraySize(arr);
    if (n > SPOOL_MAX) n = SPOOL_MAX;
    spool_item_t *items = ps_calloc(n, sizeof(spool_item_t));
    if (!items) return 0;
    int c = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (c >= n) break;
        spool_item_t *it = &items[c];
        it->id = (int)num(o, "id", 0);
        it->remaining_g = -1;
        it->total_g = -1;
        it->spool_g = -1;
        it->filament_id = it->vendor_id = -1;
        if (kind == SM_SPOOL) {
            const cJSON *f = cJSON_GetObjectItem(o, "filament");
            parse_filament(f, it);
            it->filament_id = (int)num(f, "id", -1);
            it->remaining_g = num(o, "remaining_weight", -1);
            it->total_g = num(o, "initial_weight", it->total_g);
            it->spool_g = num(o, "spool_weight", it->spool_g);
            str(o, "location", it->location, sizeof(it->location));
            str(o, "lot_nr", it->lot, sizeof(it->lot));
            it->archived = cJSON_IsTrue(cJSON_GetObjectItem(o, "archived"));
        } else if (kind == SM_FILAMENT) {
            parse_filament(o, it);
        } else {
            str(o, "name", it->name, sizeof(it->name));
            it->total_g = num(o, "empty_spool_weight", -1);
        }
        c++;
    }
    *out = items;
    return c;
}

static xf_type_t xf_type(const char *t)
{
    static const char *const N[] = { "text", "integer", "integer_range", "float", "float_range", "datetime", "boolean", "choice" };
    for (int i = 0; i < 8; i++) if (t && !strcmp(t, N[i])) return (xf_type_t)i;
    return XF_TEXT;
}

/* "extra_fields_<entity>" setting: a JSON array (as a string) of field definitions */
static int parse_xfields(const cJSON *settings, const char *key, sm_xfield_t **out)
{
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(settings, key), "value");
    cJSON *arr = cJSON_IsString(v) ? cJSON_Parse(v->valuestring) : NULL;
    int n = cJSON_GetArraySize(arr);
    if (n > SM_XF_MAX) n = SM_XF_MAX;
    sm_xfield_t *f = ps_calloc(n, sizeof(*f));
    int c = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (!f || c >= n) break;
        sm_xfield_t *x = &f[c];
        str(o, "key", x->key, sizeof(x->key));
        if (!x->key[0]) continue;
        str(o, "name", x->name, sizeof(x->name));
        if (!x->name[0]) strlcpy(x->name, x->key, sizeof(x->name));
        str(o, "unit", x->unit, sizeof(x->unit));
        x->type = xf_type(cJSON_GetStringValue(cJSON_GetObjectItem(o, "field_type")));
        x->multi = cJSON_IsTrue(cJSON_GetObjectItem(o, "multi_choice"));
        str(o, "default_value", x->deflt, sizeof(x->deflt));
        const cJSON *ch;
        cJSON_ArrayForEach(ch, cJSON_GetObjectItem(o, "choices")) {
            if (!cJSON_IsString(ch)) continue;
            if (x->choices[0]) strlcat(x->choices, "\n", sizeof(x->choices));
            strlcat(x->choices, ch->valuestring, sizeof(x->choices));
        }
        c++;
    }
    cJSON_Delete(arr);
    *out = f;
    return c;
}

static void setting_str(const cJSON *settings, const char *key, char *out, size_t cap)
{
    /* values are JSON-encoded: "\"EUR\"" */
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(settings, key), "value");
    cJSON *j = cJSON_IsString(v) ? cJSON_Parse(v->valuestring) : NULL;
    out[0] = 0;
    if (cJSON_IsString(j)) strlcpy(out, j->valuestring, cap);
    cJSON_Delete(j);
}

static void add_loc(char (*loc)[64], int *n, const char *s)
{
    if (!s || !s[0] || *n >= LOC_MAX) return;
    for (int i = 0; i < *n; i++) if (!strcmp(loc[i], s)) return;
    strlcpy(loc[(*n)++], s, 64);
}

static void currency_symbol(const char *code, char *out, size_t cap)
{
    static const char *const M[][2] = { { "EUR", "€" }, { "USD", "$" }, { "GBP", "£" }, { "JPY", "¥" }, { "CNY", "¥" },
                                        { "CAD", "CA$" }, { "AUD", "A$" }, { "CHF", "CHF" }, { "SEK", "kr" },
                                        { "NOK", "kr" }, { "DKK", "kr" }, { "PLN", "zł" }, { "RON", "lei" },
                                        { "INR", "₹" }, { "KRW", "₩" }, { "RUB", "₽" }, { "BRL", "R$" } };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (!strcasecmp(code, M[i][0])) { strlcpy(out, M[i][1], cap); return; }
    strlcpy(out, code[0] ? code : "€", cap);
}

/* Spoolman's web UI root, for the filament QR codes: its base_url setting, else
 * Moonraker's [spoolman] server (localhost there means the printer's own host). */
static void find_web_url(esp_http_client_handle_t *conn, const printer_cfg_t *pc, const char *base_url, char *out, size_t cap)
{
    out[0] = 0;
    if (base_url && base_url[0]) { strlcpy(out, base_url, cap); goto trim; }
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/server/config", pc->host, pc->port);
    http_buf_t b = { .max = 512 * 1024 };
    if (http_request(conn, HTTP_METHOD_GET, url, pc->api_key, NULL, NULL, 0, &b, 8000) == 200 && b.data) {
        cJSON *j = cJSON_ParseWithLength((char *)b.data, b.len);
        const cJSON *sm = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "config"), "spoolman");
        const char *srv = cJSON_GetStringValue(cJSON_GetObjectItem(sm, "server"));
        if (srv) {
            const char *p = strstr(srv, "://");
            const char *host = p ? p + 3 : srv;
            if (!strncmp(host, "localhost", 9) || !strncmp(host, "127.0.0.1", 9)) {
                const char *rest = host + 9;
                snprintf(out, cap, "%.*s%s%s", p ? (int)(host - srv) : 0, srv, pc->host, rest);
            } else strlcpy(out, srv, cap);
        }
        cJSON_Delete(j);
    }
    http_buf_free(&b);
trim:
    for (size_t l = strlen(out); l && out[l - 1] == '/'; l--) out[l - 1] = 0;
}

static void fetch_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    printer_cfg_t pc;
    esp_http_client_handle_t conn = NULL;
    spool_item_t *it[SM_KINDS] = {0};
    int n[SM_KINDS] = {0};
    sm_xfield_t *xf[SM_KINDS] = {0};
    int nxf[SM_KINDS] = {0};
    char (*loc)[64] = ps_calloc(LOC_MAX, 64);
    int nloc = 0;
    sm_material_t *mat = ps_calloc(MAT_MAX, sizeof(sm_material_t));
    int nmat = 0;
    char cur[16] = "EUR", base[96] = "", web[96] = "";
    bool ok = false;
    cJSON *root;
    const cJSON *arr;
    if (!printers_get_cfg(idx, &pc)) { strcpy(s_err, "No such printer"); goto done; }
    if (!loc || !mat) { strcpy(s_err, "Out of memory"); goto done; }

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

    static const char *const PATHS[SM_KINDS] = { "/v1/spool", "/v1/filament", "/v1/vendor" };
    for (int k = 0; k < SM_KINDS; k++) {
        arr = proxy(&conn, &pc, "GET", PATHS[k], k == SM_SPOOL ? "allow_archived=true" : "", NULL, 2 << 20, &root, s_err, sizeof(s_err));
        if (!cJSON_IsArray(arr)) {
            cJSON_Delete(root);
            if (k == SM_SPOOL) { if (!s_err[0]) strcpy(s_err, "Unexpected spool list"); goto done; }
            continue;
        }
        n[k] = parse_list(arr, k, &it[k]);
        cJSON_Delete(root);
    }

    /* settings: currency, base URL, extra fields */
    arr = proxy(&conn, &pc, "GET", "/v1/setting/", "", NULL, 256 * 1024, &root, s_err, sizeof(s_err));
    if (cJSON_IsObject(arr)) {
        setting_str(arr, "currency", cur, sizeof(cur));
        setting_str(arr, "base_url", base, sizeof(base));
        static const char *const XK[SM_KINDS] = { "extra_fields_spool", "extra_fields_filament", "extra_fields_vendor" };
        for (int k = 0; k < SM_KINDS; k++) nxf[k] = parse_xfields(arr, XK[k], &xf[k]);
        /* the ordered location list (settings > Locations) */
        const cJSON *lv = cJSON_GetObjectItem(cJSON_GetObjectItem(arr, "locations"), "value");
        cJSON *la = cJSON_IsString(lv) ? cJSON_Parse(lv->valuestring) : NULL;
        const cJSON *l;
        cJSON_ArrayForEach(l, la) add_loc(loc, &nloc, cJSON_GetStringValue(l));
        cJSON_Delete(la);
    }
    cJSON_Delete(root);
    arr = proxy(&conn, &pc, "GET", "/v1/location", "", NULL, 64 * 1024, &root, s_err, sizeof(s_err));
    const cJSON *l;
    cJSON_ArrayForEach(l, arr) add_loc(loc, &nloc, cJSON_GetStringValue(l));
    cJSON_Delete(root);

    /* materials: the ones in use, then Spoolman's list (with densities and temps) */
    arr = proxy(&conn, &pc, "GET", "/v1/material", "", NULL, 64 * 1024, &root, s_err, sizeof(s_err));
    cJSON_ArrayForEach(l, arr) {
        const char *m = cJSON_GetStringValue(l);
        if (m && m[0] && nmat < MAT_MAX) strlcpy(mat[nmat++].material, m, sizeof(mat[0].material));
    }
    cJSON_Delete(root);
    arr = proxy(&conn, &pc, "GET", "/v1/external/material", "", NULL, 64 * 1024, &root, s_err, sizeof(s_err));
    cJSON_ArrayForEach(l, arr) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(l, "material"));
        if (!m) continue;
        int i = 0;
        while (i < nmat && strcasecmp(mat[i].material, m)) i++;
        if (i == nmat) {
            if (nmat >= MAT_MAX) continue;
            strlcpy(mat[nmat++].material, m, sizeof(mat[0].material));
        }
        if (!mat[i].density) mat[i].density = num(l, "density", 0);
        if (!mat[i].ext_temp) mat[i].ext_temp = (int)num(l, "extruder_temp", 0);
        if (!mat[i].bed_temp) mat[i].bed_temp = (int)num(l, "bed_temp", 0);
    }
    cJSON_Delete(root);

    find_web_url(&conn, &pc, base, web, sizeof(web));
    ok = true;
    s_err[0] = 0;
    ESP_LOGI(TAG, "%d spools, %d filaments, %d vendors, %d locations, %d materials, active %d, web %s",
             n[0], n[1], n[2], nloc, nmat, s_active, web);

done:
    http_conn_close(&conn);
    lock();
    if (ok) {
        for (int k = 0; k < SM_KINDS; k++) {
            heap_caps_free(s_items[k]);
            s_items[k] = it[k];
            s_count[k] = n[k];
            heap_caps_free(s_xf[k]);
            s_xf[k] = xf[k];
            s_nxf[k] = nxf[k];
        }
        heap_caps_free(s_loc);
        s_loc = loc;
        s_nloc = nloc;
        heap_caps_free(s_mat);
        s_mat = mat;
        s_nmat = nmat;
        currency_symbol(cur, s_currency, sizeof(s_currency));
        strlcpy(s_web, web, sizeof(s_web));
        s_state = SM_OK;
    } else {
        for (int k = 0; k < SM_KINDS; k++) { heap_caps_free(it[k]); heap_caps_free(xf[k]); }
        heap_caps_free(loc);
        heap_caps_free(mat);
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
    s_printer = idx;
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
int spoolman_printer(void) { return s_printer; }

int spoolman_items(int kind, spool_item_t *out, int max)
{
    if (kind < 0 || kind >= SM_KINDS) return 0;
    lock();
    int n = s_count[kind] < max ? s_count[kind] : max;
    if (n > 0) memcpy(out, s_items[kind], n * sizeof(spool_item_t));
    unlock();
    return n;
}

bool spoolman_item(int kind, int id, spool_item_t *out)
{
    if (kind < 0 || kind >= SM_KINDS) return false;
    bool ok = false;
    lock();
    for (int i = 0; i < s_count[kind] && !ok; i++)
        if (s_items[kind][i].id == id) { *out = s_items[kind][i]; ok = true; }
    unlock();
    return ok;
}

int spoolman_xfields(int kind, sm_xfield_t *out, int max)
{
    if (kind < 0 || kind >= SM_KINDS) return 0;
    lock();
    int n = s_nxf[kind] < max ? s_nxf[kind] : max;
    if (n > 0) memcpy(out, s_xf[kind], n * sizeof(*out));
    unlock();
    return n;
}

int spoolman_locations(char (*out)[64], int max)
{
    lock();
    int n = s_nloc < max ? s_nloc : max;
    if (n > 0) memcpy(out, s_loc, n * 64);
    unlock();
    return n;
}

int spoolman_materials(sm_material_t *out, int max)
{
    lock();
    int n = s_nmat < max ? s_nmat : max;
    if (n > 0) memcpy(out, s_mat, n * sizeof(*out));
    unlock();
    return n;
}

void spoolman_currency(char *out, int cap) { strlcpy(out, s_currency[0] ? s_currency : "€", cap); }
void spoolman_web_url(char *out, int cap) { lock(); strlcpy(out, s_web, cap); unlock(); }

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

/* ── jobs (writes, single reads) ────────────────────────────────────────── */
static volatile sm_state_t s_job_state;
static char s_job_err[96];
static volatile int s_job_id = -1;
static char *s_job_resp;        /* PSRAM, under lock */
static volatile bool s_job_busy;

typedef struct { int idx, repeat; char method[8]; char path[96]; char *body; } job_t;

static void job_task(void *arg)
{
    job_t *j = arg;
    printer_cfg_t pc;
    esp_http_client_handle_t conn = NULL;
    char *resp_txt = NULL;
    int id = -1;
    bool ok = false;
    char err[96] = "";
    if (!printers_get_cfg(j->idx, &pc)) strcpy(err, "No such printer");
    else {
        for (int r = 0; r < j->repeat; r++) {
            cJSON *root;
            const cJSON *res = proxy(&conn, &pc, j->method, j->path, NULL, j->body, 1 << 20, &root, err, sizeof(err));
            if (!res && root) {
                /* DELETE answers with an empty body: no error means it worked */
                const cJSON *rr = cJSON_GetObjectItem(root, "result");
                if (rr && cJSON_IsNull(cJSON_GetObjectItem(rr, "error"))) res = rr;
            }
            if (!res) {
                if (!err[0]) strcpy(err, "Spoolman request failed");
                cJSON_Delete(root);
                ok = false;
                break;
            }
            id = (int)num(res, "id", -1);
            if (r == j->repeat - 1) {
                char *t = cJSON_PrintUnformatted(res);
                if (t) {
                    resp_txt = heap_caps_malloc(strlen(t) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (resp_txt) strcpy(resp_txt, t);
                    cJSON_free(t);
                }
            }
            cJSON_Delete(root);
            ok = true;
        }
    }
    http_conn_close(&conn);
    ESP_LOGI(TAG, "%s %s x%d -> %s id %d %s", j->method, j->path, j->repeat, ok ? "ok" : "failed", id, err);
    lock();
    heap_caps_free(s_job_resp);
    s_job_resp = resp_txt;
    s_job_id = id;
    strlcpy(s_job_err, err, sizeof(s_job_err));
    s_job_state = ok ? SM_OK : SM_ERROR;
    s_job_busy = false;
    unlock();
    heap_caps_free(j->body);
    heap_caps_free(j);
    vTaskDeleteWithCaps(NULL);
}

bool spoolman_job(const char *method, const char *path, const char *body, int repeat)
{
    if (s_job_busy || s_printer < 0) return false;
    job_t *j = ps_calloc(1, sizeof(*j));
    if (!j) return false;
    j->idx = s_printer;
    j->repeat = repeat < 1 ? 1 : repeat;
    strlcpy(j->method, method, sizeof(j->method));
    strlcpy(j->path, path, sizeof(j->path));
    if (body) {
        j->body = heap_caps_malloc(strlen(body) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!j->body) { heap_caps_free(j); return false; }
        strcpy(j->body, body);
    }
    s_job_busy = true;
    s_job_state = SM_LOADING;
    s_job_err[0] = 0;
    s_job_id = -1;
    if (xTaskCreateWithCaps(job_task, "spool_job", 8192, j, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(j->body);
        heap_caps_free(j);
        s_job_busy = false;
        s_job_state = SM_ERROR;
        strcpy(s_job_err, "Out of memory");
        return false;
    }
    return true;
}

sm_state_t spoolman_job_state(char *msg, int cap, int *id)
{
    if (msg) strlcpy(msg, s_job_err, cap);
    if (id) *id = s_job_id;
    return s_job_state;
}

char *spoolman_job_response(void)
{
    lock();
    char *r = NULL;
    if (s_job_resp && (r = malloc(strlen(s_job_resp) + 1))) strcpy(r, s_job_resp);
    unlock();
    return r;
}

void spoolman_job_clear(void)
{
    if (s_job_busy) return;
    lock();
    heap_caps_free(s_job_resp);
    s_job_resp = NULL;
    s_job_state = SM_IDLE;
    unlock();
}

static bool s_hold;
void spoolman_hold(void) { s_hold = true; }
bool spoolman_held(void) { bool h = s_hold; s_hold = false; return h; }

static int s_focus_kind = -1, s_focus_id = -1;
void spoolman_set_focus(int kind, int id) { s_focus_kind = kind; s_focus_id = id; }
bool spoolman_take_focus(int *kind, int *id)
{
    if (s_focus_kind < 0) return false;
    *kind = s_focus_kind;
    *id = s_focus_id;
    s_focus_kind = -1;
    return true;
}

/* ── external filament database ─────────────────────────────────────────── */
/* The whole list (~3.5 MB of JSON) stays in PSRAM while the form is open. An
 * index keeps each entry's offset and a lower-case "manufacturer name material
 * diameter" string, so a search never parses JSON except for the hits. */
typedef struct { uint32_t off, len, key; } ext_ix_t;
static char *s_ext;             /* raw JSON */
static ext_ix_t *s_ext_ix;
static char *s_ext_keys;        /* lower-case search strings, NUL-separated */
static int s_next;
static volatile sm_state_t s_ext_state;
static volatile int s_ext_kb;
static char s_ext_err[96];

/* value of "key" inside one JSON object's text (strings and numbers), lower-cased, appended to out */
static void ext_field(const char *o, size_t len, const char *key, char *out, size_t cap)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *end = o + len;
    const char *p = o;
    size_t pl = strlen(pat);
    for (;;) {
        p = memchr(p, '"', end - p);
        if (!p || (size_t)(end - p) < pl) return;
        if (!memcmp(p, pat, pl)) break;
        p++;
    }
    p += pl;
    while (p < end && (*p == ' ' || *p == ':')) p++;
    size_t n = strlen(out);
    if (n && n + 1 < cap) out[n++] = ' ';
    if (p < end && *p == '"') {
        for (p++; p < end && *p != '"' && n + 1 < cap; p++) {
            if (*p == '\\' && p + 1 < end) p++;
            out[n++] = tolower((unsigned char)*p);
        }
    } else {
        for (; p < end && (isdigit((unsigned char)*p) || *p == '.') && n + 1 < cap; p++) out[n++] = *p;
    }
    out[n] = 0;
}

static void ext_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    printer_cfg_t pc;
    esp_http_client_handle_t conn = NULL;
    http_buf_t b = { .max = 12 << 20 };
    bool ok = false;
    if (!printers_get_cfg(idx, &pc)) { strcpy(s_ext_err, "No such printer"); goto done; }
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/server/spoolman/proxy", pc.host, pc.port);
    static const char body[] = "{\"request_method\":\"GET\",\"path\":\"/v1/external/filament\",\"use_v2_response\":true}";
    int st = http_request(&conn, HTTP_METHOD_POST, url, pc.api_key, "application/json", body, strlen(body), &b, 60000);
    http_conn_close(&conn);
    if (st != 200 || !b.data) {
        snprintf(s_ext_err, sizeof(s_ext_err), st < 0 ? "No response from Moonraker" : "Download failed (HTTP %d)", st);
        goto done;
    }
    s_ext_kb = b.len / 1024;
    /* find the array after "response" */
    char *p = strstr((char *)b.data, "\"response\"");
    p = p ? strchr(p, '[') : NULL;
    if (!p) { strcpy(s_ext_err, "Spoolman has no filament database"); goto done; }
    char *end = (char *)b.data + b.len;
    int cap = 12000, n = 0;
    ext_ix_t *ix = ps_calloc(cap, sizeof(*ix));
    size_t kcap = 1 << 20, klen = 0;
    char *keys = heap_caps_malloc(kcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ix || !keys) { heap_caps_free(ix); heap_caps_free(keys); strcpy(s_ext_err, "Out of memory"); goto done; }
    int depth = 0;
    bool in_str = false;
    char *start = NULL;
    for (p++; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\') p++;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') { if (depth++ == 0) start = p; }
        else if (c == '}') {
            if (--depth == 0 && start && n < cap) {
                size_t len = p + 1 - start;
                char k[160] = "";
                ext_field(start, len, "manufacturer", k, sizeof(k));
                ext_field(start, len, "name", k, sizeof(k));
                ext_field(start, len, "material", k, sizeof(k));
                ext_field(start, len, "diameter", k, sizeof(k));
                size_t kl = strlen(k) + 1;
                if (klen + kl > kcap) break;
                memcpy(keys + klen, k, kl);
                ix[n].off = start - (char *)b.data;
                ix[n].len = len;
                ix[n].key = klen;
                klen += kl;
                n++;
            }
        } else if (c == ']' && depth == 0) break;
    }
    lock();
    heap_caps_free(s_ext);
    heap_caps_free(s_ext_ix);
    heap_caps_free(s_ext_keys);
    s_ext = (char *)b.data;
    s_ext_ix = ix;
    s_ext_keys = keys;
    s_next = n;
    unlock();
    b.data = NULL;   /* kept */
    ok = true;
    ESP_LOGI(TAG, "external database: %d filaments, %d KB", n, s_ext_kb);
done:
    http_buf_free(&b);
    s_ext_state = ok ? SM_OK : SM_ERROR;
    vTaskDeleteWithCaps(NULL);
}

void spoolman_ext_load(void)
{
    if (s_ext_state == SM_LOADING || s_ext_state == SM_OK || s_printer < 0) return;
    s_ext_state = SM_LOADING;
    s_ext_kb = 0;
    s_ext_err[0] = 0;
    if (xTaskCreateWithCaps(ext_task, "spool_ext", 8192, (void *)(intptr_t)s_printer, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_ext_state = SM_ERROR;
        strcpy(s_ext_err, "Out of memory");
    }
}

sm_state_t spoolman_ext_state(int *kbytes)
{
    if (kbytes) *kbytes = s_ext_kb;
    return s_ext_state;
}

static void ext_fill(const cJSON *o, sm_ext_t *e)
{
    memset(e, 0, sizeof(*e));
    str(o, "id", e->ext_id, sizeof(e->ext_id));
    str(o, "manufacturer", e->manufacturer, sizeof(e->manufacturer));
    str(o, "name", e->name, sizeof(e->name));
    str(o, "material", e->material, sizeof(e->material));
    e->density = num(o, "density", -1);
    e->diameter = num(o, "diameter", -1);
    e->weight = num(o, "weight", -1);
    e->spool_weight = num(o, "spool_weight", -1);
    str(o, "color_hex", e->color_hex, sizeof(e->color_hex));
    const cJSON *h;
    cJSON_ArrayForEach(h, cJSON_GetObjectItem(o, "color_hexes")) {
        if (!cJSON_IsString(h)) continue;
        if (e->color_hexes[0]) strlcat(e->color_hexes, ",", sizeof(e->color_hexes));
        strlcat(e->color_hexes, h->valuestring, sizeof(e->color_hexes));
    }
    str(o, "multi_color_direction", e->direction, sizeof(e->direction));
    e->ext_temp = (int)num(o, "extruder_temp", 0);
    e->bed_temp = (int)num(o, "bed_temp", 0);
}

int spoolman_ext_search(const char *q, sm_ext_t *out, int max, int *total)
{
    char words[8][32];
    int nw = 0;
    char buf[96];
    strlcpy(buf, q ? q : "", sizeof(buf));
    for (char *t = strtok(buf, " "); t && nw < 8; t = strtok(NULL, " ")) {
        int i = 0;
        for (; t[i] && i < 31; i++) words[nw][i] = tolower((unsigned char)t[i]);
        words[nw++][i] = 0;
    }
    int found = 0, got = 0;
    lock();
    for (int i = 0; i < s_next; i++) {
        const char *k = s_ext_keys + s_ext_ix[i].key;
        bool m = true;
        for (int w = 0; w < nw && m; w++) m = strstr(k, words[w]) != NULL;
        if (!m) continue;
        found++;
        if (got < max) {
            cJSON *o = cJSON_ParseWithLength(s_ext + s_ext_ix[i].off, s_ext_ix[i].len);
            if (o) { ext_fill(o, &out[got++]); cJSON_Delete(o); }
        }
    }
    unlock();
    if (total) *total = found;
    return got;
}

void spoolman_ext_free(void)
{
    if (s_ext_state == SM_LOADING) return;   /* the task frees nothing; it will be dropped next time */
    lock();
    heap_caps_free(s_ext);
    heap_caps_free(s_ext_ix);
    heap_caps_free(s_ext_keys);
    s_ext = NULL;
    s_ext_ix = NULL;
    s_ext_keys = NULL;
    s_next = 0;
    s_ext_state = SM_IDLE;
    unlock();
}

void spoolman_free(void)
{
    lock();
    for (int k = 0; k < SM_KINDS; k++) {
        heap_caps_free(s_items[k]);
        s_items[k] = NULL;
        s_count[k] = 0;
        heap_caps_free(s_xf[k]);
        s_xf[k] = NULL;
        s_nxf[k] = 0;
    }
    heap_caps_free(s_loc);
    s_loc = NULL;
    s_nloc = 0;
    heap_caps_free(s_mat);
    s_mat = NULL;
    s_nmat = 0;
    s_state = SM_IDLE;
    unlock();
    spoolman_ext_free();
    spoolman_job_clear();
}
