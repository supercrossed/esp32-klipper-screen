# Using it

The screen runs in landscape (1280×720) by default. The top bar shows a **Back** arrow, the page title, the Wi-Fi signal, the clock, and **Settings**.

## Home

Each printer has a tile. Every tile has the same layout:

1. **Name and status**: Idle, Printing, Paused, Complete, Error, Offline or Klipper not ready.
2. **Address**, with small icons when the printer has an MMU, Spoolman or a camera.
3. **Job**: the slicer thumbnail (or a printer icon), file name, progress bar, and time left with ETA.
4. **Temperatures**: hotend, bed and chamber in three columns, orange while heating and `--` when the printer doesn't have one.

**Tap** a tile to open the printer. **Long-press** it to set a nickname. The last tile is **+ Add printer**. When any printer has Spoolman, a **Spoolman** button appears below the tiles.

When a new version is available, a green **Update** button appears in the top bar (see [Updates](#updates)).

Home tiles refresh every 3 s by default, and the open printer every 1 s. Both can be changed in *Settings → Printers*.

## Printer page

A sidebar on the left switches between sections. Only the selected section is kept in memory.

### Status
- The job card shows the file, thumbnail, percentage and progress bar, followed by **Elapsed**, **Remaining**, **ETA** (clock time), **Layer**, **Filament** used and **Speed** factor.
- **Pause / Resume** and **Cancel** appear while printing. Both ask for confirmation.
- When Klipper is in an error or shutdown state, the reason is shown with **Restart** and **Firmware restart** buttons.
- **Temperatures**: tap a heater to set its target with a numpad and presets. Chamber heaters (`heater_generic`/`temperature_fan`) can be set too; a plain chamber `temperature_sensor` is read-only.
- **Quick controls**: Home all, Motors off, Heaters off, and **EMERGENCY STOP** (immediate, no confirmation).

### Move
- Jog step: 0.1 / 1 / 10 / 50 mm. XY pad with a centre **Home XY** button, and a Z column with **Home Z**.
- Position readout. Axes show orange until they're homed.
- Home all / X / Y / Z, **Gantry level** (`QUAD_GANTRY_LEVEL`) or **Z tilt** (`Z_TILT_ADJUST`) when the printer has them, and Motors off.
- Moves are blocked while printing.

### Extrude
- Hotend temperature with **PLA 210° / PETG 240° / ABS 255° / Off** presets.
- Length (1–50 mm) and speed (1–10 mm/s), then **Retract** or **Extrude**. Both are refused below 170 °C.
- **Load filament / Unload filament** buttons appear when the printer has `LOAD_FILAMENT` / `UNLOAD_FILAMENT` macros.

### MMU (AFC or Happy Hare only)
- The header shows the unit name, the current lane or tool, where the filament is, and the current action.
- The **filament path** lights up from the active lane through Hub/Selector → Bowden → Extruder → Nozzle, in the filament's colour.
- **Lanes** show colour, tool mapping, material, weight, and status. The lane in the toolhead has a green border.
- Tap a lane to load it (AFC `CHANGE_TOOL LANE=…`, Happy Hare `MMU_CHANGE_TOOL TOOL=…`) or to unload it. **Unload** and, on Happy Hare, **Home MMU** sit in the header. Lane changes are blocked while printing.

### Macros
Every `gcode_macro` whose name doesn't start with `_`, sorted. Tap one to run it; each asks for confirmation.

### Spools (Spoolman)
Shown when the printer's Moonraker has a `[spoolman]` section.
- **Spools / Filaments** tabs list colour, name, vendor, material, grams left and location.
- Tap a spool for its details and a large **QR code** (`web+spoolman:s-<id>`) that a 2D scanner can read off the screen.
- **Set active on <printer>** or **Clear active spool**. The active spool is marked with ✓.

### Camera
Opens the printer's first webcam (from Moonraker's webcam list) fullscreen. **Tap anywhere to close it.** The bottom strip shows the job status, resolution and frame rate.

## Settings

The top level is a grid of tiles:

| Tile | Contents |
|---|---|
| **Network** | Connection details (SSID, IP, gateway, signal, MAC) and **Choose a network** |
| **Printers** | Edit or remove printers, add one, refresh rates |
| **Display** | **Light mode** switch, brightness, screen rotation (a rotation change restarts the display) |
| **Time** | Clock and time zone. *Auto* detects it once from your public IP |
| **Updates** | Installed and latest version, **Check for updates**, **View update** |
| **Device** | Device name (network hostname), firmware and memory info, Restart, Factory reset |

## Updates

When a newer release is out, tap the green **Update** button, or go to *Settings → Updates → View update*. You'll see what's new and the download size. **Update now** downloads and installs it with a progress bar, then the display restarts. **Later** closes the dialog, and the button stays until you update.

## Keyboard

- Text fields use a full keyboard with a number row. `1#` switches to symbols and `ABC` to capitals.
- Host and port fields open an **IP numpad** with `.` and `:`. Tap `abc` for letters, and `123` to get back to the numpad.
