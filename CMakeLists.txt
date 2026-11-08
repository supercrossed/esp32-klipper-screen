cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.4.12")

# Which Waveshare ESP32-P4-WIFI6-Touch-LCD board to build for:
#   lcd5 (default), lcd7, lcd8, lcd10   ->   idf.py -B build_lcd7 -DBOARD=lcd7 build
# Each board has its own sdkconfig (the 5" keeps the plain one) and boards/<board>.defaults.
set(BOARD lcd5 CACHE STRING "Board: lcd5, lcd7, lcd8 or lcd10")
set(KR_BOARDS lcd5 lcd7 lcd8 lcd10)
if(NOT BOARD IN_LIST KR_BOARDS)
    message(FATAL_ERROR "BOARD must be one of: ${KR_BOARDS} (got '${BOARD}')")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${BOARD}.defaults")
if(BOARD STREQUAL "lcd5")
    set(ENV{KR_BSP} lcd5)       # waveshare/esp32_p4_wifi6_touch_lcd_5
else()
    set(ENV{KR_BSP} lcdx)       # waveshare/esp32_p4_wifi6_touch_lcd_x, panel picked in Kconfig
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT BOARD STREQUAL "lcd5")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lcdx.lock")
endif()
project(klipper_remote)
