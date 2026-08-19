#include "rules.h"

namespace riego {
namespace domain {

static IrrigationCandidate noCandidate() {
    IrrigationCandidate result = {};
    result.trigger = IrrigationTrigger::NONE;
    result.scheduleIndex = -1;
    return result;
}

static IrrigationCandidate candidate(
    IrrigationTrigger trigger,
    uint16_t durationSeconds,
    int8_t scheduleIndex = -1
) {
    IrrigationCandidate result = {};
    result.trigger = trigger;
    result.durationSeconds = durationSeconds;
    result.scheduleIndex = scheduleIndex;
    return result;
}

IrrigationCandidate selectIrrigationCandidate(
    const ZonePolicy& policy,
    const EvaluationInputs& inputs
) {
    if (inputs.manualActive) {
        return candidate(IrrigationTrigger::MANUAL, policy.defaultDurationSeconds);
    }

    if (inputs.overridePending) {
        return candidate(IrrigationTrigger::OVERRIDE, policy.defaultDurationSeconds);
    }

    if (policy.paused) return noCandidate();

    const bool scheduleAlreadyRan = inputs.hasLastScheduleMinute &&
                                    inputs.lastScheduleMinuteKey == inputs.currentMinuteKey;
    if (!scheduleAlreadyRan) {
        for (size_t i = 0; i < kMaxSchedulesPerZone; ++i) {
            const Schedule& schedule = policy.schedules[i];
            if (schedule.enabled &&
                schedule.hour == inputs.hour &&
                schedule.minute == inputs.minute) {
                return candidate(
                    IrrigationTrigger::SCHEDULE,
                    schedule.durationSeconds,
                    static_cast<int8_t>(i)
                );
            }
        }
    }

    if (inputs.zoneType == ZoneType::SUBSTRATE) {
        if (inputs.soilValid && inputs.soilHumidityPct < policy.minHumidityPct) {
            return candidate(IrrigationTrigger::THRESHOLD, policy.defaultDurationSeconds);
        }
    } else if (inputs.airValid && inputs.airTempC > inputs.maxAirTempC) {
        return candidate(IrrigationTrigger::THRESHOLD, policy.defaultDurationSeconds);
    }

    return noCandidate();
}

IrrigationTransition decideIrrigationTransition(
    const ZoneRuntime& runtime,
    const IrrigationCandidate& candidate,
    uint64_t nowUs
) {
    IrrigationTransition result = {};
    result.action = TransitionAction::NONE;

    if (!runtime.active) {
        if (candidate.valid()) {
            result.action = TransitionAction::START;
            result.trigger = candidate.trigger;
            result.durationSeconds = candidate.durationSeconds;
        }
        return result;
    }

    const bool timedOut = runtime.timeoutEnabled &&
                          deadlineReached(nowUs, runtime.deadlineUs);
    if (!candidate.selected() || timedOut) {
        result.action = TransitionAction::STOP;
        result.trigger = runtime.trigger;
        result.durationSeconds = runtime.durationSeconds;
    }
    return result;
}

bool deadlineReached(uint64_t nowUs, uint64_t deadlineUs) {
    return deadlineUs != 0 && nowUs >= deadlineUs;
}

}  // namespace domain
}  // namespace riego