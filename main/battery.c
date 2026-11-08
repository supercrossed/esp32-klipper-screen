/*
 * Battery monitor: see battery.h for the hardware.
 *
 * The board has no fuel gauge and no charger status line, so everything is
 * worked out from the cell voltage:
 *   - present:  a steady reading between 2.6 and 4.4 V for 5 s
 *   - charging: the voltage steps up when USB power arrives (charge current
 *               instead of load current through the cell's resistance) or
 *               keeps rising over two minutes; it steps down when USB goes
 *   - charge %: a resting-voltage curve for the battery type chosen in
 *               Settings (Li-ion cylindrical cell or LiPo pouch). On battery
 *               the cell sags under the board's load, and while charging it
 *               reads high, so the reading is corrected by load or charge
 *               current x cell resistance; the resistance is estimated from
 *               the capacity set in Settings (smaller cells sag more). The
 *               shown % only rises while charging and only falls on battery.
 *               The capacity also gives the time-left estimate.
 */
#include "battery.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"
#include "app_settings.h"
#include "board.h"

static const char *TAG = "battery";

#define BAT_GPIO       20      /* BAT_ADC: 200K / 100K divider */
#define BAT_DIV        3
#define BL_GPIO        26      /* LCD backlight, active high */
#define C6_RST_GPIO    54      /* CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, active high */
#define CHECK_EVERY_S  300     /* while off: wake this often to see if it is charging */
#define RESUME_MARGIN  300     /* mV above the cut-off (at rest) needed to start again */
#define OFF_MAGIC      0xBA77E2F0u

static RTC_NOINIT_ATTR uint32_t s_off_magic;
static int s_boot_mv;   /* measured before the backlight and Wi-Fi start */
static int s_boot_drop;
#define BOOT_DROP_ON_BATTERY 30   /* mV; measured ~55 on an 18650, 0..12 on USB */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_ch;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bat_info_t s_info = { .minutes_left = -1 };

/* Cell voltage -> charge left, down to 3.0 V (the lowest "Turn off when empty"
 * choice); the board has no fuel gauge to do better. */
typedef struct { int16_t mv, pct; } ocv_pt_t;
/* Li-ion cylindrical cells: the average of measured C/20 (100-150 mA)
 * discharges of four cells, each scaled to its own capacity down to 3.0 V:
 * LG HG2, Panasonic NCR18650PF and Samsung 30T (Kollmeyer, McMaster / UW-Madison,
 * Mendeley Data) and Samsung INR18650-20R (CALCE, UMD). They agree within
 * ~30-75 mV between 15% and 50%. See tools/battery_curve/. Voltages are at
 * ~CURVE_MA of load, not fully rested. */
#define CURVE_MA 130
static const ocv_pt_t OCV_LIION[] = {
    { 4180, 100 }, { 4095, 95 }, { 4059, 90 }, { 4024, 85 }, { 3976, 80 }, { 3923, 75 }, { 3878, 70 },
    { 3837, 65 },  { 3788, 60 }, { 3733, 55 }, { 3690, 50 }, { 3654, 45 }, { 3624, 40 }, { 3599, 35 },
    { 3570, 30 },  { 3534, 25 }, { 3494, 20 }, { 3442, 15 }, { 3376, 10 }, { 3300, 5 },  { 3000, 0 },
};
/* LiPo pouches: hold a higher voltage through the middle */
static const ocv_pt_t OCV_LIPO[] = {
    { 4200, 100 }, { 4100, 90 }, { 4000, 79 }, { 3930, 70 }, { 3870, 60 }, { 3820, 50 },
    { 3790, 42 },  { 3770, 35 }, { 3740, 27 }, { 3700, 19 }, { 3660, 12 }, { 3600, 6 },
    { 3500, 4 },   { 3300, 1 },  { 3000, 0 },
};

static int soc_from_mv(int mv)
{
    const ocv_pt_t *t = g_settings.bat_type == BAT_TYPE_LIPO ? OCV_LIPO : OCV_LIION;
    int n = g_settings.bat_type == BAT_TYPE_LIPO ? sizeof(OCV_LIPO) / sizeof(OCV_LIPO[0])
                                                 : sizeof(OCV_LIION) / sizeof(OCV_LIION[0]);
    if (mv >= t[0].mv) return 100;
    if (mv <= t[n - 1].mv) return 0;
    for (int i = 1; i < n; i++)
        if (mv >= t[i].mv)
            return t[i].pct + (mv - t[i].mv) * (t[i - 1].pct - t[i].pct) / (t[i - 1].mv - t[i].mv);
    return 0;
}

/* Rough charge current into the cell (mA). The 5"'s ETA6098 charges at around
 * half an amp; measured as ~40 mV above rest on an 18650. The 7/8/10.1" use an
 * ETA6003; assumed similar until measured. */
