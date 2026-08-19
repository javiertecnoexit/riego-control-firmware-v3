#include "hardware.h"

#include <Arduino.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <OneWire.h>
#include <Preferences.h>
#include <RTClib.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_timer.h>

// ============================================================================
// hardware.cpp — Capa de hardware V3 (fusion de V2: buttons, display, relay,
// rtc, sensors, calibration, onewire_map). Hardware identico a V2 (P7).
// ============================================================================

// ----------------------------------------------------------------------------
// Botones — debounce no bloqueante (igual que V2)
// ----------------------------------------------------------------------------
#define DEBOUNCE_MS      40UL
#define SAMPLE_INTERVAL  5UL

struct ButtonState {
    uint8_t pin;
    bool    stableLevel;
    bool    rawLevel;
    bool    pendingPress;
    uint32_t rawChangedMs;
    uint32_t lastSampleMs;
};

static ButtonState s_btnSelector;
static ButtonState s_btnConfirm;
static ButtonState s_btnConfig;

static void initButton(ButtonState& btn, uint8_t pin, uint8_t mode) {
    btn.pin          = pin;
    btn.stableLevel  = true;
    btn.rawLevel     = true;
    btn.pendingPress = false;
    btn.rawChangedMs = millis();
    btn.lastSampleMs = 0;
    pinMode(pin, mode);
}

void hwButtonsInit() {
    initButton(s_btnSelector, PIN_BTN_SELECTOR, INPUT_PULLUP);
    initButton(s_btnConfirm, PIN_BTN_CONFIRM, INPUT);
    initButton(s_btnConfig, PIN_BTN_CONFIG, INPUT);
}

static void updateButton(ButtonState& btn) {
    const uint32_t now = millis();
    if (now - btn.lastSampleMs < SAMPLE_INTERVAL) return;
    btn.lastSampleMs = now;

    const bool level = (digitalRead(btn.pin) == HIGH);
    if (level != btn.rawLevel) {
        btn.rawLevel = level;
        btn.rawChangedMs = now;
    }
    if (btn.rawLevel != btn.stableLevel &&
        (now - btn.rawChangedMs) >= DEBOUNCE_MS) {
        btn.stableLevel = btn.rawLevel;
        if (!btn.stableLevel) {
            btn.pendingPress = true;   // flanco de bajada = presion
        }
    }
}

void hwButtonsUpdate() {
    updateButton(s_btnSelector);
    updateButton(s_btnConfirm);
    updateButton(s_btnConfig);
}

static bool consumePress(ButtonState& btn) {
    const bool p = btn.pendingPress;
    btn.pendingPress = false;
    return p;
}

bool hwButtonSelectorPressed() { return consumePress(s_btnSelector); }
bool hwButtonConfirmPressed()  { return consumePress(s_btnConfirm); }
bool hwButtonConfigPressed()   { return consumePress(s_btnConfig); }

// ----------------------------------------------------------------------------
// Pantalla OLED 128x64 I2C (U8g2, buffer completo _F_)
// ----------------------------------------------------------------------------
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_u8g2(U8G2_R0, U8X8_PIN_NONE,
                                                  PIN_I2C_SCL, PIN_I2C_SDA);
#define DISPLAY_REFRESH_MS 1000UL
static uint32_t s_lastDrawMs = 0UL;
static ConnStatus s_connStatus = ConnStatus::PENDING;

static bool displayShouldRefresh(bool force) {
    const uint32_t now = millis();
    if (!force && (now - s_lastDrawMs) < DISPLAY_REFRESH_MS) {
        return false;
    }
    s_lastDrawMs = now;
    return true;
}

void hwDisplayInit() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    s_u8g2.begin();
    s_u8g2.setFont(u8g2_font_6x10_tf);
    hwDisplayClear();
}

void hwDisplayClear() {
    s_u8g2.clearBuffer();
    s_u8g2.sendBuffer();
}

void hwDisplaySetConnStatus(ConnStatus status) {
    s_connStatus = status;
}

