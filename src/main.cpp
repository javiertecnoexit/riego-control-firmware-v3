#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "hardware.h"
#include "rules.h"

// ============================================================================
// main.cpp — Orquestacion V3 (estructura reducida, P8).
// Fusiona de V2: state_machine, irrigation (orquestacion), manual_ui,
// local_cycle, alarm y eventos locales. Sin delay() bloqueante en el loop.
// El motor de reglas PURAS vive en src/rules.* (probado con Unity).
// ============================================================================

using riego::domain::IrrigationCandidate;
using riego::domain::IrrigationTransition;
using riego::domain::IrrigationTrigger;
using riego::domain::Schedule;
using riego::domain::TransitionAction;
using riego::domain::ZonePolicy;
using riego::domain::ZoneRuntime;
using riego::domain::ZoneType;

// ----------------------------------------------------------------------------
// Maquina de estados Normal/Configuracion
// ----------------------------------------------------------------------------
static OperationMode s_mode = OperationMode::NORMAL;

static void stateMachineEnterConfig() {
    if (s_mode != OperationMode::NORMAL) return;
    s_mode = OperationMode::CONFIG;
    Serial.println("[STATE] Modo CONFIGURACION");
}

static void stateMachineRestart() {
    Serial.println("[STATE] Reinicio (timeout de configuracion)");
    hwRelayAllOff();
    ESP.restart();
}

// ----------------------------------------------------------------------------
// Orquestacion de riego por zona (estado + aplicacion sobre relays)
// ----------------------------------------------------------------------------
struct ZoneRuntimeState {
    bool active;
    IrrigationTrigger trigger;
    uint32_t startedAtMs;
    uint32_t durationMs;
    bool suppressed;
};

static ZoneRuntimeState s_substrateState[MAX_SUBSTRATE_ZONES];
static ZoneRuntimeState s_sprinklerState[MAX_SPRINKLER_ZONES];

static Schedule s_substrateSchedules[MAX_SUBSTRATE_ZONES][MAX_SCHEDULES_PER_ZONE];
static Schedule s_sprinklerSchedules[MAX_SPRINKLER_ZONES][MAX_SCHEDULES_PER_ZONE];

static int64_t s_substrateLastScheduleMinute[MAX_SUBSTRATE_ZONES];
static int64_t s_sprinklerLastScheduleMinute[MAX_SPRINKLER_ZONES];

static bool s_substratePaused[MAX_SUBSTRATE_ZONES] = { false };
static bool s_sprinklerPaused[MAX_SPRINKLER_ZONES] = { false };

static bool s_substrateOverridePending[MAX_SUBSTRATE_ZONES] = { false };
static bool s_sprinklerOverridePending[MAX_SPRINKLER_ZONES] = { false };

static bool s_substrateManualActive[MAX_SUBSTRATE_ZONES] = { false };
static bool s_sprinklerManualActive[MAX_SPRINKLER_ZONES] = { false };

static bool s_substrateDurationSet[MAX_SUBSTRATE_ZONES] = { false };
static uint16_t s_substrateDuration[MAX_SUBSTRATE_ZONES];
static bool s_sprinklerDurationSet[MAX_SPRINKLER_ZONES] = { false };
static uint16_t s_sprinklerDuration[MAX_SPRINKLER_ZONES];

static uint8_t s_substrateMinHumidity[MAX_SUBSTRATE_ZONES] = {
    DEFAULT_MIN_HUMIDITY, DEFAULT_MIN_HUMIDITY, DEFAULT_MIN_HUMIDITY, DEFAULT_MIN_HUMIDITY
};

// Conteo local de zonas (defaults de fabrica hasta Fase 2 / portal).
static uint8_t s_substrateZones = DEFAULT_SUBSTRATE_ZONES;
static uint8_t s_sprinklerZones = DEFAULT_SPRINKLER_ZONES;

static ZoneRuntimeState* getState(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateState[zone];
    }
    if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
    return &s_sprinklerState[zone];
}

static Schedule* getSchedules(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return s_substrateSchedules[zone];
    }
    if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
    return s_sprinklerSchedules[zone];
}

static bool* getPaused(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substratePaused[zone];
    }
    if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
    return &s_sprinklerPaused[zone];
}

