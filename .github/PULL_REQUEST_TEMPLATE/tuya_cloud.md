## Tuya Cloud custom ESP32 device test checklist

- [ ] Create a custom product in Tuya IoT Platform.
- [ ] Add matching property identifiers, for example `battery1_soc`, `battery1_voltage`, `battery1_current`, `battery1_power`, `battery1_temperature`, `battery1_soh`, `battery1_remaining_capacity`, `battery1_cell_diff`, `battery1_charge_fet`, `battery1_discharge_fet`.
- [ ] Add/register a custom device under that product.
- [ ] Copy MQTT host, port, client ID/device ID, credentials and property report topic into `firmware/examples/tuya_cloud/tuya_cloud.ino`.
- [ ] Set Wi-Fi credentials and Humsienk BLE MAC address.
- [ ] Flash to ESP32 and check serial monitor for `Tuya publish ok`.
- [ ] Confirm the Tuya device receives property updates.
