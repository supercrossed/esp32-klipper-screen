# Troubleshooting

## Wi-Fi

- **"Starting radio" stays on screen.** The P4 can't reach the ESP32-C6. This firmware expects Waveshare's factory C6 firmware. If you reflashed the C6, put the stock image back.
- **It takes about 15 s to connect after boot.** That's normal: the first association attempt often fails (reason 205) and the retry succeeds.
- **The network isn't listed.** The ESP32-C6 radio is **2.4 GHz only** (Wi-Fi 6), so 5 GHz-only networks won't appear. Hidden SSIDs aren't listed either; use **Hidden** on the Wi-Fi page.

## Printers

- **The search doesn't find a printer.** It finds Moonraker through mDNS (`_moonraker._tcp`, which needs Moonraker's `[zeroconf]` section) and by scanning the display's own /24 subnet on ports 7125–7127 and 80. Printers on another subnet or VLAN have to be added by IP.
- **The tile says "Klipper not ready".** Moonraker is up but Klipper is in startup, shutdown or error. Open the printer to see the message, and use **Restart** or **Firmware restart**.
- **HTTP 401/403.** Moonraker requires authorization. Add the display's IP to `trusted_clients`, or enter an API key in the printer's settings.

## Webcam

- **"Not a JPEG snapshot" / "Camera HTTP 404".** The webcam in Moonraker's list has no snapshot URL the display can use. Set one in Mainsail/Fluidd (for crowsnest: `/webcam/?action=snapshot`).
- **"JPEG decode failed (progressive?)".** The P4's hardware decoder needs baseline JPEG. ustreamer and mjpg-streamer produce baseline by default.
- **Low frame rate.** Lower the camera resolution in crowsnest; 720p is a good balance.

## Updates

- **"Couldn't reach GitHub".** The display needs internet access (HTTPS to `api.github.com` and `github.com`). Check that your network allows it.
- **"Download interrupted - nothing was changed".** The Wi-Fi dropped during the download. The old version is still installed; try again.
- **The display restarted on the old version after an update.** The new version didn't start properly, so it rolled back automatically. Please open an issue with a serial log.

## Display

- **Touch is offset or mirrored after changing orientation.** Report it with the orientation you used. The swap/mirror flags for each orientation are in `main/main.c`.
- **The colours look wrong after switching themes.** Settings → Display → Light mode rebuilds the screen. Reboot if anything looks stale.

## Serial console

Connect at **115200 baud** to the board's USB-UART, for example with `idf.py -p <PORT> monitor`, PuTTY, or `screen /dev/ttyACM0 115200`. Logs stream continuously, and you can type commands:

| Command | Does |
|---|---|
| `status` | Wi-Fi state, IP, RSSI, free internal/PSRAM memory, webcam fps |
| `wifi <ssid> [password]` | Join and save a network (no spaces in the SSID) |
| `add <host> [port] [name]` | Add a printer |
| `list` | Printers with status, heaters, MMU, webcams |
| `lanes <idx>` | MMU lanes and webcams for printer `idx` |
| `scan` | Run network discovery and print the results |
| `focus <idx>` | Poll printer `idx` fast (as if it were open) |
| `page <id> [arg]` | Navigate the UI: 0 home, 1 printer(arg=idx), 2 webcam(arg=idx·16), 3 add, 4 settings, 5 Wi-Fi, 6 Spoolman(arg=idx) |
| `upd check` / `upd install` / `upd status` | Check for updates, install the available one, show updater state |
| `shot` | Start the screenshot server (`http://<ip>:8080/screenshot.bmp`) |
| `tap <x> <y>` | Simulate a tap |
| `theme light` / `theme dark` | Switch the theme |
| `sound` / `sound error` | Play the finished / problem chime |
| `tasks` | List running tasks and their state (helps with freeze reports) |
| `bat` | Battery status, voltage and charge (`bat cap 2500` sets the size, `bat cut 0` turns the low-battery power-off off) |
| `reboot` | Restart |

When reporting a crash, include the lines after `Guru Meditation` or `assert failed`, and the output of `status`.