static bool* getOverridePending(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateOverridePending[zone];
    }
    if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
    return &s_sprinklerOverridePending[zone];
}

static bool* getManualActive(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateManualActive[zone];
    }
    if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
    return &s_sprinklerManualActive[zone];
}

static int64_t* getLastScheduleMinute(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateLastScheduleMinute[zone];
    }
    if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
    return &s_sprinklerLastScheduleMinute[zone];
}

static uint16_t durationFor(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return DEFAULT_IRRIGATION_S;
        return s_substrateDurationSet[zone] ? s_substrateDuration[zone]
                                            : DEFAULT_IRRIGATION_S;
    }
    if (zone >= MAX_SPRINKLER_ZONES) return DEFAULT_IRRIGATION_S;
    return s_sprinklerDurationSet[zone] ? s_sprinklerDuration[zone]
                                        : DEFAULT_IRRIGATION_S;
}

static RelayRole roleForType(ZoneType type) {
    return (type == ZoneType::SUBSTRATE) ? RelayRole::SUBSTRATE
                                         : RelayRole::SPRINKLER;
}

static ZonePolicy makePolicy(ZoneType type, uint8_t zone) {
    ZonePolicy policy = {};
    bool* paused = getPaused(type, zone);
    policy.paused = paused && *paused;
    policy.defaultDurationSeconds = durationFor(type, zone);
    policy.minHumidityPct = s_substrateMinHumidity[zone];
    Schedule* schedules = getSchedules(type, zone);
    if (schedules) {
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; ++i) {
            policy.schedules[i] = schedules[i];
        }
    }
    return policy;
}

// maxTemp del aire para el umbral de aspersion (default hasta Fase 3).
static float s_maxAirTempC = DEFAULT_ALARM_MAX_TEMP;

static IrrigationCandidate selectCandidate(
    ZoneType type, uint8_t zone, const struct tm& now,
    const SensorReadings& readings
) {
    ZonePolicy policy = makePolicy(type, zone);

    riego::domain::EvaluationInputs inputs = {};
    inputs.zoneType = type;
    bool* manualActive = getManualActive(type, zone);
    bool* overridePending = getOverridePending(type, zone);
    inputs.manualActive = manualActive && *manualActive;
    inputs.overridePending = overridePending && *overridePending;
    inputs.hour = (uint8_t)now.tm_hour;
    inputs.minute = (uint8_t)now.tm_min;
    inputs.currentMinuteKey = hwRtcTmToEpoch(now) / 60LL;
    inputs.maxAirTempC = s_maxAirTempC;
    int64_t* lastMinute = getLastScheduleMinute(type, zone);
    if (lastMinute) {
        inputs.hasLastScheduleMinute = true;
        inputs.lastScheduleMinuteKey = *lastMinute;
    }
    if (type == ZoneType::SUBSTRATE && zone < s_substrateZones) {
        inputs.soilValid = readings.soil[zone].valid;
        inputs.soilHumidityPct = readings.soil[zone].humidityPct;
    }
    inputs.airValid = readings.air.valid;
    inputs.airTempC = readings.air.tempC;

    return riego::domain::selectIrrigationCandidate(policy, inputs);
}

static ZoneRuntime toDomainRuntime(const ZoneRuntimeState& rt) {
    ZoneRuntime result = {};
    result.active = rt.active;
    result.trigger = rt.trigger;
    result.durationSeconds = (uint16_t)(rt.durationMs / 1000UL);
    result.deadlineUs = (uint64_t)rt.durationMs * 1000ULL;
    result.timeoutEnabled = !rt.suppressed;
    return result;
}

static IrrigationTransition decideTransition(
    ZoneType type, uint8_t zone, const IrrigationCandidate& candidate, uint64_t nowUs
) {
    ZoneRuntimeState* state = getState(type, zone);
    if (!state) return {};
    return riego::domain::decideIrrigationTransition(
        toDomainRuntime(*state), candidate, nowUs
    );
}