#define CHARGE_MA 500

/* Rough board draw from the battery (mA): display backlight dominates, and
 * scales with the panel (board.h). */
static int load_ma(void) { return 220 + 3 * g_settings.brightness * BOARD_BL_LOAD_X10 / 10; }
/* Cell + connector resistance (mOhm), smaller cells are worse. */
static int cell_mohm(void)
{
    int cap = g_settings.bat_cap_mah ? g_settings.bat_cap_mah : 2000;
    return 40 + 100000 / cap;
}

static bool adc_open(void)
{
    if (s_adc) return true;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BAT_GPIO, &unit, &s_ch) != ESP_OK) return false;
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { s_adc = NULL; return false; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, s_ch, &cc);
    adc_cali_curve_fitting_config_t cf = {
        .unit_id = unit, .chan = s_ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
    return true;
}

/* Average of n readings, in mV at the battery. */
static int read_mv(int n)
{
    if (!s_adc) return 0;
    int sum = 0, got = 0;
    for (int i = 0; i < n; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_ch, &raw) != ESP_OK) continue;
        if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) mv = raw * 3100 / 4095;
        sum += mv;
        got++;
    }
    return got ? sum / got * BAT_DIV : 0;
}

/* Backlight off, Wi-Fi chip held in reset, deep sleep; wake now and then to
 * check whether USB power is back. */
