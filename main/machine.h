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

typedef struct {
    char   file[80];
    char   status[16];     /* completed, cancelled, error, klippy_shutdown, in_progress ... */
    double start;          /* unix s */
    float  print_s, total_s, filament_mm;
} hist_job_t;

typedef struct {
    int    jobs;
    float  total_s, print_s, filament_mm;
} hist_totals_t;

#define MESH_MAX 16
typedef struct {
    int   rows, cols;                 /* probed matrix size (0 = no mesh loaded) */
    float z[MESH_MAX][MESH_MAX];      /* [row = y][col = x] */
    float min_x, min_y, max_x, max_y;
    char  profile[24];                /* active profile name */
    char  profiles[8][24];            /* saved profiles */
    int   nprofiles;
} mesh_t;

#define HIST_MAX 50

/* System tab: one fetch of /machine/system_info, then every 5 s. */
void machine_start_services(int printer_idx);
/* Console tab: poll /server/gcode_store every second. */
void machine_start_console(int printer_idx);
/* History tab: /server/history/list + totals, every 15 s. */
void machine_start_history(int printer_idx);
/* Mesh tab: bed_mesh object, every 10 s. */
void machine_start_mesh(int printer_idx);
void machine_stop(void);
void machine_refresh_now(void);

uint32_t machine_version(void);                  /* bumps on new data */
int  machine_services(svc_t *out, int max);      /* -1 while loading */
int  machine_console(con_line_t *out, int max);  /* oldest first */
bool machine_error(char *out, int cap);          /* last fetch failed */
int  machine_history(hist_job_t *out, int max, hist_totals_t *tot);   /* -1 while loading, newest first */
bool machine_mesh(mesh_t *out);                  /* false while loading */