// Aplica la transicion sobre el estado y el relay; devuelve true si cambio.
static bool applyTransition(
    ZoneType type, uint8_t zone, const IrrigationTransition& tr,
    const struct tm& now
) {
    ZoneRuntimeState* state = getState(type, zone);
    if (!state || tr.action == TransitionAction::NONE) return false;

    if (tr.action == TransitionAction::START) {
        if (state->active || tr.durationSeconds == 0 ||
            tr.trigger == IrrigationTrigger::NONE) {
            return false;
        }
        state->active = true;
        state->trigger = tr.trigger;
        state->startedAtMs = millis();
        state->durationMs = (uint32_t)tr.durationSeconds * 1000UL;
        state->suppressed = false;

        if (tr.trigger == IrrigationTrigger::SCHEDULE) {
            int64_t* lastMinute = getLastScheduleMinute(type, zone);
            if (lastMinute) {
                *lastMinute = hwRtcTmToEpoch(now) / 60LL;
            }
        }
        if (tr.trigger == IrrigationTrigger::OVERRIDE) {
            bool* overridePending = getOverridePending(type, zone);
            if (overridePending) *overridePending = false;
        }

        hwRelaySet(roleForType(type), zone, true);
        Serial.printf("[IRRIG] Zona %s%u START trigger=%d dur=%lums\n",
                      type == ZoneType::SUBSTRATE ? "S" : "A", zone,
                      (int)tr.trigger, (unsigned long)state->durationMs);
        return true;
    }

    if (!state->active) return false;
    const IrrigationTrigger finishedTrigger = state->trigger;
    if (finishedTrigger == IrrigationTrigger::MANUAL) {
        bool* manualActive = getManualActive(type, zone);
        if (manualActive) *manualActive = false;
    }
    state->trigger = IrrigationTrigger::NONE;
    state->active = false;
    state->suppressed = false;
    hwRelaySet(roleForType(type), zone, false);
    Serial.printf("[IRRIG] Zona %s%u STOP trigger=%d\n",
                  type == ZoneType::SUBSTRATE ? "S" : "A", zone,
                  (int)finishedTrigger);
    return true;
}

static void serviceTimedOutZone(ZoneType type, uint8_t zone) {
    ZoneRuntimeState* state = getState(type, zone);
    if (!state || !state->active || state->suppressed) return;

    // El candidato mantiene la regla actual: la zona solo se detiene por
    // deadline o si la regla desaparece en la evaluacion del ciclo.
    const IrrigationCandidate candidate = {
        state->trigger, (uint16_t)(state->durationMs / 1000UL), -1
    };
    const IrrigationTransition tr = riego::domain::decideIrrigationTransition(
        toDomainRuntime(*state), candidate,
        (uint64_t)(millis() - state->startedAtMs) * 1000ULL
    );
    applyTransition(type, zone, tr, hwRtcNow());
}

static void evaluateZone(ZoneType type, uint8_t zone,
                         const struct tm& now, const SensorReadings& readings) {
    ZoneRuntimeState* state = getState(type, zone);
    if (!state) return;
    const IrrigationCandidate candidate = selectCandidate(type, zone, now, readings);
    const uint64_t nowUs = state->active
        ? (uint64_t)(millis() - state->startedAtMs) * 1000ULL
        : 0ULL;
    const IrrigationTransition tr = decideTransition(type, zone, candidate, nowUs);
    applyTransition(type, zone, tr, now);
}

static void irrigationEvaluate(const struct tm& now, const SensorReadings& readings) {
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        evaluateZone(ZoneType::SUBSTRATE, z, now, readings);
    }
    for (uint8_t z = 0; z < s_sprinklerZones; z++) {
        evaluateZone(ZoneType::SPRINKLER, z, now, readings);
    }
}

static void irrigationServiceTimeouts() {
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        serviceTimedOutZone(ZoneType::SUBSTRATE, z);
    }
    for (uint8_t z = 0; z < s_sprinklerZones; z++) {
        serviceTimedOutZone(ZoneType::SPRINKLER, z);
    }
}

