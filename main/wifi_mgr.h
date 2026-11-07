#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

typedef enum {
    WIFI_ST_OFF = 0,       /* radio not started (co-processor not up yet) */
    WIFI_ST_IDLE,          /* started, no credentials */
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,     /* has IP */
    WIFI_ST_FAILED,        /* gave up (bad password / not found) */
} wifi_state_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
    uint8_t channel;
} wifi_ap_t;

#define WIFI_MAX_APS 32

void         wifi_mgr_start(void);                /* non-blocking; brings radio up in background */
wifi_state_t wifi_mgr_state(void);
const char  *wifi_mgr_state_str(void);
void         wifi_mgr_connect(const char *ssid, const char *pass, bool save);
void         wifi_mgr_forget(void);
bool         wifi_mgr_ip(esp_netif_ip_info_t *out);
int8_t       wifi_mgr_rssi(void);
const char  *wifi_mgr_ssid(void);
bool         wifi_mgr_time_valid(void);
void         wifi_mgr_mac_str(char *out, size_t cap);
void         wifi_mgr_request_tz(void);              /* (re)detect time zone in background */

/* Scanning: start, then poll wifi_mgr_scan_results() until it returns >= 0. */
void wifi_mgr_scan_start(void);
bool wifi_mgr_scan_busy(void);
int  wifi_mgr_scan_results(wifi_ap_t *out, int max);  /* -1 while busy */
