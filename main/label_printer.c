/*
 * BLE label printer: NimBLE central on the P4, the C6's controller over
 * esp_hosted VHCI. One worker task (PSRAM stack) does everything in order:
 * start Bluetooth, scan, connect, find the printer's write characteristic,
 * send TSPL, and disconnect after LABEL_IDLE_S idle.
 *
 * The printer side is the usual "transparent UART" service (Microchip
 * 49535343-FE7D-...): TSPL text and bitmaps written to one characteristic.
 */
#include "label_printer.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "label";

/* ── settings ────────────────────────────────────────────────────────── */
static label_cfg_t s_cfg;
static bool s_cfg_loaded;
static SemaphoreHandle_t s_mtx;

static void lock(void) { if (!s_mtx) s_mtx = xSemaphoreCreateMutex(); xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static void cfg_load(void)
{
    if (s_cfg_loaded) return;
    s_cfg_loaded = true;
    s_cfg.density = 8;
    nvs_handle_t h;
    if (nvs_open("label", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_cfg);
    label_cfg_t c;
    if (nvs_get_blob(h, "cfg", &c, &n) == ESP_OK && n == sizeof(c)) s_cfg = c;
    nvs_close(h);
    if (s_cfg.density < 1 || s_cfg.density > 15) s_cfg.density = 8;
}

static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("label", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
}

void label_cfg_get(label_cfg_t *out) { lock(); cfg_load(); *out = s_cfg; unlock(); }

/* Spool QR codes: Spoolman's "web+spoolman:s-<id>" (default: what scanner setups
 * expect; some keep only the digits, so a link's IP address would end up in the
 * spool id) or a web link (phones open it). Own NVS key. */
bool label_qr_link(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open("label", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "qrlink", &v); nvs_close(h); }
    return v;
}

void label_set_qr_link(bool on)
{
    nvs_handle_t h;
    if (nvs_open("label", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "qrlink", on);
    nvs_commit(h);
    nvs_close(h);
}

/* ── BLE state ───────────────────────────────────────────────────────── */
#define EV_SYNC      BIT0
#define EV_SCAN_DONE BIT1
#define EV_CONN      BIT2
#define EV_CONN_FAIL BIT3
#define EV_DISC      BIT4   /* disconnected */
#define EV_MTU       BIT5
#define EV_SVC_DONE  BIT6
#define EV_CHR_DONE  BIT7
#define EV_WR_DONE   BIT8
#define EV_WR_FAIL   BIT9

static EventGroupHandle_t s_ev;
static bool s_ble_up;
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_conn_addr[6];
static uint16_t s_svc_start, s_svc_end, s_tx, s_tx_any;
static volatile uint16_t s_mtu = 23;

static label_dev_t s_found[LABEL_SCAN_MAX];
static int s_nfound;

static volatile label_state_t s_state;
static char s_msg[96];
static volatile uint32_t s_seq;

typedef enum { JOB_SCAN, JOB_PRINT, JOB_DROP } job_kind_t;
typedef struct { job_kind_t kind; int secs; uint8_t *data; size_t len; } job_t;
static QueueHandle_t s_q;
static volatile bool s_print_queued;
static bool s_dry;              /* testing: keep jobs, don't send */
static uint8_t *s_last;         /* the last job (PSRAM), for `label dump` */
static size_t s_last_len;

/* Microchip transparent UART service and its TX (write) characteristic */
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(0x55, 0xe4, 0x05, 0xd2, 0xaf, 0x9f, 0xa9, 0x8f,
                                                       0xe5, 0x4a, 0x7d, 0xfe, 0x43, 0x53, 0x53, 0x49);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(0xb3, 0x9b, 0x72, 0x34, 0xbe, 0xec, 0xd4, 0xa8,
                                                      0xf4, 0x43, 0x41, 0x88, 0x43, 0x53, 0x53, 0x49);

static void set_state(label_state_t st, const char *msg)
{
    lock();
    s_state = st;
    strlcpy(s_msg, msg ? msg : "", sizeof(s_msg));
    unlock();
}

label_state_t label_state(char *msg, int cap)
{
    lock();
    label_state_t st = s_state;
    if (msg) strlcpy(msg, s_msg, cap);
    unlock();
    return st;
}

uint32_t label_seq(void) { return s_seq; }
bool label_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    xEventGroupSetBits(s_ev, EV_SYNC);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset (%d)", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* NimBLE calls this on shutdown; esp_hosted has no counterpart. The SDIO link to
 * the C6 also carries Wi-Fi, so there is nothing to undo here. */
void ble_transport_ll_deinit(void) {}

/* Stop the BLE host and free its memory (the next print starts it again). */
static void ble_stop(void)
{
    if (!s_ble_up) return;
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
        s_ble_up = false;
        xEventGroupClearBits(s_ev, EV_SYNC);
        ESP_LOGI(TAG, "Bluetooth stopped");
    } else ESP_LOGW(TAG, "nimble_port_stop: %d", rc);
}

static bool ble_start(void)
{
    if (s_ble_up) return true;
    set_state(LABEL_STARTING, "Starting Bluetooth...");
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(247);
    nimble_port_freertos_init(host_task);
    if (!(xEventGroupWaitBits(s_ev, EV_SYNC, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000)) & EV_SYNC)) {
        /* no answer from the C6's Bluetooth (its firmware may not have it): undo, so a later try starts clean */
        ESP_LOGE(TAG, "BLE host did not sync with the C6");
        if (nimble_port_stop() == 0) nimble_port_deinit();
        return false;
    }
    s_ble_up = true;
    ESP_LOGI(TAG, "Bluetooth up (own address type %d)", s_own_addr_type);
    return true;
}

