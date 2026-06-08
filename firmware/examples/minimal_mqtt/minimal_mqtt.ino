/*
 * Humsienk BMC LiFePO4 BMS reader for ESP32
 * 
 * Reads battery voltage, current, SOC, SOH from 1..N batteries via BLE,
 * publishes to MQTT. From there, bridge to Tuya / Home Assistant / anything.
 *
 * Board: ESP32 (any variant with BLE)
 * Libraries (install via Arduino IDE Library Manager):
 *   - NimBLE-Arduino       (h2zero, v1.4+)
 *   - PubSubClient         (Nick O'Leary)
 *   - ArduinoJson          (Benoit Blanchon)
 *
 * Protocol reverse-engineered from HumsiENK Smart BMS app v1.2.0.
 * See humsienk_bmc_protocol.md for the full spec.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ============================================================================
// Configuration — EDIT THESE
// ============================================================================
static const char* WIFI_SSID   = "YOUR_WIFI";
static const char* WIFI_PASS   = "YOUR_PASSWORD";
static const char* MQTT_HOST   = "192.168.1.10";   // your broker
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_USER   = "";               // optional
static const char* MQTT_PASS   = "";               // optional

// List of batteries to poll. Use the BLE MAC address shown in nRF Connect.
// You can mix: poll 1, 2, or 3 batteries. Each gets its own MQTT topic.
struct BatteryCfg {
    const char* name;       // friendly name → MQTT topic suffix
    const char* mac;        // BLE MAC address, lowercase, colon-separated
};
static const BatteryCfg BATTERIES[] = {
    { "battery1", "XX:XX:XX:XX:XX:XX" },
    // { "battery2", "aa:bb:cc:dd:ee:ff" },
    // { "battery3", "11:22:33:44:55:66" },
};
static const size_t BATTERY_COUNT = sizeof(BATTERIES) / sizeof(BATTERIES[0]);

static const uint32_t POLL_INTERVAL_MS    = 30000;  // poll all batteries every 30s
static const uint32_t CONNECT_TIMEOUT_MS  = 8000;
static const uint32_t REQUEST_TIMEOUT_MS  = 2500;

// ============================================================================
// BLE UUIDs (from the APK reverse-engineering)
// ============================================================================
static const NimBLEUUID SERVICE_UUID("00000001-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID WRITE_UUID  ("00000002-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID NOTIFY_UUID ("00000003-0000-1000-8000-00805f9b34fb");

// ============================================================================
// BMC protocol command codes
// ============================================================================
static const uint8_t CMD_HANDSHAKE         = 0x00;
static const uint8_t CMD_MANUFACTURER_NAME = 0x10;
static const uint8_t CMD_PACK_NAME         = 0x11;
static const uint8_t CMD_RUNNING_STATUS    = 0x20;
static const uint8_t CMD_BATTERY_INFO      = 0x21;
static const uint8_t CMD_CELL_VOLTAGE      = 0x22;
static const uint8_t CMD_BATTERY_PARAMS    = 0x58;
static const uint8_t CMD_VERSION           = 0xF5;
static const uint8_t SOI                   = 0xAA;

// ============================================================================
// Frame builder & parser
// ============================================================================
static size_t buildFrame(uint8_t cmd, const uint8_t* data, uint8_t dataLen, uint8_t* out) {
    out[0] = SOI;
    out[1] = cmd;
    out[2] = dataLen;
    if (dataLen && data) memcpy(out + 3, data, dataLen);
    // checksum = sum of bytes [1 .. 2+dataLen] (i.e. cmd + len + data)
    uint32_t sum = 0;
    for (size_t i = 1; i < 3 + (size_t)dataLen; i++) sum += out[i];
    uint16_t cs = (uint16_t)(sum & 0xFFFF);
    out[3 + dataLen]     = cs & 0xFF;        // LE low byte
    out[3 + dataLen + 1] = (cs >> 8) & 0xFF; // LE high byte
    return 3 + dataLen + 2;
}

struct ParsedFrame {
    bool ok;
    uint8_t cmd;
    const uint8_t* data;
    uint8_t dataLen;
};

static ParsedFrame parseFrame(const uint8_t* buf, size_t len) {
    ParsedFrame f{false, 0, nullptr, 0};
    if (len < 5)             return f;        // too short
    if (buf[0] != SOI)       return f;        // bad SOI
    uint8_t n = buf[2];
    if (len < (size_t)(3 + n + 2)) return f;  // truncated
    uint32_t sum = 0;
    for (size_t i = 1; i < 3 + (size_t)n; i++) sum += buf[i];
    uint16_t expected = (uint16_t)(sum & 0xFFFF);
    uint16_t actual   = buf[3 + n] | ((uint16_t)buf[3 + n + 1] << 8);
    if (expected != actual)  return f;        // checksum fail
    f.ok      = true;
    f.cmd     = buf[1];
    f.data    = buf + 3;
    f.dataLen = n;
    return f;
}

// Returns expected total frame length from the first packet, or 0 if unknown yet.
static size_t expectedLen(const uint8_t* buf, size_t len) {
    if (len < 3 || buf[0] != SOI) return 0;
    return (size_t)buf[2] + 5;
}

// ============================================================================
// Big-endian readers (battery data is big-endian)
// ============================================================================
static int32_t rd_i32_be(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                     (uint32_t)p[2] << 8  |  (uint32_t)p[3]);
}
static int16_t rd_i16_be(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}
static uint16_t rd_u16_be(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

// ============================================================================
// BLE I/O state machine for one connection
// ============================================================================
class BmsClient {
public:
    bool connectAndPoll(const BatteryCfg& cfg, JsonDocument& out) {
        NimBLEAddress addr(cfg.mac, BLE_ADDR_PUBLIC);
        client_ = NimBLEDevice::createClient();
        client_->setConnectTimeout(CONNECT_TIMEOUT_MS / 1000);

        Serial.printf("[%s] connecting to %s\n", cfg.name, cfg.mac);
        if (!client_->connect(addr, false)) {
            Serial.printf("[%s] connect failed\n", cfg.name);
            cleanup();
            return false;
        }
        client_->exchangeMTU(247);

        auto* svc = client_->getService(SERVICE_UUID);
        if (!svc) { Serial.println("  no service"); cleanup(); return false; }
        writeChar_  = svc->getCharacteristic(WRITE_UUID);
        notifyChar_ = svc->getCharacteristic(NOTIFY_UUID);
        if (!writeChar_ || !notifyChar_) { Serial.println("  chars missing"); cleanup(); return false; }
        if (!notifyChar_->canNotify()) { Serial.println("  no notify"); cleanup(); return false; }
        notifyChar_->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
            this->onNotify(d, len);
        });

        // Handshake first.
        if (!doRequest(CMD_HANDSHAKE, nullptr, 0)) { Serial.println("  handshake fail"); cleanup(); return false; }
        // Pull the main readings
        if (doRequest(CMD_BATTERY_INFO, nullptr, 0))    parseBatteryInfo(out);
        if (doRequest(CMD_CELL_VOLTAGE, nullptr, 0))    parseCellVoltage(out);
        if (doRequest(CMD_RUNNING_STATUS, nullptr, 0))  parseRunningStatus(out);

        cleanup();
        return true;
    }

private:
    NimBLEClient* client_ = nullptr;
    NimBLERemoteCharacteristic* writeChar_  = nullptr;
    NimBLERemoteCharacteristic* notifyChar_ = nullptr;

    uint8_t rxBuf_[256];
    volatile size_t rxLen_ = 0;
    volatile size_t rxExpected_ = 0;
    volatile bool rxComplete_ = false;

    void onNotify(const uint8_t* d, size_t len) {
        if (rxLen_ + len > sizeof(rxBuf_)) { rxLen_ = 0; rxExpected_ = 0; }
        memcpy(rxBuf_ + rxLen_, d, len);
        rxLen_ += len;
        if (!rxExpected_) rxExpected_ = expectedLen(rxBuf_, rxLen_);
        if (rxExpected_ && rxLen_ >= rxExpected_) rxComplete_ = true;
    }

    bool doRequest(uint8_t cmd, const uint8_t* data, uint8_t dataLen) {
        uint8_t frame[64];
        size_t flen = buildFrame(cmd, data, dataLen, frame);

        rxLen_ = 0; rxExpected_ = 0; rxComplete_ = false;
        if (!writeChar_->writeValue(frame, flen, true)) return false;

        uint32_t t0 = millis();
        while (!rxComplete_ && (millis() - t0) < REQUEST_TIMEOUT_MS) delay(20);
        if (!rxComplete_) return false;

        auto parsed = parseFrame(rxBuf_, rxLen_);
        if (!parsed.ok || parsed.cmd != cmd) return false;
        lastData_ = parsed.data;
        lastDataLen_ = parsed.dataLen;
        return true;
    }

    const uint8_t* lastData_ = nullptr;
    uint8_t lastDataLen_ = 0;

    void parseBatteryInfo(JsonDocument& out) {
        if (lastDataLen_ < 21) return;
        const uint8_t* d = lastData_;
        out["voltage_mv"]      = rd_i32_be(d + 0);
        out["current_ma"]      = rd_i32_be(d + 4);   // signed: + = charging
        out["soc"]             = d[8];
        out["soh"]             = d[9];
        out["remaining_mah"]   = rd_i32_be(d + 10);
        out["full_mah"]        = rd_i32_be(d + 14);
        out["cycles"]          = rd_u16_be(d + 18);
        uint8_t tcount = d[20];
        if (lastDataLen_ >= 21u + tcount) {
            JsonArray temps = out["temperatures_c"].to<JsonArray>();
            for (uint8_t i = 0; i < tcount; i++) temps.add((int8_t)d[21 + i]);
        }
    }
    void parseCellVoltage(JsonDocument& out) {
        if (lastDataLen_ < 1) return;
        uint8_t cells = lastData_[0];
        if (lastDataLen_ < 1u + 2u * cells) return;
        JsonArray arr = out["cells_mv"].to<JsonArray>();
        for (uint8_t i = 0; i < cells; i++) arr.add(rd_u16_be(lastData_ + 1 + 2*i));
    }
    void parseRunningStatus(JsonDocument& out) {
        if (lastDataLen_ < 8) return;
        out["uptime_days"]    = rd_u16_be(lastData_ + 0);
        out["uptime_hours"]   = lastData_[2];
        out["uptime_minutes"] = lastData_[3];
        out["status_bits"]    = (uint32_t)rd_i32_be(lastData_ + 4);
    }

    void cleanup() {
        if (client_) {
            if (client_->isConnected()) client_->disconnect();
            NimBLEDevice::deleteClient(client_);
            client_ = nullptr;
        }
    }
};

// ============================================================================
// WiFi + MQTT plumbing
// ============================================================================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.print("WiFi connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf(" got %s\n", WiFi.localIP().toString().c_str());
}
void ensureMqtt() {
    if (mqtt.connected()) return;
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);
    while (!mqtt.connected()) {
        String cid = "humsienk-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
            Serial.println("MQTT connected");
        } else {
            Serial.printf("MQTT failed rc=%d, retry in 2s\n", mqtt.state());
            delay(2000);
        }
    }
}

// ============================================================================
// Main
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nHumsienk BMS → MQTT bridge starting");
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    ensureWifi();
}

void loop() {
    ensureWifi();
    ensureMqtt();

    for (size_t i = 0; i < BATTERY_COUNT; i++) {
        const auto& cfg = BATTERIES[i];
        JsonDocument doc;
        doc["name"] = cfg.name;
        doc["ts"]   = millis();

        BmsClient bms;
        bool ok = bms.connectAndPoll(cfg, doc);
        doc["ok"] = ok;

        char topic[128];
        snprintf(topic, sizeof(topic), "humsienk/%s/state", cfg.name);
        char payload[1024];
        size_t n = serializeJson(doc, payload, sizeof(payload));
        if (mqtt.publish(topic, payload, n)) {
            Serial.printf("[%s] published %u bytes\n", cfg.name, (unsigned)n);
        } else {
            Serial.printf("[%s] publish failed\n", cfg.name);
        }
        mqtt.loop();
        delay(500);  // small gap between batteries
    }

    // Sleep until next poll cycle
    uint32_t t = millis();
    while (millis() - t < POLL_INTERVAL_MS) { mqtt.loop(); delay(100); }
}