void hwDisplayShowNormal(uint32_t cycleCount, uint8_t hour, uint8_t minute,
                         uint8_t second, const AirReading& air,
                         bool alarmActive, AlarmCondition alarmCondition) {
    if (!displayShouldRefresh(false)) return;
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    // Fila 1: modo + estado de conexion inicial (P3).
    const char* conn = (s_connStatus == ConnStatus::OK)    ? "R:OK"
                     : (s_connStatus == ConnStatus::FAIL)  ? "R:FAIL"
                                                           : "R:-";
    char modeBuf[24];
    snprintf(modeBuf, sizeof(modeBuf), "MODO: NORMAL %s", conn);
    s_u8g2.drawStr(0, 10, modeBuf);

    char timeBuf[24];
    const char* horaLabel = hwRtcIsReal() ? "Hora: " : "Hora(sim): ";
    snprintf(timeBuf, sizeof(timeBuf), "%s%02u:%02u:%02u",
             horaLabel, hour, minute, second);
    s_u8g2.drawStr(0, 26, timeBuf);

    if (air.valid) {
        char airBuf[24];
        snprintf(airBuf, sizeof(airBuf), "Aire: %.1fC %u%%",
                 air.tempC, (unsigned)air.humidityPct);
        s_u8g2.drawStr(0, 42, airBuf);
    } else {
        s_u8g2.drawStr(0, 42, "Aire: N/D");
    }

    if (alarmActive) {
        const char* cond = "ALARMA?";
        switch (alarmCondition) {
            case AlarmCondition::AIR_TEMP_HIGH:                cond = "ALARMA TEMP MAX"; break;
            case AlarmCondition::AIR_TEMP_LOW:                 cond = "ALARMA TEMP MIN"; break;
            case AlarmCondition::NO_CONNECTION_60MIN:          cond = "ALARMA SIN CONEX"; break;
            case AlarmCondition::SOIL_HUMIDITY_PERSISTENT_LOW: cond = "ALARMA HUM. BAJA"; break;
            default: break;
        }
        s_u8g2.drawStr(0, 58, cond);
    } else {
        char cycleBuf[32];
        snprintf(cycleBuf, sizeof(cycleBuf), "Ciclo local: %lu",
                 (unsigned long)cycleCount);
        s_u8g2.drawStr(0, 58, cycleBuf);
    }

    s_u8g2.sendBuffer();
}

void hwDisplayShowManual(riego::domain::ZoneType type, uint8_t zone,
                         bool active, bool force) {
    // force=true redibuja al instante (corrige bug de V2: el primer pulso del
    // selector no mostraba el menu por el throttle de 1 Hz).
    if (!displayShouldRefresh(force)) return;
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    s_u8g2.drawStr(0, 10, "RIEGO MANUAL");

    char zoneBuf[24];
    const char* typeStr = (type == riego::domain::ZoneType::SUBSTRATE)
                              ? "Sustrato" : "Aspersion";
    snprintf(zoneBuf, sizeof(zoneBuf), "Zona: %s %u", typeStr, zone);
    s_u8g2.drawStr(0, 26, zoneBuf);

    char stateBuf[24];
    snprintf(stateBuf, sizeof(stateBuf), "Estado: %s",
             active ? "ACTIVO" : "INACTIVO");
    s_u8g2.drawStr(0, 42, stateBuf);

    s_u8g2.drawStr(0, 58, "Sel:cambia  Conf:toggle");

    s_u8g2.sendBuffer();
}

void hwDisplayShowConfig(const char* ip) {
    if (!displayShouldRefresh(false)) return;
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    s_u8g2.drawStr(0, 10, "MODO: CONFIG");
    s_u8g2.drawStr(0, 26, "Portal:");
    s_u8g2.drawStr(0, 42, ip ? ip : "192.168.4.1");

    s_u8g2.sendBuffer();
}

// ----------------------------------------------------------------------------
// Reles por rol (polaridad RELAY_ACTIVE_LOW)
// ----------------------------------------------------------------------------
static const uint8_t RELAY_PINS_SUBSTRATE[MAX_SUBSTRATE_ZONES] = {
    PIN_RELAY_SUBSTRATE_1, PIN_RELAY_SUBSTRATE_2,
    PIN_RELAY_SUBSTRATE_3, PIN_RELAY_SUBSTRATE_4
};
static const uint8_t RELAY_PINS_SPRINKLER[MAX_SPRINKLER_ZONES] = {
    PIN_RELAY_SPRINKLER_1, PIN_RELAY_SPRINKLER_2
};
static const uint8_t RELAY_PIN_ALARM = PIN_RELAY_ALARM;

