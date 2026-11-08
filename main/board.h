/*
 * The Waveshare ESP32-P4-WIFI6-Touch-LCD board this build is for (BOARD in the
 * top-level CMakeLists.txt). All four share the P4 + C6 wiring: SDIO Wi-Fi,
 * GPIO20 battery divider, GPIO26 backlight, ES8311 audio, GT911-family touch.
 * They differ in the panel and in the battery/power parts:
 *   5":         720x1280 HX8394, ETA6098 charger, no protection chip
 *   7":         720x1280 ILI9881C  \
 *   8":         800x1280 JD9365     > ETA6003 charger + S-8261 protection
 *   10.1":      800x1280 JD9365    /
 */
#pragma once
#include "sdkconfig.h"

#define BOARD_NAME       CONFIG_KR_BOARD_NAME
#define BOARD_APP_ASSET  CONFIG_KR_APP_ASSET

#if CONFIG_KR_BOARD_LCD5
#define BOARD_IS_LCD5        1
#define BOARD_HAS_BAT_PROTECT 0
#define BOARD_BL_LOAD_X10    10    /* backlight draw relative to the 5" x10 */
#else
#define BOARD_IS_LCD5        0
#define BOARD_HAS_BAT_PROTECT 1
/* Estimated from the panel area (not measured): 7" ~2x, 8" ~2.7x, 10.1" ~4.3x the 5". */
#if CONFIG_KR_BOARD_LCD7
#define BOARD_BL_LOAD_X10    20
#elif CONFIG_KR_BOARD_LCD8
#define BOARD_BL_LOAD_X10    27
#else
#define BOARD_BL_LOAD_X10    43
#endif
#endif

/* BOARD picks both the Kconfig choice and the board package; catch a mismatch
 * (for example a stale sdkconfig) at compile time. */
#if CONFIG_KR_BOARD_LCD5 && (CONFIG_BSP_LCD_TYPE_720_1280_7_INCH || CONFIG_BSP_LCD_TYPE_800_1280_8_INCH || CONFIG_BSP_LCD_TYPE_800_1280_10_1_INCH)
#error "5-inch build but the 7/8/10.1-inch board package is selected"
#endif
#if CONFIG_KR_BOARD_LCD7 && !CONFIG_BSP_LCD_TYPE_720_1280_7_INCH
#error "7-inch build needs CONFIG_BSP_LCD_TYPE_720_1280_7_INCH (boards/lcd7.defaults)"
#endif
#if CONFIG_KR_BOARD_LCD8 && !CONFIG_BSP_LCD_TYPE_800_1280_8_INCH
#error "8-inch build needs CONFIG_BSP_LCD_TYPE_800_1280_8_INCH (boards/lcd8.defaults)"
#endif
#if CONFIG_KR_BOARD_LCD10 && !CONFIG_BSP_LCD_TYPE_800_1280_10_1_INCH
#error "10.1-inch build needs CONFIG_BSP_LCD_TYPE_800_1280_10_1_INCH (boards/lcd10.defaults)"
#endif
