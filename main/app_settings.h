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
    uint8_t  light_mode;      /* 0 dark, 1 light */
} app_settings_t;

extern app_settings_t g_settings;

void settings_load(void);
void settings_save(void);
void settings_factory_reset(void);   /* erases all NVS (settings + printers) */
