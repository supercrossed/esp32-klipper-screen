/*
 * Minimal line console on the USB-UART (115200), for headless setup/debug:
 *   wifi <ssid> [password]     join and save a network
 *   add <host> [port] [name]   add a printer
 *   list                       list printers and their state
 *   status                     Wi-Fi + memory
 *   scan                       network discovery
 *   shot                       start screenshot server (GET :8080/screenshot.bmp)
 *   tap <x> <y>                simulated tap
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

bool screenshot_server_start(void);
bool screenshot_tap(int x, int y);

static void handle(char *line)
{
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
        printf("sdio rx: %u buffers in PSRAM, largest %u bytes; largest internal block %u\n",
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
        bsp_display_lock(-1);
        if (atoi(argv[1]) == 0) ui_nav_home();
        else ui_nav_push((page_id_t)atoi(argv[1]), argc >= 3 ? atoi(argv[2]) : 0);
        bsp_display_unlock();
    } else if (!strcmp(argv[0], "shot")) {
        esp_netif_ip_info_t ip;
        if (screenshot_server_start() && wifi_mgr_ip(&ip))
            printf("screenshot: http://" IPSTR ":8080/screenshot.bmp\n", IP2STR(&ip.ip));
        else
            printf("screenshot server not started (Wi-Fi?)\n");
    } else if (!strcmp(argv[0], "tap") && argc >= 3) {
        printf(screenshot_tap(atoi(argv[1]), atoi(argv[2])) ? "tapped\n" : "nothing there\n");
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
    } else if (!strcmp(argv[0], "tasks")) {
        static char list[2048];
        vTaskList(list);
        printf("Name          State Prio Stack Num\n%s", list);
    } else if (!strcmp(argv[0], "sound")) {
        sound_play(argc >= 2 && !strcmp(argv[1], "error") ? SOUND_ERROR : SOUND_DONE);
        printf("sound queued (volume %d)\n", g_settings.alert_vol);
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