// Conteo local de zonas (defaults de fabrica; el portal lo configura).
static uint8_t s_substrateZones = DEFAULT_SUBSTRATE_ZONES;
static uint8_t s_sprinklerZones = DEFAULT_SPRINKLER_ZONES;

void hwRelaySetZoneCounts(uint8_t substrate, uint8_t sprinkler) {
    if (substrate > MAX_SUBSTRATE_ZONES) substrate = MAX_SUBSTRATE_ZONES;
    if (substrate < 1) substrate = 1;
    if (sprinkler > MAX_SPRINKLER_ZONES) sprinkler = MAX_SPRINKLER_ZONES;
    s_substrateZones = substrate;
    s_sprinklerZones = sprinkler;
}

static bool s_relayStateSubstrate[MAX_SUBSTRATE_ZONES] = { false };
static bool s_relayStateSprinkler[MAX_SPRINKLER_ZONES] = { false };
static bool s_relayStateAlarm = false;

static uint8_t resolvePin(RelayRole role, uint8_t zone) {
    switch (role) {
        case RelayRole::SUBSTRATE:
            if (zone >= s_substrateZones) return 0xFF;
            return RELAY_PINS_SUBSTRATE[zone];
        case RelayRole::SPRINKLER:
            if (zone >= s_sprinklerZones) return 0xFF;
            return RELAY_PINS_SPRINKLER[zone];
        case RelayRole::ALARM:
            if (zone != 0) return 0xFF;
            return RELAY_PIN_ALARM;
    }
    return 0xFF;
}

static void writePin(uint8_t pin, bool active) {
    const bool level = RELAY_ACTIVE_LOW ? !active : active;
    digitalWrite(pin, level ? HIGH : LOW);
}

void hwRelayInit() {
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        writePin(RELAY_PINS_SUBSTRATE[z], false);
        pinMode(RELAY_PINS_SUBSTRATE[z], OUTPUT);
        s_relayStateSubstrate[z] = false;
    }
    for (uint8_t z = 0; z < s_sprinklerZones; z++) {
        writePin(RELAY_PINS_SPRINKLER[z], false);
        pinMode(RELAY_PINS_SPRINKLER[z], OUTPUT);
        s_relayStateSprinkler[z] = false;
    }
    writePin(RELAY_PIN_ALARM, false);
    pinMode(RELAY_PIN_ALARM, OUTPUT);
    s_relayStateAlarm = false;
}

void hwRelayAllOff() {
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        hwRelaySet(RelayRole::SUBSTRATE, z, false);
    }
    for (uint8_t z = 0; z < s_sprinklerZones; z++) {
        hwRelaySet(RelayRole::SPRINKLER, z, false);
    }
    hwRelaySet(RelayRole::ALARM, 0, false);
}

void hwRelaySet(RelayRole role, uint8_t zone, bool active) {
    const uint8_t pin = resolvePin(role, zone);
    if (pin == 0xFF) return;
    switch (role) {
        case RelayRole::SUBSTRATE: s_relayStateSubstrate[zone] = active; break;
        case RelayRole::SPRINKLER: s_relayStateSprinkler[zone] = active; break;
        case RelayRole::ALARM:     s_relayStateAlarm = active;           break;
    }
    writePin(pin, active);
}

