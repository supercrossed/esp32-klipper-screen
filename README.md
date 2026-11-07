# ESP32 Klipper Screen

A portable touchscreen controller for **several Klipper printers at once**, running on a
[Waveshare ESP32-P4-WIFI6-Touch-LCD-5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm).
It works like a small, wireless [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen): pick up the screen, walk around the shop, and check or control any printer on your network.

Everything comes from each printer's **Moonraker** API. Nothing needs to be installed on the printers.

[![Build](https://github.com/supercrossed/esp32-klipper-screen/actions/workflows/build.yml/badge.svg)](https://github.com/supercrossed/esp32-klipper-screen/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/supercrossed/esp32-klipper-screen)](https://github.com/supercrossed/esp32-klipper-screen/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**[Install from your browser](https://supercrossed.github.io/esp32-klipper-screen/)** · [Releases](https://github.com/supercrossed/esp32-klipper-screen/releases) · [Docs](docs/)

---

## Features

- **Several printers**: the home screen has a tile for each printer, showing its nickname, status, progress, time left and ETA, temperatures, and the print thumbnail. Long-press a tile to rename it.
- **Per-printer pages**, chosen from a sidebar:
  - **Status**: job progress with elapsed, remaining and ETA, layer, filament, and speed. Tap a temperature to set it. Pause, resume, cancel, home, motors off, heaters off, and **emergency stop**.
  - **Move**: XY/Z jog (0.1 to 50 mm), home all or one axis, quad gantry level or Z-tilt when the printer has it.
  - **Extrude**: preheat presets, extrude/retract with length and speed, and load/unload filament macros.
  - **MMU**: [AFC](https://github.com/ArmoredTurtle/AFC-Klipper-Add-On) and [Happy Hare](https://github.com/moggieuk/Happy-Hare) lanes with colour, material, weight, and what's loaded. A filament-path graphic shows where the filament is: lane, hub/selector, bowden, extruder, nozzle.
  - **Macros**: your `gcode_macro`s as buttons.
  - **Spools**: a [Spoolman](https://github.com/Donkie/Spoolman) browser. View spools and filaments, show a spool's **QR code** for a 2D scanner, and set the active spool.
  - **Camera**: a fullscreen webcam view, decoded by the P4's hardware JPEG engine.
- **Add printers** by searching the network (mDNS plus a subnet scan on ports 7125–7127 and 80) or by typing an IP.
- **Setup on the device**: Wi-Fi setup on first boot, an on-screen keyboard and IP numpad, light/dark mode, brightness, four screen orientations, NTP time with automatic time zone.
- **Designed to run all day**: no screen sleep, no Wi-Fi power save. Pages you leave free their memory, and only the printer on screen is polled quickly.

## Hardware

| | |
|---|---|
| Board | Waveshare **ESP32-P4-WIFI6-Touch-LCD-5** (ESP32-P4 rev ≥ 3.0, 32 MB PSRAM, 32 MB flash) |
| Display | 5″ 720×1280 MIPI-DSI (HX8394), GT911 capacitive touch, used in landscape by default |
| Wi-Fi | Onboard ESP32-C6 (Wi-Fi 6) over SDIO, using **Waveshare's factory C6 firmware unchanged** |
| Printers | Klipper + Moonraker. Mainsail or Fluidd are optional |

## Install

The quickest way is the **[web installer](https://supercrossed.github.io/esp32-klipper-screen/)**. Plug the board into USB, open the page in Chrome or Edge, and click *Install*.

You can also flash a release binary with esptool:

```bash
pip install esptool
esptool --chip esp32p4 -p <PORT> -b 921600 write_flash 0x0 esp32-klipper-screen-<version>.bin
```

See **[docs/INSTALL.md](docs/INSTALL.md)** for details and the first-boot walkthrough.

## Documentation

| | |
|---|---|
| [Install & first boot](docs/INSTALL.md) | Flashing, Wi-Fi setup, adding printers |
| [Using it](docs/USAGE.md) | A tour of every screen |
| [Moonraker setup](docs/MOONRAKER.md) | Webcams, Spoolman, API keys, MMU, what the display requests |
| [Building from source](docs/BUILDING.md) | ESP-IDF 5.5.6 toolchain, build and flash, regenerating icons |
| [Architecture](docs/ARCHITECTURE.md) | Tasks, memory rules, how polling works |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Wi-Fi, discovery, webcam, serial console |
| [Changelog](CHANGELOG.md) | Release history |

## Building

```bash
# ESP-IDF v5.5.6 (v5.5.0 cannot build for ESP32-P4 rev 3.x)
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

Full instructions are in [docs/BUILDING.md](docs/BUILDING.md).

## Licence

The project is MIT licensed, see [LICENSE](LICENSE). Icons are [Material Design Icons](https://pictogrammers.com/library/mdi/) (Apache-2.0). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for all bundled components and the network services the firmware uses.

This is an independent project. It isn't affiliated with Klipper, KlipperScreen, Moonraker, Mainsail, Fluidd, Spoolman, AFC, Happy Hare or Waveshare.
