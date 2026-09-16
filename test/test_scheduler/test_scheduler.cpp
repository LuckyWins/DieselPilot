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


// -- Ignition --──────────────────────────────────────────────────────────────

static const uint32_t SEC = 1000UL;

// A start was commanded a moment ago and the heater has not answered yet.
static IgnitionInput baseIgnition(void) {
    IgnitionInput in;
    in.watching      = true;
    in.attempts      = 1;
    in.nowMs         = 1000 * MIN;
    in.commandedAtMs = 1000 * MIN;
    in.heaterState   = STATE_OFF;
    in.dataFresh     = true;
    in.timeoutMs     = IGNITION_TIMEOUT_MS;
    in.maxAttempts   = IGNITION_MAX_TRIES;
    return in;
}

void test_ignition_not_watching_is_silent(void) {
    IgnitionInput in = baseIgnition();
    in.watching = false;
    in.nowMs    = in.commandedAtMs + 10 * MIN;
    TEST_ASSERT_EQUAL(IGN_NONE, checkIgnition(in));
}

void test_ignition_confirmed_when_the_heater_leaves_off(void) {
    IgnitionInput in = baseIgnition();
    in.heaterState = STATE_STARTUP;
    TEST_ASSERT_EQUAL(IGN_CONFIRMED, checkIgnition(in));
}

void test_ignition_waits_out_the_window(void) {
    IgnitionInput in = baseIgnition();
    in.nowMs = in.commandedAtMs + 29 * SEC;
    TEST_ASSERT_EQUAL(IGN_NONE, checkIgnition(in));
}

void test_ignition_retries_once_the_window_passes(void) {
    IgnitionInput in = baseIgnition();
    in.nowMs = in.commandedAtMs + 31 * SEC;
    TEST_ASSERT_EQUAL(IGN_RETRY, checkIgnition(in));
}

void test_ignition_fails_after_the_last_attempt(void) {
    IgnitionInput in = baseIgnition();
    in.attempts = IGNITION_MAX_TRIES;
    in.nowMs    = in.commandedAtMs + 31 * SEC;
    TEST_ASSERT_EQUAL(IGN_FAILED, checkIgnition(in));
}

// The command is a toggle. Re-sending it to a heater that did light but was
// not heard would switch it back off -- the exact opposite of the intent.
void test_ignition_never_retries_on_stale_data(void) {
    IgnitionInput in = baseIgnition();
    in.dataFresh = false;
    in.nowMs     = in.commandedAtMs + 31 * SEC;
    TEST_ASSERT_EQUAL(IGN_FAILED, checkIgnition(in));
}

// A stale reading showing RUNNING is the previous state, not proof that this
// command worked.
void test_ignition_does_not_confirm_on_stale_data(void) {
    IgnitionInput in = baseIgnition();
    in.heaterState = STATE_RUNNING;
    in.dataFresh   = false;
    TEST_ASSERT_EQUAL(IGN_NONE, checkIgnition(in));
}

// Whatever the reason it left OFF, there is nothing left to verify.
void test_ignition_confirmed_even_if_someone_else_started_it(void) {
    IgnitionInput in = baseIgnition();
    in.heaterState = STATE_RUNNING;
    in.nowMs       = in.commandedAtMs + 5 * MIN;
    TEST_ASSERT_EQUAL(IGN_CONFIRMED, checkIgnition(in));
}


// -- Scheduled start --──────────────────────────────────────────────────────

// An arbitrary but realistic epoch: the exact value does not matter, only the
// differences between now and the target.
static const uint32_t EPOCH_NOW = 1788000000UL;

// A schedule armed for five minutes from now, heater cold, clock synced.
static StartInput baseStart(void) {
    StartInput in;
    in.armed        = true;
    in.targetEpoch  = EPOCH_NOW + 5 * 60;
    in.nowEpoch     = EPOCH_NOW;
    in.timeValid    = true;
    in.heaterState  = STATE_OFF;
    in.heaterPaired = true;
    in.graceMin     = START_GRACE_MIN;
    return in;
}

void test_start_waits_until_its_time(void) {
    TEST_ASSERT_EQUAL(START_NONE, decideStart(baseStart()));
}

void test_start_fires_on_time(void) {
    StartInput in = baseStart();
    in.nowEpoch = in.targetEpoch;
    TEST_ASSERT_EQUAL(START_FIRE, decideStart(in));
}

void test_start_still_fires_slightly_late(void) {
    StartInput in = baseStart();
    in.nowEpoch = in.targetEpoch + 29 * 60;   // grace is 30 minutes
    TEST_ASSERT_EQUAL(START_FIRE, decideStart(in));
}

