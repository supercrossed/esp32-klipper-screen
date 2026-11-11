#pragma once
/*
 * Bluetooth LE label printer (TSPL over a BLE serial characteristic, e.g.
 * Clabel CT221D). The radio is the C6's, reached over esp_hosted (VHCI);
 * NimBLE is started on first use only. A print connects, sends, and drops the
 * connection after LABEL_IDLE_S without another print.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LABEL_IDLE_S 30
#define LABEL_SCAN_MAX 16

typedef struct {
    char    name[32];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t  rssi;
} label_dev_t;

typedef enum {
    LABEL_IDLE = 0,     /* nothing running (also after a finished print) */
    LABEL_STARTING,     /* Bluetooth starting */
    LABEL_SCANNING,
    LABEL_CONNECTING,
    LABEL_SENDING,
    LABEL_DONE,         /* last print sent */
    LABEL_ERROR,        /* last action failed: see the message */
} label_state_t;

/* Saved printer and print settings (NVS) */
typedef struct {
    bool    have;           /* a printer is chosen */
    char    name[32];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t  dx_mm, dy_mm;   /* nudge: -3..+3 mm */
    uint8_t density;        /* TSPL DENSITY 1..15 */
} label_cfg_t;

void label_cfg_get(label_cfg_t *out);
void label_cfg_set(const label_cfg_t *in);          /* saves; a new address drops any open connection */
void label_forget(void);
/* Spool QR codes: false (default) = "web+spoolman:s-<id>" (scanners), true = web link (phones). */
bool label_qr_link(void);
void label_set_qr_link(bool on);

/* Scan for named BLE devices (async, `secs` long). Printers whose names look
 * like label printers are listed first. */
bool label_scan(int secs);
int  label_scan_results(label_dev_t *out, int max);   /* label printers first */
bool label_looks_like_printer(const char *name);

/* Send one TSPL job (copied) to the saved printer: connect if needed, send,
 * one retry on failure. Returns false if nothing is saved or a job is queued. */
bool label_print(const uint8_t *tspl, size_t len);

label_state_t label_state(char *msg, int cap);
uint32_t      label_seq(void);                      /* bumps when a print finishes or fails */
bool          label_connected(void);

/* Testing: dry run (jobs are kept, not sent), and the last job's bytes. */
void   label_set_dry(bool on);
size_t label_last(uint8_t *out, size_t cap);
