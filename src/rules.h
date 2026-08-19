#ifndef RULES_H
#define RULES_H

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// rules.h — Motor de reglas de riego PURAS (sin Arduino, sin GPIO, sin red).
// Portado tal cual de V2 (lib/domain/irrigation_engine.h) con la revision
// menor de D7. Las pruebas nativas de test/rules cubren esta API.
// ============================================================================

namespace riego {
namespace domain {

static const size_t kMaxSchedulesPerZone = 4;

enum class ZoneType : uint8_t {
    SUBSTRATE = 0,
    SPRINKLER = 1
};

enum class IrrigationTrigger : uint8_t {
    NONE = 0,
    MANUAL = 1,
    OVERRIDE = 2,
    SCHEDULE = 3,
    THRESHOLD = 4
};

struct Schedule {
    uint8_t hour;
    uint8_t minute;
    uint16_t durationSeconds;
    bool enabled;
};

struct ZonePolicy {
    bool paused;
    uint16_t defaultDurationSeconds;
    uint8_t minHumidityPct;
    Schedule schedules[kMaxSchedulesPerZone];
};

struct EvaluationInputs {
    ZoneType zoneType;
    bool manualActive;
    bool overridePending;
    bool soilValid;
    float soilHumidityPct;
    bool airValid;
    float airTempC;
    float maxAirTempC;
    uint8_t hour;
    uint8_t minute;
    int64_t currentMinuteKey;
    bool hasLastScheduleMinute;
    int64_t lastScheduleMinuteKey;
};

struct IrrigationCandidate {
    IrrigationTrigger trigger;
    uint16_t durationSeconds;
    int8_t scheduleIndex;

    bool selected() const {
        return trigger != IrrigationTrigger::NONE;
    }

    bool valid() const {
        return selected() && durationSeconds > 0;
    }
};

enum class TransitionAction : uint8_t {
    NONE = 0,
    START = 1,
    STOP = 2
};

struct ZoneRuntime {
    bool active;
    IrrigationTrigger trigger;
    uint16_t durationSeconds;
    uint64_t deadlineUs;
    bool timeoutEnabled;
};

struct IrrigationTransition {
    TransitionAction action;
    IrrigationTrigger trigger;
    uint16_t durationSeconds;
};

// Precedencia: manual > override > (paused -> idle) > schedule/threshold.
// Sin efectos secundarios; no accede a Arduino, GPIO, tiempo, storage ni red.
IrrigationCandidate selectIrrigationCandidate(
    const ZonePolicy& policy,
    const EvaluationInputs& inputs
);

// Transiciones de runtime: una zona activa conserva su trigger y duracion
// originales mientras una regla siga seleccionada; se detiene cuando no hay
// regla seleccionada o vence su deadline almacenado.
IrrigationTransition decideIrrigationTransition(
    const ZoneRuntime& runtime,
    const IrrigationCandidate& candidate,
    uint64_t nowUs
);

// Deadlines con reloj monotónico de 64 bits; cero = inactivo.
bool deadlineReached(uint64_t nowUs, uint64_t deadlineUs);

}  // namespace domain
}  // namespace riego

#endif // RULES_H