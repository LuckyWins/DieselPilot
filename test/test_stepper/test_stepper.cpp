/*
 * Level stepper tests.
 *
 * The walk talks to a heater that answers every few seconds, and the failure
 * modes are the interesting part: a ladder that will not move any further, a
 * ladder that rolls over instead of stopping, and a heater that goes quiet
 * half way through. None of those are pleasant to stage on real hardware.
 */

#include <unity.h>
#include "../../src/stepper.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t SETTLE   = 4000;
static const uint32_t DEADLINE = 120000;
static const uint8_t  MAXSTEPS = 12;

// A heater whose reading moves one notch per command, bounded at both ends —
// or rolling over, which is the case the stepper has to recognise.
struct FakeHeater {
    int  value;
    int  lo;
    int  hi;
    bool wrap;
};

static FakeHeater ladder(int value, int lo, int hi, bool wrap) {
    FakeHeater h = { value, lo, hi, wrap };
    return h;
}

static StepperInput inputFor(const FakeHeater& h, uint32_t nowMs, bool fresh) {
    StepperInput in;
    in.reading    = h.value;
    in.dataFresh  = fresh;
    in.nowMs      = nowMs;
    in.settleMs   = SETTLE;
    in.deadlineMs = DEADLINE;
    in.maxSteps   = MAXSTEPS;
    return in;
}

static void apply(FakeHeater& h, StepperAction a) {
    int v = h.value + (a == STEPPER_UP ? +1 : -1);
    if(v > h.hi) v = h.wrap ? h.lo : h.hi;
    if(v < h.lo) v = h.wrap ? h.hi : h.lo;
    h.value = v;
}

// Runs the walk the way loop() does: one tick a second, applying each command
// as it comes out, until the stepper reaches a verdict.
static StepperAction runToEnd(StepperState& st, FakeHeater& h, int* commands) {
    uint32_t now  = 0;
    int      sent = 0;

    for(int tick = 0; tick < 300; tick++) {
        now += 1000;
        StepperAction a = stepperNext(st, inputFor(h, now, true));
        if(a == STEPPER_UP || a == STEPPER_DOWN) { apply(h, a); sent++; continue; }
        if(a != STEPPER_WAIT) { if(commands) *commands = sent; return a; }
    }
    if(commands) *commands = sent;
    return STEPPER_WAIT;   // never reached a verdict, which is a failure itself
}

// ── Nothing to do ─────────────────────────────────────────────────────────

void test_idle_stepper_asks_for_nothing(void) {
    StepperState st;
    stepperStop(st);
    FakeHeater h = ladder(3, 1, 6, false);
    TEST_ASSERT_EQUAL(STEPPER_WAIT, stepperNext(st, inputFor(h, 1000, true)));
}

