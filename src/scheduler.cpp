#include "scheduler.h"
#include "protocol.h"

bool heaterIsOffOrStopping(uint8_t state) {
    switch(state) {
        case STATE_OFF:
        case STATE_SHUTDOWN:
        case STATE_SHUTTING_DOWN:
        case STATE_COOLING:
            return true;
        default:
            return false;
    }
}

bool inDayWindow(int now, int start, int end) {
    if(start == end) return false;
    if(start < end)  return now >= start && now < end;
    return now >= start || now < end;   // wraps past midnight
}

SchedulerDecision decideShutdown(const SchedulerInput& in) {
    SchedulerDecision none = { SCHED_NONE, REASON_NONE };

    if(!in.heaterPaired) return none;

    // A shutdown is already under way: watch the purge through
    // SHUTDOWN -> SHUTTING_DOWN -> COOLING until the heater reports OFF.
    if(in.shutdownRequested) {
        if(in.heaterState == STATE_OFF) {
            SchedulerDecision d = { SCHED_COOLDOWN_DONE, REASON_NONE };
            return d;
        }
        uint32_t limitMs = (uint32_t)(in.cooldownExpectedMin + SCHED_COOLDOWN_MARGIN_MIN) * 60000UL;
        if(in.nowMs - in.shutdownAtMs > limitMs) {
            SchedulerDecision d = { SCHED_COOLDOWN_TIMEOUT, REASON_NONE };
            return d;
        }
        return none;   // still purging
    }

    if(in.heaterState == STATE_OFF) return none;

    // Runtime limit.
    if(in.autoOffMin > 0 && in.heaterOnSinceValid) {
        uint32_t limitMs = (uint32_t)in.autoOffMin * 60000UL;
        if(in.nowMs - in.heaterOnSinceMs >= limitMs) {
            SchedulerDecision d = { SCHED_SHUT_DOWN, REASON_RUNTIME_LIMIT };
            return d;
        }
    }

    // Absolute deadline before the mains cut. Skipped entirely without a
    // synced clock: acting on a 1970 timestamp would be worse than not acting.
    if(in.blackoutEnabled && in.timeValid && in.nowMinutes >= 0) {
        int deadline = in.blackoutMinutes - (int)in.shutdownLeadMin;
        while(deadline < 0) deadline += MINUTES_PER_DAY;
        deadline %= MINUTES_PER_DAY;

        // The window ends at the blackout itself: past that there is no power
        // to command anything with anyway. Note this also stops a heater
        // switched on inside the window -- deliberate, since the purge would
        // not finish before the power goes.
        if(inDayWindow(in.nowMinutes, deadline, in.blackoutMinutes)) {
            SchedulerDecision d = { SCHED_SHUT_DOWN, REASON_BLACKOUT };
            return d;
        }
    }

    return none;
}

// ═══════════════════════════════════════════════════════════════════════════
// IGNITION
// ═══════════════════════════════════════════════════════════════════════════

IgnitionAction checkIgnition(const IgnitionInput& in) {
    if(!in.watching) return IGN_NONE;

    // Leaving OFF is the heater acknowledging the command. Only trust it
    // against a fresh reading -- a stale one is the previous state, not proof.
    if(in.heaterState != STATE_OFF && in.dataFresh) return IGN_CONFIRMED;

    if(in.nowMs - in.commandedAtMs < in.timeoutMs) return IGN_NONE;

    // Without fresh data there is nothing to retry against: the heater may
    // well be running and simply unheard, and the command is a toggle.
    if(in.attempts < in.maxAttempts && in.dataFresh) return IGN_RETRY;

    return IGN_FAILED;
}

// ═══════════════════════════════════════════════════════════════════════════
// SCHEDULED START
// ═══════════════════════════════════════════════════════════════════════════

StartAction decideStart(const StartInput& in) {
    if(!in.armed || !in.heaterPaired) return START_NONE;

    // Comparing epochs against a clock that still believes it is 1970 would
    // fire everything at once.
    if(!in.timeValid) return START_NONE;

    if(in.nowEpoch < in.targetEpoch) return START_NONE;

    // Too late to be a preheat. A heater that lights itself hours after the
    // time it was asked for is a surprise, not a service -- most often this
    // is a schedule that fell inside the overnight power cut.
    if(in.nowEpoch - in.targetEpoch > (uint32_t)in.graceMin * 60UL) {
        return START_MISSED;
    }

    if(in.heaterState != STATE_OFF) return START_SKIP_RUNNING;

    return START_FIRE;
}

bool startCollidesWithShutdown(int startMinutes, int blackoutMinutes,
                               int shutdownLeadMin) {
    int deadline = blackoutMinutes - shutdownLeadMin;
    while(deadline < 0) deadline += MINUTES_PER_DAY;
    deadline %= MINUTES_PER_DAY;
    return inDayWindow(startMinutes, deadline, blackoutMinutes);
}
