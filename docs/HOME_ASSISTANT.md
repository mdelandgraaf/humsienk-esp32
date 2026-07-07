# Home Assistant Integration

The firmware pushes each battery's data to Home Assistant over the **REST API**.
Enable it and set the details in the device's web config. (You can also just watch the
live data on the device's own web page — see [Web page](#web-page-no-home-assistant-needed).)

## REST API (e.g. via Nabu Casa)

Enable "REST publishing" and set the HA URL plus a Long-Lived Access Token
(HA → profile → Security → Long-Lived Access Tokens). Entities are created on first POST.

Entities created per battery (`<name>` is what you set in the web UI, e.g. `battery1`):

- **Sensors:** SOC (%), voltage (V), current (A), power (W), temperature (°C),
  remaining/full capacity (Ah), SOH (%), cycles, cell voltage diff (mV).
- **Flag sensors:** every protection and warning flag as a `sensor` with state `on`/`off`
  (e.g. `sensor.battery1_cell_ov_prot`), plus `sensor.<name>_charge_fet` and
  `sensor.<name>_discharge_fet` for the MOSFET states.

> REST-created entities have no unique ID, so HA can't manage them from the UI. That's
> expected and harmless.

> **Charge/discharge control** is done from the device's own web page (the "Charge /
> Discharge control" section), not from Home Assistant.

## Web page (no Home Assistant needed)

The device serves a config page at its IP (shown on the display and serial monitor). The
top of that page shows a **live view of every battery** — SOC with a colour-coded bar,
voltage, current, power, temperature, remaining capacity, SOH, cycles, cell diff, the
charge/discharge FET states, and any active protection/warning flags. It refreshes every
few seconds on its own. This works whether or not REST publishing is enabled.

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

## Example: alert on any protection firing

The protection/warning flags arrive as `sensor` entities whose state is `on` when active.

```yaml
automation:
  - alias: "Battery protection active"
    trigger:
      - platform: state
        entity_id:
          - sensor.battery1_cell_ov_prot
          - sensor.battery1_cell_uv_prot
          - sensor.battery1_short_circuit
          - sensor.battery1_chg_ot_prot
          - sensor.battery1_dis_ot_prot
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
      - sensor.battery1_charge_fet
      - sensor.battery1_discharge_fet
```