static void power_off(void)
{
    ESP_LOGW(TAG, "battery empty: turning off");
    gpio_config_t io = { .pin_bit_mask = (1ULL << BL_GPIO) | (1ULL << C6_RST_GPIO), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(BL_GPIO, 0);
    gpio_set_level(C6_RST_GPIO, 1);
    gpio_hold_en(BL_GPIO);
    gpio_hold_en(C6_RST_GPIO);
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
    s_off_magic = OFF_MAGIC;
    esp_sleep_enable_timer_wakeup((uint64_t)CHECK_EVERY_S * 1000000ULL);
    esp_deep_sleep_start();
}

void battery_boot_check(void)
{
    bool was_off = s_off_magic == OFF_MAGIC && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    s_off_magic = 0;
    gpio_hold_dis(BL_GPIO);
    gpio_hold_dis(C6_RST_GPIO);
    if (!adc_open()) return;
    int mv = read_mv(32);
    s_boot_mv = mv;
    if (!was_off || !g_settings.bat_cut_mv || !g_settings.bat_cap_mah) return;
    ESP_LOGI(TAG, "woke from low-battery off: %d mV", mv);
    /* under ~1.5 V: battery unplugged, so running from USB */
    if (mv > 1500 && mv < g_settings.bat_cut_mv + RESUME_MARGIN) power_off();
}

const char *battery_state_str(bat_state_t s)
{
    switch (s) {
    case BAT_DISCHARGING: return "On battery";
    case BAT_CHARGING:    return "Charging";
    case BAT_FULL:        return "Charged";
    default:              return "No battery";
    }
}

void battery_get(bat_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

static void bat_task(void *arg)
{
    int win[30] = { 0 };          /* last 30 one-second readings */
    int nwin = 0;
    float slow[13] = { 0 };       /* filtered value every 10 s, 2 minutes back */
    int nslow = 0;
    float ema = 0, soc_f = -1;
    int shown = -1;               /* the % on screen */
    int last_type = g_settings.bat_type;
    bool present = false;
    int ok_s = 0, bad_s = 0, below_s = 0, shut_s = 0;
    bat_state_t st = BAT_DISCHARGING;

    for (uint32_t t = 0;; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int mv = read_mv(16);
        win[nwin % 30] = mv;
        nwin++;

        /* steady and in range? (an empty connector reads ~0 V or jumps around) */
        int n = nwin < 10 ? nwin : 10, lo = 99999, hi = 0;
        for (int i = 0; i < n; i++) {
            int v = win[(nwin - 1 - i) % 30];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        bool plausible = mv > 2600 && mv < 4400 && hi - lo < 250;
        if (plausible) { ok_s++; bad_s = 0; } else { bad_s++; ok_s = 0; }
        if (!present && ok_s >= 5) {
            present = true;
            ema = mv;
            nslow = 0;
            soc_f = -1;
            shown = -1;
            st = mv >= 4170 ? BAT_FULL : BAT_DISCHARGING;
            /* Plugged in at boot? On battery the backlight and Wi-Fi load pulls the
             * cell down from its boot reading; on USB the charger carries the load. */
            if (st == BAT_DISCHARGING && s_boot_mv > 2600 && t < 20) {
                s_boot_drop = s_boot_mv - mv;
                if (s_boot_drop < BOOT_DROP_ON_BATTERY) st = BAT_CHARGING;
                ESP_LOGI(TAG, "boot %d mV, now %d mV (drop %d): %s", s_boot_mv, mv, s_boot_drop, battery_state_str(st));
            } else if (st == BAT_DISCHARGING && t >= 20) {
                /* A battery that appears while the screen is already running was
                 * plugged in on USB power (nothing else could be running it). */
                st = BAT_CHARGING;
                ESP_LOGI(TAG, "battery connected while running: %d mV, charging", mv);
            }
        }
        if (present && bad_s >= 5) present = false;

        if (present) {
            if (plausible) ema += (mv - ema) * 0.1f;

            /* step: mean of the last 5 s against 20..25 s ago */
            if (nwin >= 30) {
                int a = 0, b = 0;
                for (int i = 0; i < 5; i++) {
                    a += win[(nwin - 1 - i) % 30];
                    b += win[(nwin - 21 - i) % 30];
                }
                int step = (a - b) / 5;
                if (step > 60 && st == BAT_DISCHARGING) st = BAT_CHARGING;
                else if (step < -60 && st != BAT_DISCHARGING) st = BAT_DISCHARGING;
            }
            /* slow trend over two minutes */
            if (t % 10 == 0) {
                slow[nslow % 13] = ema;
                nslow++;
                if (nslow > 12) {
                    float d = ema - slow[(nslow - 13) % 13];
                    if (d > 12 && st == BAT_DISCHARGING) st = BAT_CHARGING;
                    else if (d < -8 && st != BAT_DISCHARGING) st = BAT_DISCHARGING;
                }
            }
            if (st == BAT_CHARGING && ema >= 4170) st = BAT_FULL;

            int v;   /* estimated resting voltage */
            /* the curves are at CURVE_MA of discharge: correct to that load */
            if (st == BAT_DISCHARGING) v = ema + (load_ma() - CURVE_MA) * cell_mohm() / 1000;
            else if (st == BAT_CHARGING) v = ema - (CHARGE_MA + CURVE_MA) * cell_mohm() / 1000;
            else v = 4200;
            /* 0% = the "Turn off when empty" point, so the number reaches 0 just as
             * the screen turns itself off. The cut-off is checked on the loaded
             * voltage, so compare against its resting equivalent. */
            float target = soc_from_mv(v);
            if (g_settings.bat_cut_mv) {
                int floor_pct = soc_from_mv(g_settings.bat_cut_mv + (load_ma() - CURVE_MA) * cell_mohm() / 1000);
                if (floor_pct < 100) target = (target - floor_pct) * 100.0f / (100 - floor_pct);
                if (target < 0) target = 0;
            }
            if (g_settings.bat_type != last_type) { last_type = g_settings.bat_type; soc_f = -1; shown = -1; }
            soc_f = soc_f < 0 ? target : soc_f + (target - soc_f) * 0.05f;
            /* Keep the number steady: it only climbs while charging and only falls
             * on battery (a brightness change or charger taper moves the voltage
             * without changing the charge). */
            int now = (int)(soc_f + 0.5f);
            if (shown < 0) shown = now;
            else if (st == BAT_DISCHARGING ? now < shown : now > shown) shown = now;

            /* low-battery power off */
            if (st == BAT_DISCHARGING && g_settings.bat_cut_mv && g_settings.bat_cap_mah && ema < g_settings.bat_cut_mv)
                below_s++;
            else
                below_s = 0;
            if (below_s >= 30) {
                if (++shut_s >= 20) power_off();
            } else shut_s = 0;
        } else {
            below_s = shut_s = 0;
        }

        bat_info_t in = { .raw_mv = mv, .minutes_left = -1, .boot_mv = s_boot_mv, .boot_drop = s_boot_drop };
        if (present && g_settings.bat_cap_mah) {
            in.state = st;
            in.mv = (int)ema;
            in.soc = shown;
            if (in.soc > 100) in.soc = 100;
            if (st == BAT_FULL) in.soc = 100;
            if (st == BAT_DISCHARGING) in.minutes_left = g_settings.bat_cap_mah * in.soc / 100 * 60 / load_ma();
            in.low = in.soc <= 15 && st == BAT_DISCHARGING;
            in.shutting_down = below_s >= 30;
        }
        portENTER_CRITICAL(&s_mux);
        s_info = in;
        portEXIT_CRITICAL(&s_mux);

        if (t % 60 == 0)
            ESP_LOGI(TAG, "%d mV (filtered %d), %s, %d%%", mv, in.mv, battery_state_str(in.state), in.soc);
    }
}

void battery_start(void)
{
    if (!adc_open()) {
        ESP_LOGE(TAG, "ADC init failed");
        return;
    }
    xTaskCreate(bat_task, "battery", 3072, NULL, 3, NULL);
}
