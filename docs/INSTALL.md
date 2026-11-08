# Install & first boot

## What you need

- A **Waveshare ESP32-P4-WIFI6-Touch-LCD-5, -7, -8 or -10.1**. Use the board as shipped: the onboard ESP32-C6 keeps Waveshare's factory Wi-Fi firmware, and this project doesn't touch it.
- A USB-C data cable, plugged into the board's **USB-UART** port (the one that appears as a COM port or `/dev/ttyACM*`/`/dev/ttyUSB*`).
- One or more printers running **Klipper + Moonraker** on the same network.

## Option 1: web installer (easiest)

1. Open **https://supercrossed.github.io/esp32-klipper-screen/** in **Chrome or Edge** on a desktop. The installer needs Web Serial, which Firefox and Safari don't support.
2. Pick your screen size (5″, 7″, 8″ or 10.1″). Each size has its own firmware; the wrong one leaves the screen dark, and installing the right one fixes it.
3. Plug in the board and click **Install**. Choose the board's serial port.
4. Wait for *Installation complete*, then press the board's reset button or unplug and replug it.

The installer writes the bootloader, partition table and app, replacing whatever was on the P4, including Waveshare's demo firmware. Re-running it later to update keeps your settings unless you tick *Erase device*.

## Option 2: esptool

Download the file for your screen from the [latest release](https://github.com/supercrossed/esp32-klipper-screen/releases/latest): `esp32-klipper-screen-<size>-<version>.bin`, where `<size>` is `5in`, `7in`, `8in` or `10in`. Then, for example on a 5″:

```bash
pip install esptool
esptool --chip esp32p4 -p COM18 -b 921600 write_flash 0x0 esp32-klipper-screen-5in-<version>.bin
```

Replace `COM18` with your port, e.g. `/dev/ttyACM0` on Linux or `/dev/cu.usbmodem*` on macOS. If 921600 baud fails, use `-b 460800`.

The single `.bin` image is meant for **fresh installs**: it covers flash from `0x0`, so it also wipes saved settings.

Each release also includes the separate images. Use them to update without losing settings:

```bash
esptool --chip esp32p4 -p COM18 -b 921600 write_flash 0x2000 bootloader-5in.bin 0x8000 partition-table.bin 0x19000 ota_data_initial.bin 0x20000 esp32-klipper-screen-app.bin
```

| Screen | Bootloader | App |
|---|---|---|
| 5″ | `bootloader-5in.bin` | `esp32-klipper-screen-app.bin` |
| 7″ | `bootloader-7in.bin` | `esp32-klipper-screen-7in-app.bin` |
| 8″ | `bootloader-8in.bin` | `esp32-klipper-screen-8in-app.bin` |
| 10.1″ | `bootloader-10in.bin` | `esp32-klipper-screen-10in-app.bin` |

`partition-table.bin` and `ota_data_initial.bin` are the same for every size.

## Option 3: build it yourself

See [BUILDING.md](BUILDING.md).

## First boot

1. **Wi-Fi.** With no saved network, the screen opens on *Welcome - connect to Wi-Fi*. Tap your network, enter the password, and tap **Connect**. Use **Hidden** for an SSID that doesn't broadcast. The radio takes a few seconds to come up after power-on, and joining can take up to about 15 s.
2. **Add printers.** Tap **+ Add printer** on the home screen.
   - **Search network** takes about a minute. It finds Moonraker through mDNS and by scanning your /24 subnet on ports 7125, 7126, 7127 and 80. Tap **Add** next to a printer and give it a nickname.
   - Or fill in **Printer details**: host/IP (the field opens a numpad; tap `abc` for letters), port (`7125` direct, or `80` through Mainsail/Fluidd), and an API key if your Moonraker requires one.
3. **Done.** Tap a printer tile to open it, or long-press a tile to rename it.

Settings, printers and Wi-Fi are stored in flash and survive reboots and firmware updates. **Settings → Device → Factory reset** erases all of them.

## Updating

From **v0.2.0** on, the display updates itself:

- It checks GitHub for a new release a minute after starting, then every 6 hours. When there's one, a green **Update** button appears at the top of the screen.
- Tap it (or go to **Settings → Updates**) to read the release notes, then tap **Update now**. The download takes about a minute, then the display restarts on the new version. Your settings and printers are kept.
- If a new version fails to start properly, the display goes back to the previous version by itself on the next restart.

**Coming from v0.1.0?** It has no updater, so install v0.2.0 once with the web installer, leaving *Erase device* unticked.

You can still update by hand: run the web installer again with *Erase device* unticked, or flash the four separate images with esptool (above). Both leave the settings partition (NVS at `0x9000`) alone.
