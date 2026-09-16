#include "protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 FREQUENCY
// ═══════════════════════════════════════════════════════════════════════════

uint32_t freqToRegisters(uint32_t freqHz) {
    // The intermediate product does not fit in 32 bits, so compute in 64.
    return (uint32_t)(((uint64_t)freqHz * 65536) / CC1101_XTAL_HZ);
}

uint32_t registersToFreq(uint32_t regs) {
    return (uint32_t)(((uint64_t)regs * CC1101_XTAL_HZ) / 65536);
}

// ═══════════════════════════════════════════════════════════════════════════
// SIGNAL STRENGTH
// ═══════════════════════════════════════════════════════════════════════════

int rssiFromRaw(uint8_t raw) {
    int v = raw;
    if(v >= 128) v -= 256;
    return v / 2 - CC1101_RSSI_OFFSET_DB;
}

// ═══════════════════════════════════════════════════════════════════════════
// FUEL
// ═══════════════════════════════════════════════════════════════════════════

uint32_t fuelTickPerSecond(uint16_t doseUl, uint16_t pumpFreqTenths) {
    return (uint32_t)doseUl * (uint32_t)pumpFreqTenths;
}

uint32_t fuelMlFromTicks(uint32_t ticks) {
    return ticks / FUEL_TICKS_PER_ML;
}

// ═══════════════════════════════════════════════════════════════════════════
// TANK
// ═══════════════════════════════════════════════════════════════════════════

void tankDebit(uint32_t& remainingMl, uint32_t ml) {
    remainingMl = (remainingMl > ml) ? remainingMl - ml : 0;
}

void tankRefill(uint32_t& remainingMl, uint32_t capacityMl, uint32_t ml) {
    if(ml == 0 || remainingMl + ml > capacityMl) remainingMl = capacityMl;
    else                                         remainingMl += ml;
}

uint8_t tankWarnLevel(uint32_t remainingMl, uint32_t capacityMl) {
    if(capacityMl == 0) return 0;          // the feature is switched off

    // Integer percent of what is left, rounded down, so a tank sitting
    // exactly on a threshold counts as being at it.
    uint32_t pct = (uint32_t)((uint64_t)remainingMl * 100ULL / capacityMl);

    if(pct <= TANK_WARN_EMPTY_PCT) return 2;
    if(pct <= TANK_WARN_LOW_PCT)   return 1;
    return 0;
}

uint32_t fuelRateMlPerHour(uint32_t fuelMl, uint32_t burnSec, uint16_t doseUl) {
    // Under ten minutes of history the average is mostly ignition and warm-up
    // rather than steady burning, and would read far too high.
    if(burnSec >= 600 && fuelMl > 0) {
        return (uint32_t)((uint64_t)fuelMl * 3600ULL / burnSec);
    }
    uint32_t ticksPerHour =
        fuelTickPerSecond(doseUl, FUEL_NOMINAL_PUMP_HZ_TENTHS) * 3600UL;
    return fuelMlFromTicks(ticksPerHour);
}

uint32_t fuelNeededMl(uint32_t rateMlPerHour, uint16_t minutes) {
    return (uint32_t)((uint64_t)rateMlPerHour * minutes / 60ULL);
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
// DECODERS
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
