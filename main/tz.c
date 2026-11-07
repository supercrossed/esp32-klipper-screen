#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "tz.h"
#include "cJSON.h"
#include "esp_log.h"
#include "http_util.h"
#include "app_settings.h"

static const char *TAG = "tz";

const tz_entry_t TZ_TABLE[] = {
    { "UTC",               "Etc/UTC",             "UTC0" },
    { "US Pacific",        "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "US Mountain",       "America/Denver",      "MST7MDT,M3.2.0,M11.1.0" },
    { "US Arizona",        "America/Phoenix",     "MST7" },
    { "US Central",        "America/Chicago",     "CST6CDT,M3.2.0,M11.1.0" },
    { "US Eastern",        "America/New_York",    "EST5EDT,M3.2.0,M11.1.0" },
    { "UK / Ireland",      "Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe",    "Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Eastern Europe",    "Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "India",             "Asia/Kolkata",        "IST-5:30" },
    { "China / Singapore", "Asia/Shanghai",       "CST-8" },
    { "Japan / Korea",     "Asia/Tokyo",          "JST-9" },
    { "Australia East",    "Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "New Zealand",       "Pacific/Auckland",    "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
const int TZ_COUNT = sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]);

/* Zones that share rules with a table entry. */
static const struct { const char *iana; int idx; } ALIASES[] = {
    { "America/Vancouver", 1 }, { "America/Tijuana", 1 }, { "America/Edmonton", 2 }, { "America/Boise", 2 },
    { "America/Winnipeg", 4 }, { "America/Toronto", 5 }, { "America/Detroit", 5 }, { "America/Montreal", 5 },
    { "Europe/Dublin", 6 }, { "Europe/Lisbon", 6 },
    { "Asia/Singapore", 10 }, { "Asia/Hong_Kong", 10 }, { "Asia/Taipei", 10 }, { "Australia/Perth", 10 },
    { "Asia/Seoul", 11 }, { "Australia/Melbourne", 12 }, { "Australia/Hobart", 12 }, { "Australia/Canberra", 12 },
};

void tz_from_iana(const char *iana, int offset_s, char *posix, size_t cap)
{
    for (int i = 0; i < TZ_COUNT; i++)
        if (!strcmp(iana, TZ_TABLE[i].iana)) { strlcpy(posix, TZ_TABLE[i].posix, cap); return; }
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++)
        if (!strcmp(iana, ALIASES[i].iana)) { strlcpy(posix, TZ_TABLE[ALIASES[i].idx].posix, cap); return; }
    /* Most of continental Europe follows CET/EET rules. */
    if (!strncmp(iana, "Europe/", 7)) {
        if (offset_s == 3600 || offset_s == 7200) {   /* +1 std or +2 summer -> could be CET */
            if (strstr(iana, "Helsinki") || strstr(iana, "Kiev") || strstr(iana, "Kyiv") || strstr(iana, "Riga") ||
                strstr(iana, "Vilnius") || strstr(iana, "Tallinn") || strstr(iana, "Sofia") || strstr(iana, "Bucharest"))
                strlcpy(posix, TZ_TABLE[8].posix, cap);
            else
                strlcpy(posix, TZ_TABLE[7].posix, cap);
            return;
        }
    }
    /* Fixed offset (no DST rules). POSIX sign is inverted: UTC-8 is "UTC8". */
    int m = -offset_s / 60;
    if (m % 60) snprintf(posix, cap, "UTC%d:%02d", m / 60, abs(m % 60));
    else snprintf(posix, cap, "UTC%d", m / 60);
}

int tz_index(const char *posix)
{
    for (int i = 0; i < TZ_COUNT; i++) if (!strcmp(posix, TZ_TABLE[i].posix)) return i;
    return -1;
}

bool tz_autodetect(void)
{
    esp_http_client_handle_t conn = NULL;
    http_buf_t b = { .max = 4096 };
    int st = http_request(&conn, HTTP_METHOD_GET, "http://ip-api.com/json/?fields=status,timezone,offset,lat,lon",
                          NULL, NULL, NULL, 0, &b, 5000);
    http_conn_close(&conn);
    bool ok = false;
    if (st == 200 && b.data) {
        cJSON *j = cJSON_ParseWithLength((char *)b.data, b.len);
        const cJSON *zone = cJSON_GetObjectItem(j, "timezone");
        const cJSON *off = cJSON_GetObjectItem(j, "offset");
        if (cJSON_IsString(zone) && cJSON_IsNumber(off)) {
            char posix[48];
            tz_from_iana(zone->valuestring, off->valueint, posix, sizeof(posix));
            ESP_LOGI(TAG, "detected %s (offset %d) -> %s", zone->valuestring, off->valueint, posix);
            if (g_settings.tz_auto) {   /* a manually chosen zone is left alone */
                if (strcmp(posix, g_settings.tz)) {
                    strlcpy(g_settings.tz, posix, sizeof(g_settings.tz));
                    settings_save();
                }
                setenv("TZ", g_settings.tz, 1);
                tzset();
            }
            const cJSON *la = cJSON_GetObjectItem(j, "lat"), *lo = cJSON_GetObjectItem(j, "lon");
            if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
                g_settings.lat = la->valuedouble;
                g_settings.lon = lo->valuedouble;
                g_settings.has_loc = 1;
                settings_save();
            }
            ok = true;
        }
        cJSON_Delete(j);
    }
    http_buf_free(&b);
    if (!ok) ESP_LOGW(TAG, "time zone lookup failed (HTTP %d)", st);
    return ok;
}