/* label printers first, then by signal */
bool label_looks_like_printer(const char *name)
{
    static const char *const PREFIX[] = { "CT2", "CT3", "CT4", "Clabel", "CLABEL", "G4", "S1-" };
    for (size_t i = 0; i < sizeof(PREFIX) / sizeof(PREFIX[0]); i++)
        if (!strncmp(name, PREFIX[i], strlen(PREFIX[i]))) return true;
    return false;
}

static int dev_rank(const label_dev_t *d) { return (label_looks_like_printer(d->name) ? 0 : 1000) - d->rssi; }

static void sort_devs(label_dev_t *a, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && dev_rank(&a[j]) < dev_rank(&a[j - 1]); j--) {
            label_dev_t t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
}

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) || !f.name || !f.name_len) break;
        lock();
        int i = 0;
        while (i < s_nfound && memcmp(s_found[i].addr, ev->disc.addr.val, 6)) i++;
        if (i == s_nfound && s_nfound < LABEL_SCAN_MAX) s_nfound++;
        if (i < s_nfound) {
            label_dev_t *d = &s_found[i];
            int n = f.name_len < sizeof(d->name) - 1 ? f.name_len : sizeof(d->name) - 1;
            memcpy(d->name, f.name, n);
            d->name[n] = 0;
            memcpy(d->addr, ev->disc.addr.val, 6);
            d->addr_type = ev->disc.addr.type;
            d->rssi = ev->disc.rssi;
        }
        unlock();
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xEventGroupSetBits(s_ev, EV_SCAN_DONE);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            xEventGroupSetBits(s_ev, EV_CONN);
        } else xEventGroupSetBits(s_ev, EV_CONN_FAIL);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(s_ev, EV_DISC | EV_CONN_FAIL | EV_WR_FAIL);
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        xEventGroupSetBits(s_ev, EV_MTU);
        break;
    default:
        break;
    }
    return 0;
}

static void do_scan(int secs)
{
    lock();
    s_nfound = 0;
    unlock();
    set_state(LABEL_SCANNING, "Looking for printers...");
    struct ble_gap_disc_params p = { .passive = 0, .filter_duplicates = 1 };
    xEventGroupClearBits(s_ev, EV_SCAN_DONE);
    int rc = ble_gap_disc(s_own_addr_type, secs * 1000, &p, gap_cb, NULL);
    if (rc) {
        set_state(LABEL_ERROR, "Bluetooth scan failed");
        ESP_LOGE(TAG, "ble_gap_disc: %d", rc);
        return;
    }
    xEventGroupWaitBits(s_ev, EV_SCAN_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(secs * 1000 + 3000));
    lock();
    int n = s_nfound;
    unlock();
    char m[48];
    snprintf(m, sizeof(m), "%d device%s found", n, n == 1 ? "" : "s");
    set_state(LABEL_IDLE, m);
    ESP_LOGI(TAG, "scan: %d named devices", n);
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) { s_svc_start = svc->start_handle; s_svc_end = svc->end_handle; }
    else xEventGroupSetBits(s_ev, EV_SVC_DONE);
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        if (!ble_uuid_cmp(&chr->uuid.u, &TX_UUID.u)) s_tx = chr->val_handle;
        /* other printers: the first writable characteristic outside the standard GATT/GAP ones */
        else if (!s_tx_any && (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) && chr->uuid.u.type != BLE_UUID_TYPE_16)
            s_tx_any = chr->val_handle;
        else if (!s_tx_any && (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) && chr->uuid.u.type == BLE_UUID_TYPE_16 &&
                 (ble_uuid_u16(&chr->uuid.u) & 0xFF00) != 0x2A00)
            s_tx_any = chr->val_handle;
    } else xEventGroupSetBits(s_ev, EV_CHR_DONE);
    return 0;
}

