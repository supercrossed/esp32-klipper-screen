/*
 * Battery monitor for the board's BAT connector (1S Li-ion / LiPo).
 *
 * Hardware (ESP32-P4-WIFI6-Touch-LCD-5 schematic): the cell charges through an
 * ETA6098 switching charger, fixed 4.2 V, with no control line to the ESP32,
 * so the charge limit cannot be changed in software. The cell voltage reaches
 * GPIO20 (ADC1 channel 4) through a 200K / 100K divider (1/3).
 * The board has no battery protection chip, so the firmware can turn itself
 * off below a set voltage (see battery_boot_check()).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BAT_NONE = 0,       /* no battery detected (or "No battery" chosen in settings) */
    BAT_DISCHARGING,
    BAT_CHARGING,       /* voltage rising: USB connected */
    BAT_FULL,           /* USB connected, charge finished */
} bat_state_t;

typedef struct {
    bat_state_t state;
    int  mv;            /* filtered cell voltage */
    int  raw_mv;        /* last single reading */
    int  boot_mv, boot_drop;   /* diagnostics: reading at boot, and how far it fell once running */
    int  soc;           /* 0..100 % */
    int  minutes_left;  /* estimate while discharging, -1 unknown */
    bool low;           /* at or below 15 % */
    bool shutting_down; /* below the cut-off voltage: turning off soon */
} bat_info_t;

/* First thing in app_main. After a low-battery power-off the chip wakes every
 * few minutes; this goes straight back to sleep unless the battery is charging. */
void battery_boot_check(void);
void battery_start(void);
void battery_get(bat_info_t *out);
const char *battery_state_str(bat_state_t s);