bool hwRelayGet(RelayRole role, uint8_t zone) {
    switch (role) {
        case RelayRole::SUBSTRATE:
            if (zone >= s_substrateZones) return false;
            return s_relayStateSubstrate[zone];
        case RelayRole::SPRINKLER:
            if (zone >= s_sprinklerZones) return false;
            return s_relayStateSprinkler[zone];
        case RelayRole::ALARM:
            if (zone != 0) return false;
            return s_relayStateAlarm;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Reloj (DS3231 real con fallback simulado)
// ----------------------------------------------------------------------------
static RTC_DS3231 s_rtc;
static bool s_real = false;
static const char* const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static int64_t s_simBaseEpoch = 0;
static int64_t s_simStartUs = 0;

static struct tm compileTime() {
    struct tm t = {};
    char mon[4] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    int m = 0;
    for (; m < 12; m++) {
        if (strncmp(mon, MONTHS[m], 3) == 0) break;
    }
    t.tm_year = y - 1900;
    t.tm_mon  = m;
    t.tm_mday = d;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    t.tm_wday = 5;
    return t;
}

static struct tm simulatedNow() {
    const int64_t epoch = s_simBaseEpoch +
                          (esp_timer_get_time() - s_simStartUs) / 1000000LL;
    const time_t value = (time_t)epoch;
    struct tm t = {};
    gmtime_r(&value, &t);
    return t;
}

bool hwRtcInit() {
    s_simBaseEpoch = hwRtcTmToEpoch(compileTime());
    s_simStartUs = esp_timer_get_time();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    if (s_rtc.begin()) {
        s_real = true;
        if (s_rtc.lostPower()) {
            hwRtcSetTime(compileTime());
        }
        return true;
    }

    s_real = false;
    return false;
}

struct tm hwRtcNow() {
    if (!s_real) return simulatedNow();
    DateTime dt = s_rtc.now();
    struct tm t = {};
    t.tm_year = dt.year() - 1900;
    t.tm_mon  = dt.month() - 1;
    t.tm_mday = dt.day();
    t.tm_hour = dt.hour();
    t.tm_min  = dt.minute();
    t.tm_sec  = dt.second();
    t.tm_wday = dt.dayOfTheWeek();
    return t;
}

int64_t hwRtcTmToEpoch(const struct tm& t) {
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int d = t.tm_mday;
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400LL + (int64_t)t.tm_hour * 3600LL +
           (int64_t)t.tm_min * 60LL + (int64_t)t.tm_sec;
}

struct tm hwRtcNowLocal() {
    const int64_t epoch = hwRtcTmToEpoch(hwRtcNow()) +
                          (int64_t)DISPLAY_TZ_OFFSET_HOURS * 3600LL;
    const time_t localEpoch = (time_t)epoch;
    struct tm local = {};
    gmtime_r(&localEpoch, &local);
    return local;
}

bool hwRtcIsReal() {
    return s_real;
}

void hwRtcSetTime(const struct tm& t) {
    if (!s_real) {
        s_simBaseEpoch = hwRtcTmToEpoch(t);
        s_simStartUs = esp_timer_get_time();
        return;
    }
    DateTime dt(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
    s_rtc.adjust(dt);
}

// ----------------------------------------------------------------------------
// Calibracion de humedad por zona (NVS, namespace "calib")
// ----------------------------------------------------------------------------
static const char* NVS_CALIB = "calib";

void hwCalibrationInit() {
    // Sin estado interno que precargar; se lee por zona bajo demanda.
}

ZoneCalibration hwCalibrationGet(uint8_t zone) {
    ZoneCalibration cal = {};
    cal.dryRaw = SOIL_DEFAULT_DRY_RAW;
    cal.wetRaw = SOIL_DEFAULT_WET_RAW;
    cal.calibrated = false;
    if (zone >= MAX_SUBSTRATE_ZONES) return cal;

    Preferences prefs;
    prefs.begin(NVS_CALIB, true);
    char key[8];
    const char* base = (zone == 0) ? "z0" : (zone == 1) ? "z1"
                     : (zone == 2) ? "z2" : "z3";
    snprintf(key, sizeof(key), "%s_dry", base);
    cal.dryRaw = prefs.getUShort(key, SOIL_DEFAULT_DRY_RAW);
    snprintf(key, sizeof(key), "%s_wet", base);
    cal.wetRaw = prefs.getUShort(key, SOIL_DEFAULT_WET_RAW);
    snprintf(key, sizeof(key), "%s_cal", base);
    cal.calibrated = prefs.getBool(key, false);
    prefs.end();
    return cal;
}

void hwCalibrationSet(uint8_t zone, const ZoneCalibration& cal) {
    if (zone >= MAX_SUBSTRATE_ZONES) return;
    Preferences prefs;
    prefs.begin(NVS_CALIB, false);
    char key[8];
    const char* base = (zone == 0) ? "z0" : (zone == 1) ? "z1"
                     : (zone == 2) ? "z2" : "z3";
    snprintf(key, sizeof(key), "%s_dry", base);
    prefs.putUShort(key, cal.dryRaw);
    snprintf(key, sizeof(key), "%s_wet", base);
    prefs.putUShort(key, cal.wetRaw);
    snprintf(key, sizeof(key), "%s_cal", base);
    prefs.putBool(key, cal.calibrated);
    prefs.end();
}

bool hwCalibrationIsCalibrated(uint8_t zone) {
    return hwCalibrationGet(zone).calibrated;
}

void hwCalibrationClearAll() {
    Preferences prefs;
    prefs.begin(NVS_CALIB, false);
    prefs.clear();
    prefs.end();
}

// ----------------------------------------------------------------------------
// Mapeo OneWire (DS18B20, persistido en NVS)
// ----------------------------------------------------------------------------
static const char* NVS_OWMAP = "owmap";
static OneWire s_oneWire(PIN_ONEWIRE);
static uint8_t s_addresses[MAX_DS18B20][8];
static uint8_t s_owCount = 0;

static void owSaveToNvs() {
    Preferences prefs;
    prefs.begin(NVS_OWMAP, false);
    prefs.putUChar("count", s_owCount);
    for (uint8_t i = 0; i < s_owCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "addr%u", i);
        prefs.putBytes(key, s_addresses[i], 8);
    }
    prefs.end();
}

static bool owLoadFromNvs() {
    Preferences prefs;
    prefs.begin(NVS_OWMAP, true);
    s_owCount = prefs.getUChar("count", 0);
    if (s_owCount == 0 || s_owCount > MAX_DS18B20) {
        prefs.end();
        return false;
    }
    for (uint8_t i = 0; i < s_owCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "addr%u", i);
        size_t len = prefs.getBytes(key, s_addresses[i], 8);
        if (len != 8) {
            prefs.end();
            return false;
        }
    }
    prefs.end();
    return true;
}

static bool owScanBus() {
    s_owCount = 0;
    uint8_t addr[8];
    while (s_oneWire.search(addr) && s_owCount < MAX_DS18B20) {
        if (addr[0] == 0x28) {
            memcpy(s_addresses[s_owCount], addr, 8);
            s_owCount++;
        }
    }
    s_oneWire.reset_search();
    return (s_owCount > 0);
}

static uint8_t owCount() { return s_owCount; }

static bool owGetAddress(uint8_t index, uint8_t addr[8]) {
    if (index >= s_owCount) return false;
    memcpy(addr, s_addresses[index], 8);
    return true;
}

static OneWire* owGetBus() { return &s_oneWire; }

// ----------------------------------------------------------------------------
// Sensores
// ----------------------------------------------------------------------------
static const uint8_t SOIL_ADC_PINS[MAX_SUBSTRATE_ZONES] = {
    PIN_SOIL_ADC_1, PIN_SOIL_ADC_2, PIN_SOIL_ADC_3, PIN_SOIL_ADC_4
};

#if AIR_SENSOR_MODEL == 1
static DHT s_dht(PIN_DHT21, DHT11);    // reemplazo temporal
#else
static DHT s_dht(PIN_DHT21, DHT21);    // sensor final del proyecto
#endif
static DallasTemperature s_dallas;
static bool s_dhtInit = false;
static bool s_requestOutstanding = false;

static float rawToPercent(int raw, const ZoneCalibration& cal) {
    const int span = (int)cal.dryRaw - (int)cal.wetRaw;
    if (span == 0) return 50.0f;
    float pct = 100.0f * ((int)cal.dryRaw - raw) / (float)span;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}

static uint16_t readSoilAdc(uint8_t pin) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < SOIL_ADC_SAMPLES; i++) {
        sum += analogRead(pin);
    }
    return (uint16_t)(sum / SOIL_ADC_SAMPLES);
}

