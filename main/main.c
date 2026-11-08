/*
 * Klipper Remote - a small KlipperScreen-style controller for several
 * Klipper/Moonraker printers, on the Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_adapter_input.h"
#include "app_settings.h"
#include "wifi_mgr.h"
#include "printers.h"
#include "ui.h"
#include "updater.h"
#include "battery.h"

static const char *TAG = "main";
void serial_console_start(void);

/* Draw buffer height in panel lines. The BSP uses 50, which in landscape means
 * a scrolling list is drawn in ~20 strips per frame, each one walking the
 * object tree and laying out every visible label again. */
#define DRAW_BUF_LINES 240

/* Same steps as bsp_display_start_with_config(), with our own draw buffer. */
static lv_display_t *display_start(const bsp_display_cfg_t *cfg)
{
    if (esp_lv_adapter_init(&cfg->lv_adapter_cfg) != ESP_OK) return NULL;
    if (bsp_display_brightness_init() != ESP_OK) return NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (bsp_display_new(NULL, &panel, &io) != ESP_OK) return NULL;
    esp_lv_adapter_display_config_t dc = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .rotation = cfg->rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = cfg->tear_avoid_mode,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&dc);
    if (!disp) return NULL;
    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(cfg, &tp) != ESP_OK) return NULL;
    const esp_lv_adapter_touch_config_t tc = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    if (!esp_lv_adapter_register_touch(&tc)) return NULL;
    if (esp_lv_adapter_start() != ESP_OK) return NULL;
    return disp;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    battery_boot_check();   /* may go straight back to sleep after a low-battery power off */

    static const esp_lv_adapter_rotation_t rot[4] = {
        ESP_LV_ADAPTER_ROTATE_90,    /* landscape */
        ESP_LV_ADAPTER_ROTATE_0,     /* portrait (native panel orientation) */
        ESP_LV_ADAPTER_ROTATE_270,   /* landscape flipped */
        ESP_LV_ADAPTER_ROTATE_180,   /* portrait flipped */
    };
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = rot[g_settings.orientation & 3],
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = { 0 },
    };
    /* The adapter rotates pixels in its flush but passes raw panel touch coordinates
     * (720x1280) through, so map them into the rotated space here. esp_lcd_touch
     * applies mirror first (panel space), then swap. Derived from the adapter's
     * rotation mapping: ROTATE_90 puts logical (x,y) at panel (W-1-y, x). */
    switch (g_settings.orientation & 3) {
    case 0: cfg.touch_flags.mirror_x = 1; cfg.touch_flags.swap_xy = 1; break;   /* 90  */
    case 2: cfg.touch_flags.mirror_y = 1; cfg.touch_flags.swap_xy = 1; break;   /* 270 */
    case 3: cfg.touch_flags.mirror_x = 1; cfg.touch_flags.mirror_y = 1; break;  /* 180 */
    default: break;                                                             /* 0   */
    }
    cfg.lv_adapter_cfg.task_stack_size = 16 * 1024;
    lv_display_t *disp = display_start(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    /* Networking and pollers start before the UI is built; the UI only reads their state. */
    wifi_mgr_start();
    printers_init();
    updater_init();
    battery_start();
    serial_console_start();

    bsp_display_lock(-1);
    ui_init(disp);
    bsp_display_unlock();
    ESP_LOGI(TAG, "up: %dx%d", lv_display_get_horizontal_resolution(disp), lv_display_get_vertical_resolution(disp));
}
