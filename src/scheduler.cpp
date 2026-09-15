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
