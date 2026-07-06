# Quickstart: ESP32 as a Tuya custom IoT device

This is the intended setup when you do **not** have an existing Tuya device.

## 1. Create the product in Tuya

In Tuya IoT Platform:

1. Create a custom product.
2. Add custom properties for the battery values.
3. Add a custom device under that product.
4. Copy the MQTT connection details for that custom device.

Suggested property identifiers for one battery:

```text
battery1_soc
battery1_voltage
battery1_current
battery1_power
battery1_temperature
battery1_soh
battery1_remaining_capacity
battery1_cell_diff
battery1_charge_fet
battery1_discharge_fet
```

For a second battery, use `battery2_...`, etc.

## 2. Configure the ESP32 sketch

Open:

```text
firmware/examples/tuya_cloud/tuya_cloud.ino
```

Fill in:

```cpp
WIFI_SSID
WIFI_PASS
TUYA_MQTT_HOST
TUYA_MQTT_PORT
TUYA_MQTT_CLIENT_ID
TUYA_MQTT_USER
TUYA_MQTT_PASS
TUYA_PUBLISH_TOPIC
```

Then set the Humsienk BLE MAC address:

```cpp
static const BatteryCfg BATTERIES[] = {
    { "battery1", "XX:XX:XX:XX:XX:XX" },
};
```

## 3. Flash and test

Open the serial monitor at `115200` baud. A successful run should show:

```text
WiFi connecting... got 192.168.x.x
Tuya MQTT connecting...ok
[battery1] V=13.42 I=-4.22 SOC=82%
[battery1] Tuya publish ok
```

If MQTT connects but Tuya does not show data, the usual causes are:

- property identifiers in Tuya do not match the JSON keys;
- the publish topic is not the property-report topic for the custom device;
- the custom device expects a different JSON envelope;
- TLS certificate validation needs the correct Tuya/root CA.

## 4. Move into the main firmware

After this example works, move the Tuya config fields and `publishToTuya()` into:

```text
firmware/humsienk_bms_webconfig/humsienk_bms_webconfig.ino
```

Then add Tuya fields to the captive portal next to the existing Home Assistant REST and MQTT settings.
