#include "stepper.h"

void stepperStart(StepperState& st, StepperMode mode, int target,
                  int8_t countDir, uint32_t nowMs) {
    st.running     = true;
    st.mode        = mode;
    st.target      = target;
    st.countDir    = countDir;
    st.lastDir     = 0;
    st.lastSeen    = 0;
    st.startedAtMs = nowMs;
    st.sentAtMs    = 0;
    st.sent        = 0;
}

void stepperStop(StepperState& st) {
    st.running = false;
}

StepperAction stepperNext(StepperState& st, const StepperInput& in) {
    if(!st.running) return STEPPER_WAIT;

    // One bound on the whole walk, so a heater that goes quiet mid-way cannot
    // leave a stepper running for ever.
    if(in.nowMs - st.startedAtMs >= in.deadlineMs) {
        st.running = false;
        return STEPPER_STUCK;
    }

    // Every decision below compares readings, and a stale reading is the
    // previous value rather than evidence of anything.
    if(!in.dataFresh) return STEPPER_WAIT;

    if(st.sent > 0) {
        // Judge the step already sent before sending another, or the walk
        // would run away from a heater that reports once every few seconds.
        if(in.nowMs - st.sentAtMs < in.settleMs) return STEPPER_WAIT;

        int moved = in.reading - st.lastSeen;

        if(moved == 0) {
            st.running = false;
            // The bottom of the ladder is precisely what FLOOR was after.
            return (st.mode == STEP_FLOOR) ? STEPPER_DONE : STEPPER_STUCK;
        }

        // Moving against the command means the ladder rolled over rather than
        // stopping at its end. Carrying on would walk it round in circles.
        if((st.lastDir > 0 && moved < 0) || (st.lastDir < 0 && moved > 0)) {
            st.running = false;
            return STEPPER_WRAPPED;
        }

        if(st.mode == STEP_COUNT && --st.target <= 0) {
            st.running = false;
            return STEPPER_DONE;
        }
    }

    if(st.mode == STEP_SEEK && in.reading == st.target) {
        st.running = false;
        return STEPPER_DONE;
    }

    if(st.mode == STEP_COUNT && st.target <= 0) {
        st.running = false;
        return STEPPER_DONE;
    }

    if(st.sent >= in.maxSteps) {
        st.running = false;
        return STEPPER_EXHAUSTED;
    }

    int8_t dir;
    if(st.mode == STEP_SEEK)       dir = (st.target > in.reading) ? +1 : -1;
    else if(st.mode == STEP_FLOOR) dir = -1;
    else                           dir = st.countDir;

    st.lastDir  = dir;
    st.lastSeen = in.reading;
    st.sentAtMs = in.nowMs;
    st.sent++;

    return (dir > 0) ? STEPPER_UP : STEPPER_DOWN;
}