static void irrigationStopAll() {
    const struct tm now = hwRtcNow();
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        ZoneRuntimeState* state = getState(ZoneType::SUBSTRATE, z);
        if (state && state->active) {
            const IrrigationTransition stop = riego::domain::decideIrrigationTransition(
                toDomainRuntime(*state),
                IrrigationCandidate{IrrigationTrigger::NONE, 0, -1},
                0
            );
            applyTransition(ZoneType::SUBSTRATE, z, stop, now);
        }
    }
    for (uint8_t z = 0; z < MAX_SPRINKLER_ZONES; z++) {
        ZoneRuntimeState* state = getState(ZoneType::SPRINKLER, z);
        if (state && state->active) {
            const IrrigationTransition stop = riego::domain::decideIrrigationTransition(
                toDomainRuntime(*state),
                IrrigationCandidate{IrrigationTrigger::NONE, 0, -1},
                0
            );
            applyTransition(ZoneType::SPRINKLER, z, stop, now);
        }
    }
}

static bool irrigationAnyActive() {
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        if (s_substrateState[z].active) return true;
    }
    for (uint8_t z = 0; z < MAX_SPRINKLER_ZONES; z++) {
        if (s_sprinklerState[z].active) return true;
    }
    return false;
}

static bool irrigationManualToggle(ZoneType type, uint8_t zone) {
    bool* manualActive = getManualActive(type, zone);
    ZoneRuntimeState* state = getState(type, zone);
    if (!manualActive || !state) return false;

    const bool activating = !(*manualActive);
    const struct tm now = hwRtcNow();

    if (activating) {
        const uint32_t durationMs = (uint32_t)durationFor(type, zone) * 1000UL;
        if (durationMs == 0) return false;

        if (state->active) {
            const IrrigationTransition stop = riego::domain::decideIrrigationTransition(
                toDomainRuntime(*state),
                IrrigationCandidate{IrrigationTrigger::NONE, 0, -1},
                0
            );
            applyTransition(type, zone, stop, now);
        }
        *manualActive = true;
        // Arranque INMEDIATO del relay (sin esperar el proximo ciclo local).
        const IrrigationCandidate cand = {
            IrrigationTrigger::MANUAL, (uint16_t)(durationMs / 1000UL), -1
        };
        const IrrigationTransition tr = riego::domain::decideIrrigationTransition(
            toDomainRuntime(*state), cand, 0
        );
        applyTransition(type, zone, tr, now);
        Serial.printf("[IRRIG] Manual zona %s%u ON inmediato (relay)\n",
                      type == ZoneType::SUBSTRATE ? "S" : "A", zone);
        return true;
    }

    *manualActive = false;
    const IrrigationTransition stop = riego::domain::decideIrrigationTransition(
        toDomainRuntime(*state),
        IrrigationCandidate{IrrigationTrigger::NONE, 0, -1},
        0
    );
    const bool changed = applyTransition(type, zone, stop, now);
    Serial.printf("[IRRIG] Manual zona %s%u OFF\n",
                  type == ZoneType::SUBSTRATE ? "S" : "A", zone);
    return changed;
}

static ZoneRuntimeState irrigationGetState(ZoneType type, uint8_t zone) {
    ZoneRuntimeState* state = getState(type, zone);
    if (state) return *state;
    return {};
}

// ----------------------------------------------------------------------------
// UI de riego manual (P2: el primer pulso del selector ABRE el menu en
// pantalla; se fuerza el redibujado inmediato, corrigiendo el bug de V2).
// ----------------------------------------------------------------------------
static bool s_selecting = false;
static ZoneType s_selectedType = ZoneType::SUBSTRATE;
static uint8_t s_selectedZone = 0;
static uint32_t s_lastActivityMs = 0;

static uint8_t totalConfiguredZones() {
    return s_substrateZones + s_sprinklerZones;
}

static void indexToZone(uint8_t index, ZoneType& outType, uint8_t& outZone) {
    if (index < s_substrateZones) {
        outType = ZoneType::SUBSTRATE;
        outZone = index;
    } else {
        outType = ZoneType::SPRINKLER;
        outZone = index - s_substrateZones;
    }
}

static uint8_t zoneToIndex(ZoneType type, uint8_t zone) {
    return (type == ZoneType::SUBSTRATE) ? zone
                                         : s_substrateZones + zone;
}

