# Building from source

## Toolchain

**ESP-IDF v5.5.6.** The board's ESP32-P4 is chip revision 3.x, and IDF 5.5.0 rejects it (its maximum supported revision is 1.99). Any IDF ≥ 5.5.1 on the 5.5 line should work; CI builds with 5.5.6.

```bash
git clone -b v5.5.6 --recursive --shallow-submodules --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4      # Windows: install.ps1 esp32p4
. ./export.sh                           # Windows PowerShell: . .\export.ps1
```

It can sit next to an older IDF install. Each shell uses whichever `export` script you ran.

## Build & flash

```bash
git clone https://github.com/supercrossed/esp32-klipper-screen.git
cd esp32-klipper-screen
idf.py set-target esp32p4        # first time only
idf.py build
idf.py -p <PORT> flash monitor   # Ctrl+] quits the monitor
```

The first build downloads components through the IDF component manager. [`dependencies.lock`](../dependencies.lock) pins their versions.

To produce the single image used for releases:

```bash
idf.py merge-bin -o merged.bin   # -> build/merged.bin, flash at 0x0
```

## Flash layout

| Partition | Offset | Size | |
|---|---|---|---|
| nvs | `0x9000` | 64 KB | settings and printers (kept across updates) |
| otadata | `0x19000` | 8 KB | which app slot boots |
| phy_init | `0x1b000` | 4 KB | |
| ota_0 | `0x20000` | 6 MB | app slot A |
| ota_1 | `0x620000` | 6 MB | app slot B |
| storage | `0xc20000` | ~4 MB | spare |

Updates download into the inactive slot. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` makes the bootloader return to the previous slot if a new image doesn't confirm itself within a minute of starting (`updater.c`).

## Configuration notes

`sdkconfig` is generated and not committed; **edit `sdkconfig.defaults`**. Changing `sdkconfig.defaults` doesn't update an existing `sdkconfig`. Delete `sdkconfig` (or run `idf.py fullclean`) and rebuild, then grep `sdkconfig` to confirm the setting took effect.

Settings that matter, and why:

| Setting | Why |
|---|---|
| `ESP32P4_REV_MIN_300`, `ESP32P4_SELECTS_REV_LESS_V3=n` | rev 3.x silicon |
| `SPIRAM_*`, `CACHE_L2_*` | Waveshare's rev3 PSRAM profile |
| `LV_USE_CUSTOM_MALLOC` (+ `main/lv_mem_psram.c`) | LVGL's heap in PSRAM |
| `SPIRAM_MALLOC_ALWAYSINTERNAL=1024`, `SPIRAM_MALLOC_RESERVE_INTERNAL=65536` | keep internal RAM free for esp_hosted's SDIO buffers |
| `LWIP_MAX_SOCKETS=48`, `LWIP_MAX_ACTIVE_TCP=48`, `LWIP_SO_LINGER` | network discovery |
| `LV_USE_TINY_TTF`, `LV_USE_QRCODE`, `LV_USE_LODEPNG` | icons, Spoolman QR codes, print thumbnails |
| `esp_hosted 1.4.*`, `esp_wifi_remote 0.14.*` (in `main/idf_component.yml`) | match Waveshare's factory C6 firmware |
| `ESP_HTTP_CLIENT_ENABLE_HTTPS`, `MBEDTLS_EXTERNAL_MEM_ALLOC`, `MBEDTLS_DYNAMIC_BUFFER` | HTTPS to GitHub for updates, with TLS buffers in PSRAM |
| `BOOTLOADER_APP_ROLLBACK_ENABLE` | automatic rollback of a bad update |
| `LV_USE_SNAPSHOT` | screenshots (`tools/screenshot.py`) |

**Don't** raise `LWIP_TCP_WND_DEFAULT` / `TCP_SND_BUF` to 64 KB. Under webcam load, esp_hosted runs out of internal DMA memory and asserts (`transport_drv_sta_tx … copy_buff`).

## Icons

The UI text is [Inter](https://rsms.me/inter/) (OFL-1.1), pre-rendered into LVGL bitmap fonts by `python tools/gen_fonts.py` (needs Pillow) from the TTFs in `main/ui/fonts`; bitmap fonts draw much faster than rendering the TTF at run time. `FONT_S` to `FONT_XXL` are the regular sizes and `ui_font_bold(px)` gives the nearest generated Inter Bold size; add sizes in `SIZES` in the script. `ui_font_icon(px)` gives the icon font alone at any size. Icons are glyphs from the [Material Design Icons](https://pictogrammers.com/library/mdi/) webfont, the fallback of every text font; LVGL's built-in Montserrat comes last, for the `LV_SYMBOL_*` glyphs. To add an icon:

1. Find its name on pictogrammers.com, e.g. `printer-3d-nozzle`.
2. Add `"MY_ICON": "printer-3d-nozzle"` to `ICONS` in `tools/gen_icons.py`.
3. Run `python tools/gen_icons.py`. This regenerates `main/ui/icons.h`; the font itself (`main/ui/icons/mdi.ttf`) is already committed.
4. Use `ICON_MY_ICON` in any label string: `ICON_MY_ICON "  Text"`.

## Releasing

1. Bump `PROJECT_VER` in the top-level `CMakeLists.txt` and add a section to `CHANGELOG.md`. Write it in plain language: that section becomes the release notes, which the display shows in its update dialog.
2. Commit, then tag and push: `git tag v0.2.0 && git push origin v0.2.0`.
3. The **Build** workflow builds the firmware, creates the GitHub Release with the binaries attached, and redeploys the web installer on GitHub Pages.

## Screenshots

Run `shot` on the serial console, then `python tools/screenshot.py <display-ip> out.png`. The console's `page`, `tap` and `theme` commands let you set up each screen without touching it.

## Headless testing

The USB-UART console (115200 baud) takes commands such as `wifi`, `add`, `list`, `scan` and `page`, which make it possible to test without touching the screen. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md#serial-console).
