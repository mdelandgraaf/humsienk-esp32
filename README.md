# Humsienk BMS Monitor (ESP32)

Read your **Humsienk LiFePO4 batteries** over Bluetooth with an ESP32, show them on a
LilyGO TTGO T-Display, and push the data to **Home Assistant** (REST or MQTT). Includes
full charge/discharge MOSFET control and decoded protection/warning flags — everything
the official "HumsiENK Smart BMS" app does, but local, open, and integrable.

The Humsienk "BMC" BLE protocol was reverse-engineered from the official Android app;
the full protocol is documented in [`docs/PROTOCOL.md`](docs/PROTOCOL.md) so anyone can
build their own client.

> **Not affiliated with Humsienk.** This is an independent, community project. Names and
> trademarks belong to their respective owners. Use at your own risk — see
> [Safety](#safety).

![status: working](https://img.shields.io/badge/status-working-brightgreen)
![platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue)
![license: MIT](https://img.shields.io/badge/license-MIT-lightgrey)
[![Buy Me A Coffee](https://img.shields.io/badge/buy%20me%20a%20coffee-donate-yellow)](https://www.buymeacoffee.com/mdelandgraaf)

---

## Features

- **BLE polling** of one or more Humsienk batteries (the ones that advertise as `HS...`).
- **Live values:** pack voltage, current (signed), SOC, SOH, remaining/full capacity,
  cycle count, temperatures, per-cell min/max/delta.
- **Charge & discharge FET control** — toggle the battery MOSFETs like the app does.
- **All 32 BMS status flags decoded** — protections and warnings exposed as binary
  sensors (over/under voltage, over/under temperature, overcurrent, short-circuit,
  AFE error, MOS over-temp, etc.).
- **Home Assistant integration two ways:**
  - **MQTT** with auto-discovery (sensors, switches, and binary sensors appear
    automatically, no YAML).
  - **REST API** (works great with a Nabu Casa URL + long-lived token).
- **On-device display** (TTGO T-Display): cycles through batteries, colour-coded SOC,
  current direction, temperature, and a fault line.
- **Web configuration portal** — set Wi-Fi, HA, MQTT, and batteries from a browser.
  No recompiling to change settings, no secrets baked into the firmware.
- **Captive-portal AP mode** on first boot or when Wi-Fi is unavailable.
- **BLE scan-and-add** — the web UI finds nearby `HS...` batteries and adds them with a tap.

---

## Hardware

| Item | Notes |
|------|-------|
| **LilyGO TTGO T-Display (ESP32, ST7789 240×135)** | V1.1 tested. Any ESP32 works if you drop the display code. |
| **Humsienk LiFePO4 battery with Bluetooth** | Must advertise a name beginning with `HS`. See [Compatibility](#compatibility). |
| USB-C cable + 5V supply | The ESP32 stays near the batteries (BLE range). |

No wiring to the battery is required — communication is wireless over BLE. Power the
ESP32 from any USB source within Bluetooth range of the packs.

---

## Quick start

1. **Install the Arduino IDE** and the **ESP32 board package** (Boards Manager → "esp32" by Espressif).
2. **Install libraries** (Library Manager):
   - `NimBLE-Arduino` (h2zero) — v2.x
   - `ArduinoJson` (Benoit Blanchon) — v7+
   - `PubSubClient` (Nick O'Leary) — for MQTT
   - `TFT_eSPI` (Bodmer) — for the display
3. **Configure TFT_eSPI** for the T-Display: in
   `Documents/Arduino/libraries/TFT_eSPI/User_Setup_Select.h`, comment out the default
   `User_Setup.h` include and uncomment:
   ```cpp
   #include <User_Setups/Setup25_TTGO_T_Display.h>
   ```
4. **Open** [`firmware/humsienk_bms_webconfig/humsienk_bms_webconfig.ino`](firmware/humsienk_bms_webconfig).
5. **Board settings:**
   - Board: **ESP32 Dev Module**
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)** ← required, the build is large
6. **Flash.** On first boot the device has no config and starts an access point.

### First-time setup (captive portal)

1. Connect a phone/laptop to the Wi-Fi network **`Humsienk-Setup`** (password `batterymon`).
2. A configuration page should pop up automatically (or browse to `http://192.168.4.1`).
3. Enter your Wi-Fi credentials, your Home Assistant URL + token and/or MQTT broker.
4. Hit **Scan now** to find nearby `HS...` batteries, tick them, **Add selected**.
5. **Save & Reboot.** The device connects to your network; the config page stays
   reachable at the device's IP (shown on the display and serial monitor).

---

## Home Assistant

### Option A — MQTT (recommended for local/fast updates)

Enable MQTT in the web config and point it at your broker (e.g. the Mosquitto add-on).
With MQTT enabled, the firmware publishes Home Assistant **auto-discovery** messages, so
a device called e.g. *battery1* appears automatically with:

- Sensors: SOC, voltage, current, power, temperature, remaining capacity, SOH, cell-diff.
- Switches: **Charge FET**, **Discharge FET**.
- Binary sensors: every protection/warning flag, with `device_class: problem` so they go
  red when active.

### Option B — REST API (works with Nabu Casa)

Enable "REST publishing" and provide your HA URL (e.g. `https://xxxx.ui.nabu.casa`) and a
**Long-Lived Access Token** (HA → profile → Security → Long-Lived Access Tokens). Entities
are created on first POST. Note: REST-created entities have no unique ID, so HA can't manage
them from the UI — this is normal and harmless. MQTT is the cleaner path if you have a broker.

---

## Safety

This firmware can switch the battery's charge and discharge MOSFETs.

- **Disabling the discharge FET cuts power to whatever the battery feeds** (e.g. an inverter).
- **Disabling the charge FET stops charging.**
- The web UI guards these with confirmation dialogs, and MQTT switches are explicit, but
  there is no undo. Don't expose the switches somewhere they can be hit by accident if
  something critical runs off the pack.

LiFePO4 packs store a lot of energy. This project talks to a safety-critical device that
you did not get full documentation for. **Use at your own risk.** The protection/warning
flags are decoded best-effort from the app and are provided for monitoring convenience,
not as a substitute for the BMS's own safety functions or for proper system design.

---

## Compatibility

Confirmed working with Humsienk LiFePO4 packs whose BLE name begins with **`HS`** and which
expose the GATT service `00000001-0000-1000-8000-00805f9b34fb` (the "BMC" protocol). The
official app also supports a second protocol family ("WATT", PACE-derived, service `0xFFF0`)
for differently-branded packs — that one is **not** implemented here.

To check your battery: scan with **nRF Connect**. If the name starts with `HS` and you see
a service `0x0001` with a write characteristic `0x0002` and a notify characteristic `0x0003`,
you're compatible. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for details.

If your pack is similar but slightly different, please
[open an issue](../../issues) with the nRF Connect service/characteristic dump and a sample
of the `0x21` response — it may be an easy addition.

---

## Repository layout

```
firmware/
  humsienk_bms_webconfig/   Main firmware: web config, MQTT, REST, display, FET control
  examples/
    minimal_mqtt/           Stripped-down: read + MQTT, no display/web (good starting point)
    rest_only/              Read + REST to HA, hardcoded config (simplest)
docs/
  PROTOCOL.md               Full reverse-engineered BMC protocol spec
  WIRING.md                 Board setup, TFT_eSPI config, partition notes
  HOME_ASSISTANT.md         Dashboard + automation examples
LICENSE
CONTRIBUTING.md
```

---

## How it was built

The protocol wasn't published anywhere, so it was reverse-engineered from the official
Android app: the APK was decompiled, the BLE command/response handlers were read out of the
Kotlin bytecode, and the field layouts + the 32-bit status bit map were verified against a
real battery. The whole thing is written up in [`docs/PROTOCOL.md`](docs/PROTOCOL.md) so the
work is reusable even if you don't care about this particular firmware.

---

## Contributing

Issues and PRs welcome — especially compatibility reports for other `HS` pack variants,
additional decoded fields, and ports to other ESP32 displays. See
[`CONTRIBUTING.md`](CONTRIBUTING.md).

---

## Support

If this saved you some time or got your batteries into Home Assistant, you can support the
work here:

<a href="https://www.buymeacoffee.com/mdelandgraaf" target="_blank">☕ Buy me a coffee</a>

It's entirely optional and always appreciated — but the project stays free and open either way.

---

## License

[MIT](LICENSE). This is independent interoperability work; it ships no Humsienk code or
assets and is not endorsed by or affiliated with Humsienk.