// The realistic case: the schedule fell inside the overnight power cut, and
// the controller only came back hours later.
void test_start_missed_while_powered_down(void) {
    StartInput in = baseStart();
    in.nowEpoch = in.targetEpoch + 3 * 3600;
    TEST_ASSERT_EQUAL(START_MISSED, decideStart(in));
}

void test_start_disarmed_does_nothing(void) {
    StartInput in = baseStart();
    in.armed    = false;
    in.nowEpoch = in.targetEpoch + 60;
    TEST_ASSERT_EQUAL(START_NONE, decideStart(in));
}

// A clock still on the 1970 epoch would make every schedule look overdue.
void test_start_never_fires_without_a_synced_clock(void) {
    StartInput in = baseStart();
    in.timeValid = false;
    in.nowEpoch  = in.targetEpoch + 60;
    TEST_ASSERT_EQUAL(START_NONE, decideStart(in));
}

void test_start_skipped_when_already_burning(void) {
    StartInput in = baseStart();
    in.nowEpoch    = in.targetEpoch;
    in.heaterState = STATE_RUNNING;
    TEST_ASSERT_EQUAL(START_SKIP_RUNNING, decideStart(in));
}

void test_start_needs_a_paired_heater(void) {
    StartInput in = baseStart();
    in.heaterPaired = false;
    in.nowEpoch     = in.targetEpoch;
    TEST_ASSERT_EQUAL(START_NONE, decideStart(in));
}

// A start inside the pre-blackout window would be lit and stopped moments
// later, so it is refused when the schedule is set rather than obeyed.
void test_start_collision_with_the_shutdown_window(void) {
    // Blackout 22:00, shut down 15 minutes earlier -> window is 21:45..22:00
    TEST_ASSERT_TRUE (startCollidesWithShutdown(21 * 60 + 50, 22 * 60, 15));
    TEST_ASSERT_TRUE (startCollidesWithShutdown(21 * 60 + 45, 22 * 60, 15));
    TEST_ASSERT_FALSE(startCollidesWithShutdown(21 * 60 + 44, 22 * 60, 15));
    TEST_ASSERT_FALSE(startCollidesWithShutdown(22 * 60,      22 * 60, 15));
    TEST_ASSERT_FALSE(startCollidesWithShutdown(6 * 60,       22 * 60, 15));
}

void test_start_collision_when_the_window_wraps_midnight(void) {
    // Blackout 00:30, shut down 45 minutes earlier -> window is 23:45..00:30
    TEST_ASSERT_TRUE (startCollidesWithShutdown(23 * 60 + 50, 30, 45));
    TEST_ASSERT_TRUE (startCollidesWithShutdown(10,           30, 45));
    TEST_ASSERT_FALSE(startCollidesWithShutdown(23 * 60 + 40, 30, 45));
}

// ── Manual start ──────────────────────────────────────────────────────────
//
// The chat and the web GUI both come through checkManualStart(). Before it
// existed the GUI sent a bare toggle: it could light the heater ten minutes
// before the mains went, which is exactly the abuse the whole module is here
// to prevent.

// Heater off, clock synced, 10:00, mains cut at 22:00 with a 15 min lead.
static ManualStartInput baseManualStart(void) {
    ManualStartInput in;
    in.heaterPaired    = true;
    in.heaterState     = STATE_OFF;
    in.timeValid       = true;
    in.nowMinutes      = 10 * 60;
    in.blackoutEnabled = true;
    in.blackoutMinutes = 22 * 60;
    in.shutdownLeadMin = 15;
    return in;
}

void test_manual_start_allowed_mid_day(void) {
    TEST_ASSERT_EQUAL(START_ALLOWED, checkManualStart(baseManualStart()));
}

void test_manual_start_needs_a_paired_heater(void) {
    ManualStartInput in = baseManualStart();
    in.heaterPaired = false;
    TEST_ASSERT_EQUAL(START_NO_HEATER, checkManualStart(in));
}

// The command is a toggle, so sending it to a running heater stops it.
void test_manual_start_refused_while_running(void) {
    ManualStartInput in = baseManualStart();
    in.heaterState = STATE_RUNNING;
    TEST_ASSERT_EQUAL(START_BUSY, checkManualStart(in));
}

// Mid-purge the toggle is simply lost, and restarting now would abort a purge
// that has to finish.
void test_manual_start_refused_while_purging(void) {
    ManualStartInput in = baseManualStart();
    in.heaterState = STATE_COOLING;
    TEST_ASSERT_EQUAL(START_BUSY, checkManualStart(in));
}

