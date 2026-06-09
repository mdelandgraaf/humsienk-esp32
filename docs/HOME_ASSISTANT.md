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

## Detecting an offline battery (`last_seen`)

The firmware only publishes a battery's data after a **successful** BLE read — it never
pushes 0/blank values when a battery is unreachable. The downside is that an offline
battery's metrics just freeze at their last value rather than going `unavailable`.

To make drop-offs visible, each successful read also updates a
`sensor.<name>_last_seen` (`device_class: timestamp`). It stops advancing when the
battery goes offline, so HA shows "x minutes ago" and you can alert on staleness.
The device syncs time over NTP on boot, so the first reading may take a few seconds
to appear after a cold start.

```yaml
automation:
  - alias: "Battery 1 offline"
    trigger:
      - platform: template
        # fires once last_seen is older than 10 minutes
        value_template: >
          {{ now() - states.sensor.battery1_last_seen.state | as_datetime
             > timedelta(minutes=10) }}
    action:
      - service: notify.mobile_app_your_phone
        data:
          title: "Battery 1 offline"
          message: "No BLE reading since {{ states('sensor.battery1_last_seen') }}"
```

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