static void readSoil(SoilReading readings[]) {
    digitalWrite(PIN_SENSOR_POWER, HIGH);
    delay(SOIL_STABILIZE_MS);   // unica excepcion permitida (spec 8.5)
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        const ZoneCalibration cal = hwCalibrationGet(z);
        const uint16_t raw = readSoilAdc(SOIL_ADC_PINS[z]);
        readings[z].valid       = cal.calibrated;
        readings[z].rawAdc      = raw;
        readings[z].humidityPct = rawToPercent(raw, cal);
    }
    digitalWrite(PIN_SENSOR_POWER, LOW);
}

static void requestTempsNow() {
    if (owCount() > 0 && s_substrateZones > 0) {
        s_dallas.requestTemperatures();
        s_requestOutstanding = true;
    }
}

static void readTemps(SoilTempReading readings[]) {
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        readings[z].valid = false;
        readings[z].tempC = 0.0f;
    }
    if (s_requestOutstanding) {
        const uint8_t active = (s_substrateZones < owCount())
                                   ? s_substrateZones : owCount();
        for (uint8_t z = 0; z < active; z++) {
            uint8_t addr[8];
            if (owGetAddress(z, addr)) {
                const float t = s_dallas.getTempC(addr);
                if (t != DEVICE_DISCONNECTED_C) {
                    readings[z].valid = true;
                    readings[z].tempC = t;
                }
            }
        }
        s_requestOutstanding = false;
    }
    requestTempsNow();
}

