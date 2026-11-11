/*
 * Minimal line console on the USB-UART (115200), for headless setup/debug:
 *   wifi <ssid> [password]     join and save a network
 *   add <host> [port] [name]   add a printer
 *   list                       list printers and their state
 *   status                     Wi-Fi + memory
 *   scan                       network discovery
 *   shot                       start screenshot server (GET :8080/screenshot.bmp)
 *   tap <x> <y>                simulated touch (a virtual finger)
 *   swipe <x> <y0> <y1>        simulated vertical drag
 *   type <text> / type!        type into the focused field / press OK
 *   theme light|dark
 *   reboot
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "wifi_mgr.h"
#include "app_settings.h"
#include "printers.h"
#include "discovery.h"
#include "bsp/esp-bsp.h"
#include "ui.h"
#include "webcam.h"
#include "updater.h"
#include "sound.h"
#include "battery.h"
#include "label_printer.h"

bool screenshot_server_start(void);
bool screenshot_tap(int x, int y);

static void handle(char *line)
{
    /* type <text>: into the field the on-screen keyboard is on (testing); "type!" = press OK */
    if (!strncmp(line, "type ", 5) || !strcmp(line, "type!")) {
        bsp_display_lock(-1);
        bool ok = line[4] == '!' ? ui_type_text(NULL, true) : ui_type_text(line + 5, false);
        bsp_display_unlock();
        printf(ok ? "typed\n" : "no field has the keyboard\n");
        return;
    }
    char *argv[4] = {0};
    int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 4; t = strtok(NULL, argc == 2 && !strcmp(argv[0], "add") ? "" : " \t"))
        argv[argc++] = t;
    if (!argc) return;
    if (!strcmp(argv[0], "wifi") && argc >= 2) {
        wifi_mgr_connect(argv[1], argc >= 3 ? argv[2] : "", true);
        printf("connecting to \"%s\"\n", argv[1]);
    } else if (!strcmp(argv[0], "add") && argc >= 2) {
        printer_cfg_t c = { .port = 7125 };
        strlcpy(c.host, argv[1], sizeof(c.host));
        char *rest = argc >= 3 ? argv[2] : NULL;
        if (rest) {
            char *end;
            long p = strtol(rest, &end, 10);
            if (p > 0) { c.port = p; rest = *end ? end + 1 : NULL; }
        }
        strlcpy(c.name, rest && *rest ? rest : c.host, sizeof(c.name));
        printf(printers_add(&c) >= 0 ? "added %s\n" : "add failed %s\n", c.host);
    } else if (!strcmp(argv[0], "list")) {
        static printer_state_t st;
        for (int i = 0; i < printers_count(); i++) {
            printer_cfg_t c;
            printers_get_cfg(i, &c);
            printers_get_state(i, &st);
            printf("%d: %s %s:%u  %s  %.0f%%  heaters=%d mmu=%d lanes=%d cams=%d  %s\n", i, c.name, c.host, c.port,
                   printer_status_str(st.status), st.progress * 100, st.nheaters, st.mmu.type, st.mmu.nlanes,
                   st.nwebcams, st.message);
            if (st.eta_s >= 0)
                printf("   left %.0f min (printing %.0f min, filament %.0f/%.0f mm, slicer %.0f min, gcode bytes %lu-%lu)\n",
                       st.eta_s / 60, st.print_duration / 60, st.filament_used, st.filament_total, st.est_slicer / 60,
                       (unsigned long)st.gcode_start, (unsigned long)st.gcode_end);
        }
    } else if (!strcmp(argv[0], "status")) {
        esp_netif_ip_info_t ip;
        bool up = wifi_mgr_ip(&ip);
        printf("wifi: %s %s rssi=%d ip=" IPSTR "  heap int=%u psram=%u min_int=%u\n", wifi_mgr_state_str(),
               wifi_mgr_ssid(), wifi_mgr_rssi(), IP2STR(up ? &ip.ip : &(esp_ip4_addr_t){0}),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        extern volatile uint32_t g_hosted_rx_psram_n, g_hosted_rx_psram_max;
        printf("sdio: %u buffers in PSRAM, largest %u bytes; largest internal block %u\n",
               (unsigned)g_hosted_rx_psram_n, (unsigned)g_hosted_rx_psram_max,
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        char cs[64];
        webcam_status(cs, sizeof(cs));
        printf("webcam: %s\n", cs);
    } else if (!strcmp(argv[0], "scan")) {
        discovery_start();
        while (discovery_busy()) vTaskDelay(pdMS_TO_TICKS(500));
        static disc_result_t r[DISC_MAX];
        int n = discovery_results(r, DISC_MAX);
        for (int i = 0; i < n; i++) printf("found: %s %s:%u %s (%s)\n", r[i].name, r[i].host, r[i].port, r[i].klippy, r[i].via);
        printf("scan done: %d\n", n);
    } else if (!strcmp(argv[0], "focus") && argc >= 2) {
        printers_set_focus(atoi(argv[1]));
    } else if (!strcmp(argv[0], "lanes") && argc >= 2) {
        static printer_state_t st;
        if (printers_get_state(atoi(argv[1]), &st)) {
            printf("mmu type=%d unit=%s current=%s filament=%s stage=%d active=%d\n", st.mmu.type, st.mmu.unit,
                   st.mmu.current, st.mmu.filament, st.mmu.stage, st.mmu.active_lane);
            for (int i = 0; i < st.mmu.nlanes; i++) {
                const mmu_lane_t *l = &st.mmu.lanes[i];
                printf("  %s %s %s col=%06lX present=%d loaded=%d hub=%d wt=%d %s\n", l->name, l->tool, l->material,
                       (unsigned long)l->color, l->present, l->loaded, l->at_hub, l->weight_g, l->status);
            }
            for (int i = 0; i < st.nwebcams; i++) printf("  cam %s snap=%s stream=%s\n", st.webcams[i].name, st.webcams[i].snapshot_url, st.webcams[i].stream_url);
        }
    } else if (!strcmp(argv[0], "page") && argc >= 2) {
        /* page <id> [arg]: 0 home 1 printer 2 webcam(arg=idx*16+cam) 3 add 4 settings 5 wifi */
        extern void bench_page_begin(void), bench_page_end(void);
        bsp_display_lock(-1);
        bench_page_begin();
        if (atoi(argv[1]) == 0) ui_nav_home();
        else ui_nav_push((page_id_t)atoi(argv[1]), argc >= 3 ? atoi(argv[2]) : 0);
        bench_page_end();   /* prints build time and time to first frame */
        bsp_display_unlock();
    } else if (!strcmp(argv[0], "shot")) {
        esp_netif_ip_info_t ip;
        if (screenshot_server_start() && wifi_mgr_ip(&ip))
            printf("screenshot: http://" IPSTR ":8080/screenshot.bmp\n", IP2STR(&ip.ip));
        else
            printf("screenshot server not started (Wi-Fi?)\n");
    } else if (!strcmp(argv[0], "tap") && argc >= 3) {
        printf(screenshot_tap(atoi(argv[1]), atoi(argv[2])) ? "tapped\n" : "busy\n");
    } else if (!strcmp(argv[0], "swipe") && argc >= 4) {
        /* swipe <x> <y0> <y1>: vertical drag (scrolling) */
        extern bool screenshot_swipe(int x0, int y0, int x1, int y1);
        int x = atoi(argv[1]);
        printf(screenshot_swipe(x, atoi(argv[2]), x, atoi(argv[3])) ? "swiped\n" : "busy\n");
    } else if (!strcmp(argv[0], "theme") && argc >= 2) {
        bsp_display_lock(-1);
        ui_set_light_mode(!strcmp(argv[1], "light"));
        bsp_display_unlock();
    } else if (!strcmp(argv[0], "upd") && argc >= 2) {
        if (!strcmp(argv[1], "check")) updater_check_now();
        else if (!strcmp(argv[1], "install")) updater_install();
        else if (!strcmp(argv[1], "dialog")) { bsp_display_lock(-1); ui_update_dialog(); bsp_display_unlock(); }
        static upd_info_t u;
        updater_get(&u);
        printf("updater: state=%d current=%s latest=%s size=%u progress=%d err=%s\n", u.state, u.current, u.latest,
               (unsigned)u.size, u.progress, u.error);
        printf("notes: %.300s\n", u.notes);
    } else if (!strcmp(argv[0], "fbcheck")) {
        extern void screenshot_fbcheck(void);
        screenshot_fbcheck();
    } else if (!strcmp(argv[0], "bench")) {
        extern void screenshot_bench(void);
        screenshot_bench();
    } else if (!strcmp(argv[0], "tasks")) {
        static char list[2048];
        vTaskList(list);
        printf("Name          State Prio Stack Num\n%s", list);
    } else if (!strcmp(argv[0], "sound")) {
        sound_play(argc >= 2 && !strcmp(argv[1], "error") ? SOUND_ERROR : SOUND_DONE);
        printf("sound queued (volume %d)\n", g_settings.alert_vol);
    } else if (!strcmp(argv[0], "bat")) {
        /* bat [cap <mAh> | cut <mV>]: show the battery, or change its settings */
        if (argc >= 3 && !strcmp(argv[1], "cap")) { g_settings.bat_cap_mah = atoi(argv[2]); settings_save(); }
        if (argc >= 3 && !strcmp(argv[1], "cut")) { g_settings.bat_cut_mv = atoi(argv[2]); settings_save(); }
        bat_info_t b;
        battery_get(&b);
        printf("battery: %s  %d mV (reading %d mV)  %d%%  left=%d min  low=%d off=%d  boot=%d drop=%d  [cap %u mAh, cut-off %u mV]\n",
               battery_state_str(b.state), b.mv, b.raw_mv, b.soc, b.minutes_left, b.low, b.shutting_down,
               b.boot_mv, b.boot_drop, g_settings.bat_cap_mah, g_settings.bat_cut_mv);
        static const char *const RR[] = { "unknown", "power-on", "external pin", "software", "panic", "int-wdt",
                                          "task-wdt", "other-wdt", "deep-sleep", "brownout", "sdio", "usb", "jtag",
                                          "efuse", "power-glitch", "cpu-lockup" };
        int rr = esp_reset_reason();
        printf("last restart: %s (%d)\n", rr >= 0 && rr < (int)(sizeof(RR) / sizeof(RR[0])) ? RR[rr] : "?", rr);
    } else if (!strcmp(argv[0], "label")) {
        /* label scan | label use <n> | label test | label forget | label (status) */
        static label_dev_t d[LABEL_SCAN_MAX];
        const char *sub = argc >= 2 ? argv[1] : "";
        if (!strcmp(sub, "scan")) {
            label_scan(6);
            vTaskDelay(pdMS_TO_TICKS(500));
            char m[96];
            while (label_state(m, sizeof(m)) == LABEL_SCANNING || label_state(m, sizeof(m)) == LABEL_STARTING)
                vTaskDelay(pdMS_TO_TICKS(300));
            int n = label_scan_results(d, LABEL_SCAN_MAX);
            for (int i = 0; i < n; i++)
                printf("  %d: %-24s %02X:%02X:%02X:%02X:%02X:%02X type %d  %d dBm\n", i, d[i].name, d[i].addr[5], d[i].addr[4],
                       d[i].addr[3], d[i].addr[2], d[i].addr[1], d[i].addr[0], d[i].addr_type, d[i].rssi);
            printf("label scan: %s\n", m);
        } else if (!strcmp(sub, "use") && argc >= 3) {
            int i = atoi(argv[2]), n = label_scan_results(d, LABEL_SCAN_MAX);
            if (i >= 0 && i < n) {
                label_cfg_t c;
                label_cfg_get(&c);
                c.have = true;
                strlcpy(c.name, d[i].name, sizeof(c.name));
                memcpy(c.addr, d[i].addr, 6);
                c.addr_type = d[i].addr_type;
                label_cfg_set(&c);
                printf("label printer: %s\n", c.name);
            } else printf("run 'label scan' first\n");
        } else if (!strcmp(sub, "test")) {
            extern void ui_label_test(void);
            bsp_display_lock(-1);
            ui_label_test();
            bsp_display_unlock();
        } else if (!strcmp(sub, "forget")) {
            label_forget();
        } else if (!strcmp(sub, "dry") && argc >= 3) {
            label_set_dry(atoi(argv[2]));
        } else if (!strcmp(sub, "dump")) {
            /* the last label as hex, 64 bytes per line */
            size_t n = label_last(NULL, 0);
            uint8_t *b = n ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
            if (b) {
                n = label_last(b, n);
                printf("LABELDUMP %u\n", (unsigned)n);
                for (size_t i = 0; i < n; i += 64) {
                    printf("LD ");
                    for (size_t k = i; k < n && k < i + 64; k++) printf("%02x", b[k]);
                    printf("\n");
                }
                printf("LABELEND\n");
                heap_caps_free(b);
            }
        }
        label_cfg_t c;
        label_cfg_get(&c);
        char m[96];
        label_state_t st = label_state(m, sizeof(m));
        printf("label: printer %s, state %d (%s), connected %d, nudge %d,%d mm, density %u\n", c.have ? c.name : "(none)", st, m,
               label_connected(), c.dx_mm, c.dy_mm, c.density);
    } else if (!strcmp(argv[0], "orient") && argc >= 2) {
        /* orient 0..3: landscape, portrait, landscape flipped, portrait flipped (saved, restarts) */
        g_settings.orientation = atoi(argv[1]) & 3;
        settings_save();
        printf("orientation %d, restarting\n", g_settings.orientation);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strcmp(argv[0], "reboot")) {
        esp_restart();
    } else {
        printf("commands: wifi <ssid> [pass] | add <host> [port] [name] | list | status | reboot\n");
    }
}

static void console_task(void *arg)
{
    char line[160];
    int n = 0;
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            line[n] = 0;
            if (n) handle(line);
            n = 0;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = ch;
        }
    }
}

void serial_console_start(void)
{
    if (uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0) != ESP_OK) return;
    uart_vfs_dev_use_driver(UART_NUM_0);
    xTaskCreate(console_task, "console", 6144, NULL, 2, NULL);
}
