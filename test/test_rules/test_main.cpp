#include <unity.h>

#include "rules.h"

using riego::domain::deadlineReached;
using riego::domain::decideIrrigationTransition;
using riego::domain::EvaluationInputs;
using riego::domain::IrrigationCandidate;
using riego::domain::IrrigationTrigger;
using riego::domain::IrrigationTransition;
using riego::domain::Schedule;
using riego::domain::selectIrrigationCandidate;
using riego::domain::TransitionAction;
using riego::domain::ZonePolicy;
using riego::domain::ZoneRuntime;
using riego::domain::ZoneType;

void setUp() {}
void tearDown() {}

static ZonePolicy defaultPolicy() {
    ZonePolicy policy = {};
    policy.defaultDurationSeconds = 60;
    policy.minHumidityPct = 20;
    return policy;
}

static EvaluationInputs defaultInputs(ZoneType type = ZoneType::SUBSTRATE) {
    EvaluationInputs inputs = {};
    inputs.zoneType = type;
    inputs.maxAirTempC = 45.0f;
    inputs.hour = 8;
    inputs.minute = 30;
    inputs.currentMinuteKey = 1000;
    return inputs;
}

static void assertTrigger(
    IrrigationTrigger expected,
    const IrrigationCandidate& actual
) {
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(expected),
        static_cast<uint8_t>(actual.trigger)
    );
}

static void assertAction(
    TransitionAction expected,
    const IrrigationTransition& actual
) {
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(expected),
        static_cast<uint8_t>(actual.action)
    );
}

void test_idle_when_no_condition_matches() {
    const IrrigationCandidate result = selectIrrigationCandidate(
        defaultPolicy(),
        defaultInputs()
    );

    assertTrigger(IrrigationTrigger::NONE, result);
    TEST_ASSERT_FALSE(result.selected());
    TEST_ASSERT_FALSE(result.valid());
}

void test_manual_has_highest_precedence() {
    ZonePolicy policy = defaultPolicy();
    policy.paused = true;
    policy.schedules[0] = Schedule{8, 30, 10, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.manualActive = true;
    inputs.overridePending = true;
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::MANUAL, result);
    TEST_ASSERT_EQUAL_UINT16(60, result.durationSeconds);
}

void test_override_wins_over_pause_and_automatic_rules() {
    ZonePolicy policy = defaultPolicy();
    policy.paused = true;
    policy.schedules[0] = Schedule{8, 30, 10, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.overridePending = true;
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::OVERRIDE, result);
}

void test_pause_blocks_schedule_and_threshold() {
    ZonePolicy policy = defaultPolicy();
    policy.paused = true;
    policy.schedules[0] = Schedule{8, 30, 10, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::NONE, result);
}

void test_schedule_wins_over_threshold() {
    ZonePolicy policy = defaultPolicy();
    policy.schedules[2] = Schedule{8, 30, 15, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::SCHEDULE, result);
    TEST_ASSERT_EQUAL_UINT16(15, result.durationSeconds);
    TEST_ASSERT_EQUAL_INT8(2, result.scheduleIndex);
}

void test_schedule_does_not_repeat_in_same_minute() {
    ZonePolicy policy = defaultPolicy();
    policy.schedules[0] = Schedule{8, 30, 15, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.hasLastScheduleMinute = true;
    inputs.lastScheduleMinuteKey = inputs.currentMinuteKey;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::NONE, result);
}

void test_schedule_can_run_again_with_new_absolute_minute_key() {
    ZonePolicy policy = defaultPolicy();
    policy.schedules[0] = Schedule{8, 30, 15, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.hasLastScheduleMinute = true;
    inputs.lastScheduleMinuteKey = inputs.currentMinuteKey - (24 * 60);

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::SCHEDULE, result);
}

void test_deduplicated_schedule_falls_through_to_threshold() {
    ZonePolicy policy = defaultPolicy();
    policy.schedules[0] = Schedule{8, 30, 15, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.hasLastScheduleMinute = true;
    inputs.lastScheduleMinuteKey = inputs.currentMinuteKey;
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::THRESHOLD, result);
}

void test_substrate_threshold_requires_valid_dry_reading() {
    ZonePolicy policy = defaultPolicy();
    EvaluationInputs inputs = defaultInputs();
    inputs.soilValid = true;
    inputs.soilHumidityPct = 19.9f;

    IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);
    assertTrigger(IrrigationTrigger::THRESHOLD, result);

    inputs.soilValid = false;
    result = selectIrrigationCandidate(policy, inputs);
    assertTrigger(IrrigationTrigger::NONE, result);
}

void test_sprinkler_threshold_requires_valid_high_air_temperature() {
    ZonePolicy policy = defaultPolicy();
    EvaluationInputs inputs = defaultInputs(ZoneType::SPRINKLER);
    inputs.airValid = true;
    inputs.airTempC = 45.1f;

    IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);
    assertTrigger(IrrigationTrigger::THRESHOLD, result);

    inputs.airTempC = 45.0f;
    result = selectIrrigationCandidate(policy, inputs);
    assertTrigger(IrrigationTrigger::NONE, result);

    inputs.airTempC = 50.0f;
    inputs.airValid = false;
    result = selectIrrigationCandidate(policy, inputs);
    assertTrigger(IrrigationTrigger::NONE, result);
}

void test_zero_duration_manual_vetoes_lower_priority_rules() {
    ZonePolicy policy = defaultPolicy();
    policy.defaultDurationSeconds = 0;
    policy.schedules[0] = Schedule{8, 30, 15, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.manualActive = true;
    inputs.overridePending = true;
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::MANUAL, result);
    TEST_ASSERT_TRUE(result.selected());
    TEST_ASSERT_FALSE(result.valid());
}

