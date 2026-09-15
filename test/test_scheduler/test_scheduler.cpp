/*
 * Shutdown scheduler tests.
 *
 * This is the most valuable suite in the project: the code under test
 * switches off a heater, and states like "21:45 with an unsynced clock"
 * or "the window wraps past midnight" are awkward to stage on real hardware.
 */

#include <unity.h>
#include "../../src/scheduler.h"
#include "../../src/protocol.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t MIN = 60000UL;

// A heater that has been running for one minute, nothing scheduled,
// clock synced, 10:00 in the morning. Tests tweak one field at a time.
static SchedulerInput baseInput(void) {
    SchedulerInput in;
    in.heaterState         = STATE_RUNNING;
    in.heaterPaired        = true;
    in.nowMs               = 1000 * MIN;
    in.heaterOnSinceMs     = 999 * MIN;
    in.heaterOnSinceValid  = true;
    in.timeValid           = true;
    in.nowMinutes          = 10 * 60;
    in.shutdownRequested   = false;
    in.shutdownAtMs        = 0;
    in.autoOffMin          = 180;
    in.blackoutEnabled     = true;
    in.blackoutMinutes     = 22 * 60;
    in.shutdownLeadMin     = 15;
    in.cooldownExpectedMin = 5;
    return in;
}

// -- Idle cases ------------------------------------------------------------

void test_running_heater_mid_day_is_left_alone(void) {
    SchedulerDecision d = decideShutdown(baseInput());
    TEST_ASSERT_EQUAL(SCHED_NONE, d.action);
}

