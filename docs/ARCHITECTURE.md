# Architecture

```
            ┌───────────────── ESP32-P4 ─────────────────┐
 touch ───► │ LVGL task (UI)  ◄─ snapshots ─  pollers ×N │ ── HTTP ──► Moonraker (each printer)
 display ◄─ │   pages: home / printer / webcam / …        │
            │ webcam task ─► HW JPEG ─► PPA ─► 3 buffers  │ ── HTTP ──► camera MJPEG stream
            │ cmd tasks (one per command), discovery task │
            └──────────── esp_wifi_remote / esp_hosted ───┘
                                  │ SDIO
                          ESP32-C6 (Wi-Fi 6, factory firmware)
```

## Source map

| File | Role |
|---|---|
| `main/main.c` | Boot: NVS, settings, display (rotation + touch mapping), Wi-Fi, pollers, UI |
| `main/wifi_mgr.c` | Wi-Fi state machine. Every `esp_wifi_*` call is an RPC to the C6, so they all run on one task, never in event handlers. Also SNTP and time-zone detection |
| `main/printers.c` | Printer registry (NVS), one **poller task per printer**, Moonraker parsing (status, heaters, AFC/HH, webcams, macros, Spoolman), command tasks |
| `main/http_util.c` | Keep-alive HTTP helper: growable PSRAM buffer, redirects, retry on stale sockets |
| `main/discovery.c` | mDNS plus subnet sweep in batches of 9 hosts (sized to lwIP's 10-entry ARP table) |
| `main/webcam.c` | MJPEG stream reader (snapshot loop as fallback) → hardware JPEG decode → PPA scale/rotate/mirror → triple buffer. A reader task and a decoder task overlap, with two JPEG input buffers |
| `main/spoolman.c` | Spoolman over Moonraker's proxy |
| `main/tz.c` | POSIX time-zone table and ip-api.com lookup |
| `main/updater.c` | GitHub Releases check, HTTPS OTA into the inactive slot, rollback confirmation |
| `main/files.c` | G-code file list and per-file metadata/thumbnail for the Files page |
| `main/sound.c` | Alert chimes on the built-in speaker (ES8311), codec started on first use |
| `main/ui/ui_alert.c` | Job-ended banners (queue filled by the pollers in `printers.c`) |
| `main/machine.c` | Console history and system services, polled only while those tabs are open |
| `main/screenshot.c` | On-demand screenshot server (`shot` command) and simulated taps |
| `main/serial_console.c` | USB-UART debug console |
| `main/lv_mem_psram.c` | LVGL allocator in PSRAM |
| `main/ui/ui.c` | Shell (header + content), navigation stack, palette/theme, fonts, keyboard, dialogs |
| `main/ui/page_*.c` | One file per page. Each has `create`, `refresh` (4 Hz) and `destroy` |
| `main/ui/ui_keyboard.c` | Keyboard layouts (number row, IP numpad) |
| `main/ui/icons.h`, `icons/mdi.ttf` | Generated icon macros and the icon font |

## Threading rules

- **Only the LVGL task touches widgets.** Other tasks publish data under a mutex, and pages copy a `printer_state_t` snapshot in their `refresh()`.
- **The UI never does network I/O.** Commands (`printers_command`), Spoolman fetches and the webcam each run on their own task.
- **Wi-Fi RPCs** to the C6 run only on the Wi-Fi task. Event handlers just post to its queue.
- Emergency stop runs at a higher priority than other commands and is never queued behind a slow `G28`.

## Memory rules

Internal SRAM (about 100 KB free) is the scarce resource. esp_hosted allocates a DMA buffer from internal RAM for every outgoing packet and aborts if it can't. So:

- Worker task stacks are in **PSRAM** (`xTaskCreateWithCaps`): pollers, commands, webcam, discovery, Spoolman.
- LVGL objects, styles and decoded images live in **PSRAM**.
- `SPIRAM_MALLOC_ALWAYSINTERNAL=1024`: anything larger goes to PSRAM. 64 KB of internal RAM is reserved for DMA.
- cJSON and HTTP response buffers are in PSRAM.

## Page lifecycle (why leaving a page frees it)

`ui_nav_push/back/home` calls the old page's `destroy()`, deletes all of its widgets, then builds the next page. Only the page on screen exists:

- **Printer page**: only the selected sidebar tab is built. On close, polling for that printer drops back to the slow, light query.
- **Webcam**: the task, JPEG and PPA engines, and the roughly 5–10 MB of frame buffers exist only while it's open.
- **Spoolman**: its lists are freed on close.
- **Thumbnails**: each tile holds its own copy, freed with the tile.

## Polling

Each printer's poller:

1. `GET /server/info`. If Klipper isn't ready, it shows the state message and backs off.
2. On first contact or reconnect: `/printer/info`, `/printer/objects/list` (builds the query), `/server/webcams/list`, `/server/spoolman/status`.
3. Every cycle: `/printer/objects/query`. The **full** query (MMU, toolhead, fans) runs when the printer is open; a **light** one (status, progress, temperatures) runs otherwise.
4. When the file changes: `/server/files/metadata` and the thumbnail PNG.

The interval comes from Settings (1 s focused, 3 s background). A command or focus change wakes the poller early through a task notification.

## Updates

`updater.c` checks `api.github.com/repos/<repo>/releases/latest` a minute after boot and every 6 hours, on a short-lived task. It compares the tag with the running `PROJECT_VER`, turns the release body (above the `---` divider) into plain text, and records the URL of the `esp32-klipper-screen-app.bin` asset. Installing runs `esp_https_ota` into the inactive slot on an internal-stack task, because it writes flash. On its first boot the new image marks itself valid after a minute; if it crashes before that, the bootloader rolls back.

## Display & touch

The BSP drives the HX8394 panel through `esp_lvgl_adapter` in `TRIPLE_PARTIAL` mode. Rotation is done in the adapter's flush (on the PPA). The adapter doesn't rotate touch input, so `main.c` sets the GT911's swap/mirror flags to match each orientation.
