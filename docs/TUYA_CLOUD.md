# Tuya Cloud custom ESP32 device

This repository includes a Tuya Cloud publishing example at:

```text
firmware/examples/tuya_cloud/tuya_cloud.ino
```

It is meant for a **custom ESP32 IoT device** created in the Tuya IoT Platform. You do **not** need an existing Tuya plug, sensor, gateway, or Smart Life device.

The ESP32 itself becomes the cloud-connected custom device:

```text
Humsienk battery --BLE--> ESP32 --MQTT/TLS--> Tuya Cloud
```

## Tuya side

Create a custom product in Tuya IoT Platform and add a custom device under that product. You will need the credentials/details Tuya gives for that custom device, typically:

- MQTT host / endpoint
- MQTT port, usually TLS port `8883`
- Device ID or client ID
- Device secret / MQTT password / token
- Property report topic
- Product property identifiers / datapoint identifiers

Exact names differ between Tuya project types and regions, but the firmware only needs the final MQTT connection details and the topic that accepts property reports.

## What it sends

For every configured battery the example publishes these values:

| Value | Default property identifier suffix |
| --- | --- |
| State of charge | `soc` |
| Voltage | `voltage` |
| Current | `current` |
| Power | `power` |
| Temperature | `temperature` |
| State of health | `soh` |
| Remaining capacity | `remaining_capacity` |
| Cell voltage difference | `cell_diff` |
| Charge FET state | `charge_fet` |
| Discharge FET state | `discharge_fet` |

For multi-battery setups the example prefixes every property with the battery name, for example:

```text
battery1_soc
battery1_voltage
battery2_soc
battery2_voltage
```

Create matching custom product property identifiers in Tuya, or change the identifiers in the sketch.

## Configuration in the ESP32 sketch

Edit these fields in `tuya_cloud.ino`:

```cpp
static const char* WIFI_SSID = "YOUR_WIFI";
static const char* WIFI_PASS = "YOUR_PASSWORD";

static const char* TUYA_MQTT_HOST = "YOUR_TUYA_MQTT_HOST";
static const uint16_t TUYA_MQTT_PORT = 8883;
static const char* TUYA_MQTT_CLIENT_ID = "YOUR_TUYA_CLIENT_ID";
static const char* TUYA_MQTT_USER = "YOUR_TUYA_USERNAME_OR_DEVICE_ID";
static const char* TUYA_MQTT_PASS = "YOUR_TUYA_PASSWORD_OR_TOKEN";
static const char* TUYA_PUBLISH_TOPIC = "YOUR_TUYA_PROPERTY_REPORT_TOPIC";
```

Then set your batteries:

```cpp
static const BatteryCfg BATTERIES[] = {
    { "battery1", "XX:XX:XX:XX:XX:XX" },
};
```

## Payload shape

The sketch publishes JSON in this shape:

```json
{
  "msgId": "humsienk-battery1-1783330000",
  "time": 1783330000,
  "data": {
    "battery1_soc": { "value": 82, "time": 1783330000 },
    "battery1_voltage": { "value": 13.421, "time": 1783330000 },
    "battery1_current": { "value": -4.225, "time": 1783330000 }
  }
}
```

If your Tuya custom device expects another envelope, only change `publishToTuya()`; the BLE reader and snapshot fields can stay the same.

## TLS note

The example starts with:

```cpp
static const bool TUYA_MQTT_INSECURE = true;
```

That makes TLS setup easy while testing on ESP32. For production, install the Tuya/root CA certificate in the sketch and set this to `false`.

## Next step

After you confirm that Tuya accepts the payload and datapoint identifiers, the same `publishToTuya()` function can be moved into the main web-config firmware and the Tuya credentials can be added to the captive portal next to Home Assistant REST and MQTT.
