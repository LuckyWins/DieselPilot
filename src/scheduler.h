/*
 * Shutdown scheduling — pure logic with no hardware access.
 *
 * Two independent mechanisms decide when the heater must be stopped,
 * whichever comes first:
 *
 *   1. A runtime limit, protecting against "switched it on and forgot".
 *   2. An absolute deadline before the mains power is cut.
 *
 * The second one is the reason this module exists at all. Cutting power to a
 * running diesel heater skips the purge cycle: unburnt fuel stays in the
 * chamber and the heat exchanger cools with no airflow. Done nightly, that
 * cokes up the burner. So the heater has to be stopped early enough for the
 * purge to finish before the power goes.
 *
 * Everything here is a pure function of its input, so it builds on the host
 * and is covered by `pio test -e native`. Reproducing "21:45 with unsynced
 * time" on real hardware is awkward, and this code switches off a heater.
 */

#pragma once

#include <stdint.h>

// Extra slack on top of the expected purge duration before the controller
// decides something went wrong and raises an alarm.
#define SCHED_COOLDOWN_MARGIN_MIN 3

#define MINUTES_PER_DAY 1440

enum SchedulerAction {
    SCHED_NONE = 0,          // nothing to do
    SCHED_SHUT_DOWN,         // send the off command now
    SCHED_COOLDOWN_DONE,     // heater reached OFF after a scheduled shutdown
    SCHED_COOLDOWN_TIMEOUT,  // it failed to reach OFF in the expected time
};

enum SchedulerReason {
    REASON_NONE = 0,
    REASON_RUNTIME_LIMIT,    // the heater has been running for too long
    REASON_BLACKOUT,         // mains power is about to be cut
};

struct SchedulerDecision {
    SchedulerAction action;
    SchedulerReason reason;
};

struct SchedulerInput {
    // -- Heater --
    uint8_t  heaterState;         // STATE_* from protocol.h
    bool     heaterPaired;

    // -- Monotonic clock (millis) --
    uint32_t nowMs;
    uint32_t heaterOnSinceMs;     // when the heater last left OFF
    bool     heaterOnSinceValid;

    // -- Wall clock --
    bool     timeValid;           // false until NTP has synced
    int      nowMinutes;          // minutes since local midnight

    // -- Shutdown in progress --
    bool     shutdownRequested;
    uint32_t shutdownAtMs;        // when the off command was sent

    // -- Settings --
    uint16_t autoOffMin;          // runtime limit, 0 = disabled
    bool     blackoutEnabled;
    int      blackoutMinutes;     // when mains power is cut
    uint16_t shutdownLeadMin;     // how early to stop before that
    uint16_t cooldownExpectedMin; // how long the purge normally takes
};

// True when the heater is off or already on its way there: shutdown ordered,
// burner out, fan purging. The protocol only offers a power toggle, so a
// command sent in any of these states would start the heater back up.
bool heaterIsOffOrStopping(uint8_t state);

// True if `now` falls inside [start, end) on a 24-hour circle. The window may
// wrap past midnight, which it does whenever the lead time pushes the
// shutdown into the previous day.
bool inDayWindow(int now, int start, int end);

SchedulerDecision decideShutdown(const SchedulerInput& in);

// ═══════════════════════════════════════════════════════════════════════════
// IGNITION
// ═══════════════════════════════════════════════════════════════════════════
//
// Shutdown is watched all the way to OFF and raises an alarm if it never gets
// there. Starting had no such check at all: the command went out over the air
// and was forgotten, so a heater that failed to light said nothing until
// somebody arrived to a cold garage. This closes that asymmetry.

// How long one attempt is given before it counts as failed, and how many
// attempts are made. State is polled every 3 s while a command is recent,
// so the timeout covers roughly ten readings.
#define IGNITION_TIMEOUT_MS  30000
#define IGNITION_MAX_TRIES   2

enum IgnitionAction {
    IGN_NONE = 0,       // still within the window, or not watching
    IGN_CONFIRMED,      // the heater left OFF, it is lighting
    IGN_RETRY,          // send the command once more
    IGN_FAILED,         // give up and raise the alarm
};

struct IgnitionInput {
    bool     watching;        // a start was commanded and is being verified
    uint8_t  attempts;        // commands sent so far, 1 after the first
    uint32_t nowMs;
    uint32_t commandedAtMs;
    uint8_t  heaterState;
    // False when the heater has not been heard from recently. Retrying blind
    // is the dangerous case: the command is a toggle, so re-sending it to a
    // heater that did light would switch it back off.
    bool     dataFresh;
    uint32_t timeoutMs;
    uint8_t  maxAttempts;
};

IgnitionAction checkIgnition(const IgnitionInput& in);

// ═══════════════════════════════════════════════════════════════════════════
// SCHEDULED START
// ═══════════════════════════════════════════════════════════════════════════
//
// The whole point of this installation is a preheat an hour or two before
// arrival, which otherwise means remembering to press a button at the right
// moment. The target is stored as an absolute epoch timestamp rather than a
// time of day: mains power is cut overnight, so a schedule set in the evening
// has to survive a reboot, and "is 07:00 today or tomorrow" has no answer
// after one.

// How late a start may fire after its target. Booting at 06:05 for a 06:00
// schedule is a late start; three hours after a blackout is a surprise.
#define START_GRACE_MIN 30

enum StartAction {
    START_NONE = 0,       // waiting, or nothing scheduled
    START_FIRE,           // light the heater now
    START_MISSED,         // the target passed while the controller was down
    START_SKIP_RUNNING,   // already burning, nothing to do
};

struct StartInput {
    bool     armed;
    uint32_t targetEpoch;
    uint32_t nowEpoch;
    bool     timeValid;
    uint8_t  heaterState;
    bool     heaterPaired;
    uint16_t graceMin;
};

StartAction decideStart(const StartInput& in);

// True if a start at `startMinutes` would land inside the window during which
// the scheduler shuts the heater down before the mains cut -- where it would
// be lit and immediately stopped. Checked when the schedule is set, so the
// refusal is immediate and explained rather than surprising later.
bool startCollidesWithShutdown(int startMinutes, int blackoutMinutes,
                               int shutdownLeadMin);