static int wr_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    xEventGroupSetBits(s_ev, err->status == 0 ? EV_WR_DONE : EV_WR_FAIL);
    return 0;
}

static void drop_conn(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    xEventGroupClearBits(s_ev, EV_DISC);
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    xEventGroupWaitBits(s_ev, EV_DISC, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

static bool do_connect(const label_cfg_t *c, char *err, size_t ecap)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && !memcmp(s_conn_addr, c->addr, 6) && s_tx) return true;
    drop_conn();
    char m[64];
    snprintf(m, sizeof(m), "Connecting to %s...", c->name);
    set_state(LABEL_CONNECTING, m);
    ble_addr_t a = { .type = c->addr_type };
    memcpy(a.val, c->addr, 6);
    ble_gap_disc_cancel();
    xEventGroupClearBits(s_ev, EV_CONN | EV_CONN_FAIL | EV_DISC | EV_MTU | EV_SVC_DONE | EV_CHR_DONE);
    /* 15..30 ms connection interval: a 12 KB label goes out in about a second */
    struct ble_gap_conn_params cp = { .scan_itvl = 0x30, .scan_window = 0x30, .itvl_min = 12, .itvl_max = 24,
                                      .latency = 0, .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0 };
    int rc = ble_gap_connect(s_own_addr_type, &a, 8000, &cp, gap_cb, NULL);
    if (rc) { snprintf(err, ecap, "Couldn't start the connection (%d)", rc); return false; }
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_CONN | EV_CONN_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(b & EV_CONN)) {
        ble_gap_conn_cancel();
        snprintf(err, ecap, "%s didn't answer - is it on and nearby?", c->name);
        return false;
    }
    memcpy(s_conn_addr, c->addr, 6);
    ble_gattc_exchange_mtu(s_conn, NULL, NULL);
    xEventGroupWaitBits(s_ev, EV_MTU, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000));
    s_svc_start = s_svc_end = s_tx = s_tx_any = 0;
    /* the transparent-UART service if it is there, else every characteristic */
    rc = ble_gattc_disc_svc_by_uuid(s_conn, &SVC_UUID.u, svc_cb, NULL);
    if (!rc) xEventGroupWaitBits(s_ev, EV_SVC_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    uint16_t lo = s_svc_start ? s_svc_start : 1, hi = s_svc_start ? s_svc_end : 0xFFFF;
    rc = ble_gattc_disc_all_chrs(s_conn, lo, hi, chr_cb, NULL);
    if (!rc) xEventGroupWaitBits(s_ev, EV_CHR_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!s_tx) s_tx = s_tx_any;
    if (!s_tx || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        snprintf(err, ecap, "%s has no printer data channel", c->name);
        drop_conn();
        return false;
    }
    ESP_LOGI(TAG, "connected to %s: MTU %u, TX handle %u", c->name, s_mtu, s_tx);
    return true;
}

static bool do_send(const uint8_t *d, size_t len, char *err, size_t ecap)
{
    set_state(LABEL_SENDING, "Sending the label...");
    int64_t t0 = esp_timer_get_time();
    size_t chunk = s_mtu > 3 ? s_mtu - 3 : 20;
    for (size_t off = 0; off < len;) {
        size_t n = len - off < chunk ? len - off : chunk;
        bool last = off + n >= len;
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) { snprintf(err, ecap, "The printer disconnected"); return false; }
        int rc;
        if (last) {   /* the last piece with a response: it's all there once it's answered */
            xEventGroupClearBits(s_ev, EV_WR_DONE | EV_WR_FAIL);
            rc = ble_gattc_write_flat(s_conn, s_tx, d + off, n, wr_cb, NULL);
        } else rc = ble_gattc_write_no_rsp_flat(s_conn, s_tx, d + off, n);
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }   /* buffers full: wait */
        if (rc) { snprintf(err, ecap, "Bluetooth write failed (%d)", rc); return false; }
        off += n;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_WR_DONE | EV_WR_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(b & EV_WR_DONE)) { snprintf(err, ecap, "The printer didn't confirm the label"); return false; }
    ESP_LOGI(TAG, "sent %u bytes in %lld ms", (unsigned)len, (esp_timer_get_time() - t0) / 1000);
    return true;
}

