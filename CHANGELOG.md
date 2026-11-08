# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This project uses [semantic versioning](https://semver.org/).

## [0.4.7] - 2026-11-07

### Added
- **Desk case.** A 3D-printable desk case holds the screen at a 20° tilt, with a magnetic bezel, buttons for POWER, BOOT and RESET, and room for a battery. Every part prints without supports. The print files (`.3mf`) and the full assembly (`.step`) are attached to this release, and the [build guide](https://github.com/supercrossed/esp32-klipper-screen/blob/main/case/README.md) walks through printing and assembly.

The screen's firmware is unchanged in this release.

## [0.4.6] - 2026-11-07

### Added
- **3D bed mesh view.** The Mesh tab now shows the bed in 3D, like Mainsail's heightmap: a coloured surface on the bed with the probe points marked, and the height exaggerated so small differences are easy to see. Drag to turn and tilt it, tap a point to read its exact height, and use **Reset view** to go back. **Top** shows the flat grid with values. Both have a colour scale with the lowest and highest point.

## [0.4.5] - 2026-11-07

### Fixed
- Wi-Fi is reliable again. Printers connect, scanning for printers no longer restarts the screen, and the update check reaches GitHub again.
- The Console tab, the webcam and online updates no longer crash the screen. Wi-Fi data now goes through the board's large external memory, so a burst of data or many connections at once can't run the small internal memory out.

Note: 0.4.3 and 0.4.4 had connection problems and are marked as pre-releases. If your screen is on 0.4.3, its updater may not reach GitHub; install 0.4.5 over USB with the web installer (your settings are kept).

## [0.4.4] - 2026-11-07 (pre-release, replaced by 0.4.5)

### Fixed
- Wi-Fi is reliable again. 0.4.3's crash fix made the connection drop data, so updates, the time sync and some printer requests could fail. The crash fix now works a different way: incoming Wi-Fi data goes into the large external memory instead of the small internal one.
- This also fixes online updates from 0.4.0, which crashed as soon as the download started.

## [0.4.3] - 2026-11-07

### Fixed
- The screen no longer restarts when you open the Console tab or the webcam. A burst of incoming data could run the Wi-Fi link out of memory.
- Long names no longer run under buttons or off the edge in the Wi-Fi list, the found-printers list, Settings → Printers and Spoolman. They now end in "..." instead.
- If the screen ever stops responding, it now restarts by itself after 20 seconds instead of staying frozen.

## [0.4.2] - 2026-11-07

### Fixed
- Alert sounds and the test sound now play. The speaker setup was redone to match the board maker's own working example.

### Changed
- Alert and sound settings moved from Settings → Display to **Settings → Device**.
- Keyboard works like a phone: tap the arrow key for one capital letter, double-tap it for caps lock, tap again to turn it off.

## [0.4.1] - 2026-11-07

### Fixed
- Files: the file name no longer overlaps the date and size underneath it.
- History: same fix for the job list.

## [0.4.0] - 2026-11-07

### Added
- **Tune** (left menu): adjust the Z offset (babystep) during a print, plus speed, flow and part-fan sliders.
- **Files** (left menu): browse the printer's files, newest first, with the picture, print time and filament, and start a print.
- **Print alerts.** When any printer finishes, fails or is cancelled, a banner pops up with an *Open* button, and the display plays a chime. You can turn it off or change the volume in Settings → Display.
- **History** (right menu): recent jobs and lifetime totals.
- **Mesh** (right menu): a colour map of the bed mesh, saved profiles to load, and Calibrate / Clear.
- **Camera picker** when a printer has more than one camera.
- **Auto theme:** switches to light at sunrise and dark at sunset where you are. Choose Dark, Light or Auto in Settings → Display.
- **Accent colours:** Blue, Orange, Green, Purple, Pink or Teal, each tuned for both light and dark mode.

### Changed
- Automatic time zone now also remembers your rough location, used only for sunrise and sunset.

## [0.3.0] - 2026-11-07

### Added
- **Console.** See the printer's Klipper console and send commands, just like in Mainsail or Fluidd. Shortcut buttons for common commands.
- **System controls.** Restart Klipper or the printer's firmware, restart, stop or start services (Klipper, Moonraker, Crowsnest, KlipperScreen and so on), and reboot or shut down the printer's computer. Every risky action asks first.
- A **second menu on the right** of each printer page, for Camera, Spools, Console and System. More features will appear here.

### Changed
- Emergency stop and quick controls are now at the top of the Status page, so they're always visible.
- Moving and extruding no longer leave the printer in relative mode afterwards.
- Fewer pop-up messages: moving, extruding and setting temperatures only show a message if something goes wrong. Messages now say which printer they're about.
- If an update fails, you can try again straight away.

### Fixed
- Long macro names could be cut off and fail to run.
- Status page text could overlap on smaller layouts.
- Console messages with special characters showed empty boxes.
- An empty temperature entry no longer turns the heater off.
- Opening the camera right after closing it could fail.
- Status labels are readable on every status colour.

## [0.2.0] - 2026-11-07

### Added
- **Built-in updates.** The display checks for new versions by itself. When one is out, a green **Update** button appears at the top of the screen. Tap it to read what's new and install. You can also check by hand in Settings → Updates.
- If an update ever fails to start properly, the display goes back to the previous version on its own.
- Updates tile in Settings with the installed and latest version.

### Changed
- Printer tiles on the home screen have a cleaner, consistent layout: same spacing on every tile, temperatures in three even columns, and small matching icons for MMU, Spoolman and camera.
- Chamber heaters and chamber sensors now have different names.
- Tidier filament-path graphic on the MMU page.

### Upgrading from 0.1.0
Version 0.1.0 can't update itself. Install 0.2.0 once with the [web installer](https://supercrossed.github.io/esp32-klipper-screen/) (leave *Erase device* unticked to keep your printers and Wi-Fi). Every later version installs from the screen.

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

[0.4.7]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.7
[0.4.6]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.6
[0.4.5]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.5
[0.4.4]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.4
[0.4.3]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.3
[0.4.2]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.2
[0.4.1]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.1
[0.4.0]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.4.0
[0.3.0]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.3.0
[0.2.0]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.2.0
[0.1.0]: https://github.com/supercrossed/esp32-klipper-screen/releases/tag/v0.1.0
