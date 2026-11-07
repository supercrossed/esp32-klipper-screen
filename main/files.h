#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FILES_MAX 200

typedef struct {
    char   path[128];     /* relative to gcodes/, e.g. "parts/bracket.gcode" */
    double modified;      /* unix seconds */
    uint32_t size;
} gfile_t;

typedef struct {
    char   path[128];
    float  est_time;      /* s, 0 unknown */
    float  filament_mm;   /* 0 unknown */
    float  filament_g;    /* 0 unknown */
    float  layer_h;       /* 0 unknown */
    char   slicer[32];
    uint8_t *thumb;       /* PNG (PSRAM), NULL if none - owned by files.c */
    size_t thumb_len;
    bool   ok;
} gmeta_t;

typedef enum { FL_IDLE = 0, FL_LOADING, FL_OK, FL_ERROR } fl_state_t;

void       files_fetch(int printer_idx);           /* list, newest first (async) */
void       files_fetch_meta(int printer_idx, const char *path);   /* details + thumbnail (async) */
fl_state_t files_state(char *err, int cap);
uint32_t   files_version(void);
int        files_list(gfile_t *out, int max);
/* Copy metadata for `path` if loaded; thumbnail is copied into a new PSRAM buffer the caller frees. */
bool       files_meta(const char *path, gmeta_t *out);
void       files_free(void);
