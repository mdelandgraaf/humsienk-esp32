# Hardware & Build Setup

## Boards

- **LilyGO TTGO T-Display** (ESP32 + ST7789 240×135). V1.1 tested; V1.0 uses the same
  pins and Setup file.
- Any other ESP32 works if you remove or stub the `TFT_eSPI` display code — Wi-Fi, BLE,
  MQTT and REST don't depend on the screen.

No physical connection to the battery is needed; all communication is over BLE. Just power
the ESP32 from USB within Bluetooth range of the packs.

## Arduino IDE setup

1. Install the ESP32 board package: Boards Manager → search **esp32** (by Espressif).
2. Install libraries (Library Manager):
   - **NimBLE-Arduino** (h2zero) — v2.x. The firmware uses the v2 API
     (`NimBLEDevice::setMTU`, integer connect timeout). On v1.x it won't compile.
   - **ArduinoJson** (Benoit Blanchon) — v7+.
   - **PubSubClient** (Nick O'Leary) — MQTT.
   - **TFT_eSPI** (Bodmer) — display.

## TFT_eSPI configuration (display boards only)

`TFT_eSPI` needs to know which panel/pins to use. Edit
`Documents/Arduino/libraries/TFT_eSPI/User_Setup_Select.h`:

- Comment out the default `#include <User_Setup.h>`
- Uncomment `#include <User_Setups/Setup25_TTGO_T_Display.h>`

This is a **library-wide** setting (affects every sketch that uses TFT_eSPI). If you skip
it, the display shows garbage or stays blank, but the rest of the firmware still works.

## Board settings (Tools menu)

| Setting | Value |
|---|---|
| Board | **ESP32 Dev Module** |
| Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** |
| Upload Speed | 921600 (drop to 115200 if uploads fail) |

The **Huge APP** partition is **required** — the default partition is too small for
Wi-Fi + TLS + BLE + web server + MQTT and the build will overflow with
`text section exceeds available space`.

## Flashing tips

- If upload sticks at "Connecting…", hold the **GPIO 0 / BOOT** button while it connects,
  release once writing starts (some T-Display batches need this).
- USB-serial chip is CP210x or CH9102 depending on batch; install the matching driver if
  the port doesn't appear.

## Display orientation

`tft.setRotation(1)` is landscape with USB-C on the right. Use `3` to flip 180°. Portrait
(`0`/`2`) would need layout changes.

## Buttons (TTGO)

- **GPIO 0** (BOOT): pause auto-cycle / advance to next battery.
- **GPIO 35**: resume cycling, or toggle backlight.
