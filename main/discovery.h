#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DISC_MAX 24

typedef struct {
    char     name[48];      /* hostname from /printer/info or mDNS */
    char     host[48];      /* IP */
    uint16_t port;
    char     klippy[16];    /* klippy_state */
    char     via[8];        /* "mdns" / "scan" */
} disc_result_t;

void discovery_start(void);          /* no-op if already running */
void discovery_cancel(void);
bool discovery_busy(void);
int  discovery_progress(void);       /* 0..100 */
int  discovery_results(disc_result_t *out, int max);
