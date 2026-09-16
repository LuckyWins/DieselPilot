/*
 * Pure heater protocol logic — no hardware access.
 *
 * Everything here builds both for the ESP32 and for the host, so it is
 * covered by tests via `pio test -e native`.
 */

#pragma once

#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════
// HEATER STATES
// ═══════════════════════════════════════════════════════════════════════════

#define STATE_OFF            0
#define STATE_STARTUP        1
#define STATE_WARMING        2
#define STATE_WARMING_WAIT   3
#define STATE_PRE_RUN        4
#define STATE_RUNNING        5
#define STATE_SHUTDOWN       6
#define STATE_SHUTTING_DOWN  7
#define STATE_COOLING        8

// ═══════════════════════════════════════════════════════════════════════════
// ERROR CODES (BYTE[7])
// ═══════════════════════════════════════════════════════════════════════════

#define ERR_NONE           0x00
#define ERR_ON             0x01
#define ERR_UNDERVOLTAGE   0x02
#define ERR_OVERVOLTAGE    0x03
#define ERR_SPARK_PLUG     0x04
#define ERR_OIL_PUMP       0x05
#define ERR_OVERHEAT       0x06
#define ERR_MOTOR          0x07
#define ERR_DISCONNECT     0x08
#define ERR_EXTINGUISHED   0x09
#define ERR_SENSOR         0x0A
#define ERR_IGNITION       0x0B
#define ERR_STANDBY        0x0C

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 FREQUENCY
// ═══════════════════════════════════════════════════════════════════════════

// CC1101 crystal. It defines the tuning step:
// 26 MHz / 2^16 = 396.73 Hz per register unit.
#define CC1101_XTAL_HZ 26000000UL

// Converts a frequency in hertz into the 24-bit FREQ2/FREQ1/FREQ0 value.
// High byte goes to 0x0D, middle to 0x0E, low to 0x0F.
uint32_t freqToRegisters(uint32_t freqHz);

// Reverse conversion — needed to show which frequency the module actually
// settled on after rounding to the tuning step.
uint32_t registersToFreq(uint32_t regs);

// ═══════════════════════════════════════════════════════════════════════════
// SIGNAL STRENGTH
// ═══════════════════════════════════════════════════════════════════════════

// Offset for the 433 MHz band, per the CC1101 datasheet.
#define CC1101_RSSI_OFFSET_DB 74

// Converts a raw CC1101 RSSI byte into dBm. The value is two's complement:
// anything from 128 up is negative. Used both for the byte the radio appends
// to a received frame and for the RSSI status register.
int rssiFromRaw(uint8_t raw);

// ═══════════════════════════════════════════════════════════════════════════
// FUEL
// ═══════════════════════════════════════════════════════════════════════════
//
// Consumption follows directly from the pump: each stroke doses a fixed
// volume, so litres per hour = dose (ml) x frequency (Hz) x 3.6. Most pumps
// on these heaters dose 0.020-0.023 ml and run between 1.4 and 5.5 Hz.
//
// Everything is integer to keep the floating-point printf machinery out of
// it. One tick of the accumulator is 0.1 microlitre: adding
// doseUl x pumpFreqTenths once a second gives dose x Hz microlitres per
// second, scaled by ten.

#define FUEL_DOSE_UL_DEFAULT 22

// One second's worth of accumulator ticks at the given pump rate.
uint32_t fuelTickPerSecond(uint16_t doseUl, uint16_t pumpFreqTenths);

// Ticks are 0.1 ul, so this many of them make a millilitre. Exposed because
// billing whole millilitres to a running total has to carry the remainder:
// rounding it away every second would lose most of the fuel.
#define FUEL_TICKS_PER_ML 10000UL

// Accumulated ticks converted to millilitres, rounded down.
uint32_t fuelMlFromTicks(uint32_t ticks);

// ═══════════════════════════════════════════════════════════════════════════
// TANK
// ═══════════════════════════════════════════════════════════════════════════
//
// What is left in the tank is worked out by dead reckoning from the pump --
// there is no sensor, and none is needed for the question that actually
// matters here, which is whether a hundred-kilometre drive ends at a heater
// that runs. The estimate drifts, because the dose varies with pump wear,
// temperature and supply voltage, so it is always shown as an approximation
// and a real refill resets the accumulated error outright.

// Warning bands, as a percentage of tank capacity.
#define TANK_WARN_LOW_PCT   25
#define TANK_WARN_EMPTY_PCT 10

// A pump rate to estimate with before the device has burnt enough to have an
// average of its own. Mid-range for these heaters.
#define FUEL_NOMINAL_PUMP_HZ_TENTHS 30

// Takes `ml` out of the tank, stopping at empty rather than wrapping.
void tankDebit(uint32_t& remainingMl, uint32_t ml);

// Puts `ml` in, stopping at capacity. `ml` of zero means a full tank, which
// is what "/filled" without a figure says.
void tankRefill(uint32_t& remainingMl, uint32_t capacityMl, uint32_t ml);

// 0 when there is plenty, 1 below TANK_WARN_LOW_PCT, 2 below
// TANK_WARN_EMPTY_PCT. Warning when this rises -- and only then -- is what
// keeps one message per crossing; a refill lowers it and rearms the warning
// with no separate latch to reset.
uint8_t tankWarnLevel(uint32_t remainingMl, uint32_t capacityMl);

// Millilitres an hour, averaged over whatever history exists. Falls back to
// the nominal rate at this dose until there is enough of it, so a fresh
// device still estimates rather than refusing to.
uint32_t fuelRateMlPerHour(uint32_t fuelMl, uint32_t burnSec, uint16_t doseUl);

// What a burn of that length would take at that rate.
uint32_t fuelNeededMl(uint32_t rateMlPerHour, uint16_t minutes);

// ═══════════════════════════════════════════════════════════════════════════
// CRC-16/MODBUS
// ═══════════════════════════════════════════════════════════════════════════

uint16_t crc16_modbus(const uint8_t* buf, int len);

// ═══════════════════════════════════════════════════════════════════════════
// DECODERS
// ═══════════════════════════════════════════════════════════════════════════

const char* getStateName(uint8_t state);
const char* getErrorName(uint8_t code);