static void do_print(job_t *j)
{
    label_cfg_t c;
    label_cfg_get(&c);
    char err[96] = "";
    bool ok = false;
    if (!c.have) strcpy(err, "Choose a label printer first");
    else {
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (attempt) { ESP_LOGW(TAG, "retrying: %s", err); drop_conn(); vTaskDelay(pdMS_TO_TICKS(500)); }
            ok = do_connect(&c, err, sizeof(err)) && do_send(j->data, j->len, err, sizeof(err));
        }
    }
    if (ok) {
        char m[64];
        snprintf(m, sizeof(m), "Label sent to %s", c.name);
        set_state(LABEL_DONE, m);
    } else set_state(LABEL_ERROR, err);
    ESP_LOGI(TAG, "print %u bytes: %s", (unsigned)j->len, ok ? "ok" : err);
    s_seq++;
}

static void worker(void *arg)
{
    for (;;) {
        job_t j;
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(LABEL_IDLE_S * 1000)) != pdTRUE) {
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) { ESP_LOGI(TAG, "idle: disconnecting"); drop_conn(); }
            ble_stop();   /* nothing kept running between prints */
            continue;
        }
        if (!ble_start()) {
            set_state(LABEL_ERROR, "Bluetooth didn't start (does the Wi-Fi chip's firmware have it?)");
            if (j.kind == JOB_PRINT) s_seq++;
        } else if (j.kind == JOB_SCAN) do_scan(j.secs);
        else if (j.kind == JOB_PRINT) do_print(&j);
        else drop_conn();
        if (j.kind == JOB_PRINT) s_print_queued = false;
        heap_caps_free(j.data);
    }
}

static bool post(job_t *j)
{
    if (!s_ev) s_ev = xEventGroupCreate();
    if (!s_q) {
        s_q = xQueueCreate(4, sizeof(job_t));
        if (!s_q || xTaskCreateWithCaps(worker, "label", 6144, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "can't start the label task");
            return false;
        }
    }
    return xQueueSend(s_q, j, 0) == pdTRUE;
}

bool label_scan(int secs)
{
    job_t j = { .kind = JOB_SCAN, .secs = secs };
    set_state(LABEL_SCANNING, "Looking for printers...");
    return post(&j);
}

int label_scan_results(label_dev_t *out, int max)
{
    lock();
    int n = s_nfound < max ? s_nfound : max;
    memcpy(out, s_found, n * sizeof(*out));
    unlock();
    sort_devs(out, n);   /* label printers first, then the strongest signal */
    return n;
}

bool label_print(const uint8_t *tspl, size_t len)
{
    label_cfg_t c;
    label_cfg_get(&c);
    lock();
    heap_caps_free(s_last);
    s_last = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_last_len = s_last ? len : 0;
    if (s_last) memcpy(s_last, tspl, len);
    unlock();
    if (s_dry) {
        set_state(LABEL_DONE, "Dry run: label kept, not sent");
        s_seq++;
        return true;
    }
    if (!c.have || s_print_queued) return false;
    job_t j = { .kind = JOB_PRINT, .len = len };
    j.data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j.data) return false;
    memcpy(j.data, tspl, len);
    s_print_queued = true;
    set_state(LABEL_CONNECTING, "Preparing...");
    if (!post(&j)) { heap_caps_free(j.data); s_print_queued = false; return false; }
    return true;
}

void label_cfg_set(const label_cfg_t *in)
{
    lock();
    cfg_load();
    bool moved = memcmp(in->addr, s_cfg.addr, 6) || in->have != s_cfg.have;
    s_cfg = *in;
    cfg_save();
    unlock();
    if (moved && s_q) { job_t j = { .kind = JOB_DROP }; post(&j); }
}

void label_forget(void)
{
    label_cfg_t c;
    label_cfg_get(&c);
    c.have = false;
    c.name[0] = 0;
    memset(c.addr, 0, 6);
    label_cfg_set(&c);
}

void label_set_dry(bool on) { s_dry = on; }

size_t label_last(uint8_t *out, size_t cap)
{
    lock();
    size_t n = !out ? s_last_len : s_last_len < cap ? s_last_len : cap;   /* no buffer: the length */
    if (out && s_last) memcpy(out, s_last, n);
    unlock();
    return n;
}
