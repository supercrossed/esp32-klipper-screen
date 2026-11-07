#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_http_client.h"

/* Growable response buffer (allocated in PSRAM). Always NUL-terminated. */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    size_t   max;      /* hard limit; 0 = 1 MB */
} http_buf_t;

void http_buf_free(http_buf_t *b);

/*
 * Do one request on a reusable keep-alive handle (*conn is created on first
 * use and recreated after errors). Returns HTTP status, or -1 on transport
 * error. body/body_len may be NULL/0.
 */
int http_request(esp_http_client_handle_t *conn, esp_http_client_method_t method,
                 const char *url, const char *api_key,
                 const char *content_type, const char *body, size_t body_len,
                 http_buf_t *out, int timeout_ms);

void http_conn_close(esp_http_client_handle_t *conn);

/* Percent-encode for a URL query value or path segment ('/' kept if keep_slash). */
void url_encode(const char *in, char *out, size_t cap, int keep_slash);
