/*
 * Humsienk BMC LiFePO4 BMS reader for ESP32
 * Publishes to Home Assistant via Nabu Casa REST API (HTTPS).
 *
 * Board: ESP32 (any variant with BLE)
 * Libraries (Arduino IDE → Library Manager):
 *   - NimBLE-Arduino       (h2zero, v1.4+)
 *   - ArduinoJson          (Benoit Blanchon, v7+)
 * (HTTPClient + WiFiClientSecure are bundled with ESP32 core.)
 *
 * Protocol reverse-engineered from HumsiENK Smart BMS app v1.2.0.
 * See humsienk_bmc_protocol.md for the full spec.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ============================================================================
// Configuration — EDIT THESE
// ============================================================================
static const char* WIFI_SSID = "YOUR_WIFI";
static const char* WIFI_PASS = "YOUR_PASSWORD";

// Your Nabu Casa URL (no trailing slash)
static const char* HA_URL    = "https://YOUR-ID.ui.nabu.casa";

// Long-Lived Access Token from HA profile page
static const char* HA_TOKEN  = "PASTE_YOUR_LONG_LIVED_TOKEN_HERE";

// List of batteries to poll. Use the BLE MAC address from nRF Connect.
struct BatteryCfg {
    const char* name;       // used as HA entity prefix, e.g. "battery1" → sensor.battery1_soc
    const char* mac;        // BLE MAC, lowercase, colon-separated
};
static const BatteryCfg BATTERIES[] = {
    { "battery1", "XX:XX:XX:XX:XX:XX" },
    // { "battery2", "aa:bb:cc:dd:ee:ff" },
    // { "battery3", "11:22:33:44:55:66" },
};
static const size_t BATTERY_COUNT = sizeof(BATTERIES) / sizeof(BATTERIES[0]);

static const uint32_t POLL_INTERVAL_MS   = 60000;  // 60s — REST is heavier than MQTT
static const uint32_t CONNECT_TIMEOUT_MS = 8000;
static const uint32_t REQUEST_TIMEOUT_MS = 2500;

// ============================================================================
// BLE UUIDs (from APK reverse-engineering)
// ============================================================================
static const NimBLEUUID SERVICE_UUID("00000001-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID WRITE_UUID  ("00000002-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID NOTIFY_UUID ("00000003-0000-1000-8000-00805f9b34fb");

// BMC command codes
static const uint8_t CMD_HANDSHAKE      = 0x00;
static const uint8_t CMD_RUNNING_STATUS = 0x20;
static const uint8_t CMD_BATTERY_INFO   = 0x21;
static const uint8_t CMD_CELL_VOLTAGE   = 0x22;
static const uint8_t SOI                = 0xAA;

// ============================================================================
// Frame builder & parser
// ============================================================================
static size_t buildFrame(uint8_t cmd, const uint8_t* data, uint8_t dataLen, uint8_t* out) {
    out[0] = SOI; out[1] = cmd; out[2] = dataLen;
    if (dataLen && data) memcpy(out + 3, data, dataLen);
    uint32_t sum = 0;
    for (size_t i = 1; i < 3 + (size_t)dataLen; i++) sum += out[i];
    uint16_t cs = (uint16_t)(sum & 0xFFFF);
    out[3 + dataLen]     = cs & 0xFF;
    out[3 + dataLen + 1] = (cs >> 8) & 0xFF;
    return 3 + dataLen + 2;
}

struct ParsedFrame { bool ok; uint8_t cmd; const uint8_t* data; uint8_t dataLen; };

static ParsedFrame parseFrame(const uint8_t* buf, size_t len) {
    ParsedFrame f{false, 0, nullptr, 0};
    if (len < 5 || buf[0] != SOI) return f;
    uint8_t n = buf[2];
    if (len < (size_t)(3 + n + 2)) return f;
    uint32_t sum = 0;
    for (size_t i = 1; i < 3 + (size_t)n; i++) sum += buf[i];
    uint16_t expected = (uint16_t)(sum & 0xFFFF);
    uint16_t actual   = buf[3 + n] | ((uint16_t)buf[3 + n + 1] << 8);
    if (expected != actual) return f;
    f.ok = true; f.cmd = buf[1]; f.data = buf + 3; f.dataLen = n;
    return f;
}

static size_t expectedLen(const uint8_t* buf, size_t len) {
    if (len < 3 || buf[0] != SOI) return 0;
    return (size_t)buf[2] + 5;
}

static int32_t  rd_i32_be(const uint8_t* p) { return (int32_t)((uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]); }
static uint16_t rd_u16_be(const uint8_t* p) { return (uint16_t)((uint16_t)p[0]<<8 | p[1]); }

// ============================================================================
// Parsed battery snapshot
// ============================================================================
struct BatterySnapshot {
    bool     ok = false;
    int32_t  voltage_mv      = 0;
    int32_t  current_ma      = 0;   // signed: + = charging
    uint8_t  soc             = 0;
    uint8_t  soh             = 0;
    int32_t  remaining_mah   = 0;
    int32_t  full_mah        = 0;
    uint16_t cycles          = 0;
    int8_t   temp_c          = 0;   // first temperature sensor
    uint8_t  cell_count      = 0;
    uint16_t cell_min_mv     = 0;
    uint16_t cell_max_mv     = 0;
    uint16_t cell_diff_mv    = 0;
    uint16_t uptime_days     = 0;
};

// ============================================================================
// BLE I/O state machine
// ============================================================================
class BmsClient {
public:
    bool connectAndPoll(const BatteryCfg& cfg, BatterySnapshot& s) {
        NimBLEAddress addr(cfg.mac, BLE_ADDR_PUBLIC);
        client_ = NimBLEDevice::createClient();
        client_->setConnectTimeout(CONNECT_TIMEOUT_MS / 1000);

        Serial.printf("[%s] BLE connect %s\n", cfg.name, cfg.mac);
        if (!client_->connect(addr, false)) { Serial.println("  connect fail"); cleanup(); return false; }
        client_->exchangeMTU(247);

        auto* svc = client_->getService(SERVICE_UUID);
        if (!svc) { Serial.println("  no service"); cleanup(); return false; }
        writeChar_  = svc->getCharacteristic(WRITE_UUID);
        notifyChar_ = svc->getCharacteristic(NOTIFY_UUID);
        if (!writeChar_ || !notifyChar_ || !notifyChar_->canNotify()) {
            Serial.println("  chars missing"); cleanup(); return false;
        }
        notifyChar_->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool){
            this->onNotify(d, l);
        });

        if (!doRequest(CMD_HANDSHAKE, nullptr, 0))         { cleanup(); return false; }
        if (doRequest(CMD_BATTERY_INFO, nullptr, 0))       parseBatteryInfo(s);
        if (doRequest(CMD_CELL_VOLTAGE, nullptr, 0))       parseCellVoltage(s);
        if (doRequest(CMD_RUNNING_STATUS, nullptr, 0))     parseRunningStatus(s);

        s.ok = true;
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
        if (lastDataLen_ < 21) return;
        const uint8_t* d = lastData_;
        s.voltage_mv    = rd_i32_be(d + 0);
        s.current_ma    = rd_i32_be(d + 4);
        s.soc           = d[8];
        s.soh           = d[9];
        s.remaining_mah = rd_i32_be(d + 10);
        s.full_mah      = rd_i32_be(d + 14);
        s.cycles        = rd_u16_be(d + 18);
        uint8_t tcount  = d[20];
        if (tcount > 0 && lastDataLen_ >= 22) s.temp_c = (int8_t)d[21];
    }
    void parseCellVoltage(BatterySnapshot& s) {
        if (lastDataLen_ < 1) return;
        uint8_t cells = lastData_[0];
        if (lastDataLen_ < 1u + 2u * cells || cells == 0) return;
        s.cell_count = cells;
        uint16_t mn = 0xFFFF, mx = 0;
        for (uint8_t i = 0; i < cells; i++) {
            uint16_t v = rd_u16_be(lastData_ + 1 + 2*i);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        s.cell_min_mv  = mn;
        s.cell_max_mv  = mx;
        s.cell_diff_mv = mx - mn;
    }
    void parseRunningStatus(BatterySnapshot& s) {
        if (lastDataLen_ < 2) return;
        s.uptime_days = rd_u16_be(lastData_ + 0);
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
// WiFi + HA REST publisher
// ============================================================================
WiFiClientSecure secureClient;

void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.print("WiFi connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf(" got %s\n", WiFi.localIP().toString().c_str());
}

// POST one sensor state to Home Assistant.
// Unit and device_class let HA render the value nicely and link to long-term stats.
static bool postSensor(const char* entity_id,
                       const char* state,
                       const char* unit,
                       const char* device_class,
                       const char* friendly_name) {
    String url = String(HA_URL) + "/api/states/" + entity_id;

    HTTPClient http;
    http.setTimeout(8000);
    http.setReuse(true);
    if (!http.begin(secureClient, url)) {
        Serial.printf("  http.begin fail: %s\n", url.c_str());
        return false;
    }
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
    http.addHeader("Content-Type", "application/json");

    JsonDocument body;
    body["state"] = state;
    JsonObject attr = body["attributes"].to<JsonObject>();
    attr["unit_of_measurement"] = unit;
    if (device_class && device_class[0]) attr["device_class"] = device_class;
    if (device_class && device_class[0]) attr["state_class"] = "measurement";
    attr["friendly_name"] = friendly_name;

    String payload;
    serializeJson(body, payload);
    int code = http.POST(payload);
    http.end();

    if (code == 200 || code == 201) return true;
    Serial.printf("  POST %s → HTTP %d\n", entity_id, code);
    return false;
}

static void publishBattery(const BatteryCfg& cfg, const BatterySnapshot& s) {
    char ent[64], val[32], fname[64];

    snprintf(ent,   sizeof(ent),   "sensor.%s_soc", cfg.name);
    snprintf(val,   sizeof(val),   "%u", s.soc);
    snprintf(fname, sizeof(fname), "%s SOC", cfg.name);
    postSensor(ent, val, "%", "battery", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_voltage", cfg.name);
    snprintf(val,   sizeof(val),   "%.3f", s.voltage_mv / 1000.0);
    snprintf(fname, sizeof(fname), "%s Voltage", cfg.name);
    postSensor(ent, val, "V", "voltage", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_current", cfg.name);
    snprintf(val,   sizeof(val),   "%.3f", s.current_ma / 1000.0);
    snprintf(fname, sizeof(fname), "%s Current", cfg.name);
    postSensor(ent, val, "A", "current", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_power", cfg.name);
    double power_w = (s.voltage_mv / 1000.0) * (s.current_ma / 1000.0);
    snprintf(val,   sizeof(val),   "%.1f", power_w);
    snprintf(fname, sizeof(fname), "%s Power", cfg.name);
    postSensor(ent, val, "W", "power", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_remaining_capacity", cfg.name);
    snprintf(val,   sizeof(val),   "%.2f", s.remaining_mah / 1000.0);
    snprintf(fname, sizeof(fname), "%s Remaining Capacity", cfg.name);
    postSensor(ent, val, "Ah", "", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_full_capacity", cfg.name);
    snprintf(val,   sizeof(val),   "%.2f", s.full_mah / 1000.0);
    snprintf(fname, sizeof(fname), "%s Full Capacity", cfg.name);
    postSensor(ent, val, "Ah", "", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_soh", cfg.name);
    snprintf(val,   sizeof(val),   "%u", s.soh);
    snprintf(fname, sizeof(fname), "%s SOH", cfg.name);
    postSensor(ent, val, "%", "", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_cycles", cfg.name);
    snprintf(val,   sizeof(val),   "%u", s.cycles);
    snprintf(fname, sizeof(fname), "%s Cycles", cfg.name);
    postSensor(ent, val, "", "", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_temperature", cfg.name);
    snprintf(val,   sizeof(val),   "%d", (int)s.temp_c);
    snprintf(fname, sizeof(fname), "%s Temperature", cfg.name);
    postSensor(ent, val, "°C", "temperature", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_cell_diff", cfg.name);
    snprintf(val,   sizeof(val),   "%u", s.cell_diff_mv);
    snprintf(fname, sizeof(fname), "%s Cell Voltage Diff", cfg.name);
    postSensor(ent, val, "mV", "voltage", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_cell_min", cfg.name);
    snprintf(val,   sizeof(val),   "%.3f", s.cell_min_mv / 1000.0);
    snprintf(fname, sizeof(fname), "%s Min Cell Voltage", cfg.name);
    postSensor(ent, val, "V", "voltage", fname);

    snprintf(ent,   sizeof(ent),   "sensor.%s_cell_max", cfg.name);
    snprintf(val,   sizeof(val),   "%.3f", s.cell_max_mv / 1000.0);
    snprintf(fname, sizeof(fname), "%s Max Cell Voltage", cfg.name);
    postSensor(ent, val, "V", "voltage", fname);
}

// ============================================================================
// Main
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nHumsienk BMS → Home Assistant (Nabu Casa) starting");
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    secureClient.setInsecure();   // Nabu Casa has a valid cert; skipping CA check keeps things simple
    ensureWifi();
}

void loop() {
    ensureWifi();

    for (size_t i = 0; i < BATTERY_COUNT; i++) {
        const auto& cfg = BATTERIES[i];
        BatterySnapshot s;
        BmsClient bms;
        bool ok = bms.connectAndPoll(cfg, s);
        if (ok) {
            Serial.printf("[%s] V=%.2fV I=%.2fA SOC=%u%% T=%dC cellΔ=%umV\n",
                cfg.name,
                s.voltage_mv / 1000.0,
                s.current_ma / 1000.0,
                s.soc, (int)s.temp_c, s.cell_diff_mv);
            publishBattery(cfg, s);
        } else {
            Serial.printf("[%s] read failed, skipping publish\n", cfg.name);
        }
        delay(500);
    }

    Serial.printf("Sleeping %lus until next cycle\n", POLL_INTERVAL_MS / 1000);
    delay(POLL_INTERVAL_MS);
}