void test_inactive_zone_starts_with_valid_candidate() {
    ZoneRuntime runtime = {};
    IrrigationCandidate candidate = {
        IrrigationTrigger::SCHEDULE,
        15,
        0,
    };

    const IrrigationTransition result = decideIrrigationTransition(
        runtime,
        candidate,
        1000
    );

    assertAction(TransitionAction::START, result);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(IrrigationTrigger::SCHEDULE),
        static_cast<uint8_t>(result.trigger)
    );
    TEST_ASSERT_EQUAL_UINT16(15, result.durationSeconds);
}

void test_inactive_zone_ignores_selected_zero_duration() {
    ZoneRuntime runtime = {};
    IrrigationCandidate candidate = {
        IrrigationTrigger::MANUAL,
        0,
        -1,
    };

    const IrrigationTransition result = decideIrrigationTransition(
        runtime,
        candidate,
        1000
    );

    assertAction(TransitionAction::NONE, result);
}

void test_active_zone_stops_when_no_rule_is_selected() {
    ZoneRuntime runtime = {
        true,
        IrrigationTrigger::SCHEDULE,
        60,
        60000000ULL,
        true,
    };
    IrrigationCandidate candidate = {IrrigationTrigger::NONE, 0, -1};

    const IrrigationTransition result = decideIrrigationTransition(
        runtime,
        candidate,
        1000000ULL
    );

    assertAction(TransitionAction::STOP, result);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(IrrigationTrigger::SCHEDULE),
        static_cast<uint8_t>(result.trigger)
    );
}

void test_active_zone_keeps_stored_runtime_when_candidate_changes() {
    ZoneRuntime runtime = {
        true,
        IrrigationTrigger::SCHEDULE,
        15,
        15000000ULL,
        true,
    };
    IrrigationCandidate candidate = {
        IrrigationTrigger::THRESHOLD,
        60,
        -1,
    };

    const IrrigationTransition result = decideIrrigationTransition(
        runtime,
        candidate,
        10000000ULL
    );

    assertAction(TransitionAction::NONE, result);
}

void test_active_zone_stops_at_stored_deadline() {
    ZoneRuntime runtime = {
        true,
        IrrigationTrigger::OVERRIDE,
        60,
        60000000ULL,
        true,
    };
    IrrigationCandidate candidate = {
        IrrigationTrigger::OVERRIDE,
        90,
        -1,
    };

    const IrrigationTransition result = decideIrrigationTransition(
        runtime,
        candidate,
        60000000ULL
    );

    assertAction(TransitionAction::STOP, result);
    TEST_ASSERT_EQUAL_UINT16(60, result.durationSeconds);
}

void test_disabled_timeout_keeps_active_zone_while_rule_selected() {
    ZoneRuntime runtime = {
        true,
        IrrigationTrigger::MANUAL,
        60,
        60000000ULL,
        false,
    };
    IrrigationCandidate candidate = {
        IrrigationTrigger::MANUAL,
        60,
        -1,
    };

    const IrrigationTransition result = decideIrrigationTransition(
        runtime,
        candidate,
        90000000ULL
    );

    assertAction(TransitionAction::NONE, result);
}

void test_zero_duration_schedule_vetoes_later_automatic_rules() {
    ZonePolicy policy = defaultPolicy();
    policy.schedules[0] = Schedule{8, 30, 0, true};
    policy.schedules[1] = Schedule{8, 30, 15, true};

    EvaluationInputs inputs = defaultInputs();
    inputs.soilValid = true;
    inputs.soilHumidityPct = 5.0f;

    const IrrigationCandidate result = selectIrrigationCandidate(policy, inputs);

    assertTrigger(IrrigationTrigger::SCHEDULE, result);
    TEST_ASSERT_TRUE(result.selected());
    TEST_ASSERT_FALSE(result.valid());
}

void test_deadline_uses_monotonic_64_bit_time() {
    TEST_ASSERT_FALSE(deadlineReached(999999ULL, 1000000ULL));
    TEST_ASSERT_TRUE(deadlineReached(1000000ULL, 1000000ULL));
    TEST_ASSERT_TRUE(deadlineReached(1000001ULL, 1000000ULL));
    TEST_ASSERT_FALSE(deadlineReached(1000001ULL, 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_when_no_condition_matches);
    RUN_TEST(test_manual_has_highest_precedence);
    RUN_TEST(test_override_wins_over_pause_and_automatic_rules);
    RUN_TEST(test_pause_blocks_schedule_and_threshold);
    RUN_TEST(test_schedule_wins_over_threshold);
    RUN_TEST(test_schedule_does_not_repeat_in_same_minute);
    RUN_TEST(test_schedule_can_run_again_with_new_absolute_minute_key);
    RUN_TEST(test_deduplicated_schedule_falls_through_to_threshold);
    RUN_TEST(test_substrate_threshold_requires_valid_dry_reading);
    RUN_TEST(test_sprinkler_threshold_requires_valid_high_air_temperature);
    RUN_TEST(test_zero_duration_manual_vetoes_lower_priority_rules);
    RUN_TEST(test_zero_duration_schedule_vetoes_later_automatic_rules);
    RUN_TEST(test_inactive_zone_starts_with_valid_candidate);
    RUN_TEST(test_inactive_zone_ignores_selected_zero_duration);
    RUN_TEST(test_active_zone_stops_when_no_rule_is_selected);
    RUN_TEST(test_active_zone_keeps_stored_runtime_when_candidate_changes);
    RUN_TEST(test_active_zone_stops_at_stored_deadline);
    RUN_TEST(test_disabled_timeout_keeps_active_zone_while_rule_selected);
    RUN_TEST(test_deadline_uses_monotonic_64_bit_time);
    return UNITY_END();
}