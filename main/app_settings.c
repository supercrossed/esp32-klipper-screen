#include "app_settings.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
app_settings_t g_settings;

static void defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = 80;
    s->orientation = 0;
    s->poll_ms = 1000;
    s->poll_bg_ms = 3000;
    strcpy(s->tz, "UTC0");
    s->tz_auto = 1;
    strcpy(s->device_name, "KlipperRemote");
}

static void get_str(nvs_handle_t h, const char *k, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, k, dst, &len) != ESP_OK) return;
    dst[cap - 1] = 0;
}

void settings_load(void)
{
    defaults(&g_settings);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return;
    }
    get_str(h, "ssid", g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid));
    get_str(h, "pass", g_settings.wifi_pass, sizeof(g_settings.wifi_pass));
    get_str(h, "tz", g_settings.tz, sizeof(g_settings.tz));
    get_str(h, "name", g_settings.device_name, sizeof(g_settings.device_name));
    nvs_get_u8(h, "bright", &g_settings.brightness);
    nvs_get_u8(h, "orient", &g_settings.orientation);
    nvs_get_u8(h, "tzauto", &g_settings.tz_auto);
    nvs_get_u8(h, "light", &g_settings.light_mode);
    nvs_get_u16(h, "poll", &g_settings.poll_ms);
    nvs_get_u16(h, "pollbg", &g_settings.poll_bg_ms);
    nvs_close(h);
    if (g_settings.brightness < 5) g_settings.brightness = 5;
    if (g_settings.poll_ms < 250) g_settings.poll_ms = 250;
    if (g_settings.poll_bg_ms < 500) g_settings.poll_bg_ms = 500;
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_str(h, "ssid", g_settings.wifi_ssid);
    nvs_set_str(h, "pass", g_settings.wifi_pass);
    nvs_set_str(h, "tz", g_settings.tz);
    nvs_set_str(h, "name", g_settings.device_name);
    nvs_set_u8(h, "bright", g_settings.brightness);
    nvs_set_u8(h, "orient", g_settings.orientation);
    nvs_set_u8(h, "tzauto", g_settings.tz_auto);
    nvs_set_u8(h, "light", g_settings.light_mode);
    nvs_set_u16(h, "poll", g_settings.poll_ms);
    nvs_set_u16(h, "pollbg", g_settings.poll_bg_ms);
    nvs_commit(h);
    nvs_close(h);
}

void settings_factory_reset(void)
{
    nvs_flash_erase();
    nvs_flash_init();
    defaults(&g_settings);
}
