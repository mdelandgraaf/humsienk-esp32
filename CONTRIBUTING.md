# Contributing

Thanks for helping out! This started as a reverse-engineering project to get Humsienk
batteries into Home Assistant, and it gets better with more hardware variety behind it.

## Most useful contributions

- **Compatibility reports.** If your `HS` pack works, say so (model, cell count). If it
  doesn't, open an issue with:
  - an **nRF Connect** dump of the GATT services/characteristics, and
  - a sample raw `0x21` response (hex) plus what the official app shows for the same moment,
    so field offsets can be checked.
- **Additional decoded fields** — e.g. mapping the `0x58` parameter/threshold block, or the
  cell-balance / disconnection bit arrays in the `0x20` response.
- **Ports** to other ESP32 displays or to a headless (no-display) build.
- **Bug fixes** and reliability improvements (BLE reconnect logic, MQTT edge cases, etc.).

## Ground rules

- **Don't commit secrets.** No Wi-Fi passwords, HA tokens, MQTT creds, or real device MACs
  in code, issues, or screenshots. All config is runtime (web portal / NVS) for this reason.
- **No copyrighted/third-party app code or assets.** This repo documents an
  interoperability protocol and ships only original firmware. Keep it that way.
- Keep the protocol doc (`docs/PROTOCOL.md`) the single source of truth — if you decode
  something new, document it there with how it was verified.
- Match the existing style: the firmware is a single `.ino` for easy Arduino-IDE flashing;
  keep it self-contained unless there's a strong reason to split.

## Testing a change

Please note in your PR what you tested against (board, number of batteries, MQTT and/or
REST) and include serial output if it's behaviour-related.

## Safety

Changes that touch the charge/discharge FET control path get extra scrutiny — these switch
a safety-critical device. Keep the confirmation guards and the "read-back actual state"
behaviour intact.