static void showSelectedZone(bool force) {
    const ZoneRuntimeState st = irrigationGetState(s_selectedType, s_selectedZone);
    // El flag manual se refleja al instante aunque el START fisico se aplique
    // en el proximo ciclo local (hasta 30 s).
    bool* manualActive = getManualActive(s_selectedType, s_selectedZone);
    const bool shownActive = st.active || (manualActive && *manualActive);
    hwDisplayShowManual(s_selectedType, s_selectedZone, shownActive, force);
}

static void manualUiUpdate() {
    if (s_selecting) {
        if (hwButtonSelectorPressed()) {
            const uint8_t currentIndex = zoneToIndex(s_selectedType, s_selectedZone);
            const uint8_t nextIndex = (currentIndex + 1) % totalConfiguredZones();
            indexToZone(nextIndex, s_selectedType, s_selectedZone);
            s_lastActivityMs = millis();
            showSelectedZone(true);
            Serial.printf("[MANUAL] Selector -> zona %s%u\n",
                          s_selectedType == ZoneType::SUBSTRATE ? "S" : "A",
                          s_selectedZone);
        }
        if (hwButtonConfirmPressed()) {
            irrigationManualToggle(s_selectedType, s_selectedZone);
            s_lastActivityMs = millis();
            showSelectedZone(true);
        }
        if ((millis() - s_lastActivityMs) >= MANUAL_TIMEOUT_MS) {
            s_selecting = false;
            Serial.println("[MANUAL] Timeout 5s -> reposo");
        }
    } else {
        // En reposo: el primer pulso del selector abre el menu al instante (P2).
        if (hwButtonSelectorPressed()) {
            s_selecting = true;
            s_selectedType = ZoneType::SUBSTRATE;
            s_selectedZone = 0;
            s_lastActivityMs = millis();
            showSelectedZone(true);
            Serial.println("[MANUAL] Selector -> modo seleccion (zona S0)");
        }
        // Confirmar en reposo no hace nada.
    }
}

static void manualUiForceIdle() {
    s_selecting = false;
}

// ----------------------------------------------------------------------------
// Alarma (condiciones de V2; la de no-conexion se activa en Fase 3)
// ----------------------------------------------------------------------------
static bool s_alarmActive = false;
static AlarmCondition s_activeCondition = AlarmCondition::NONE;
static struct tm s_alarmStartedAt = {};
static float s_alarmMaxTemp = -1.0f;   // -1 = usar default de fabrica
static float s_alarmMinTemp = -1.0f;
static uint16_t s_persistentLowCycles[MAX_SUBSTRATE_ZONES] = { 0 };
static int64_t s_lastBackendSeenEpoch = 0;   // Fase 3 la actualiza

static float alarmMaxTempEffective() {
    return (s_alarmMaxTemp >= 0.0f) ? s_alarmMaxTemp : DEFAULT_ALARM_MAX_TEMP;
}
static float alarmMinTempEffective() {
    return (s_alarmMinTemp >= 0.0f) ? s_alarmMinTemp : DEFAULT_ALARM_MIN_TEMP;
}

static bool alarmCheckPersistentLow(const SensorReadings& readings) {
    bool anyZonePersistent = false;
    for (uint8_t z = 0; z < s_substrateZones; z++) {
        const SoilReading& soil = readings.soil[z];
        if (soil.valid && soil.humidityPct < (float)s_substrateMinHumidity[z]) {
            if (s_persistentLowCycles[z] < UINT16_MAX) {
                s_persistentLowCycles[z]++;
            }
            if (s_persistentLowCycles[z] >= SOIL_HUMIDITY_PERSISTENT_CYCLES) {
                anyZonePersistent = true;
            }
        } else {
            s_persistentLowCycles[z] = 0;
        }
    }
    return anyZonePersistent;
}