void test_seek_already_on_target_sends_no_command(void) {
    StepperState st;
    FakeHeater   h = ladder(4, 1, 6, false);
    stepperStart(st, STEP_SEEK, 4, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_DONE, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(0, commands);
    TEST_ASSERT_EQUAL(4, h.value);
}

// ── Seeking a value ───────────────────────────────────────────────────────

void test_seek_climbs_one_notch_per_step(void) {
    StepperState st;
    FakeHeater   h = ladder(2, 1, 6, false);
    stepperStart(st, STEP_SEEK, 5, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_DONE, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(3, commands);
    TEST_ASSERT_EQUAL(5, h.value);
}

void test_seek_descends_when_the_target_is_lower(void) {
    StepperState st;
    FakeHeater   h = ladder(22, 5, 35, false);
    stepperStart(st, STEP_SEEK, 18, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_DONE, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(4, commands);
    TEST_ASSERT_EQUAL(18, h.value);
}

// A second command before the heater has reported the first would be counted
// against a reading that has not caught up yet.
void test_seek_waits_out_the_settle_window(void) {
    StepperState st;
    FakeHeater   h = ladder(1, 1, 6, false);
    stepperStart(st, STEP_SEEK, 3, 0, 0);

    TEST_ASSERT_EQUAL(STEPPER_UP, stepperNext(st, inputFor(h, 1000, true)));
    apply(h, STEPPER_UP);
    TEST_ASSERT_EQUAL(STEPPER_WAIT, stepperNext(st, inputFor(h, 2000, true)));
    TEST_ASSERT_EQUAL(STEPPER_WAIT, stepperNext(st, inputFor(h, 4000, true)));
    TEST_ASSERT_EQUAL(STEPPER_UP,   stepperNext(st, inputFor(h, 5000, true)));
}

// A stale reading is the previous value, so judging a step against it would
// either double-count the step or declare the ladder stuck.
void test_seek_does_not_act_on_stale_readings(void) {
    StepperState st;
    FakeHeater   h = ladder(1, 1, 6, false);
    stepperStart(st, STEP_SEEK, 3, 0, 0);

    TEST_ASSERT_EQUAL(STEPPER_WAIT, stepperNext(st, inputFor(h, 1000, false)));
    TEST_ASSERT_EQUAL(STEPPER_UP,   stepperNext(st, inputFor(h, 2000, true)));
}

// ── Ends of the ladder ────────────────────────────────────────────────────

void test_seek_beyond_the_top_reports_stuck(void) {
    StepperState st;
    FakeHeater   h = ladder(5, 1, 6, false);
    stepperStart(st, STEP_SEEK, 9, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_STUCK, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(6, h.value);   // it did get as far as the ladder goes
}

// The V2 case the whole two-phase walk exists for: no level number in the
// frame, so the bottom is found by stepping into it.
void test_floor_stops_at_the_bottom_and_calls_it_done(void) {
    StepperState st;
    FakeHeater   h = ladder(4, 1, 6, false);
    stepperStart(st, STEP_FLOOR, 0, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_DONE, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(1, h.value);
    TEST_ASSERT_EQUAL(4, commands);  // three to descend, one that would not move
}

// A ladder that rolls over never stops moving, so "keep going until it stops"
// would walk it round for ever.
void test_a_wrapping_ladder_is_recognised_not_chased(void) {
    StepperState st;
    FakeHeater   h = ladder(3, 1, 6, true);
    stepperStart(st, STEP_FLOOR, 0, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_WRAPPED, runToEnd(st, h, &commands));
}

// ── Counting steps ────────────────────────────────────────────────────────

void test_count_takes_exactly_the_steps_asked_for(void) {
    StepperState st;
    FakeHeater   h = ladder(1, 1, 6, false);
    stepperStart(st, STEP_COUNT, 3, +1, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_DONE, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(3, commands);
    TEST_ASSERT_EQUAL(4, h.value);
}

// Level 1 after a descent to the floor: already there, nothing to climb.
void test_count_of_zero_is_done_immediately(void) {
    StepperState st;
    FakeHeater   h = ladder(1, 1, 6, false);
    stepperStart(st, STEP_COUNT, 0, +1, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_DONE, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(0, commands);
}

void test_count_that_runs_into_the_top_reports_stuck(void) {
    StepperState st;
    FakeHeater   h = ladder(5, 1, 6, false);
    stepperStart(st, STEP_COUNT, 4, +1, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_STUCK, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(6, h.value);
}

// ── Bounds ────────────────────────────────────────────────────────────────

void test_step_budget_is_not_exceeded(void) {
    StepperState st;
    FakeHeater   h = ladder(1, 1, 100, false);
    stepperStart(st, STEP_SEEK, 50, 0, 0);

    int commands = -1;
    TEST_ASSERT_EQUAL(STEPPER_EXHAUSTED, runToEnd(st, h, &commands));
    TEST_ASSERT_EQUAL(MAXSTEPS, commands);
}

// A heater that goes quiet mid-walk must not leave a stepper running.
void test_the_whole_walk_is_bounded_in_time(void) {
    StepperState st;
    FakeHeater   h = ladder(1, 1, 6, false);
    stepperStart(st, STEP_SEEK, 5, 0, 0);

    TEST_ASSERT_EQUAL(STEPPER_UP, stepperNext(st, inputFor(h, 1000, true)));
    apply(h, STEPPER_UP);
    TEST_ASSERT_EQUAL(STEPPER_STUCK, stepperNext(st, inputFor(h, DEADLINE, true)));
    TEST_ASSERT_EQUAL(STEPPER_WAIT,  stepperNext(st, inputFor(h, DEADLINE + 1000, true)));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_idle_stepper_asks_for_nothing);
    RUN_TEST(test_seek_already_on_target_sends_no_command);

    RUN_TEST(test_seek_climbs_one_notch_per_step);
    RUN_TEST(test_seek_descends_when_the_target_is_lower);
    RUN_TEST(test_seek_waits_out_the_settle_window);
    RUN_TEST(test_seek_does_not_act_on_stale_readings);

    RUN_TEST(test_seek_beyond_the_top_reports_stuck);
    RUN_TEST(test_floor_stops_at_the_bottom_and_calls_it_done);
    RUN_TEST(test_a_wrapping_ladder_is_recognised_not_chased);

    RUN_TEST(test_count_takes_exactly_the_steps_asked_for);
    RUN_TEST(test_count_of_zero_is_done_immediately);
    RUN_TEST(test_count_that_runs_into_the_top_reports_stuck);

    RUN_TEST(test_step_budget_is_not_exceeded);
    RUN_TEST(test_the_whole_walk_is_bounded_in_time);

    return UNITY_END();
}
