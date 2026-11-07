/*
 * Find Moonraker instances on the local network.
 *  1. mDNS: _moonraker._tcp (Moonraker's [zeroconf] component)
 *  2. Subnet sweep of the local /24: non-blocking TCP connects in batches
 *     to 7125-7127 (Moonraker) and 80 (Mainsail/Fluidd nginx proxy).
 *  3. Every open port is verified with GET /server/info.
 */
#include "discovery.h"
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* task stacks in PSRAM: keeps internal SRAM for Wi-Fi/SDIO DMA */
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "mdns.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "wifi_mgr.h"
#include "http_util.h"

static const char *TAG = "discover";

static SemaphoreHandle_t s_mtx;
static disc_result_t s_res[DISC_MAX];
static int s_nres;
static volatile bool s_busy, s_cancel;
static volatile int s_progress;

/* lwIP's ARP table holds only 10 entries (ARP_TABLE_SIZE). Probing many new IPs at
 * once evicts entries mid-handshake and silently drops SYNs, so probe a few hosts
 * at a time on all ports (one ARP entry per host). */
#define HOSTS_PER_BATCH 9
#define NPORTS 4
#define BATCH (HOSTS_PER_BATCH * NPORTS)
/* Printers on Wi-Fi in power-save can take >1 s to answer the first ARP request. */
#define CONNECT_TIMEOUT_MS 1500

typedef struct { uint32_t ip; uint16_t port; } cand_t;

static bool have_result(const char *host, uint16_t port)
{
    for (int i = 0; i < s_nres; i++)
        if (!strcmp(s_res[i].host, host) && (port == 0 || s_res[i].port == port)) return true;
    return false;
}

/* Verify an open port really is Moonraker and fetch a display name. */
static void verify(const char *host, uint16_t port, const char *via, const char *mdns_name)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool dup = have_result(host, port) || (port == 80 && have_result(host, 7125)) || s_nres >= DISC_MAX;
    xSemaphoreGive(s_mtx);
    if (dup) return;

    char url[96];
    esp_http_client_handle_t conn = NULL;
    http_buf_t buf = { .max = 16 * 1024 };
    snprintf(url, sizeof(url), "http://%s:%u/server/info", host, port);
    int st = http_request(&conn, HTTP_METHOD_GET, url, NULL, NULL, NULL, 0, &buf, 3000);
    if (st < 0) st = http_request(&conn, HTTP_METHOD_GET, url, NULL, NULL, NULL, 0, &buf, 3000);
    /* 401/403 = Moonraker with auth enabled; still worth listing */
    bool ok = false;
    disc_result_t r = {0};
    strlcpy(r.host, host, sizeof(r.host));
    r.port = port;
    strlcpy(r.via, via, sizeof(r.via));
    if (st == 200 && buf.data) {
        cJSON *j = cJSON_ParseWithLength((char *)buf.data, buf.len);
        const cJSON *res = cJSON_GetObjectItem(j, "result");
        const cJSON *ks = cJSON_GetObjectItem(res, "klippy_state");
        if (cJSON_IsString(ks)) {
            ok = true;
            strlcpy(r.klippy, ks->valuestring, sizeof(r.klippy));
        }
        cJSON_Delete(j);
    } else if (st == 401 || st == 403) {
        ok = true;
        strcpy(r.klippy, "auth required");
    }
    if (!ok) ESP_LOGD(TAG, "verify %s:%u failed: HTTP %d", host, port, st);
    if (ok) {
        snprintf(url, sizeof(url), "http://%s:%u/printer/info", host, port);
        if (http_request(&conn, HTTP_METHOD_GET, url, NULL, NULL, NULL, 0, &buf, 3000) == 200 && buf.data) {
            cJSON *j = cJSON_ParseWithLength((char *)buf.data, buf.len);
            const cJSON *hn = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "hostname");
            if (cJSON_IsString(hn)) strlcpy(r.name, hn->valuestring, sizeof(r.name));
            cJSON_Delete(j);
        }
        if (!r.name[0] && mdns_name) strlcpy(r.name, mdns_name, sizeof(r.name));
        if (!r.name[0]) strlcpy(r.name, host, sizeof(r.name));
        if (port != 7125 && port != 80) {
            char t[48];
            snprintf(t, sizeof(t), "%s:%u", r.name, port);
            strlcpy(r.name, t, sizeof(r.name));
        }
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (!have_result(host, port) && s_nres < DISC_MAX) s_res[s_nres++] = r;
        xSemaphoreGive(s_mtx);
        ESP_LOGI(TAG, "found %s at %s:%u (%s)", r.name, host, port, r.klippy);
    }
    http_conn_close(&conn);
    http_buf_free(&buf);
}

static void do_mdns(void)
{
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_moonraker", "_tcp", 2500, 16, &results) != ESP_OK || !results) return;
    for (mdns_result_t *r = results; r && !s_cancel; r = r->next) {
        char ip[20] = "";
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                break;
            }
        }
        if (ip[0]) verify(ip, r->port ? r->port : 7125, "mdns", r->instance_name ? r->instance_name : r->hostname);
    }
    mdns_query_results_free(results);
}