static void readAir(AirReading& air) {
    air.valid = false;
    air.tempC = 0.0f;
    air.humidityPct = 0.0f;
    if (!s_dhtInit) return;
    const float h = s_dht.readHumidity();
    const float t = s_dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
        air.valid = true;
        air.tempC = t;
        air.humidityPct = h;
    }
}

void hwSensorsInit() {
    hwCalibrationInit();

    // Mapeo OneWire: carga desde NVS o escanea el bus una vez.
    if (!owLoadFromNvs()) {
        if (owScanBus()) owSaveToNvs();
    }

    pinMode(PIN_SENSOR_POWER, OUTPUT);
    digitalWrite(PIN_SENSOR_POWER, LOW);
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        pinMode(SOIL_ADC_PINS[z], INPUT);
    }

    s_dallas.setOneWire(owGetBus());
    s_dallas.begin();
    s_dallas.setWaitForConversion(false);
    s_dallas.setResolution(12);

    s_dht.begin();
    s_dhtInit = true;

    s_requestOutstanding = false;
    requestTempsNow();
}

SensorReadings hwSensorsRead() {
    SensorReadings r = {};
    r.timestamp = hwRtcNow();
    readSoil(r.soil);
    readTemps(r.soilTemp);
    readAir(r.air);
    return r;
}

void hwSensorsPrint(const SensorReadings& r) {
    Serial.println("--- Lecturas de sensores ---");
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        const SoilReading& s = r.soil[z];
        Serial.printf("  Humedad zona %u: %s ADC=%u (%.1f%%)\n",
                      z, s.valid ? "OK" : "FAIL", s.rawAdc, s.humidityPct);
    }
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        const SoilTempReading& t = r.soilTemp[z];
        Serial.printf("  Temp sustrato zona %u: %s %.1f C\n",
                      z, t.valid ? "OK" : "FAIL", t.tempC);
    }
    const AirReading& a = r.air;
    Serial.printf("  Aire: %s T=%.1f C H=%.1f%%\n",
                  a.valid ? "OK" : "FAIL", a.tempC, a.humidityPct);
}

uint16_t hwSensorsReadSoilRawAdc(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) return 0;
    digitalWrite(PIN_SENSOR_POWER, HIGH);
    delay(SOIL_STABILIZE_MS);
    const uint16_t raw = readSoilAdc(SOIL_ADC_PINS[zone]);
    digitalWrite(PIN_SENSOR_POWER, LOW);
    return raw;
}

void hwSensorsSoilPowerSet(bool on) {
    digitalWrite(PIN_SENSOR_POWER, on ? HIGH : LOW);
}

uint16_t hwSensorsReadSoilRawAdcPowered(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) return 0;
    return readSoilAdc(SOIL_ADC_PINS[zone]);
}