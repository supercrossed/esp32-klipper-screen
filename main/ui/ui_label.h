#pragma once
/* Spoolman labels on the BLE label printer (ui_label.c). */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char qr[160];        /* QR code content ("" = none) */
    char name[64];       /* big text, up to two lines */
    char sub[80];        /* material, maker */
    char note[24];       /* small, after sub: the colour hex */
    int  ext_temp, bed_temp;   /* 0 = leave out */
    char foot[64];       /* "Spool #12" */
} ui_label_t;

/* The finished TSPL job for one 50 x 30 mm label (PSRAM, caller frees). UI thread. */
uint8_t *ui_label_tspl(const ui_label_t *l, size_t *len);

/* Print it, with toasts for progress and the result. Without a saved printer,
 * the chooser opens first. UI thread. */
void ui_label_print(const ui_label_t *l);

/* Search for printers and let the user pick one (saved). done() runs after a pick. */
void ui_label_choose(void (*done)(void *user), void *user);

/* A test label (Settings, console `label test`). */
void ui_label_test(void);
