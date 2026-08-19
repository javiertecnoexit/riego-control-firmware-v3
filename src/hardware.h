#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>
#include <time.h>

#include "config.h"
#include "rules.h"

// ============================================================================
// hardware.h — Capa de hardware V3 (estructura reducida, P8).
// Fusiona los modulos de V2: buttons, display, relay, rtc, sensors,
// calibration y onewire_map en una sola interfaz plana.
// ============================================================================

// ----------------------------------------------------------------------------
// Botones (debounce no bloqueante, igual que V2)
// ----------------------------------------------------------------------------
void hwButtonsInit();
void hwButtonsUpdate();                    // llamar cada pasada del loop
bool hwButtonSelectorPressed();            // consume el flanco
bool hwButtonConfirmPressed();
bool hwButtonConfigPressed();

// ----------------------------------------------------------------------------
// Pantalla OLED 128x64 I2C
// ----------------------------------------------------------------------------
enum class ConnStatus : uint8_t {         // estado de conexion inicial (P3)
    PENDING = 0,                           // aun no definido (boot)
    OK = 1,                                // conexion inicial exitosa
    FAIL = 2                               // fallo en la conexion inicial
};

void hwDisplayInit();
void hwDisplayClear();
void hwDisplaySetConnStatus(ConnStatus status);
void hwDisplayShowNormal(uint32_t cycleCount, uint8_t hour, uint8_t minute,
                         uint8_t second, const struct AirReading& air,
                         bool alarmActive, AlarmCondition alarmCondition);
void hwDisplayShowManual(riego::domain::ZoneType type, uint8_t zone,
                         bool active, bool force);
void hwDisplayShowConfig(const char* ip);

// ----------------------------------------------------------------------------
// Reles por rol
// ----------------------------------------------------------------------------
enum class RelayRole : uint8_t {
    SUBSTRATE = 0,
    SPRINKLER = 1,
    ALARM     = 2
};

void hwRelayInit();                        // estado seguro: todos apagados
void hwRelayAllOff();
void hwRelaySet(RelayRole role, uint8_t zone, bool active);
bool hwRelayGet(RelayRole role, uint8_t zone);

// ----------------------------------------------------------------------------
// Reloj (DS3231 real con fallback simulado, igual que V2)
// ----------------------------------------------------------------------------
bool hwRtcInit();
struct tm hwRtcNow();                      // UTC
struct tm hwRtcNowLocal();                 // UTC-3 solo para pantalla
void hwRtcSetTime(const struct tm& t);
bool hwRtcIsReal();
int64_t hwRtcTmToEpoch(const struct tm& t);

// ----------------------------------------------------------------------------
// Sensores (humedad ADC, DS18B20, DHT)
// ----------------------------------------------------------------------------
struct SoilReading {
    bool     valid;
    uint16_t rawAdc;
    float    humidityPct;
};

struct SoilTempReading {
    bool  valid;
    float tempC;
};

struct AirReading {
    bool  valid;
    float tempC;
    float humidityPct;
};

struct SensorReadings {
    struct tm        timestamp;
    SoilReading      soil[MAX_SUBSTRATE_ZONES];
    SoilTempReading  soilTemp[MAX_SUBSTRATE_ZONES];
    AirReading       air;
};

void hwSensorsInit();
SensorReadings hwSensorsRead();
void hwSensorsPrint(const SensorReadings& readings);
uint16_t hwSensorsReadSoilRawAdc(uint8_t zone);      // calibracion (portal)
void hwSensorsSoilPowerSet(bool on);
uint16_t hwSensorsReadSoilRawAdcPowered(uint8_t zone);

// ----------------------------------------------------------------------------
// Calibracion de humedad por zona (persistida en NVS)
// ----------------------------------------------------------------------------
struct ZoneCalibration {
    uint16_t dryRaw;
    uint16_t wetRaw;
    bool     calibrated;
};

void hwCalibrationInit();
ZoneCalibration hwCalibrationGet(uint8_t zone);
void hwCalibrationSet(uint8_t zone, const ZoneCalibration& cal);
bool hwCalibrationIsCalibrated(uint8_t zone);

#endif // HARDWARE_H