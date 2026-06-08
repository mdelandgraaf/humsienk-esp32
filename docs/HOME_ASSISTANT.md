# Home Assistant Integration

Two independent paths; you can run either or both. Toggle each in the device's web config.

## MQTT (recommended)

With MQTT enabled and pointed at your broker (e.g. the Mosquitto add-on), the firmware
publishes **auto-discovery** messages. A device per battery appears under
**Settings → Devices & Services → MQTT** with no manual YAML.

Entities created per battery (`<name>` is what you set in the web UI, e.g. `battery1`):

- **Sensors:** SOC (%), voltage (V), current (A), power (W), temperature (°C),
  remaining capacity (Ah), SOH (%), cell voltage diff (mV).
- **Switches:** Charge FET, Discharge FET.
- **Binary sensors:** every protection and warning flag, with `device_class: problem`
  (they show red and read "Problem/OK").

State topic: `humsienk/<name>/state` (JSON). Command topics:
`humsienk/<name>/charge/set` and `humsienk/<name>/discharge/set` (payload `ON`/`OFF`).

## REST API (e.g. via Nabu Casa)

Enable "REST publishing" and set the HA URL plus a Long-Lived Access Token
(HA → profile → Security → Long-Lived Access Tokens). Entities are created on first POST.

> REST-created entities have no unique ID, so HA can't manage them from the UI. That's
> expected and harmless. If you want full device grouping + switches, prefer MQTT.

---

## Example: low-SOC notification

```yaml
automation:
  - alias: "Battery 1 low SOC"
    trigger:
      - platform: numeric_state
        entity_id: sensor.battery1_soc
        below: 20
    action:
      - service: notify.mobile_app_your_phone
        data:
          title: "Battery 1 low"
          message: "SOC is {{ states('sensor.battery1_soc') }}%"
```

## Example: alert on any protection firing (MQTT binary sensors)

```yaml
automation:
  - alias: "Battery protection active"
    trigger:
      - platform: state
        entity_id:
          - binary_sensor.battery1_cell_ov_prot
          - binary_sensor.battery1_cell_uv_prot
          - binary_sensor.battery1_short_circuit
          - binary_sensor.battery1_chg_ot_prot
          - binary_sensor.battery1_dis_ot_prot
        to: "on"
    action:
      - service: notify.mobile_app_your_phone
        data:
          title: "⚠ BMS protection active"
          message: "{{ trigger.to_state.attributes.friendly_name }} triggered"
```

## Example: simple dashboard card

```yaml
type: vertical-stack
cards:
  - type: gauge
    entity: sensor.battery1_soc
    name: Battery 1
    min: 0
    max: 100
    severity: { green: 50, yellow: 20, red: 0 }
  - type: entities
    entities:
      - sensor.battery1_voltage
      - sensor.battery1_current
      - sensor.battery1_power
      - sensor.battery1_temperature
      - sensor.battery1_remaining_capacity
      - switch.battery1_charge_fet
      - switch.battery1_discharge_fet
```

> **Safety:** putting the FET switches on a dashboard is convenient but means a tap can cut
> power to your loads. Consider leaving them off shared/quick-access dashboards.
