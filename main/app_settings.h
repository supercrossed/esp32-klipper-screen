#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Persistent device settings (NVS namespace "cfg"). */
typedef struct {
    char     wifi_ssid[33];
    char     wifi_pass[65];
    uint8_t  brightness;      /* 5..100 % */
    uint8_t  orientation;     /* 0 landscape, 1 portrait, 2 landscape flipped, 3 portrait flipped */
    uint16_t poll_ms;         /* poll interval for the selected printer */
    uint16_t poll_bg_ms;      /* poll interval for printers on the home screen */
    char     tz[48];          /* POSIX TZ string */
    uint8_t  tz_auto;         /* 1 = detect from public IP once online */
    char     device_name[32];
    uint8_t  light_mode;      /* currently applied: 0 dark, 1 light */
    uint8_t  theme_mode;      /* 0 dark, 1 light, 2 auto (sunrise/sunset) */
    uint8_t  accent;          /* index into the accent colour list */
    float    lat, lon;        /* location for sunrise/sunset (from the IP lookup) */
    uint8_t  has_loc;
    uint8_t  alert_vol;       /* 0 = alerts silent, else speaker volume % */
    uint8_t  alerts;          /* 1 = show job-finished banners */
    uint16_t bat_cap_mah;     /* battery size, 0 = no battery fitted */
    uint16_t bat_cut_mv;      /* turn off below this voltage, 0 = never */
    uint8_t  bat_type;        /* BAT_TYPE_LIION (18650 etc.) or BAT_TYPE_LIPO */
} app_settings_t;

enum { BAT_TYPE_LIION = 0, BAT_TYPE_LIPO = 1 };

extern app_settings_t g_settings;

void settings_load(void);
void settings_save(void);
void settings_factory_reset(void);   /* erases all NVS (settings + printers) */
