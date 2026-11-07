#include "http_util.h"
#include <string.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "http";

void http_buf_free(http_buf_t *b)
{
    if (b->data) heap_caps_free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool buf_reserve(http_buf_t *b, size_t need)
{
    size_t max = b->max ? b->max : (1 << 20);
    if (need + 1 > max) return false;
    if (need + 1 <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 8192;
    while (ncap < need + 1) ncap *= 2;
    if (ncap > max) ncap = max;
    uint8_t *n = heap_caps_realloc(b->data, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!n) n = realloc(b->data, ncap);
    if (!n) return false;
    b->data = n;
    b->cap = ncap;
    return true;
}

void http_conn_close(esp_http_client_handle_t *conn)
{
    if (*conn) {
        esp_http_client_cleanup(*conn);
        *conn = NULL;
    }
}

static int request_once(esp_http_client_handle_t *conn, esp_http_client_method_t method,
                        const char *url, const char *api_key,
                        const char *content_type, const char *body, size_t body_len,
                        http_buf_t *out, int timeout_ms)
{
    if (!*conn) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = timeout_ms,
            .keep_alive_enable = true,
            .buffer_size = 4096,
            .buffer_size_tx = 2048,
            .user_agent = "KlipperRemote-ESP32P4",
        };
        *conn = esp_http_client_init(&cfg);
        if (!*conn) return -1;
    } else {
        if (esp_http_client_set_url(*conn, url) != ESP_OK) return -1;
        esp_http_client_set_timeout_ms(*conn, timeout_ms);
    }
    esp_http_client_handle_t h = *conn;
    esp_http_client_set_method(h, method);
    if (api_key && api_key[0]) esp_http_client_set_header(h, "X-Api-Key", api_key);
    else esp_http_client_delete_header(h, "X-Api-Key");
    if (content_type) esp_http_client_set_header(h, "Content-Type", content_type);
    else esp_http_client_delete_header(h, "Content-Type");

    for (int redirects = 0; redirects < 4; redirects++) {
        if (esp_http_client_open(h, body ? (int)body_len : 0) != ESP_OK) return -1;
        if (body && body_len) {
            if (esp_http_client_write(h, body, body_len) != (int)body_len) return -1;
        }
        int64_t clen = esp_http_client_fetch_headers(h);
        if (clen < 0) return -1;
        int status = esp_http_client_get_status_code(h);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_flush_response(h, NULL);
            if (esp_http_client_set_redirection(h) != ESP_OK) return status;
            continue;
        }
        if (out) {
            out->len = 0;
            if (clen > 0 && !buf_reserve(out, clen)) {
                ESP_LOGW(TAG, "response too large (%lld)", clen);
                esp_http_client_flush_response(h, NULL);
                return -1;
            }
            for (;;) {
                if (out->cap - out->len < 2048 && !buf_reserve(out, out->len + 16384)) {
                    esp_http_client_flush_response(h, NULL);
                    return -1;
                }
                int r = esp_http_client_read(h, (char *)out->data + out->len, out->cap - out->len - 1);
                if (r < 0) return -1;
                if (r == 0) {
                    if (esp_http_client_is_complete_data_received(h)) break;
                    if (clen > 0 && out->len >= (size_t)clen) break;
                    /* chunked/close-delimited with no more data */
                    break;
                }
                out->len += r;
            }
            if (out->data) out->data[out->len] = 0;
        } else {
            esp_http_client_flush_response(h, NULL);
        }
        return status;
    }
    return -1;
}

int http_request(esp_http_client_handle_t *conn, esp_http_client_method_t method,
                 const char *url, const char *api_key,
                 const char *content_type, const char *body, size_t body_len,
                 http_buf_t *out, int timeout_ms)
{
    bool reused = *conn != NULL;
    int st = request_once(conn, method, url, api_key, content_type, body, body_len, out, timeout_ms);
    if (st < 0) {
        http_conn_close(conn);
        /* A reused keep-alive socket may have been closed by the server: retry once fresh. */
        if (reused) st = request_once(conn, method, url, api_key, content_type, body, body_len, out, timeout_ms);
        if (st < 0) http_conn_close(conn);
    }
    return st;
}

void url_encode(const char *in, char *out, size_t cap, int keep_slash)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}