// The case this was written for: 21:50 against a 22:00 cut.
void test_manual_start_refused_inside_the_window(void) {
    ManualStartInput in = baseManualStart();
    in.nowMinutes = 21 * 60 + 50;
    TEST_ASSERT_EQUAL(START_TOO_LATE, checkManualStart(in));
}

void test_manual_start_allowed_a_minute_before_the_window(void) {
    ManualStartInput in = baseManualStart();
    in.nowMinutes = 21 * 60 + 44;
    TEST_ASSERT_EQUAL(START_ALLOWED, checkManualStart(in));
}

void test_manual_start_window_wraps_past_midnight(void) {
    ManualStartInput in = baseManualStart();
    in.blackoutMinutes = 30;        // 00:30
    in.shutdownLeadMin = 45;        // window opens at 23:45
    in.nowMinutes      = 23 * 60 + 50;
    TEST_ASSERT_EQUAL(START_TOO_LATE, checkManualStart(in));
    in.nowMinutes = 10;             // 00:10, still inside
    TEST_ASSERT_EQUAL(START_TOO_LATE, checkManualStart(in));
    in.nowMinutes = 23 * 60 + 40;   // just before it opens
    TEST_ASSERT_EQUAL(START_ALLOWED, checkManualStart(in));
}

void test_manual_start_ignores_the_window_when_blackout_is_off(void) {
    ManualStartInput in = baseManualStart();
    in.blackoutEnabled = false;
    in.nowMinutes      = 21 * 60 + 50;
    TEST_ASSERT_EQUAL(START_ALLOWED, checkManualStart(in));
}

// The realistic morning: mains power is back before the modem is, so the
// clock is still at 1970. Refusing on that would leave the heater
// uncontrollable precisely when somebody wants it on.
void test_manual_start_ignores_the_window_without_a_synced_clock(void) {
    ManualStartInput in = baseManualStart();
    in.timeValid  = false;
    in.nowMinutes = -1;
    TEST_ASSERT_EQUAL(START_ALLOWED, checkManualStart(in));
}

void test_minutes_until_counts_forward_around_the_clock(void) {
    TEST_ASSERT_EQUAL(10,   minutesUntil(21 * 60 + 50, 22 * 60));
    TEST_ASSERT_EQUAL(0,    minutesUntil(22 * 60,      22 * 60));
    TEST_ASSERT_EQUAL(40,   minutesUntil(23 * 60 + 50, 30));        // past midnight
    TEST_ASSERT_EQUAL(1439, minutesUntil(1,            0));
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
    RUN_TEST(test_ignition_not_watching_is_silent);
    RUN_TEST(test_ignition_confirmed_when_the_heater_leaves_off);
    RUN_TEST(test_ignition_waits_out_the_window);
    RUN_TEST(test_ignition_retries_once_the_window_passes);
    RUN_TEST(test_ignition_fails_after_the_last_attempt);
    RUN_TEST(test_ignition_never_retries_on_stale_data);
    RUN_TEST(test_ignition_does_not_confirm_on_stale_data);
    RUN_TEST(test_ignition_confirmed_even_if_someone_else_started_it);

    RUN_TEST(test_start_waits_until_its_time);
    RUN_TEST(test_start_fires_on_time);
    RUN_TEST(test_start_still_fires_slightly_late);
    RUN_TEST(test_start_missed_while_powered_down);
    RUN_TEST(test_start_disarmed_does_nothing);
    RUN_TEST(test_start_never_fires_without_a_synced_clock);
    RUN_TEST(test_start_skipped_when_already_burning);
    RUN_TEST(test_start_needs_a_paired_heater);
    RUN_TEST(test_start_collision_with_the_shutdown_window);
    RUN_TEST(test_start_collision_when_the_window_wraps_midnight);

    RUN_TEST(test_manual_start_allowed_mid_day);
    RUN_TEST(test_manual_start_needs_a_paired_heater);
    RUN_TEST(test_manual_start_refused_while_running);
    RUN_TEST(test_manual_start_refused_while_purging);
    RUN_TEST(test_manual_start_refused_inside_the_window);
    RUN_TEST(test_manual_start_allowed_a_minute_before_the_window);
    RUN_TEST(test_manual_start_window_wraps_past_midnight);
    RUN_TEST(test_manual_start_ignores_the_window_when_blackout_is_off);
    RUN_TEST(test_manual_start_ignores_the_window_without_a_synced_clock);
    RUN_TEST(test_minutes_until_counts_forward_around_the_clock);

    RUN_TEST(test_day_window_basics);

    return UNITY_END();
}
