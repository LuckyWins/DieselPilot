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
// CRC-16/MODBUS
// ═══════════════════════════════════════════════════════════════════════════

uint16_t crc16_modbus(const uint8_t* buf, int len);

// ═══════════════════════════════════════════════════════════════════════════
// DECODERS
// ═══════════════════════════════════════════════════════════════════════════

const char* getStateName(uint8_t state);
const char* getErrorName(uint8_t code);
