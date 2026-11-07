#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const char *label;
    const char *iana;    /* representative IANA zone */
    const char *posix;   /* POSIX TZ string for newlib */
} tz_entry_t;

extern const tz_entry_t TZ_TABLE[];
extern const int TZ_COUNT;

/* Find a POSIX TZ for an IANA name (+ current UTC offset in seconds as fallback). */
void tz_from_iana(const char *iana, int offset_s, char *posix, size_t cap);
/* Index in TZ_TABLE matching a POSIX string, or -1. */
int tz_index(const char *posix);
/* Look up the zone from the public IP (ip-api.com) and apply it. Blocking (~1 s). */
bool tz_autodetect(void);
