# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This project uses [semantic versioning](https://semver.org/).

## [0.1.0] - 2026-11-07

First public release.

### Added
- Multi-printer home screen. Tiles show nickname (long-press to rename), status, progress, ETA, temperatures, thumbnail, and MMU/Spoolman/webcam badges.
- Printer page with a sidebar:
  - **Status**: job, temperatures with tap-to-set, pause/resume/cancel, home, motors off, heaters off, emergency stop.
  - **Move**: XY/Z jog, homing, QGL or Z-tilt.
  - **Extrude**: preheat presets, extrude/retract with length and speed, load/unload filament macros.
  - **MMU**: AFC and Happy Hare lanes, plus a filament-path graphic.
  - **Macros**.
  - **Spools**: Spoolman spools and filaments, a QR code per spool, set or clear the active spool.
  - **Camera**: fullscreen webcam on the hardware JPEG decoder and PPA.
- Printer discovery (mDNS plus subnet scan) and manual add with an IP numpad.
- First-boot Wi-Fi setup through the onboard ESP32-C6, NTP time with automatic time zone.
- Settings grid: Network, Printers, Display (light/dark mode, brightness, rotation), Time, Device.
- Material Design Icons throughout.
- USB serial console for headless setup and debugging.
- Web installer (GitHub Pages) and CI release builds.

[0.1.0]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.1.0
