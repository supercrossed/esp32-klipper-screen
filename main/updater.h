#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    UPD_IDLE = 0,       /* never checked */
    UPD_CHECKING,
    UPD_UP_TO_DATE,
    UPD_AVAILABLE,
    UPD_INSTALLING,
    UPD_DONE,           /* installed, restarting */
    UPD_ERROR,
} upd_state_t;

typedef struct {
    upd_state_t state;
    char current[24];      /* running firmware, e.g. "0.2.0" */
    char latest[24];       /* newest release, e.g. "0.3.0" */
    char notes[2048];      /* release notes (plain text) */
    uint32_t size;         /* app image size in bytes */
    int progress;          /* 0..100 while installing */
    char error[96];
    time_t checked_at;     /* 0 = never */
} upd_info_t;

void updater_init(void);           /* auto-check ~1 min after boot, then every 6 h */
void updater_check_now(void);
void updater_install(void);        /* only when state == UPD_AVAILABLE */
void updater_get(upd_info_t *out); /* copy current state (UI thread) */
upd_state_t updater_state(void);   /* cheap */
