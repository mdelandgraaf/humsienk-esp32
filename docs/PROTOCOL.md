# Humsienk "BMC" BLE Protocol

Reverse-engineered from the official **HumsiENK Smart BMS** Android app (v1.2.0) and
verified against real hardware. Applies to batteries whose BLE name begins with **`HS`**.

> The same app also implements a second protocol family ("WATT", PACE/Pylontech-derived,
> using GATT service `0xFFF0`) for differently-branded packs. That is **out of scope** here.

---

## 1. BLE transport

| | Value |
|---|---|
| Advertised name prefix | `HS` (e.g. `HS030102AB26030826`) |
| Primary GATT service | `00000001-0000-1000-8000-00805f9b34fb` |
| Write characteristic (commands → battery) | `00000002-0000-1000-8000-00805f9b34fb` |
| Notify characteristic (responses ← battery) | `00000003-0000-1000-8000-00805f9b34fb` |

Enable notifications on the notify characteristic, write a command frame to the write
characteristic, and assemble the response from the notifications. Responses can span
multiple BLE packets (see [§5](#5-packet-reassembly)).

---

## 2. Frame format

Both commands and responses share one structure:

```
byte 0     1        2        3 .. 3+N-1     3+N .. 3+N+1
+------+---------+--------+----------------+---------------+
| 0xAA |   cmd   |  len N | data (N bytes) | checksum (LE) |
+------+---------+--------+----------------+---------------+
  SOI    command   data       payload        uint16, LE
                   length
```

- **SOI**: always `0xAA`.
- **cmd**: command code (request); echoed back in the response.
- **len**: length of the data field, single byte.
- **data**: parameters (request) or payload (response).
- **checksum**: 2 bytes, **little-endian uint16**, equal to the unsigned sum of all bytes
  from index `1` through `2+N` inclusive (i.e. `cmd + len + data`), truncated to 16 bits.

### Reference implementation (Python)

```python
def build_frame(cmd: int, data: bytes = b"") -> bytes:
    body = bytes([cmd, len(data)]) + data
    cs = sum(body) & 0xFFFF
    return bytes([0xAA]) + body + cs.to_bytes(2, "little")

def parse_frame(buf: bytes):
    assert buf[0] == 0xAA, "bad SOI"
    n = buf[2]
    assert len(buf) >= 3 + n + 2, "truncated"
    data = buf[3:3+n]
    cs_actual = int.from_bytes(buf[3+n:3+n+2], "little")
    cs_expect = sum(buf[1:3+n]) & 0xFFFF
    assert cs_actual == cs_expect, "checksum mismatch"
    return buf[1], data            # (cmd, data)
```

---

## 3. Command codes

### Read commands (request data length = 0)

| Code | Hex | Meaning | Response payload |
|---|---|---|---|
| 0  | `0x00` | Handshake (send once after connect) | — |
| 16 | `0x10` | Manufacturer name | ASCII string |
| 17 | `0x11` | Battery pack name | ASCII string |
| 32 | `0x20` | Running status | see [§4.2](#42-0x20-running-status) |
| 33 | `0x21` | Battery info (main metrics) | see [§4.1](#41-0x21-battery-info) |
| 34 | `0x22` | Cell voltages | see [§4.3](#43-0x22-cell-voltages) |
| 88 | `0x58` | Battery parameters / thresholds | int16 array |
| 245 | `0xF5` | Firmware version | ASCII string |

### Control commands

| Code | Hex | Meaning | Data |
|---|---|---|---|
| 80 | `0x50` | Charge FET    | `[0x01]` on / `[0x00]` off |
| 81 | `0x51` | Discharge FET | `[0x01]` on / `[0x00]` off |
| 82 | `0x52` | Heating       | `[0x01]` on / `[0x00]` off |
| 83 | `0x53` | Clear status  | — |
| 102 | `0x66` | Calibrate static current | — |
| 103 | `0x67` | Calibrate charge current   | int16 BE (mA) |
| 104 | `0x68` | Calibrate discharge current | int16 BE (mA) |

> The BMS echoes the command code in its response when a control command succeeds.

---

## 4. Response payloads

**All multi-byte integers in responses are LITTLE-ENDIAN.** (This was the single most
important finding — an early big-endian assumption produced garbage values.)

### 4.1 `0x21` Battery info

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0  | 4 | int32 LE | pack voltage | **÷ 1000 → volts** |
| 4  | 4 | int32 LE | current | **÷ 1000 → amps**, signed; + = charging |
| 8  | 1 | uint8 | SOC | % |
| 9  | 1 | uint8 | SOH | % |
| 10 | 4 | int32 LE | remaining capacity | mAh |
| 14 | 4 | int32 LE | full capacity | mAh |
| 18 | 2 | uint16 LE | cycle count | |
| 20 | 1 | uint8 | temperature 1 | °C, raw byte, no offset |
| 21 | 1 | uint8 | temperature 2 | °C |
| 22 | 1 | uint8 | temperature 3 | °C |
| 23 | 1 | uint8 | temperature 4 | °C |
| 24 | 1 | uint8 | ambient temperature | °C |
| 25 | 1 | uint8 | MOS temperature | °C |

Minimum payload length 26 bytes.

### 4.2 `0x20` Running status

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 2 | uint16 LE | uptime days |
| 2 | 1 | uint8 | uptime hours |
| 3 | 1 | uint8 | uptime minutes |
| 4 | 4 | int32 LE | **status bitfield** (see below) |
| 8 | 4 | bytes | cell-balance status (per-cell bits) |
| 12 | 3 | bytes | disconnection status (per-cell bits) |

#### Status bitfield (offset 4, 32 bits)

Bit → meaning, verified by matching the app's bit-test order against the `RunningStatus`
constructor field order:

| Bit | Mask | Meaning |
|---|---|---|
| 0  | `0x00000001` | Charge overcurrent **protection** |
| 1  | `0x00000002` | Charge over-temperature **protection** |
| 2  | `0x00000004` | Charge under-temperature **protection** |
| 3  | `0x00000008` | Cell overvoltage **protection** |
| 4  | `0x00000010` | Pack overvoltage **protection** |
| 5  | `0x00000020` | AFE error |
| 6  | `0x00000040` | Charging stopped |
| 7  | `0x00000080` | **Charge FET on** |
| 8  | `0x00000100` | Charge overcurrent warning |
| 9  | `0x00000200` | Charge over-temperature warning |
| 10 | `0x00000400` | Charge under-temperature warning |
| 11 | `0x00000800` | Cell overvoltage warning |
| 12 | `0x00001000` | Pack overvoltage warning |
| 13 | `0x00002000` | Voltage-difference warning |
| 14 | `0x00004000` | Voltage difference too large |
| 15 | `0x00008000` | Heating |
| 16 | `0x00010000` | Discharge overcurrent **protection** |
| 17 | `0x00020000` | Discharge over-temperature **protection** |
| 18 | `0x00040000` | Discharge under-temperature **protection** |
| 19 | `0x00080000` | Cell undervoltage **protection** |
| 20 | `0x00100000` | Short-circuit **protection** |
| 21 | `0x00200000` | Pack undervoltage **protection** |
| 22 | `0x00400000` | Discharging stopped |
| 23 | `0x00800000` | **Discharge FET on** |
| 24 | `0x01000000` | Discharge overcurrent warning |
| 25 | `0x02000000` | Discharge over-temperature warning |
| 26 | `0x04000000` | Discharge low-temperature warning |
| 27 | `0x08000000` | Cell undervoltage warning |
| 28 | `0x10000000` | Pack undervoltage warning |
| 29 | `0x20000000` | MOS over-temperature warning |
| 30 | `0x40000000` | MOS over-temperature **protection** |
| 31 | `0x80000000` | Pre-discharge FET on |

### 4.3 `0x22` Cell voltages

A fixed array of **24 × uint16 LE**, each **÷ 1000 → volts**. There is **no leading count
byte**. Unused cell slots read as `0`, so ignore zero entries to find the real cell count,
min, and max.

---

## 5. Packet reassembly

A response may exceed the BLE MTU and arrive as several notifications. The full frame
length is known from the first packet:

```
expected_total_length = data_len_byte + 5      # SOI + cmd + len + data + 2-byte checksum
```

Accumulate notification chunks until the buffer reaches `expected_total_length`, then
parse and validate the checksum.

---

## 6. Connection flow

1. Connect, discover services, enable notifications on `0x0003`.
2. (Optional) negotiate a larger MTU (the app uses up to 247).
3. Send **handshake** (`0x00`) and wait for the response.
4. Poll `0x21` (info), `0x22` (cells), `0x20` (status) as needed — every 5–30 s is plenty.
5. Read once at startup: `0xF5` (version), `0x11` (name), `0x58` (parameters).

Keep **one request in flight at a time** per device (the app uses a serial request queue).
Don't send the next command until the previous response has been received or timed out.

---

## 7. Worked examples

```
Handshake:                 AA 00 00 00 00
Read battery info (0x21):  AA 21 00 21 00
Read cell voltages (0x22): AA 22 00 22 00
Read running status (0x20):AA 20 00 20 00
Charge FET ON  (0x50,01):  AA 50 01 01 52 00
Charge FET OFF (0x50,00):  AA 50 01 00 51 00
Discharge FET OFF(0x51,00):AA 51 01 00 52 00
```

---

## 8. Notes & caveats

- Temperatures are raw signed/unsigned bytes with no offset (verified — no Kelvin or +40
  bias on this protocol, unlike some other BMS families).
- The `0x58` parameter block contains protection thresholds as a series of int16 values;
  some temperature thresholds appear to use a `(°C × 10) + 2731` Kelvin encoding. This
  block is not fully mapped here because it isn't needed for monitoring.
- Everything in this document was verified against a 4-cell pack; higher cell counts should
  work (the cell array is fixed at 24 slots) but haven't been tested. Reports welcome.