void test_unpaired_heater_is_never_touched(void) {
    SchedulerInput in = baseInput();
    in.heaterPaired = false;
    in.nowMinutes   = 21 * 60 + 50;   // inside the blackout window
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// An already-off heater must not receive a command: the protocol only has a
// power toggle, so that would switch it back on.
void test_off_heater_gets_no_command(void) {
    SchedulerInput in = baseInput();
    in.heaterState = STATE_OFF;
    in.nowMinutes  = 21 * 60 + 50;
    in.nowMs       = 5000 * MIN;      // runtime limit long exceeded too
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// -- Runtime limit ---------------------------------------------------------

void test_runtime_limit_fires_when_exceeded(void) {
    SchedulerInput in = baseInput();
    in.heaterOnSinceMs = in.nowMs - 180 * MIN;
    SchedulerDecision d = decideShutdown(in);
    TEST_ASSERT_EQUAL(SCHED_SHUT_DOWN, d.action);
    TEST_ASSERT_EQUAL(REASON_RUNTIME_LIMIT, d.reason);
}

void test_runtime_limit_does_not_fire_early(void) {
    SchedulerInput in = baseInput();
    in.heaterOnSinceMs = in.nowMs - 179 * MIN;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

void test_runtime_limit_zero_disables_it(void) {
    SchedulerInput in = baseInput();
    in.autoOffMin      = 0;
    in.heaterOnSinceMs = in.nowMs - 5000 * MIN;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// The runtime limit works on the monotonic clock, so it stays useful
// even when NTP never came up.
void test_runtime_limit_works_without_a_synced_clock(void) {
    SchedulerInput in = baseInput();
    in.timeValid       = false;
    in.nowMinutes      = -1;
    in.heaterOnSinceMs = in.nowMs - 200 * MIN;
    SchedulerDecision d = decideShutdown(in);
    TEST_ASSERT_EQUAL(SCHED_SHUT_DOWN, d.action);
    TEST_ASSERT_EQUAL(REASON_RUNTIME_LIMIT, d.reason);
}

// -- Blackout deadline -----------------------------------------------------

void test_blackout_fires_at_the_lead_time(void) {
    SchedulerInput in = baseInput();
    in.nowMinutes = 21 * 60 + 45;     // 22:00 minus 15 minutes
    SchedulerDecision d = decideShutdown(in);
    TEST_ASSERT_EQUAL(SCHED_SHUT_DOWN, d.action);
    TEST_ASSERT_EQUAL(REASON_BLACKOUT, d.reason);
}

void test_blackout_does_not_fire_a_minute_early(void) {
    SchedulerInput in = baseInput();
    in.nowMinutes = 21 * 60 + 44;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// Without a synced clock the deadline must be skipped entirely: acting on a
// 1970 timestamp is worse than not acting.
void test_blackout_is_skipped_without_a_synced_clock(void) {
    SchedulerInput in = baseInput();
    in.timeValid  = false;
    in.autoOffMin = 0;                // isolate the deadline
    in.nowMinutes = 21 * 60 + 50;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

void test_blackout_can_be_switched_off(void) {
    SchedulerInput in = baseInput();
    in.blackoutEnabled = false;
    in.autoOffMin      = 0;
    in.nowMinutes      = 21 * 60 + 50;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// A heater switched on inside the window is stopped straight away: the purge
// would not finish before the power goes.
void test_heater_started_inside_the_window_is_stopped(void) {
    SchedulerInput in = baseInput();
    in.nowMinutes      = 21 * 60 + 50;
    in.heaterOnSinceMs = in.nowMs;    // just started
    TEST_ASSERT_EQUAL(SCHED_SHUT_DOWN, decideShutdown(in).action);
}

// Lead time long enough to push the deadline into the previous day.
void test_blackout_window_wraps_past_midnight(void) {
    SchedulerInput in = baseInput();
    in.autoOffMin      = 0;
    in.blackoutMinutes = 30;          // 00:30
    in.shutdownLeadMin = 45;          // deadline lands at 23:45

    in.nowMinutes = 23 * 60 + 50;     // before midnight, inside
    TEST_ASSERT_EQUAL(SCHED_SHUT_DOWN, decideShutdown(in).action);

    in.nowMinutes = 10;               // after midnight, still inside
    TEST_ASSERT_EQUAL(SCHED_SHUT_DOWN, decideShutdown(in).action);

    in.nowMinutes = 23 * 60 + 40;     // just before the window
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);

    in.nowMinutes = 40;               // past the blackout itself
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// -- Purge tracking --------------------------------------------------------

void test_purge_in_progress_produces_no_new_command(void) {
    SchedulerInput in = baseInput();
    in.shutdownRequested = true;
    in.shutdownAtMs      = in.nowMs - 2 * MIN;
    in.heaterState       = STATE_COOLING;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

void test_purge_completion_is_reported(void) {
    SchedulerInput in = baseInput();
    in.shutdownRequested = true;
    in.shutdownAtMs      = in.nowMs - 4 * MIN;
    in.heaterState       = STATE_OFF;
    TEST_ASSERT_EQUAL(SCHED_COOLDOWN_DONE, decideShutdown(in).action);
}

void test_purge_that_overruns_raises_an_alarm(void) {
    SchedulerInput in = baseInput();
    in.shutdownRequested = true;
    in.heaterState       = STATE_COOLING;
    // 5 min expected + 3 min margin
    in.shutdownAtMs      = in.nowMs - 9 * MIN;
    TEST_ASSERT_EQUAL(SCHED_COOLDOWN_TIMEOUT, decideShutdown(in).action);
}

void test_purge_is_not_declared_overrun_too_early(void) {
    SchedulerInput in = baseInput();
    in.shutdownRequested = true;
    in.heaterState       = STATE_COOLING;
    in.shutdownAtMs      = in.nowMs - 7 * MIN;
    TEST_ASSERT_EQUAL(SCHED_NONE, decideShutdown(in).action);
}

// -- Helpers ---------------------------------------------------------------

void test_off_or_stopping_covers_the_whole_shutdown_path(void) {
    TEST_ASSERT_TRUE(heaterIsOffOrStopping(STATE_OFF));
    TEST_ASSERT_TRUE(heaterIsOffOrStopping(STATE_SHUTDOWN));
    TEST_ASSERT_TRUE(heaterIsOffOrStopping(STATE_SHUTTING_DOWN));
    TEST_ASSERT_TRUE(heaterIsOffOrStopping(STATE_COOLING));

    TEST_ASSERT_FALSE(heaterIsOffOrStopping(STATE_STARTUP));
    TEST_ASSERT_FALSE(heaterIsOffOrStopping(STATE_WARMING));
    TEST_ASSERT_FALSE(heaterIsOffOrStopping(STATE_RUNNING));
}

void test_day_window_basics(void) {
    TEST_ASSERT_TRUE(inDayWindow(100, 50, 150));
    TEST_ASSERT_TRUE(inDayWindow(50, 50, 150));     // start is inclusive
    TEST_ASSERT_FALSE(inDayWindow(150, 50, 150));   // end is exclusive
    TEST_ASSERT_FALSE(inDayWindow(49, 50, 150));
    TEST_ASSERT_FALSE(inDayWindow(100, 100, 100));  // empty window
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_running_heater_mid_day_is_left_alone);
    RUN_TEST(test_unpaired_heater_is_never_touched);
    RUN_TEST(test_off_heater_gets_no_command);

    RUN_TEST(test_runtime_limit_fires_when_exceeded);
    RUN_TEST(test_runtime_limit_does_not_fire_early);
    RUN_TEST(test_runtime_limit_zero_disables_it);
    RUN_TEST(test_runtime_limit_works_without_a_synced_clock);

    RUN_TEST(test_blackout_fires_at_the_lead_time);
    RUN_TEST(test_blackout_does_not_fire_a_minute_early);
    RUN_TEST(test_blackout_is_skipped_without_a_synced_clock);
    RUN_TEST(test_blackout_can_be_switched_off);
    RUN_TEST(test_heater_started_inside_the_window_is_stopped);
    RUN_TEST(test_blackout_window_wraps_past_midnight);

    RUN_TEST(test_purge_in_progress_produces_no_new_command);
    RUN_TEST(test_purge_completion_is_reported);
    RUN_TEST(test_purge_that_overruns_raises_an_alarm);
    RUN_TEST(test_purge_is_not_declared_overrun_too_early);

    RUN_TEST(test_off_or_stopping_covers_the_whole_shutdown_path);
    RUN_TEST(test_day_window_basics);

    return UNITY_END();
}
