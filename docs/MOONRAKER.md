# Moonraker setup

The display is a regular Moonraker HTTP client. Most installs work with **no changes**; this page covers the optional pieces.

## Connection

| Setting | Use |
|---|---|
| Port **7125** | Moonraker directly (preferred) |
| Port **80** | Through the Mainsail/Fluidd nginx proxy, if 7125 isn't reachable |
| Ports 7126/7127 | Additional Moonraker instances on one host (multi-printer Pis) |

**Authorization.** Most installs list the LAN under `trusted_clients` in `[authorization]`, and then no key is needed. Otherwise create an API key (Mainsail: *Settings → General → API key*, or `GET /access/api_key`) and enter it in the printer's **API key** field. CORS settings don't matter, because the display isn't a browser.

## Webcams

The display reads `GET /server/webcams/list`, the same list Mainsail and Fluidd use. It plays the camera's **MJPEG stream URL** (such as `/webcam/?action=stream`). If the camera has no stream, or the stream doesn't work, it falls back to fetching the **snapshot URL** over and over. A missing URL is filled in from the other one by swapping `action=stream` ↔ `action=snapshot` or `/stream` ↔ `/snapshot`. Relative URLs are resolved against port 80 on the printer's host.

Works with crowsnest using ustreamer, mjpg-streamer or camera-streamer. Not supported: WebRTC- or HLS-only cameras without an MJPEG or snapshot endpoint, HTTPS camera URLs (rewritten to HTTP), and progressive JPEGs (the P4's hardware decoder only handles baseline).

Frame rate is limited by Wi-Fi: the screen receives roughly 1.2–2 MB/s. A 1080p camera sends about 370 KB per frame, so it plays at about 3–6 fps. At 720p a frame is around a third of that, so it's roughly three times smoother. For the smoothest picture set `resolution: 1280x720` in `crowsnest.conf` (the screen is 1280×720 anyway), and lower the JPEG quality if your streamer has an option for it.

## Spoolman

Requires Moonraker's [`[spoolman]`](https://moonraker.readthedocs.io/en/latest/configuration/#spoolman) section:

```ini
[spoolman]
server: http://192.168.1.50:7912
```

The display uses `GET /server/spoolman/status`, `POST /server/spoolman/spool_id` (to set the active spool) and `POST /server/spoolman/proxy` for everything else, so it never talks to Spoolman directly:

- It reads `/v1/spool` (archived ones too), `/v1/filament`, `/v1/vendor`, `/v1/setting/` (currency, base URL, extra fields, locations), `/v1/location`, `/v1/material` and `/v1/external/material`.
- The forms use `POST`, `PATCH` and `DELETE` on `/v1/spool`, `/v1/filament` and `/v1/vendor`. **Weigh** uses `PUT /v1/spool/<id>/measure`.
- `/v1/external/filament` (about 3.5 MB) is downloaded only when you search the filament database, and freed when the form closes.

QR codes: a spool's encodes `web+spoolman:s-<id>`, the payload of Spoolman's printed labels and what scanner setups expect, or, if set in Settings → Label printer, `<spoolman>/spool/show/<id>` for phones. A filament's is `<spoolman>/filament/show/<id>`. `<spoolman>` is Spoolman's *Base URL* setting if set, otherwise the `server` from Moonraker's `[spoolman]` section (with `localhost` replaced by the printer's address).

If several printers share one Spoolman, open it from any of them: changes show up on all.

## Multi-material

| System | Detected by | Read from |
|---|---|---|
| **AFC** (Box Turtle etc.) | an `AFC` object | `AFC`, plus each `AFC_stepper <lane>` / `AFC_lane <lane>`: `load`, `prep`, `tool_loaded`, `loaded_to_hub`, `material`, `color`, `weight`, `spool_id`, `map`, `status` |
| **Happy Hare** | an `mmu` object | `mmu`: `gate`, `tool`, `filament`, `filament_pos`, `action`, `gate_status`, `gate_material`, `gate_color`, `gate_filament_name`, `ttg_map` |

Commands sent: AFC `CHANGE_TOOL LANE=<lane>`, `TOOL_UNLOAD`; Happy Hare `MMU_CHANGE_TOOL TOOL=<n>`, `MMU_SELECT GATE=<n>`, `MMU_UNLOAD`, `MMU_HOME`.

## Other things it uses

- **Status:** `print_stats`, `virtual_sdcard`, `display_status`, `webhooks`, `toolhead`, `gcode_move`, `idle_timeout`, `fan`, every `extruder*`, `heater_bed`, and any `temperature_sensor` / `heater_generic` / `temperature_fan` whose name contains `chamber`.
- **Thumbnails** come from `/server/files/metadata`; the largest thumbnail up to 400 px wide is used. The same metadata gives `gcode_start_byte`/`gcode_end_byte` and `filament_total`.
- **Progress and time left** work like Mainsail's defaults. Progress is `virtual_sdcard.file_position` within the G-code byte range, so the thumbnails and slicer settings stored in the file don't count. Time left is the average of the file-progress estimate (`print_duration / progress - print_duration`) and the filament estimate (the same, using `filament_used / filament_total`). Below 5% progress, the slicer's `estimated_time` is used instead, because the other two swing wildly that early.
- **Macros:** every `gcode_macro` not starting with `_`. Also `LOAD_FILAMENT` / `UNLOAD_FILAMENT` on the Extrude tab, `QUAD_GANTRY_LEVEL` / `Z_TILT_ADJUST` on the Move tab, and `TURN_OFF_HEATERS`.
- **Remaining time** is estimated from file progress (`print_duration / progress − print_duration`), the same as Mainsail's default.

## Console and system controls

- **Console:** `GET /server/gcode_store?count=150` every second while the Console tab is open. Commands are sent with `POST /printer/gcode/script`.
- **System:** `GET /machine/system_info` every 5 s while the tab is open, which supplies `available_services` and `service_state`. Actions go to `POST /machine/services/{restart|stop|start}?service=…`, `POST /machine/reboot` and `POST /machine/shutdown`. Moonraker only lists services allowed by its `moonraker.asvc` file.

## Tune, files, history and mesh

- **Tune:** `SET_GCODE_OFFSET Z_ADJUST=… [MOVE=1]`, `M220`, `M221`, `M106`/`M107`. The current values come from `gcode_move` (`homing_origin`, `speed_factor`, `extrude_factor`) and `fan`.
- **Files:** `GET /server/files/list?root=gcodes`, `GET /server/files/metadata`, the thumbnail from `/server/files/gcodes/…`, and `POST /printer/print/start?filename=…`.
- **History:** `GET /server/history/list?limit=50&order=desc` and `/server/history/totals` (Moonraker's `[history]`, on by default).
- **Mesh:** `bed_mesh` (`probed_matrix`, `mesh_min`, `mesh_max`, `profile_name`, `profiles`), plus `BED_MESH_PROFILE LOAD=`, `BED_MESH_CALIBRATE` and `BED_MESH_CLEAR`.

## Load on the printer

The printer you're viewing is polled every 1 s with one `objects/query`. Every other printer gets a smaller query every 3 s. Both rates are adjustable in *Settings → Printers*. Offline printers back off to 8 s.
