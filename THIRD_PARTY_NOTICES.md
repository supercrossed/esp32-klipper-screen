# Third-party notices

This project's own source code is MIT-licensed (see [LICENSE](LICENSE)). Released firmware also contains the following components, each under its own licence.

| Component | Used for | Licence | In this repository |
|---|---|---|---|
| [Material Design Icons](https://pictogrammers.com/library/mdi/) 7.4.47 (Pictogrammers) | UI icons (`main/ui/icons/mdi.ttf`) | Apache-2.0 | Yes: the font file |
| [Inter](https://rsms.me/inter/) 4.1 (The Inter Project Authors) | UI text font (`main/ui/fonts/inter_regular.ttf`, `inter_bold.ttf`) | SIL Open Font License 1.1 (`main/ui/fonts/OFL.txt`) | Yes: the font files |
| [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.6 | SDK, drivers, FreeRTOS, lwIP, cJSON, esp_http_client | Apache-2.0 (and the licences of its bundled parts) | No: build-time dependency |
| [LVGL](https://github.com/lvgl/lvgl) 9.5.0 | Graphics library, Tiny TTF, QR code, Montserrat (for a few symbols) | MIT | No: downloaded by the IDF component manager |
| [esp_lvgl_adapter](https://components.espressif.com/components/espressif/esp_lvgl_adapter) | LVGL display/touch glue | Apache-2.0 | No: downloaded |
| [esp_hosted](https://github.com/espressif/esp-hosted-mcu) / [esp_wifi_remote](https://components.espressif.com/components/espressif/esp_wifi_remote) | Wi-Fi through the onboard ESP32-C6 | Apache-2.0 | No: downloaded |
| [mdns](https://components.espressif.com/components/espressif/mdns) | Printer discovery | Apache-2.0 | No: downloaded |
| [Waveshare ESP32-P4-WIFI6-Touch-LCD-5 BSP](https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_5) and HX8394 driver | Board support (display, touch, backlight) | Apache-2.0 | No: downloaded |
| GT911 touch driver, esp_lcd_touch | Touch controller | Apache-2.0 | No: downloaded |

The full set of downloaded components and their exact versions is listed in [`dependencies.lock`](dependencies.lock).

The firmware also talks to these services at runtime:
- **ip-api.com**: used once after connecting, to look up the time zone (while it's set to *Auto*) and a rough location (for the sunrise/sunset theme) from your public IP address. Once a location is stored and a time zone is chosen by hand, it isn't contacted again.
- **pool.ntp.org / time.google.com**: clock sync (NTP).
