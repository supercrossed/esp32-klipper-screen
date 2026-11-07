#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Data for the printer page's System and Console tabs. Both are fetched only
 * while their tab is open (machine_stop() ends the background task and frees
 * everything).
 */

#define SVC_MAX      16
#define CONSOLE_MAX  150

typedef struct {
    char name[32];
    char active[16];   /* "active", "inactive", "failed", ... */
    char sub[16];      /* "running", "dead", ... */
} svc_t;

typedef struct {
    double time;        /* unix seconds from Moonraker */
    char   type;        /* 'c' command, 'r' response */
    char   msg[160];
} con_line_t;

/* System tab: one fetch of /machine/system_info, then every 5 s. */
void machine_start_services(int printer_idx);
/* Console tab: poll /server/gcode_store every second. */
void machine_start_console(int printer_idx);
void machine_stop(void);
void machine_refresh_now(void);

uint32_t machine_version(void);                  /* bumps on new data */
int  machine_services(svc_t *out, int max);      /* -1 while loading */
int  machine_console(con_line_t *out, int max);  /* oldest first */
bool machine_error(char *out, int cap);          /* last fetch failed */