/* Probe one batch of candidates in parallel; report open ones. */
/* Abort (RST) instead of a graceful close so the PCB is freed immediately. */
static void close_now(int fd)
{
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
}

static int probe_batch(const cand_t *c, int n, cand_t *open_out)
{
    int fds[BATCH];
    int nopen = 0;
    fd_set wset;
    FD_ZERO(&wset);
    int maxfd = -1;
    for (int i = 0; i < n; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        for (int retry = 0; fds[i] < 0 && retry < 10; retry++) {   /* pollers may hold sockets briefly */
            vTaskDelay(pdMS_TO_TICKS(50));
            fds[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }
        if (fds[i] < 0) { ESP_LOGW(TAG, "out of sockets"); continue; }
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(c[i].port), .sin_addr.s_addr = c[i].ip };
        int r = connect(fds[i], (struct sockaddr *)&sa, sizeof(sa));
        if (r == 0) {
            open_out[nopen++] = c[i];
            close_now(fds[i]);
            fds[i] = -1;
        } else if (errno != EINPROGRESS) {
            close_now(fds[i]);
            fds[i] = -1;
        } else {
            FD_SET(fds[i], &wset);
            if (fds[i] > maxfd) maxfd = fds[i];
        }
    }
    int64_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONNECT_TIMEOUT_MS);
    while (maxfd >= 0) {
        int64_t left = deadline - xTaskGetTickCount();
        if (left <= 0) break;
        fd_set ws = wset;
        struct timeval tv = { .tv_sec = 0, .tv_usec = (int)pdTICKS_TO_MS(left) * 1000 };
        int r = select(maxfd + 1, NULL, &ws, NULL, &tv);
        if (r <= 0) break;
        for (int i = 0; i < n; i++) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &ws)) continue;
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) open_out[nopen++] = c[i];
            FD_CLR(fds[i], &wset);
            close_now(fds[i]);
            fds[i] = -1;
        }
        maxfd = -1;
        for (int i = 0; i < n; i++) if (fds[i] >= 0 && fds[i] > maxfd) maxfd = fds[i];
    }
    for (int i = 0; i < n; i++) if (fds[i] >= 0) close_now(fds[i]);
    return nopen;
}

static void disc_task(void *arg)
{
    s_progress = 0;
    do_mdns();
    s_progress = 5;

    esp_netif_ip_info_t ip;
    if (!s_cancel && wifi_mgr_ip(&ip)) {
        uint32_t me = ip.ip.addr;   /* network byte order */
        uint32_t base = ntohl(me) & 0xFFFFFF00u;
        static const uint16_t ports[NPORTS] = { 7125, 80, 7126, 7127 };   /* 7125 first: preferred over 80 */
        cand_t batch[BATCH], open[BATCH];
        for (int h0 = 1; h0 <= 254 && !s_cancel; h0 += HOSTS_PER_BATCH) {
            int nb = 0;
            for (int h = h0; h < h0 + HOSTS_PER_BATCH && h <= 254; h++) {
                uint32_t a = htonl(base | h);
                if (a == me) continue;
                for (int pi = 0; pi < NPORTS; pi++) {
                    batch[nb].ip = a;
                    batch[nb].port = ports[pi];
                    nb++;
                }
            }
            int64_t t0 = esp_timer_get_time();
            int no = probe_batch(batch, nb, open);
            for (int k = 0; k < no; k++)
                ESP_LOGD(TAG, "open " IPSTR ":%u (%lld ms)", IP2STR((esp_ip4_addr_t *)&open[k].ip), open[k].port,
                         (esp_timer_get_time() - t0) / 1000);
            /* verify 7125 hits before 80 so the dedupe keeps the direct Moonraker port */
            for (int pass = 0; pass < 2; pass++)
                for (int k = 0; k < no && !s_cancel; k++) {
                    if ((open[k].port == 80) != (pass == 1)) continue;
                    char hs[20];
                    snprintf(hs, sizeof(hs), IPSTR, IP2STR((esp_ip4_addr_t *)&open[k].ip));
                    verify(hs, open[k].port, "scan", NULL);
                }
            int pr = 5 + (h0 + HOSTS_PER_BATCH - 1) * 95 / 254;
            s_progress = pr > 99 ? 99 : pr;
        }
    }
    s_progress = 100;
    s_busy = false;
    ESP_LOGI(TAG, "discovery finished: %d found", s_nres);
    vTaskDeleteWithCaps(NULL);
}

void discovery_start(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (s_busy) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_nres = 0;
    xSemaphoreGive(s_mtx);
    s_busy = true;
    s_cancel = false;
    s_progress = 0;
    if (xTaskCreateWithCaps(disc_task, "discover", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) s_busy = false;
}

void discovery_cancel(void) { s_cancel = true; }
bool discovery_busy(void) { return s_busy; }
int discovery_progress(void) { return s_progress; }

int discovery_results(disc_result_t *out, int max)
{
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_nres < max ? s_nres : max;
    memcpy(out, s_res, n * sizeof(disc_result_t));
    xSemaphoreGive(s_mtx);
    return n;
}
