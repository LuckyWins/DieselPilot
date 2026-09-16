/*
 * Driving the heater to a requested level — pure logic, no hardware access.
 *
 * The protocol has no "set level" command, only UP and DOWN by one. Sending
 * five UPs in a row does not work: sendCommand() already repeats each frame
 * ten times and the heater answers on its own schedule, so a burst is as
 * likely to be counted once as five times. The only reliable way is one step
 * at a time, each confirmed by the next status poll before the next goes out.
 *
 * What is being driven depends on the heater:
 *
 *   AUTO        the setpoint in degrees, which UP/DOWN move by one
 *   MANUAL, V1  the power level 1..6, carried outright by the V1 frame
 *   MANUAL, V2  the V2 frame has no level number at all, only the pump rate.
 *               So the ladder is walked rather than addressed: step down to
 *               the bottom, then climb to the level asked for. That needs no
 *               table of pump frequencies and survives a heater whose ladder
 *               is not the one we guessed.
 *
 * Everything here is a pure function of its input, so it builds on the host
 * and is covered by `pio test -e native`.
 */

#pragma once

#include <stdint.h>

enum StepperMode {
    STEP_SEEK = 0,   // step until the reading equals the target
    STEP_FLOOR,      // step down until the reading stops changing
    STEP_COUNT,      // take a fixed number of confirmed steps
};

enum StepperAction {
    STEPPER_WAIT = 0,   // not running, or waiting for the reading to settle
    STEPPER_UP,         // send CMD_UP
    STEPPER_DOWN,       // send CMD_DOWN
    STEPPER_DONE,
    STEPPER_STUCK,      // the reading stopped responding short of the target
    STEPPER_WRAPPED,    // the ladder rolled over instead of stopping at an end
    STEPPER_EXHAUSTED,  // the step budget ran out
};

struct StepperState {
    bool        running;
    StepperMode mode;
    int         target;        // SEEK: the wanted reading. COUNT: steps left.
    int8_t      countDir;      // COUNT: +1 to climb, -1 to descend
    int8_t      lastDir;       // direction of the command still being judged
    int         lastSeen;      // reading at the moment that command went out
    uint32_t    startedAtMs;
    uint32_t    sentAtMs;
    uint8_t     sent;
};

struct StepperInput {
    int      reading;       // the value being driven
    bool     dataFresh;     // false when the heater has not been heard from
    uint32_t nowMs;
    uint32_t settleMs;      // how long one step is given to show up
    uint32_t deadlineMs;    // bound on the whole operation
    uint8_t  maxSteps;
};

// `target` is the wanted reading for SEEK and the number of steps for COUNT;
// FLOOR ignores it. `countDir` only matters for COUNT.
void stepperStart(StepperState& st, StepperMode mode, int target,
                  int8_t countDir, uint32_t nowMs);

void stepperStop(StepperState& st);

// Advances the walk by at most one command. The caller sends the frame that
// comes back and calls again on the next tick; everything else -- confirming
// the previous step, spotting a ladder that will not move, counting the
// budget -- happens in here.
StepperAction stepperNext(StepperState& st, const StepperInput& in);
