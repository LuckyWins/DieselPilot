/*
 * Чистая логика протокола отопителя — без обращений к железу.
 *
 * Всё, что здесь лежит, собирается и на ESP32, и на хосте,
 * поэтому покрывается тестами через `pio test -e native`.
 */

#pragma once

#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════
// СОСТОЯНИЯ ОТОПИТЕЛЯ
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
// КОДЫ ОШИБОК (BYTE[7])
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
// ЧАСТОТА CC1101
// ═══════════════════════════════════════════════════════════════════════════

// Кварц модуля CC1101. Из него считается шаг перестройки частоты:
// 26 МГц / 2^16 = 396.73 Гц на единицу регистра.
#define CC1101_XTAL_HZ 26000000UL

// Переводит частоту в герцах в 24-битное значение регистров FREQ2/FREQ1/FREQ0.
// Старший байт результата идёт в 0x0D, средний в 0x0E, младший в 0x0F.
uint32_t freqToRegisters(uint32_t freqHz);

// Обратное преобразование — нужно, чтобы показать пользователю,
// на какую частоту модуль реально настроился после округления.
uint32_t registersToFreq(uint32_t regs);

// ═══════════════════════════════════════════════════════════════════════════
// CRC-16/MODBUS
// ═══════════════════════════════════════════════════════════════════════════

uint16_t crc16_modbus(const uint8_t* buf, int len);

// ═══════════════════════════════════════════════════════════════════════════
// ДЕКОДЕРЫ
// ═══════════════════════════════════════════════════════════════════════════

const char* getStateName(uint8_t state);
const char* getErrorName(uint8_t code);
