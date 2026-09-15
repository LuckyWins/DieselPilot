#include "protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// ЧАСТОТА CC1101
// ═══════════════════════════════════════════════════════════════════════════

uint32_t freqToRegisters(uint32_t freqHz) {
    // Промежуточное произведение не влезает в 32 бита, считаем в 64.
    return (uint32_t)(((uint64_t)freqHz * 65536) / CC1101_XTAL_HZ);
}

uint32_t registersToFreq(uint32_t regs) {
    return (uint32_t)(((uint64_t)regs * CC1101_XTAL_HZ) / 65536);
}

// ═══════════════════════════════════════════════════════════════════════════
// CRC-16/MODBUS
// ═══════════════════════════════════════════════════════════════════════════

uint16_t crc16_modbus(const uint8_t* buf, int len) {
    uint16_t crc = 0xFFFF;
    for(int pos = 0; pos < len; pos++) {
        crc ^= (uint8_t)buf[pos];
        for(int i = 8; i != 0; i--) {
            if((crc & 0x0001) != 0) { crc >>= 1; crc ^= 0xA001; }
            else { crc >>= 1; }
        }
    }
    return crc;
}

// ═══════════════════════════════════════════════════════════════════════════
// ДЕКОДЕРЫ
// ═══════════════════════════════════════════════════════════════════════════

const char* getStateName(uint8_t state) {
    switch(state) {
        case STATE_OFF:          return "OFF";
        case STATE_STARTUP:      return "STARTUP";
        case STATE_WARMING:      return "WARMING";
        case STATE_WARMING_WAIT: return "WARM WAIT";
        case STATE_PRE_RUN:      return "PRE-RUN";
        case STATE_RUNNING:      return "RUNNING";
        case STATE_SHUTDOWN:     return "SHUTDOWN";
        case STATE_SHUTTING_DOWN:return "SHUTTING";
        case STATE_COOLING:      return "COOLING";
        default:                 return "UNKNOWN";
    }
}

const char* getErrorName(uint8_t code) {
    switch(code) {
        case ERR_NONE:         return "NORMAL";
        case ERR_ON:           return "NORMAL";
        case ERR_UNDERVOLTAGE: return "UNDERVOLTAGE";
        case ERR_OVERVOLTAGE:  return "OVERVOLTAGE";
        case ERR_SPARK_PLUG:   return "SPARK PLUG";
        case ERR_OIL_PUMP:     return "OIL PUMP";
        case ERR_OVERHEAT:     return "OVERHEAT";
        case ERR_MOTOR:        return "MOTOR";
        case ERR_DISCONNECT:   return "DISCONNECT";
        case ERR_EXTINGUISHED: return "EXTINGUISHED";
        case ERR_SENSOR:       return "SENSOR";
        case ERR_IGNITION:     return "IGNITION";
        case ERR_STANDBY:      return "STANDBY";
        default:               return "UNKNOWN";
    }
}
