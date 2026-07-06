/*
 * Humsienk BMC LiFePO4 BMS reader for ESP32 -> Tuya Cloud MQTT
 *
 * Reads one or more Humsienk batteries over BLE and publishes the measured
 * values to a Tuya Cloud MQTT endpoint or Tuya MQTT gateway.
 *
 * This example is intentionally separate from the full web-config firmware so
 * you can test the Tuya mapping without changing the stable Home Assistant / MQTT
 * build. Copy the TuyaPublisher parts into the web-config sketch when your Tuya
 * product datapoint mapping is confirmed.
 *
 * Libraries:
 *   - NimBLE-Arduino (h2zero, v2.x)
 *   - PubSubClient
 *   - ArduinoJson v7+
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <time.h>

// ============================================================================
// Configuration - EDIT THESE
// ============================================================================
static const char* WIFI_SSID = "YOUR_WIFI";
static const char* WIFI_PASS = "YOUR_PASSWORD";

// Tuya MQTT endpoint details. Use the endpoint/topic/credentials from your Tuya
// IoT Core / gateway product. Keep TUYA_MQTT_INSECURE true while testing; install
// the Tuya root CA and set it false for production.
static const char* TUYA_MQTT_HOST = "YOUR_TUYA_MQTT_HOST";
static const uint16_t TUYA_MQTT_PORT = 8883;
static const char* TUYA_MQTT_CLIENT_ID = "YOUR_TUYA_CLIENT_ID";
static const char* TUYA_MQTT_USER = "YOUR_TUYA_USERNAME_OR_DEVICE_ID";
static const char* TUYA_MQTT_PASS = "YOUR_TUYA_PASSWORD_OR_TOKEN";
static const char* TUYA_PUBLISH_TOPIC = "YOUR_TUYA_PROPERTY_REPORT_TOPIC";
static const bool TUYA_MQTT_INSECURE = true;

// Optional datapoint names. Match these to the identifiers you configured in
// Tuya IoT Platform for your custom product.
static const char* DP_SOC = "soc";
static const char* DP_VOLTAGE = "voltage";
static const char* DP_CURRENT = "current";
static const char* DP_POWER = "power";
static const char* DP_TEMPERATURE = "temperature";
static const char* DP_SOH = "soh";
static const char* DP_REMAINING_AH = "remaining_capacity";
static const char* DP_CELL_DIFF_MV = "cell_diff";
static const char* DP_CHARGE_FET = "charge_fet";
static const char* DP_DISCHARGE_FET = "discharge_fet";

struct BatteryCfg {
    const char* name;
    const char* mac;
};

static const BatteryCfg BATTERIES[] = {
    { "battery1", "XX:XX:XX:XX:XX:XX" },
    // { "battery2", "aa:bb:cc:dd:ee:ff" },
};
static const size_t BATTERY_COUNT = sizeof(BATTERIES) / sizeof(BATTERIES[0]);

static const uint32_t POLL_INTERVAL_MS = 30000;
static const uint32_t CONNECT_TIMEOUT_MS = 4000;
static const uint32_t REQUEST_TIMEOUT_MS = 2500;

// ============================================================================
// BLE protocol
// ============================================================================
static const NimBLEUUID SERVICE_UUID("00000001-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID WRITE_UUID  ("00000002-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID NOTIFY_UUID ("00000003-0000-1000-8000-00805f9b34fb");

static const uint8_t CMD_HANDSHAKE      = 0x00;
static const uint8_t CMD_RUNNING_STATUS = 0x20;
static const uint8_t CMD_BATTERY_INFO   = 0x21;
static const uint8_t CMD_CELL_VOLTAGE   = 0x22;
static const uint8_t SOI                = 0xAA;

struct ParsedFrame { bool ok; uint8_t cmd; const uint8_t* data; uint8_t dataLen; };

struct BatterySnapshot {
    bool     valid          = false;
    float    voltage_v      = 0;
    float    current_a      = 0;
    uint8_t  soc            = 0;
    uint8_t  soh            = 0;
    int32_t  remaining_mah  = 0;
    int32_t  full_mah       = 0;
    uint16_t cycles         = 0;
    int8_t   temp_c         = 0;
    int8_t   temp_mos       = 0;
    uint8_t  cell_count     = 0;
    uint16_t cell_min_mv    = 0;
    uint16_t cell_max_mv    = 0;
    uint16_t cell_diff_mv   = 0;
    bool     status_valid   = false;
    bool     chargeFetOn    = false;
    bool     dischargeFetOn = false;
    uint32_t status_bits    = 0;
};

static size_t buildFrame(uint8_t cmd, const uint8_t* data, uint8_t dataLen, uint8_t* out) {
    out[0] = SOI; out[1] = cmd; out[2] = dataLen;
    if (dataLen && data) memcpy(out + 3, data, dataLen);
    uint32_t sum = 0;
    for (size_t i = 1; i < 3 + (size_t)dataLen; i++) sum += out[i];
    uint16_t cs = (uint16_t)(sum & 0xFFFF);
    out[3 + dataLen] = cs & 0xFF;
    out[3 + dataLen + 1] = (cs >> 8) & 0xFF;
    return 3 + dataLen + 2;
}

static ParsedFrame parseFrame(const uint8_t* buf, size_t len) {
    ParsedFrame f{false, 0, nullptr, 0};
    if (len < 5 || buf[0] != SOI) return f;
    uint8_t n = buf[2];
    if (len < (size_t)(3 + n + 2)) return f;
    uint32_t sum = 0;
    for (size_t i = 1; i < 3 + (size_t)n; i++) sum += buf[i];
    uint16_t expected = (uint16_t)(sum & 0xFFFF);
    uint16_t actual = buf[3 + n] | ((uint16_t)buf[3 + n + 1] << 8);
    if (expected != actual) return f;
    f.ok = true; f.cmd = buf[1]; f.data = buf + 3; f.dataLen = n;
    return f;
}

static size_t expectedLen(const uint8_t* buf, size_t len) {
    if (len < 3 || buf[0] != SOI) return 0;
    return (size_t)buf[2] + 5;
}

static int32_t  rd_i32_le(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24); }
static uint16_t rd_u16_le(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1]<<8); }

class BmsClient {
public:
    bool connectAndPoll(const BatteryCfg& cfg, BatterySnapshot& s) {
        NimBLEAddress addr(cfg.mac, BLE_ADDR_PUBLIC);
        client_ = NimBLEDevice::createClient();
        client_->setConnectTimeout(CONNECT_TIMEOUT_MS);
        Serial.printf("[%s] connecting to %s\n", cfg.name, cfg.mac);
        if (!client_->connect(addr, false)) { cleanup(); return false; }
        auto* svc = client_->getService(SERVICE_UUID);
        if (!svc) { cleanup(); return false; }
        writeChar_  = svc->getCharacteristic(WRITE_UUID);
        notifyChar_ = svc->getCharacteristic(NOTIFY_UUID);
        if (!writeChar_ || !notifyChar_ || !notifyChar_->canNotify()) { cleanup(); return false; }
        notifyChar_->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool){ this->onNotify(d, l); });
        if (!doRequest(CMD_HANDSHAKE, nullptr, 0)) { cleanup(); return false; }
        if (doRequest(CMD_BATTERY_INFO, nullptr, 0)) parseBatteryInfo(s);
        if (doRequest(CMD_CELL_VOLTAGE, nullptr, 0)) parseCellVoltage(s);
        if (doRequest(CMD_RUNNING_STATUS, nullptr, 0)) parseRunningStatus(s);
        s.valid = true;
        cleanup();
        return true;
    }

private:
    NimBLEClient* client_ = nullptr;
    NimBLERemoteCharacteristic *writeChar_ = nullptr, *notifyChar_ = nullptr;
    uint8_t rxBuf_[256];
    volatile size_t rxLen_ = 0, rxExpected_ = 0;
    volatile bool rxComplete_ = false;
    const uint8_t* lastData_ = nullptr;
    uint8_t lastDataLen_ = 0;

    void onNotify(const uint8_t* d, size_t l) {
        if (rxLen_ + l > sizeof(rxBuf_)) { rxLen_ = 0; rxExpected_ = 0; }
        memcpy(rxBuf_ + rxLen_, d, l); rxLen_ += l;
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
        auto p = parseFrame(rxBuf_, rxLen_);
        if (!p.ok || p.cmd != cmd) return false;
        lastData_ = p.data; lastDataLen_ = p.dataLen;
        return true;
    }

    void parseBatteryInfo(BatterySnapshot& s) {
        if (lastDataLen_ < 26) return;
        const uint8_t* d = lastData_;
        s.voltage_v     = rd_i32_le(d + 0) / 1000.0f;
        s.current_a     = rd_i32_le(d + 4) / 1000.0f;
        s.soc           = d[8];
        s.soh           = d[9];
        s.remaining_mah = rd_i32_le(d + 10);
        s.full_mah      = rd_i32_le(d + 14);
        s.cycles        = rd_u16_le(d + 18);
        s.temp_c        = (int8_t)d[20];
        s.temp_mos      = (int8_t)d[25];
    }

    void parseCellVoltage(BatterySnapshot& s) {
        const uint8_t slots = 24;
        if (lastDataLen_ < (size_t)slots * 2) return;
        uint16_t mn = 0xFFFF, mx = 0; uint8_t count = 0;
        for (uint8_t i = 0; i < slots; i++) {
            uint16_t mv = rd_u16_le(lastData_ + 2 * i);
            if (mv == 0) continue;
            count++; if (mv < mn) mn = mv; if (mv > mx) mx = mv;
        }
        if (count == 0) return;
        s.cell_count = count; s.cell_min_mv = mn; s.cell_max_mv = mx; s.cell_diff_mv = mx - mn;
    }

    void parseRunningStatus(BatterySnapshot& s) {
        if (lastDataLen_ < 8) return;
        uint32_t st = (uint32_t)rd_i32_le(lastData_ + 4);
        s.status_bits = st;
        s.chargeFetOn = (st & 0x00000080UL) != 0;
        s.dischargeFetOn = (st & 0x00800000UL) != 0;
        s.status_valid = true;
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
// Tuya MQTT publishing
// ============================================================================
WiFiClientSecure tuyaNet;
PubSubClient tuyaMqtt(tuyaNet);

static uint32_t unixSeconds() {
    time_t now = time(nullptr);
    if (now < 1700000000) return millis() / 1000;
    return (uint32_t)now;
}

static void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.print("WiFi connecting");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf(" got %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

static bool ensureTuyaMqtt() {
    if (tuyaMqtt.connected()) return true;
    if (TUYA_MQTT_INSECURE) tuyaNet.setInsecure();
    tuyaMqtt.setServer(TUYA_MQTT_HOST, TUYA_MQTT_PORT);
    tuyaMqtt.setBufferSize(2048);

    Serial.print("Tuya MQTT connecting...");
    bool ok = tuyaMqtt.connect(TUYA_MQTT_CLIENT_ID, TUYA_MQTT_USER, TUYA_MQTT_PASS);
    Serial.println(ok ? "ok" : "failed");
    if (!ok) Serial.printf("Tuya MQTT rc=%d\n", tuyaMqtt.state());
    return ok;
}

template <typename T>
static void addValue(JsonObject parent, const String& key, T value, uint32_t ts) {
    JsonObject obj = parent[key].to<JsonObject>();
    obj["value"] = value;
    obj["time"] = ts;
}

static bool publishToTuya(const BatteryCfg& cfg, const BatterySnapshot& s) {
    if (!tuyaMqtt.connected()) return false;

    uint32_t ts = unixSeconds();
    JsonDocument doc;
    doc["msgId"] = String("humsienk-") + cfg.name + "-" + String(ts);
    doc["time"] = ts;

    // Payload shape: property-name -> { value, time }.
    // Match the property names below to your Tuya custom product DP identifiers.
    JsonObject data = doc["data"].to<JsonObject>();
    String prefix = String(cfg.name) + "_";   // keep multi-battery names unique
    addValue(data, prefix + DP_SOC, s.soc, ts);
    addValue(data, prefix + DP_VOLTAGE, round(s.voltage_v * 1000.0f) / 1000.0f, ts);
    addValue(data, prefix + DP_CURRENT, round(s.current_a * 1000.0f) / 1000.0f, ts);
    addValue(data, prefix + DP_POWER, round(s.voltage_v * s.current_a * 10.0f) / 10.0f, ts);
    addValue(data, prefix + DP_TEMPERATURE, (int)s.temp_c, ts);
    addValue(data, prefix + DP_SOH, s.soh, ts);
    addValue(data, prefix + DP_REMAINING_AH, round((s.remaining_mah / 1000.0f) * 100.0f) / 100.0f, ts);
    addValue(data, prefix + DP_CELL_DIFF_MV, s.cell_diff_mv, ts);
    if (s.status_valid) {
        addValue(data, prefix + DP_CHARGE_FET, s.chargeFetOn, ts);
        addValue(data, prefix + DP_DISCHARGE_FET, s.dischargeFetOn, ts);
    }

    char payload[1800];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0 || n >= sizeof(payload)) {
        Serial.println("Tuya payload too large");
        return false;
    }

    bool ok = tuyaMqtt.publish(TUYA_PUBLISH_TOPIC, (const uint8_t*)payload, (unsigned int)n);
    Serial.printf("[%s] Tuya publish %s (%u bytes)\n", cfg.name, ok ? "ok" : "failed", (unsigned)n);
    return ok;
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nHumsienk BMS -> Tuya Cloud MQTT bridge starting");
    NimBLEDevice::init("");
    NimBLEDevice::setMTU(247);
    ensureWifi();
    ensureTuyaMqtt();
}

void loop() {
    ensureWifi();
    ensureTuyaMqtt();
    tuyaMqtt.loop();

    for (size_t i = 0; i < BATTERY_COUNT; i++) {
        BatterySnapshot s;
        BmsClient bms;
        bool ok = bms.connectAndPoll(BATTERIES[i], s);
        if (ok && s.valid) {
            Serial.printf("[%s] V=%.2f I=%.2f SOC=%u%%\n", BATTERIES[i].name, s.voltage_v, s.current_a, s.soc);
            publishToTuya(BATTERIES[i], s);
        } else {
            Serial.printf("[%s] read failed\n", BATTERIES[i].name);
        }
        tuyaMqtt.loop();
        delay(500);
    }

    uint32_t t = millis();
    while (millis() - t < POLL_INTERVAL_MS) { tuyaMqtt.loop(); delay(100); }
}
