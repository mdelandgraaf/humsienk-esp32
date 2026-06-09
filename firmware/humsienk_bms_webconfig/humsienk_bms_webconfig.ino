/*
 * Humsienk BMC LiFePO4 BMS monitor for LilyGO TTGO T-Display V1.1
 * (ESP32 + ST7789 240x135)
 *
 * Features:
 *   - Reads 1..N Humsienk BMC batteries over BLE (verified protocol)
 *   - Cycles readings on the TTGO display
 *   - Publishes to Home Assistant via REST API (Nabu Casa or local)
 *   - WEB CONFIG: all settings (WiFi, HA URL/token, battery MACs) set via
 *     a web interface. No recompiling to change config.
 *   - AP MODE: if no WiFi is configured or connection fails, the ESP32
 *     starts its own WiFi access point + captive portal for setup.
 *   - BLE SCAN: web UI can scan for nearby "HS"-prefixed batteries and
 *     add them with one click (multiple supported).
 *
 * Libraries (Arduino IDE -> Library Manager):
 *   - NimBLE-Arduino    (h2zero, v2.x)
 *   - ArduinoJson       (Benoit Blanchon, v7+)
 *   - TFT_eSPI          (Bodmer)   -- configure Setup25_TTGO_T_Display
 * (WebServer, Preferences, DNSServer, WiFi are bundled with ESP32 core.)
 *
 * Board: "ESP32 Dev Module"  |  Partition: "Huge APP (3MB No OTA)"
 *
 * FIRST RUN:
 *   1. Flash. Device has no WiFi config, so it starts an access point:
 *        SSID:  "Humsienk-Setup"   Password: "batterymon"
 *   2. Connect your phone/laptop to that WiFi. A config page should pop up
 *      (or browse to http://192.168.4.1).
 *   3. Enter your WiFi, HA URL, HA token. Scan + add batteries. Save.
 *   4. Device reboots and connects. The config page stays reachable at the
 *      device's IP on your network (shown on the display + serial).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>   // NTP-synced wall clock for the "last seen" timestamp

// ============================================================================
// Forward declarations
// ============================================================================
struct ParsedFrame;
struct BatterySnapshot;

// ============================================================================
// Compile-time limits / AP credentials
// ============================================================================
static const uint8_t  MAX_BATTERIES   = 5;
static const char*    AP_SSID         = "Humsienk-Setup";
static const char*    AP_PASS         = "batterymon";   // >= 8 chars
static const uint32_t POLL_INTERVAL_MS_DEFAULT = 30000;
static const uint32_t SCREEN_CYCLE_MS = 4000;
static const uint32_t CONNECT_TIMEOUT_MS = 4000;   // keep a stalled/sleeping battery from blocking the whole sweep
static const uint32_t REQUEST_TIMEOUT_MS = 2500;

// TTGO T-Display V1.1 buttons
static const int BTN_LEFT  = 0;
static const int BTN_RIGHT = 35;

// ============================================================================
// Persistent configuration (stored in NVS via Preferences)
// ============================================================================
struct Config {
    String   wifiSsid;
    String   wifiPass;
    String   haUrl;          // e.g. https://xxx.ui.nabu.casa  (no trailing slash)
    String   haToken;
    bool     haEnabled = true;
    // MQTT
    bool     mqttEnabled = false;
    String   mqttHost;
    uint16_t mqttPort = 1883;
    String   mqttUser;
    String   mqttPass;
    String   mqttBase = "humsienk";   // base topic
    uint32_t pollMs = POLL_INTERVAL_MS_DEFAULT;
    uint8_t  batteryCount = 0;
    String   batMac[MAX_BATTERIES];
    String   batName[MAX_BATTERIES];
};
static Config cfg;
static Preferences prefs;

static void loadConfig() {
    prefs.begin("humsienk", true);
    cfg.wifiSsid = prefs.getString("wifiSsid", "");
    cfg.wifiPass = prefs.getString("wifiPass", "");
    cfg.haUrl    = prefs.getString("haUrl", "");
    cfg.haToken  = prefs.getString("haToken", "");
    cfg.haEnabled = prefs.getBool("haEn", true);
    cfg.mqttEnabled = prefs.getBool("mqEn", false);
    cfg.mqttHost = prefs.getString("mqHost", "");
    cfg.mqttPort = prefs.getUShort("mqPort", 1883);
    cfg.mqttUser = prefs.getString("mqUser", "");
    cfg.mqttPass = prefs.getString("mqPass", "");
    cfg.mqttBase = prefs.getString("mqBase", "humsienk");
    cfg.pollMs   = prefs.getUInt("pollMs", POLL_INTERVAL_MS_DEFAULT);
    cfg.batteryCount = prefs.getUChar("batCount", 0);
    if (cfg.batteryCount > MAX_BATTERIES) cfg.batteryCount = MAX_BATTERIES;
    for (uint8_t i = 0; i < cfg.batteryCount; i++) {
        cfg.batMac[i]  = prefs.getString(("mac" + String(i)).c_str(), "");
        cfg.batName[i] = prefs.getString(("name" + String(i)).c_str(), "battery" + String(i + 1));
    }
    prefs.end();
}

static void saveConfig() {
    prefs.begin("humsienk", false);
    prefs.putString("wifiSsid", cfg.wifiSsid);
    prefs.putString("wifiPass", cfg.wifiPass);
    prefs.putString("haUrl",    cfg.haUrl);
    prefs.putString("haToken",  cfg.haToken);
    prefs.putBool("haEn",       cfg.haEnabled);
    prefs.putBool("mqEn",       cfg.mqttEnabled);
    prefs.putString("mqHost",   cfg.mqttHost);
    prefs.putUShort("mqPort",   cfg.mqttPort);
    prefs.putString("mqUser",   cfg.mqttUser);
    prefs.putString("mqPass",   cfg.mqttPass);
    prefs.putString("mqBase",   cfg.mqttBase);
    prefs.putUInt("pollMs",     cfg.pollMs);
    prefs.putUChar("batCount",  cfg.batteryCount);
    for (uint8_t i = 0; i < cfg.batteryCount; i++) {
        prefs.putString(("mac" + String(i)).c_str(),  cfg.batMac[i]);
        prefs.putString(("name" + String(i)).c_str(), cfg.batName[i]);
    }
    prefs.end();
}

// ============================================================================
// BLE protocol (verified from decompiled HumsiENK app) - LITTLE ENDIAN
// ============================================================================
static const NimBLEUUID SERVICE_UUID("00000001-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID WRITE_UUID  ("00000002-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID NOTIFY_UUID ("00000003-0000-1000-8000-00805f9b34fb");

static const uint8_t CMD_HANDSHAKE      = 0x00;
static const uint8_t CMD_RUNNING_STATUS = 0x20;
static const uint8_t CMD_BATTERY_INFO   = 0x21;
static const uint8_t CMD_CELL_VOLTAGE   = 0x22;
static const uint8_t CMD_CHARGE_FET     = 0x50;   // data [0x01]=on [0x00]=off
static const uint8_t CMD_DISCHARGE_FET  = 0x51;   // data [0x01]=on [0x00]=off
static const uint8_t SOI                = 0xAA;

struct ParsedFrame { bool ok; uint8_t cmd; const uint8_t* data; uint8_t dataLen; };

struct BatterySnapshot {
    bool     valid          = false;
    uint32_t last_update_ms = 0;
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
    // Running-status flags (from 0x20, verified bit map)
    bool     status_valid   = false;
    bool     chargeFetOn     = false;
    bool     dischargeFetOn  = false;
    uint32_t status_bits     = 0;   // full 32-bit field; decode via STATUS_FLAGS table
};

// Verified bit map for the 0x20 status int (bit index -> meaning).
// Order extracted from parseRunningStatusBits + RunningStatus constructor.
struct StatusFlag { uint8_t bit; const char* key; const char* name; bool isWarning; };
static const StatusFlag STATUS_FLAGS[] = {
    { 0,  "chg_oc_prot",     "Charge Overcurrent Protection",        false},
    { 1,  "chg_ot_prot",     "Charge Over-Temp Protection",          false},
    { 2,  "chg_ut_prot",     "Charge Under-Temp Protection",         false},
    { 3,  "cell_ov_prot",    "Cell Overvoltage Protection",          false},
    { 4,  "pack_ov_prot",    "Pack Overvoltage Protection",          false},
    { 5,  "afe_error",       "AFE Error",                            false},
    { 6,  "chg_stopped",     "Charging Stopped",                     false},
    { 7,  "chg_fet_on",      "Charge FET On",                        false},
    { 8,  "chg_oc_warn",     "Charge Overcurrent Warning",           true },
    { 9,  "chg_ot_warn",     "Charge Over-Temp Warning",             true },
    {10,  "chg_ut_warn",     "Charge Under-Temp Warning",            true },
    {11,  "cell_ov_warn",    "Cell Overvoltage Warning",             true },
    {12,  "pack_ov_warn",    "Pack Overvoltage Warning",             true },
    {13,  "vdiff_warn",      "Voltage Diff Warning",                 true },
    {14,  "vdiff_large",     "Voltage Diff Too Large",               true },
    {15,  "heating",         "Heating",                              false},
    {16,  "dis_oc_prot",     "Discharge Overcurrent Protection",     false},
    {17,  "dis_ot_prot",     "Discharge Over-Temp Protection",       false},
    {18,  "dis_ut_prot",     "Discharge Under-Temp Protection",      false},
    {19,  "cell_uv_prot",    "Cell Undervoltage Protection",         false},
    {20,  "short_circuit",   "Short Circuit Protection",             false},
    {21,  "pack_uv_prot",    "Pack Undervoltage Protection",         false},
    {22,  "dis_stopped",     "Discharging Stopped",                  false},
    {23,  "dis_fet_on",      "Discharge FET On",                     false},
    {24,  "dis_oc_warn",     "Discharge Overcurrent Warning",        true },
    {25,  "dis_ot_warn",     "Discharge Over-Temp Warning",          true },
    {26,  "dis_lt_warn",     "Discharge Low-Temp Warning",           true },
    {27,  "cell_uv_warn",    "Cell Undervoltage Warning",            true },
    {28,  "pack_uv_warn",    "Pack Undervoltage Warning",            true },
    {29,  "mos_ot_warn",     "MOS Over-Temp Warning",                true },
    {30,  "mos_ot_prot",     "MOS Over-Temp Protection",             false},
    {31,  "predis_fet_on",   "Pre-Discharge FET On",                 false},
};
static const uint8_t STATUS_FLAG_COUNT = sizeof(STATUS_FLAGS) / sizeof(STATUS_FLAGS[0]);
static BatterySnapshot snapshots[MAX_BATTERIES];

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
static int32_t  rd_i32_le(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24); }
static uint16_t rd_u16_le(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1]<<8); }

// ============================================================================
// BLE client
// ============================================================================
class BmsClient {
public:
    bool connectAndPoll(const String& mac, BatterySnapshot& s) {
        NimBLEAddress addr(mac.c_str(), BLE_ADDR_PUBLIC);
        client_ = NimBLEDevice::createClient();
        client_->setConnectTimeout(CONNECT_TIMEOUT_MS);
        if (!client_->connect(addr, false)) { cleanup(); return false; }
        auto* svc = client_->getService(SERVICE_UUID);
        if (!svc) { cleanup(); return false; }
        writeChar_  = svc->getCharacteristic(WRITE_UUID);
        notifyChar_ = svc->getCharacteristic(NOTIFY_UUID);
        if (!writeChar_ || !notifyChar_ || !notifyChar_->canNotify()) { cleanup(); return false; }
        notifyChar_->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool){
            this->onNotify(d, l);
        });
        if (!doRequest(CMD_HANDSHAKE, nullptr, 0))   { cleanup(); return false; }
        if (doRequest(CMD_BATTERY_INFO, nullptr, 0)) parseBatteryInfo(s);
        if (doRequest(CMD_CELL_VOLTAGE, nullptr, 0)) parseCellVoltage(s);
        if (doRequest(CMD_RUNNING_STATUS, nullptr, 0)) parseRunningStatus(s);
        s.valid = true; s.last_update_ms = millis();
        cleanup();
        return true;
    }

    // Connect and set a FET (charge or discharge) on/off, then disconnect.
    // cmd = CMD_CHARGE_FET or CMD_DISCHARGE_FET.  on = true -> enable.
    bool connectAndSetFet(const String& mac, uint8_t cmd, bool on) {
        NimBLEAddress addr(mac.c_str(), BLE_ADDR_PUBLIC);
        client_ = NimBLEDevice::createClient();
        client_->setConnectTimeout(CONNECT_TIMEOUT_MS);
        if (!client_->connect(addr, false)) { cleanup(); return false; }
        auto* svc = client_->getService(SERVICE_UUID);
        if (!svc) { cleanup(); return false; }
        writeChar_  = svc->getCharacteristic(WRITE_UUID);
        notifyChar_ = svc->getCharacteristic(NOTIFY_UUID);
        if (!writeChar_ || !notifyChar_ || !notifyChar_->canNotify()) { cleanup(); return false; }
        notifyChar_->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool){
            this->onNotify(d, l);
        });
        bool ok = false;
        if (doRequest(CMD_HANDSHAKE, nullptr, 0)) {
            uint8_t arg = on ? 0x01 : 0x00;
            ok = doRequest(cmd, &arg, 1);   // BMS echoes the command on success
        }
        cleanup();
        return ok;
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
    // 0x20 response (LITTLE-ENDIAN, verified):
    //   [0..1] u16 days, [2] hours, [3] mins,
    //   [4..7] int32 status bitfield, [8..11] cellBalance, [12..14] disconnection
    // FET bits in the status int: chargeFET = bit7 (0x80), dischargeFET = bit23 (0x800000)
    void parseRunningStatus(BatterySnapshot& s) {
        if (lastDataLen_ < 8) return;
        uint32_t st = (uint32_t)rd_i32_le(lastData_ + 4);
        s.status_bits     = st;
        s.chargeFetOn     = (st & 0x00000080UL) != 0;   // bit 7  isChargingFetOn
        s.dischargeFetOn  = (st & 0x00800000UL) != 0;   // bit 23 isDischargingFetOn
        s.status_valid    = true;
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
// BLE scan for HS-prefixed batteries (used by web UI)
// ============================================================================
struct ScanHit { String mac; String name; int rssi; };
static ScanHit scanHits[12];
static uint8_t scanHitCount = 0;

static void scanForBatteries(uint32_t durationMs = 5000) {
    scanHitCount = 0;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults results = scan->getResults(durationMs, false);
    for (int i = 0; i < results.getCount() && scanHitCount < 12; i++) {
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        String name = dev->getName().c_str();
        // Humsienk batteries advertise a name beginning with "HS"
        if (name.startsWith("HS")) {
            scanHits[scanHitCount].mac  = dev->getAddress().toString().c_str();
            scanHits[scanHitCount].name = name;
            scanHits[scanHitCount].rssi = dev->getRSSI();
            scanHitCount++;
        }
    }
    scan->clearResults();
}

// ============================================================================
// HA REST publish
// ============================================================================
WiFiClientSecure secureClient;
WiFiClient plainClient;

// One persistent HTTPClient kept alive across all the POSTs in a sweep. With
// setReuse(true) the underlying TCP/TLS socket stays open, so the (expensive,
// ~1-2s over Nabu Casa) TLS handshake is paid roughly once per sweep instead
// of once per entity. This is the single biggest win for REST publish speed.
static HTTPClient httpHA;

// Per-battery change tracking so we only POST protection/warning flags that
// actually changed, instead of all ~30 every cycle. A periodic full refresh
// self-heals any missed update.
static uint32_t lastStatusBitsPub[MAX_BATTERIES] = {0};
static bool     statusEverPub[MAX_BATTERIES]     = {false};
static uint16_t haSweepCounter = 0;
static const uint16_t HA_FLAG_FULL_REFRESH_EVERY = 10;   // force-publish all flags every N sweeps

static bool postSensor(const char* entity_id, const char* state, const char* unit,
                       const char* device_class, const char* friendly_name) {
    if (!cfg.haEnabled || cfg.haUrl.isEmpty() || cfg.haToken.isEmpty()) return false;
    String url = cfg.haUrl + "/api/states/" + entity_id;
    httpHA.setReuse(true);          // keep-alive: reuse the socket for the next entity
    httpHA.setTimeout(5000);
    bool ok;
    if (cfg.haUrl.startsWith("https"))
        ok = httpHA.begin(secureClient, url);
    else
        ok = httpHA.begin(plainClient, url);
    if (!ok) return false;
    httpHA.addHeader("Authorization", String("Bearer ") + cfg.haToken);
    httpHA.addHeader("Content-Type", "application/json");
    JsonDocument body;
    body["state"] = state;
    JsonObject attr = body["attributes"].to<JsonObject>();
    attr["unit_of_measurement"] = unit;
    if (device_class && device_class[0]) {
        attr["device_class"] = device_class;
        attr["state_class"]  = "measurement";
    }
    attr["friendly_name"] = friendly_name;
    String payload; serializeJson(body, payload);
    int code = httpHA.POST(payload);
    httpHA.end();                   // with reuse=true this keeps the connection open
    return (code == 200 || code == 201);
}

// Current UTC time as ISO8601, or "" if NTP hasn't synced yet.
static String isoNow() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "";          // < ~2023-11 -> clock not set yet
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
    return String(buf);
}

// Publish a Home Assistant 'timestamp' sensor (no unit/state_class). Used for
// the per-battery last_seen entity, which stops advancing when a battery drops
// off so HA shows "x minutes ago" and you can alert on staleness.
static bool postTimestampSensor(const char* entity_id, const char* iso,
                                const char* friendly_name) {
    if (!cfg.haEnabled || cfg.haUrl.isEmpty() || cfg.haToken.isEmpty()) return false;
    if (!iso || !iso[0]) return false;
    String url = cfg.haUrl + "/api/states/" + entity_id;
    httpHA.setReuse(true);
    httpHA.setTimeout(5000);
    bool ok;
    if (cfg.haUrl.startsWith("https"))
        ok = httpHA.begin(secureClient, url);
    else
        ok = httpHA.begin(plainClient, url);
    if (!ok) return false;
    httpHA.addHeader("Authorization", String("Bearer ") + cfg.haToken);
    httpHA.addHeader("Content-Type", "application/json");
    JsonDocument body;
    body["state"] = iso;
    JsonObject attr = body["attributes"].to<JsonObject>();
    attr["device_class"]  = "timestamp";
    attr["friendly_name"] = friendly_name;
    String payload; serializeJson(body, payload);
    int code = httpHA.POST(payload);
    httpHA.end();
    return (code == 200 || code == 201);
}

static void publishBattery(uint8_t idx, const String& name, const BatterySnapshot& s) {
    char ent[64], fname[64], vbuf[32];
    #define PUB(suffix, unit, dclass, label) do { \
        snprintf(ent,   sizeof(ent),   "sensor.%s_" suffix, name.c_str()); \
        snprintf(fname, sizeof(fname), "%s " label,         name.c_str()); \
        postSensor(ent, vbuf, unit, dclass, fname); \
    } while (0)
    snprintf(vbuf, sizeof(vbuf), "%u", s.soc);                      PUB("soc",      "%",  "battery",  "SOC");
    snprintf(vbuf, sizeof(vbuf), "%.3f", s.voltage_v);              PUB("voltage",  "V",  "voltage",  "Voltage");
    snprintf(vbuf, sizeof(vbuf), "%.3f", s.current_a);              PUB("current",  "A",  "current",  "Current");
    snprintf(vbuf, sizeof(vbuf), "%.1f", s.voltage_v*s.current_a);  PUB("power",    "W",  "power",    "Power");
    snprintf(vbuf, sizeof(vbuf), "%.2f", s.remaining_mah/1000.0);   PUB("remaining_capacity", "Ah", "", "Remaining Capacity");
    snprintf(vbuf, sizeof(vbuf), "%.2f", s.full_mah/1000.0);        PUB("full_capacity", "Ah", "", "Full Capacity");
    snprintf(vbuf, sizeof(vbuf), "%u", s.soh);                      PUB("soh",      "%",  "",         "SOH");
    snprintf(vbuf, sizeof(vbuf), "%d", (int)s.temp_c);              PUB("temperature", "°C", "temperature", "Temperature");
    snprintf(vbuf, sizeof(vbuf), "%u", s.cycles);                   PUB("cycles",   "",   "",         "Cycles");
    snprintf(vbuf, sizeof(vbuf), "%u", s.cell_diff_mv);             PUB("cell_diff", "mV", "voltage", "Cell Voltage Diff");
    if (s.status_valid) {
        snprintf(vbuf, sizeof(vbuf), "%s", s.chargeFetOn ? "on" : "off");    PUB("charge_fet",    "", "", "Charge FET");
        snprintf(vbuf, sizeof(vbuf), "%s", s.dischargeFetOn ? "on" : "off"); PUB("discharge_fet", "", "", "Discharge FET");
        // Protection/warning flags as on/off. These almost never change, so only
        // POST the ones that flipped since last publish (with a periodic full
        // refresh). This trims a steady-state sweep from ~42 to ~12 POSTs/battery.
        bool forceAll = !statusEverPub[idx] ||
                        (haSweepCounter % HA_FLAG_FULL_REFRESH_EVERY == 0);
        uint32_t changed = s.status_bits ^ lastStatusBitsPub[idx];
        for (uint8_t k = 0; k < STATUS_FLAG_COUNT; k++) {
            const StatusFlag& fl = STATUS_FLAGS[k];
            // Skip the FET-on bits already published above (7,23) and informational on-states
            if (fl.bit == 7 || fl.bit == 23) continue;
            if (!forceAll && !((changed >> fl.bit) & 1)) continue;   // unchanged -> skip
            bool active = (s.status_bits >> fl.bit) & 1;
            char ent2[80], fname2[96];
            snprintf(ent2,   sizeof(ent2),   "sensor.%s_%s", name.c_str(), fl.key);
            snprintf(fname2, sizeof(fname2), "%s %s", name.c_str(), fl.name);
            postSensor(ent2, active ? "on" : "off", "", "", fname2);
        }
        lastStatusBitsPub[idx] = s.status_bits;
        statusEverPub[idx]     = true;
    }
    // last_seen: only updated on a successful read, so a battery that drops off
    // leaves this frozen -> HA shows "x ago" and stops advancing.
    String iso = isoNow();
    if (iso.length()) {
        char ent2[80], fname2[96];
        snprintf(ent2,   sizeof(ent2),   "sensor.%s_last_seen", name.c_str());
        snprintf(fname2, sizeof(fname2), "%s Last Seen", name.c_str());
        postTimestampSensor(ent2, iso.c_str(), fname2);
    }
    #undef PUB
}

// ============================================================================
// MQTT  (local broker; supports HA MQTT discovery + FET control topics)
// ============================================================================
WiFiClient   mqttNet;
PubSubClient mqtt(mqttNet);

// Forward declare: a queued FET command from MQTT, handled in main loop
struct FetCommand { bool pending=false; uint8_t batIndex; uint8_t cmd; bool on; };
static FetCommand pendingFet;

static String mqttStateTopic(const String& batName) { return cfg.mqttBase + "/" + batName + "/state"; }
static String mqttCmdTopicCharge(const String& batName)    { return cfg.mqttBase + "/" + batName + "/charge/set"; }
static String mqttCmdTopicDischarge(const String& batName) { return cfg.mqttBase + "/" + batName + "/discharge/set"; }

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
    String t = topic;
    String msg; for (unsigned i = 0; i < len; i++) msg += (char)payload[i];
    msg.trim(); msg.toUpperCase();
    bool on = (msg == "ON" || msg == "1" || msg == "TRUE");
    for (uint8_t i = 0; i < cfg.batteryCount; i++) {
        if (t == mqttCmdTopicCharge(cfg.batName[i])) {
            pendingFet = { true, i, CMD_CHARGE_FET, on }; return;
        }
        if (t == mqttCmdTopicDischarge(cfg.batName[i])) {
            pendingFet = { true, i, CMD_DISCHARGE_FET, on }; return;
        }
    }
}

static void mqttPublishDiscovery() {
    // Publish HA MQTT-discovery configs so entities + switches auto-appear.
    if (!mqtt.connected()) return;
    for (uint8_t i = 0; i < cfg.batteryCount; i++) {
        const String& n = cfg.batName[i];
        String devId = cfg.mqttBase + "_" + n;
        String devBlock = "\"dev\":{\"ids\":[\"" + devId + "\"],\"name\":\"" + n +
                          "\",\"mf\":\"Humsienk\",\"mdl\":\"BMC LiFePO4\"}";
        String state = mqttStateTopic(n);

        struct { const char* key; const char* name; const char* unit; const char* dc; const char* tmpl; } sensors[] = {
            {"soc","SOC","%","battery","{{ value_json.soc }}"},
            {"voltage","Voltage","V","voltage","{{ value_json.voltage }}"},
            {"current","Current","A","current","{{ value_json.current }}"},
            {"power","Power","W","power","{{ value_json.power }}"},
            {"temperature","Temperature","\\u00b0C","temperature","{{ value_json.temp }}"},
            {"remaining","Remaining Capacity","Ah","","{{ value_json.remaining_ah }}"},
            {"soh","SOH","%","","{{ value_json.soh }}"},
            {"cell_diff","Cell Diff","mV","","{{ value_json.cell_diff_mv }}"},
        };
        for (auto& s : sensors) {
            String cfgTopic = "homeassistant/sensor/" + devId + "_" + s.key + "/config";
            String payload = "{\"name\":\"" + String(s.name) + "\",";
            payload += "\"uniq_id\":\"" + devId + "_" + s.key + "\",";
            payload += "\"stat_t\":\"" + state + "\",";
            payload += "\"val_tpl\":\"" + String(s.tmpl) + "\",";
            if (s.unit[0]) payload += "\"unit_of_meas\":\"" + String(s.unit) + "\",";
            if (s.dc[0])   payload += "\"dev_cla\":\"" + String(s.dc) + "\",\"stat_cla\":\"measurement\",";
            payload += devBlock + "}";
            mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
        // Two switches: charge + discharge
        struct { const char* key; const char* name; String cmdT; const char* tmpl; } sw[] = {
            {"charge","Charge FET", mqttCmdTopicCharge(n), "{{ value_json.charge }}"},
            {"discharge","Discharge FET", mqttCmdTopicDischarge(n), "{{ value_json.discharge }}"},
        };
        for (auto& w : sw) {
            String cfgTopic = "homeassistant/switch/" + devId + "_" + w.key + "/config";
            String payload = "{\"name\":\"" + String(w.name) + "\",";
            payload += "\"uniq_id\":\"" + devId + "_" + w.key + "\",";
            payload += "\"cmd_t\":\"" + w.cmdT + "\",";
            payload += "\"stat_t\":\"" + state + "\",";
            payload += "\"val_tpl\":\"" + String(w.tmpl) + "\",";
            payload += "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",";
            payload += devBlock + "}";
            mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
        // Binary sensors for every protection / warning / info flag
        for (uint8_t k = 0; k < STATUS_FLAG_COUNT; k++) {
            const StatusFlag& fl = STATUS_FLAGS[k];
            if (fl.bit == 7 || fl.bit == 23) continue;  // FETs are switches, handled above
            String cfgTopic = "homeassistant/binary_sensor/" + devId + "_" + fl.key + "/config";
            String payload = "{\"name\":\"" + String(fl.name) + "\",";
            payload += "\"uniq_id\":\"" + devId + "_" + fl.key + "\",";
            payload += "\"stat_t\":\"" + state + "\",";
            payload += "\"val_tpl\":\"{{ value_json." + String(fl.key) + " }}\",";
            payload += "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",";
            // Warnings + protections get device_class 'problem' so HA shows them red when active
            if (fl.isWarning || String(fl.key).indexOf("prot") >= 0 ||
                String(fl.key) == "afe_error" || String(fl.key) == "short_circuit")
                payload += "\"dev_cla\":\"problem\",";
            payload += devBlock + "}";
            mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
        }
    }
}

static void mqttReconnect() {
    if (!cfg.mqttEnabled || cfg.mqttHost.isEmpty()) return;
    if (mqtt.connected()) return;
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    mqtt.setBufferSize(1024);
    mqtt.setCallback(mqttCallback);
    String cid = "humsienk-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = cfg.mqttUser.isEmpty()
        ? mqtt.connect(cid.c_str())
        : mqtt.connect(cid.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
    if (ok) {
        Serial.println("MQTT connected");
        for (uint8_t i = 0; i < cfg.batteryCount; i++) {
            mqtt.subscribe(mqttCmdTopicCharge(cfg.batName[i]).c_str());
            mqtt.subscribe(mqttCmdTopicDischarge(cfg.batName[i]).c_str());
        }
        mqttPublishDiscovery();
    } else {
        Serial.printf("MQTT connect failed rc=%d\n", mqtt.state());
    }
}

// FET state cache (so the switch reflects last commanded state)
static bool chargeState[MAX_BATTERIES];
static bool dischargeState[MAX_BATTERIES];
static bool fetStateKnown[MAX_BATTERIES];

static void mqttPublishState(uint8_t i, const BatterySnapshot& s) {
    if (!mqtt.connected()) return;
    JsonDocument doc;
    doc["soc"] = s.soc;
    doc["voltage"] = round(s.voltage_v * 1000) / 1000.0;
    doc["current"] = round(s.current_a * 1000) / 1000.0;
    doc["power"]   = round(s.voltage_v * s.current_a * 10) / 10.0;
    doc["temp"]    = (int)s.temp_c;
    doc["soh"]     = s.soh;
    doc["remaining_ah"] = round(s.remaining_mah / 1000.0 * 100) / 100.0;
    doc["cycles"]  = s.cycles;
    doc["cell_diff_mv"] = s.cell_diff_mv;
    if (s.status_valid) {
        // Real, read-back FET states (verified bit decode)
        doc["charge"]    = s.chargeFetOn ? "ON" : "OFF";
        doc["discharge"] = s.dischargeFetOn ? "ON" : "OFF";
        // All protection/warning/info flags as ON/OFF
        for (uint8_t k = 0; k < STATUS_FLAG_COUNT; k++) {
            const StatusFlag& fl = STATUS_FLAGS[k];
            if (fl.bit == 7 || fl.bit == 23) continue;  // already covered
            doc[fl.key] = ((s.status_bits >> fl.bit) & 1) ? "ON" : "OFF";
        }
    } else if (fetStateKnown[i]) {
        // Fallback to last-commanded if a status read hasn't landed yet
        doc["charge"]    = chargeState[i] ? "ON" : "OFF";
        doc["discharge"] = dischargeState[i] ? "ON" : "OFF";
    }
    String payload; serializeJson(doc, payload);
    mqtt.publish(mqttStateTopic(cfg.batName[i]).c_str(), payload.c_str(), true);
}

// ============================================================================
// Display
// ============================================================================
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
static size_t currentScreen = 0;
static uint32_t lastScreenAdvanceMs = 0;
static bool cyclingPaused = false;
static bool backlightOn = true;

static uint16_t socColor(uint8_t soc, float current_a) {
    if (current_a > 0.1f) return TFT_BLUE;
    if (soc >= 50)        return TFT_GREEN;
    if (soc >= 20)        return TFT_ORANGE;
    return TFT_RED;
}
static void drawStatusScreen(const char* l1, const char* l2, const char* l3 = nullptr) {
    spr.fillSprite(TFT_BLACK);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(MC_DATUM);
    spr.setTextFont(4);
    spr.drawString(l1, 120, 40);
    spr.setTextFont(2);
    spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    spr.drawString(l2, 120, 76);
    if (l3) { spr.setTextColor(TFT_CYAN, TFT_BLACK); spr.drawString(l3, 120, 100); }
    spr.pushSprite(0, 0);
}
static void drawBatteryScreen(size_t idx) {
    if (idx >= cfg.batteryCount) { drawStatusScreen("No batteries", "add via web config"); return; }
    const BatterySnapshot& s = snapshots[idx];
    spr.fillSprite(TFT_BLACK);
    spr.fillRect(0, 0, 240, 18, TFT_DARKGREY);
    spr.setTextColor(TFT_WHITE, TFT_DARKGREY);
    spr.setTextDatum(TL_DATUM); spr.setTextFont(2);
    spr.drawString(cfg.batName[idx], 4, 1);
    char idxbuf[16];
    snprintf(idxbuf, sizeof(idxbuf), "%u/%u", (unsigned)(idx + 1), (unsigned)cfg.batteryCount);
    spr.setTextDatum(TR_DATUM); spr.drawString(idxbuf, 236, 1);

    if (!s.valid) {
        spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        spr.setTextDatum(MC_DATUM); spr.setTextFont(2);
        spr.drawString("waiting for first read...", 120, 70);
        spr.pushSprite(0, 0); return;
    }
    spr.setTextColor(socColor(s.soc, s.current_a), TFT_BLACK);
    spr.setTextDatum(ML_DATUM); spr.setTextFont(8);
    char socbuf[8]; snprintf(socbuf, sizeof(socbuf), "%u", s.soc);
    spr.drawString(socbuf, 6, 70);
    int x_after = spr.textWidth(socbuf, 8) + 6;
    spr.setTextFont(4); spr.setTextDatum(BL_DATUM);
    spr.drawString("%", x_after + 4, 108);

    spr.setTextColor(TFT_WHITE, TFT_BLACK); spr.setTextFont(2); spr.setTextDatum(TR_DATUM);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f V", s.voltage_v);  spr.drawString(buf, 236, 28);
    float cur_a = s.current_a;
    snprintf(buf, sizeof(buf), "%+.2f A", cur_a);
    spr.setTextColor(cur_a > 0.1f ? TFT_BLUE : (cur_a < -0.1f ? TFT_YELLOW : TFT_LIGHTGREY), TFT_BLACK);
    spr.drawString(buf, 236, 48);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(buf, sizeof(buf), "%d C", (int)s.temp_c);  spr.drawString(buf, 236, 68);
    snprintf(buf, sizeof(buf), "%.1fAh", s.remaining_mah / 1000.0); spr.drawString(buf, 236, 88);

    spr.setTextColor(TFT_DARKGREY, TFT_BLACK); spr.setTextDatum(BL_DATUM); spr.setTextFont(1);
    uint32_t age_s = (millis() - s.last_update_ms) / 1000;
    snprintf(buf, sizeof(buf), "SOH %u%%  dV %umV  cyc %u  %lus ago",
             s.soh, s.cell_diff_mv, s.cycles, (unsigned long)age_s);
    spr.drawString(buf, 4, 132);
    if (cyclingPaused) spr.fillCircle(228, 9, 5, TFT_YELLOW);
    spr.pushSprite(0, 0);
}

// ============================================================================
// Buttons
// ============================================================================
static bool btnLeftPrev = HIGH, btnRightPrev = HIGH;
static uint32_t btnLeftDeb = 0, btnRightDeb = 0;
static void pollButtons() {
    bool l = digitalRead(BTN_LEFT), r = digitalRead(BTN_RIGHT);
    uint32_t now = millis();
    if (l != btnLeftPrev && (now - btnLeftDeb) > 40) {
        btnLeftDeb = now;
        if (l == LOW && cfg.batteryCount > 0) {
            if (cyclingPaused) { currentScreen = (currentScreen + 1) % cfg.batteryCount; }
            else cyclingPaused = true;
            drawBatteryScreen(currentScreen);
        }
        btnLeftPrev = l;
    }
    if (r != btnRightPrev && (now - btnRightDeb) > 40) {
        btnRightDeb = now;
        if (r == LOW) {
            if (cyclingPaused) { cyclingPaused = false; drawBatteryScreen(currentScreen); }
            else { backlightOn = !backlightOn; digitalWrite(TFT_BL, backlightOn ? HIGH : LOW); }
        }
        btnRightPrev = r;
    }
}

// ============================================================================
// Web server + captive portal
// ============================================================================
WebServer  server(80);
DNSServer  dnsServer;
bool       apMode = false;

static String htmlHeader(const String& title) {
    return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>" + title + "</title><style>"
           "body{font-family:system-ui,sans-serif;max-width:640px;margin:0 auto;padding:16px;background:#111;color:#eee}"
           "h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.4em;border-bottom:1px solid #333;padding-bottom:4px}"
           "label{display:block;margin:10px 0 3px;font-size:.9em;color:#aaa}"
           "input{width:100%;padding:9px;border:1px solid #444;border-radius:6px;background:#1c1c1c;color:#eee;box-sizing:border-box}"
           "button{margin-top:14px;padding:11px 16px;border:0;border-radius:6px;background:#0a84ff;color:#fff;font-size:1em;cursor:pointer}"
           "button.sec{background:#333}.bat{border:1px solid #333;border-radius:8px;padding:10px;margin:8px 0}"
           ".hit{display:flex;justify-content:space-between;align-items:center;border:1px solid #2a2a2a;border-radius:6px;padding:8px;margin:6px 0}"
           ".muted{color:#888;font-size:.85em}a{color:#0a84ff}"
           "</style></head><body>";
}
static String htmlFooter() { return "</body></html>"; }

static void handleRoot() {
    String h = htmlHeader("Humsienk BMS Config");
    h += "<h1>Humsienk BMS Monitor</h1>";
    if (apMode) h += "<p class='muted'>Setup mode (AP). Enter your WiFi to connect to your network.</p>";
    else h += "<p class='muted'>Connected: " + WiFi.localIP().toString() + "</p>";

    h += "<form method='POST' action='/save'>";
    h += "<h2>WiFi</h2>";
    h += "<label>SSID</label><input name='ssid' value='" + cfg.wifiSsid + "'>";
    h += "<label>Password</label><input name='wpass' type='password' value='" + cfg.wifiPass + "'>";

    h += "<h2>Home Assistant (REST)</h2>";
    h += "<label><input type='checkbox' name='haen' style='width:auto' " + String(cfg.haEnabled ? "checked" : "") + "> Enable REST publishing</label>";
    h += "<label>URL (no trailing slash, e.g. https://xxxx.ui.nabu.casa)</label>";
    h += "<input name='haurl' value='" + cfg.haUrl + "'>";
    h += "<label>Long-Lived Access Token</label>";
    h += "<input name='hatoken' type='password' value='" + cfg.haToken + "'>";

    h += "<h2>MQTT (local broker)</h2>";
    h += "<label><input type='checkbox' name='mqen' style='width:auto' " + String(cfg.mqttEnabled ? "checked" : "") + "> Enable MQTT (auto-discovery + switches)</label>";
    h += "<label>Broker host/IP</label><input name='mqhost' value='" + cfg.mqttHost + "'>";
    h += "<label>Port</label><input name='mqport' type='number' value='" + String(cfg.mqttPort) + "'>";
    h += "<label>Username (optional)</label><input name='mquser' value='" + cfg.mqttUser + "'>";
    h += "<label>Password (optional)</label><input name='mqpass' type='password' value='" + cfg.mqttPass + "'>";
    h += "<label>Base topic</label><input name='mqbase' value='" + cfg.mqttBase + "'>";

    h += "<label>Poll interval (seconds)</label>";
    h += "<input name='poll' type='number' min='10' value='" + String(cfg.pollMs / 1000) + "'>";

    h += "<h2>Batteries (" + String(cfg.batteryCount) + "/" + String(MAX_BATTERIES) + ")</h2>";
    for (uint8_t i = 0; i < cfg.batteryCount; i++) {
        h += "<div class='bat'>";
        h += "<label>Name</label><input name='bname" + String(i) + "' value='" + cfg.batName[i] + "'>";
        h += "<label>MAC</label><input name='bmac" + String(i) + "' value='" + cfg.batMac[i] + "'>";
        h += "</div>";
    }
    h += "<button type='submit'>Save &amp; Reboot</button>";
    h += "</form>";

    // FET control (outside the save form so buttons act immediately)
    if (!apMode && cfg.batteryCount > 0) {
        h += "<h2>Charge / Discharge control</h2>";
        h += "<p class='muted'>Toggles the battery's MOSFETs, like the official app. "
             "Turning discharge OFF cuts power to loads; charge OFF stops charging.</p>";
        for (uint8_t i = 0; i < cfg.batteryCount; i++) {
            const BatterySnapshot& s = snapshots[i];
            String cur = "";
            if (s.status_valid) {
                cur = " <span class='muted'>(now: chg " + String(s.chargeFetOn ? "ON" : "OFF") +
                      ", dis " + String(s.dischargeFetOn ? "ON" : "OFF") + ")</span>";
                // list any active protection/warning flags
                String active = "";
                for (uint8_t k = 0; k < STATUS_FLAG_COUNT; k++) {
                    const StatusFlag& fl = STATUS_FLAGS[k];
                    if (fl.bit == 7 || fl.bit == 23 || fl.bit == 15 || fl.bit == 31) continue; // skip FET/heating info bits
                    if ((s.status_bits >> fl.bit) & 1) {
                        if (active.length()) active += ", ";
                        active += fl.name;
                    }
                }
                if (active.length())
                    cur += "<br><span style='color:#ff5b5b'>\u26a0 " + active + "</span>";
            }
            h += "<div class='bat'><b>" + cfg.batName[i] + "</b>" + cur + "<br>";
            h += "Charge: "
                 "<a href='/fet?b=" + String(i) + "&t=c&v=1' onclick=\"return confirm('Enable charging?')\"><button class='sec' type='button'>ON</button></a> "
                 "<a href='/fet?b=" + String(i) + "&t=c&v=0' onclick=\"return confirm('Disable charging?')\"><button class='sec' type='button'>OFF</button></a><br>";
            h += "Discharge: "
                 "<a href='/fet?b=" + String(i) + "&t=d&v=1' onclick=\"return confirm('Enable discharge?')\"><button class='sec' type='button'>ON</button></a> "
                 "<a href='/fet?b=" + String(i) + "&t=d&v=0' onclick=\"return confirm('WARNING: disabling discharge cuts power to loads. Continue?')\"><button type='button'>OFF</button></a>";
            h += "</div>";
        }
    }

    h += "<h2>Find batteries</h2>";
    h += "<p class='muted'>Scan for nearby Humsienk (HS...) batteries and add them.</p>";
    h += "<form method='POST' action='/scan'><button class='sec' type='submit'>Scan now (5s)</button></form>";

    h += htmlFooter();
    server.send(200, "text/html", h);
}

static void handleFet() {
    int b = server.arg("b").toInt();
    String t = server.arg("t");
    bool on = server.arg("v") == "1";
    if (b < 0 || b >= cfg.batteryCount) { server.send(400, "text/plain", "bad battery"); return; }
    uint8_t cmd = (t == "c") ? CMD_CHARGE_FET : CMD_DISCHARGE_FET;
    // Queue it for the main loop (avoid BLE work inside the web handler)
    pendingFet = { true, (uint8_t)b, cmd, on };
    String h = htmlHeader("FET command");
    h += "<h1>Command queued</h1><p>" + cfg.batName[b] + ": " +
         String(t == "c" ? "charge" : "discharge") + " -> " + (on ? "ON" : "OFF") +
         "</p><p class='muted'>Applied on next BLE contact (a few seconds).</p><a href='/'>back</a>";
    h += htmlFooter();
    server.send(200, "text/html", h);
}

static void handleScan() {
    drawStatusScreen("BLE scan", "looking for HS...");
    scanForBatteries(5000);
    String h = htmlHeader("Scan results");
    h += "<h1>Scan results</h1><p class='muted'>Found " + String(scanHitCount) + " HS device(s).</p>";
    if (scanHitCount == 0) h += "<p>Nothing found. Make sure batteries are awake and in range.</p>";
    h += "<form method='POST' action='/add'>";
    for (uint8_t i = 0; i < scanHitCount; i++) {
        h += "<div class='hit'><div><b>" + scanHits[i].name + "</b><br><span class='muted'>" +
             scanHits[i].mac + "  (" + String(scanHits[i].rssi) + " dBm)</span></div>"
             "<label style='margin:0'><input type='checkbox' name='add" + String(i) + "' value='1' style='width:auto'> add</label></div>";
        h += "<input type='hidden' name='mac" + String(i) + "' value='" + scanHits[i].mac + "'>";
        h += "<input type='hidden' name='nm" + String(i) + "' value='" + scanHits[i].name + "'>";
    }
    if (scanHitCount > 0) h += "<button type='submit'>Add selected</button>";
    h += " <a href='/'>back</a></form>";
    h += htmlFooter();
    server.send(200, "text/html", h);
}

static void handleAdd() {
    for (uint8_t i = 0; i < scanHitCount && cfg.batteryCount < MAX_BATTERIES; i++) {
        if (server.hasArg("add" + String(i))) {
            String mac = server.arg("mac" + String(i));
            // skip duplicates
            bool dup = false;
            for (uint8_t j = 0; j < cfg.batteryCount; j++) if (cfg.batMac[j] == mac) dup = true;
            if (dup) continue;
            uint8_t idx = cfg.batteryCount;
            cfg.batMac[idx]  = mac;
            cfg.batName[idx] = "battery" + String(idx + 1);
            cfg.batteryCount++;
        }
    }
    saveConfig();
    server.sendHeader("Location", "/");
    server.send(303);
}

static void handleSave() {
    cfg.wifiSsid = server.arg("ssid");
    cfg.wifiPass = server.arg("wpass");
    cfg.haEnabled = server.hasArg("haen");
    cfg.haUrl    = server.arg("haurl");
    cfg.haToken  = server.arg("hatoken");
    cfg.mqttEnabled = server.hasArg("mqen");
    cfg.mqttHost = server.arg("mqhost");
    cfg.mqttPort = server.arg("mqport").toInt();
    if (cfg.mqttPort == 0) cfg.mqttPort = 1883;
    cfg.mqttUser = server.arg("mquser");
    cfg.mqttPass = server.arg("mqpass");
    cfg.mqttBase = server.arg("mqbase");
    if (cfg.mqttBase.isEmpty()) cfg.mqttBase = "humsienk";
    uint32_t pollSec = server.arg("poll").toInt();
    if (pollSec < 10) pollSec = 10;
    cfg.pollMs = pollSec * 1000;
    for (uint8_t i = 0; i < cfg.batteryCount; i++) {
        if (server.hasArg("bname" + String(i))) cfg.batName[i] = server.arg("bname" + String(i));
        if (server.hasArg("bmac"  + String(i))) cfg.batMac[i]  = server.arg("bmac"  + String(i));
    }
    saveConfig();
    String h = htmlHeader("Saved");
    h += "<h1>Saved</h1><p>Rebooting...</p>" + htmlFooter();
    server.send(200, "text/html", h);
    delay(800);
    ESP.restart();
}

static void startWebServer() {
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/scan", HTTP_POST, handleScan);
    server.on("/add",  HTTP_POST, handleAdd);
    server.on("/fet",  HTTP_GET,  handleFet);
    server.onNotFound([]() {           // captive-portal catch-all
        if (apMode) { server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString()); server.send(302); }
        else server.send(404, "text/plain", "Not found");
    });
    server.begin();
}

// ============================================================================
// WiFi bring-up
// ============================================================================
static bool connectWifi() {
    if (cfg.wifiSsid.isEmpty()) return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    drawStatusScreen("WiFi", ("connecting to " + cfg.wifiSsid).c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000) { delay(300); }
    return WiFi.status() == WL_CONNECTED;
}

static void startAP() {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());   // captive portal
    drawStatusScreen("Setup mode", AP_SSID, WiFi.softAPIP().toString().c_str());
}

// ============================================================================
// Setup / loop
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nHumsienk BMS monitor (web-config)");

    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    tft.init(); tft.setRotation(1);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    spr.setColorDepth(8); spr.createSprite(240, 135);
    drawStatusScreen("Starting", "loading config...");

    loadConfig();

    NimBLEDevice::init("");
    NimBLEDevice::setMTU(247);

    secureClient.setInsecure();

    if (connectWifi()) {
        Serial.print("WiFi OK: "); Serial.println(WiFi.localIP());
        // Start NTP so the per-battery last_seen timestamp is real wall-clock time.
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC; HA renders local
        drawStatusScreen("WiFi OK", WiFi.localIP().toString().c_str(), "web config on this IP");
        delay(1200);
    } else {
        Serial.println("No WiFi -> starting AP");
        startAP();
    }
    startWebServer();
}

static uint32_t lastPollMs = 0;

void loop() {
    if (apMode) dnsServer.processNextRequest();
    server.handleClient();
    pollButtons();

    // MQTT upkeep
    if (!apMode && cfg.mqttEnabled) {
        if (!mqtt.connected()) {
            static uint32_t lastTry = 0;
            if (millis() - lastTry > 5000) { lastTry = millis(); mqttReconnect(); }
        }
        mqtt.loop();
    }

    // Execute a pending FET command (from web or MQTT) ASAP
    if (pendingFet.pending) {
        FetCommand c = pendingFet;          // copy
        pendingFet.pending = false;
        if (c.batIndex < cfg.batteryCount && !cfg.batMac[c.batIndex].isEmpty()) {
            BmsClient bms;
            bool ok = bms.connectAndSetFet(cfg.batMac[c.batIndex], c.cmd, c.on);
            Serial.printf("[%s] %s FET -> %s : %s\n", cfg.batName[c.batIndex].c_str(),
                c.cmd == CMD_CHARGE_FET ? "charge" : "discharge",
                c.on ? "ON" : "OFF", ok ? "ok" : "FAILED");
            if (ok) {
                fetStateKnown[c.batIndex] = true;
                if (c.cmd == CMD_CHARGE_FET) chargeState[c.batIndex] = c.on;
                else                         dischargeState[c.batIndex] = c.on;
                if (cfg.mqttEnabled && mqtt.connected())
                    mqttPublishState(c.batIndex, snapshots[c.batIndex]);
            }
        }
    }

    if (!apMode && cfg.batteryCount > 0 &&
        (lastPollMs == 0 || (millis() - lastPollMs) >= cfg.pollMs)) {
        lastPollMs = millis();
        haSweepCounter++;
        for (uint8_t i = 0; i < cfg.batteryCount; i++) {
            if (cfg.batMac[i].isEmpty()) continue;
            BmsClient bms;
            BatterySnapshot fresh = snapshots[i];
            if (bms.connectAndPoll(cfg.batMac[i], fresh)) {
                snapshots[i] = fresh;
                if (fresh.status_valid) {
                    fetStateKnown[i]   = true;
                    chargeState[i]     = fresh.chargeFetOn;
                    dischargeState[i]  = fresh.dischargeFetOn;
                }
                Serial.printf("[%s] V=%.2f I=%.2f SOC=%u%% chgFET=%d disFET=%d\n",
                    cfg.batName[i].c_str(), fresh.voltage_v, fresh.current_a, fresh.soc,
                    fresh.chargeFetOn, fresh.dischargeFetOn);
                publishBattery(i, cfg.batName[i], fresh);
                if (cfg.mqttEnabled && mqtt.connected()) mqttPublishState(i, fresh);
            } else {
                Serial.printf("[%s] read failed\n", cfg.batName[i].c_str());
            }
            if (i == currentScreen) drawBatteryScreen(currentScreen);
            server.handleClient();
            if (cfg.mqttEnabled) mqtt.loop();
            pollButtons();
            delay(300);
        }
    }

    if (!apMode && !cyclingPaused && cfg.batteryCount > 0 &&
        (millis() - lastScreenAdvanceMs) >= SCREEN_CYCLE_MS) {
        lastScreenAdvanceMs = millis();
        currentScreen = (currentScreen + 1) % cfg.batteryCount;
        drawBatteryScreen(currentScreen);
    }

    delay(10);
}
