#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SPOOL_MAX 160

typedef struct {
    int      id;
    char     name[40];       /* filament name */
    char     vendor[28];
    char     material[16];
    uint32_t color;          /* 0xRRGGBB */
    bool     has_color;
    float    remaining_g;    /* -1 unknown */
    float    total_g;        /* net filament weight, -1 unknown */
    char     location[28];
    char     lot[24];
    int      ext_temp, bed_temp;   /* filament settings, 0 unknown */
    bool     archived;
} spool_item_t;

typedef enum { SM_IDLE = 0, SM_LOADING, SM_OK, SM_ERROR } sm_state_t;

/* Fetch spools + filaments via the printer's Moonraker proxy (async). */
void        spoolman_fetch(int printer_idx);
sm_state_t  spoolman_state(char *err, int cap);
uint32_t    spoolman_version(void);          /* bumps when new data lands */
int         spoolman_active_spool(void);     /* printer's active spool id, -1 none */

/* Copy out (UI thread). kind 0 = spools, 1 = filaments. Returns count. */
int  spoolman_items(int kind, spool_item_t *out, int max);

/* Set / clear (id < 0) the printer's active spool (async, result as toast via printers_last_result). */
void spoolman_set_active(int printer_idx, int spool_id);
void spoolman_free(void);                    /* release cached data when the page closes */