static void alarmEvaluate(const struct tm& now, const SensorReadings& readings) {
    AlarmCondition newCondition = AlarmCondition::NONE;

    if (readings.air.valid && readings.air.tempC > alarmMaxTempEffective()) {
        newCondition = AlarmCondition::AIR_TEMP_HIGH;
    } else if (readings.air.valid && readings.air.tempC < alarmMinTempEffective()) {
        newCondition = AlarmCondition::AIR_TEMP_LOW;
    } else if (s_lastBackendSeenEpoch > 0 &&
               (hwRtcTmToEpoch(now) - s_lastBackendSeenEpoch) >=
                   (int64_t)ALARM_NO_CONNECTION_SEC) {
        newCondition = AlarmCondition::NO_CONNECTION_60MIN;
    } else if (alarmCheckPersistentLow(readings)) {
        newCondition = AlarmCondition::SOIL_HUMIDITY_PERSISTENT_LOW;
    }

    const bool shouldBeActive = (newCondition != AlarmCondition::NONE);

    if (shouldBeActive != s_alarmActive) {
        s_alarmActive = shouldBeActive;
        s_activeCondition = newCondition;
        s_alarmStartedAt = now;
        hwRelaySet(RelayRole::ALARM, 0, shouldBeActive);
        Serial.printf("[ALARM] %s condition=%d\n",
                      shouldBeActive ? "ACTIVADA" : "DESACTIVADA",
                      (int)newCondition);
    } else if (s_alarmActive && newCondition != s_activeCondition) {
        s_activeCondition = newCondition;
        s_alarmStartedAt = now;
        Serial.printf("[ALARM] Condicion cambiada a %d\n", (int)newCondition);
    }
}

static bool alarmIsActive() {
    return s_alarmActive;
}

static AlarmCondition alarmGetActiveCondition() {
    return s_activeCondition;
}

// ----------------------------------------------------------------------------
// Ciclo local (30 s por defecto; configurable en portal, P1)
// ----------------------------------------------------------------------------
static uint32_t s_lastTickMs = 0;
static uint32_t s_cycleCount = 0;
static SensorReadings s_lastReadings = {};

static void localCycleUpdate() {
    const uint32_t now = millis();
    if ((now - s_lastTickMs) < LOCAL_CYCLE_MS) return;
    s_lastTickMs = now;
    s_cycleCount++;

    const SensorReadings readings = hwSensorsRead();
    hwSensorsPrint(readings);
    s_lastReadings = readings;

    const struct tm tmNow = hwRtcNow();
    irrigationEvaluate(tmNow, readings);
    alarmEvaluate(tmNow, readings);
}

// ----------------------------------------------------------------------------
// setup / loop
// ----------------------------------------------------------------------------
static void enterConfigSafely() {
    irrigationStopAll();
    hwRelayAllOff();
    manualUiForceIdle();
    stateMachineEnterConfig();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[RIEGO] Firmware V3 iniciado");

    // Watchdog de tarea principal: 10 s sin alimentar = reset.
    esp_task_wdt_init(10, true);
    esp_task_wdt_add(NULL);

    hwRelayInit();
    hwRtcInit();
    hwButtonsInit();
    hwDisplayInit();
    hwSensorsInit();

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        s_substrateState[z] = {};
        s_sprinklerState[z] = {};
        s_substrateLastScheduleMinute[z] = -1;
        s_sprinklerLastScheduleMinute[z] = -1;
    }

    // P3: la conexion inicial se define cuando exista la capa de red (Fase 3);
    // hasta entonces la pantalla muestra "R:-".
    hwDisplaySetConnStatus(ConnStatus::PENDING);

    s_lastTickMs = millis();
}

void loop() {
    esp_task_wdt_reset();
    hwButtonsUpdate();

    if (s_mode == OperationMode::NORMAL) {
        if (hwButtonConfigPressed()) {
            enterConfigSafely();
            return;
        }

        manualUiUpdate();              // responde al instante a Selector/Confirmar
        irrigationServiceTimeouts();   // apagado por duracion en cada pasada
        localCycleUpdate();            // 30 s: sensores + riego + alarma

        if (!s_selecting) {
            const struct tm t = hwRtcNowLocal();
            const SensorReadings& last = s_lastReadings;
            hwDisplayShowNormal(s_cycleCount, (uint8_t)t.tm_hour, (uint8_t)t.tm_min,
                                (uint8_t)t.tm_sec, last.air,
                                alarmIsActive(), alarmGetActiveCondition());
        }
    } else {
        // Modo Configuracion: el portal llega en Fase 2. Por ahora solo se
        // muestra la pantalla y se espera el timeout para reiniciar.
        hwDisplayShowConfig("192.168.4.1");
        if (millis() >= CONFIG_TIMEOUT_MS) {
            stateMachineRestart();
        }
    }
}