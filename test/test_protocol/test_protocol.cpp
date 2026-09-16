/*
 * Tests for the pure protocol logic: frequency maths, CRC, decoders.
 */

#include <unity.h>
#include <string.h>
#include "../../src/protocol.h"

void setUp(void) {}
void tearDown(void) {}

// -- Frequency -─────────────────────────────────────────────────────────────

// Tuning step: 26 MHz / 2^16. Nothing can be targeted more precisely.
static const uint32_t FREQ_STEP_HZ = 397;

// V2 register set from cc1101_init(): FREQ2=0x10, FREQ1=0xB0, FREQ0=0x9C.
void test_v2_registers_match_declared_frequency(void) {
    TEST_ASSERT_EQUAL_HEX32(0x10B09C, freqToRegisters(433937000UL));
}

void test_v2_registers_decode_back(void) {
    uint32_t hz = registersToFreq(0x10B09C);
    TEST_ASSERT_UINT32_WITHIN(FREQ_STEP_HZ, 433937000UL, hz);
}

// V1 register set from cc1101_init_V1(): FREQ2=0x10, FREQ1=0xB0, FREQ0=0x71.
//
// Both the code and the web GUI dropdown label this "433.892 MHz", but the
// registers yield ~433.920. This test pins down the real hardware behaviour
// so that the label gets corrected to match it, not the other way round.
void test_v1_registers_are_not_the_documented_frequency(void) {
    uint32_t hz = registersToFreq(0x10B071);

    TEST_ASSERT_UINT32_WITHIN(1000UL, 433920000UL, hz);
    TEST_ASSERT_TRUE(hz > 433900000UL);   // the "433.892" label does not match
}

void test_frequency_roundtrip_is_stable(void) {
    const uint32_t probes[] = {433050000UL, 433920000UL, 433937000UL, 434790000UL};

    for(unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint32_t back = registersToFreq(freqToRegisters(probes[i]));
        TEST_ASSERT_UINT32_WITHIN(FREQ_STEP_HZ, probes[i], back);
    }
}

// The freqHz * 65536 product exceeds 32 bits — make sure the intermediate
// arithmetic does not overflow.
void test_frequency_does_not_overflow_32bit(void) {
    TEST_ASSERT_EQUAL_HEX32(0x10B09C, freqToRegisters(433937000UL));
    TEST_ASSERT_TRUE(freqToRegisters(928000000UL) > freqToRegisters(433937000UL));
}

// -- CRC-16/MODBUS --────────────────────────────────────────────────────────

// Standard check vector: "123456789" -> 0x4B37.
void test_crc_reference_vector(void) {
    const uint8_t data[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x4B37, crc16_modbus(data, 9));
}

void test_crc_empty_is_initial_value(void) {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16_modbus((const uint8_t*)"", 0));
}

void test_crc_detects_single_bit_flip(void) {
    uint8_t frame[] = {0x09, 0x23, 0xCA, 0x00, 0x44, 0x5B, 0x01};
    uint16_t good = crc16_modbus(frame, sizeof(frame));

    frame[3] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL(good, crc16_modbus(frame, sizeof(frame)));
}

// -- Signal strength --──────────────────────────────────────────────────────

// The raw byte is two's complement, so the sign flips at 128. Getting that
// boundary wrong silently halves the reported range.
void test_rssi_positive_half(void) {
    TEST_ASSERT_EQUAL_INT(-74, rssiFromRaw(0));
    TEST_ASSERT_EQUAL_INT(-24, rssiFromRaw(100));
    TEST_ASSERT_EQUAL_INT(-11, rssiFromRaw(127));
}

void test_rssi_negative_half(void) {
    TEST_ASSERT_EQUAL_INT(-138, rssiFromRaw(128));
    TEST_ASSERT_EQUAL_INT(-102, rssiFromRaw(200));
}

// 128 is the first negative value and must not be read as the largest
// positive one.
void test_rssi_sign_boundary(void) {
    TEST_ASSERT_TRUE(rssiFromRaw(128) < rssiFromRaw(127));
}

// Integer division truncates toward zero, so 255 lands on -74 rather than
// -75. Pinned because it is what the firmware has always reported.
void test_rssi_truncation_at_the_top(void) {
    TEST_ASSERT_EQUAL_INT(-74, rssiFromRaw(255));
}

void test_rssi_stays_in_a_plausible_range(void) {
    for(int raw = 0; raw <= 255; raw++) {
        int dbm = rssiFromRaw((uint8_t)raw);
        TEST_ASSERT_TRUE(dbm >= -138 && dbm <= -11);
    }
}

// -- Decoders --────────────────────────────────────────────────────────────

void test_state_names(void) {
    TEST_ASSERT_EQUAL_STRING("OFF",     getStateName(STATE_OFF));
    TEST_ASSERT_EQUAL_STRING("RUNNING", getStateName(STATE_RUNNING));
    TEST_ASSERT_EQUAL_STRING("COOLING", getStateName(STATE_COOLING));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", getStateName(200));
}

void test_error_names(void) {
    TEST_ASSERT_EQUAL_STRING("OVERHEAT", getErrorName(ERR_OVERHEAT));
    TEST_ASSERT_EQUAL_STRING("OIL PUMP", getErrorName(ERR_OIL_PUMP));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",  getErrorName(0xFE));
}

// Codes 0x00 and 0x01 both mean normal operation. Notification
// suppression relies on that, so pin it down with a test.
void test_error_none_and_on_are_both_normal(void) {
    TEST_ASSERT_EQUAL_STRING("NORMAL", getErrorName(ERR_NONE));
    TEST_ASSERT_EQUAL_STRING("NORMAL", getErrorName(ERR_ON));
}


// -- Fuel --─────────────────────────────────────────────────────────────────

// Reference from the heater documentation: 0.022 ml per stroke at 2.4 Hz
// works out to 0.19 l/h. One hour of accumulation must land on 190 ml.
void test_fuel_reference_hour_at_2_4_hz(void) {
    uint32_t ticks = 0;
    for(int sec = 0; sec < 3600; sec++) ticks += fuelTickPerSecond(22, 24);
    TEST_ASSERT_EQUAL_UINT32(190, fuelMlFromTicks(ticks));
}

// The other published figure: 0.022 ml at 5 Hz is 0.396 l/h.
void test_fuel_reference_hour_at_5_hz(void) {
    uint32_t ticks = 0;
    for(int sec = 0; sec < 3600; sec++) ticks += fuelTickPerSecond(22, 50);
    TEST_ASSERT_EQUAL_UINT32(396, fuelMlFromTicks(ticks));
}

void test_fuel_scales_with_the_pump_rate(void) {
    TEST_ASSERT_EQUAL_UINT32(2 * fuelTickPerSecond(22, 20),
                                 fuelTickPerSecond(22, 40));
}

void test_fuel_is_zero_while_the_pump_is_idle(void) {
    TEST_ASSERT_EQUAL_UINT32(0, fuelTickPerSecond(22, 0));
    TEST_ASSERT_EQUAL_UINT32(0, fuelMlFromTicks(0));
}

// Pumps differ, so the dose is a setting rather than a constant.
void test_fuel_honours_a_different_dose(void) {
    uint32_t a = 0, b = 0;
    for(int sec = 0; sec < 3600; sec++) {
        a += fuelTickPerSecond(20, 24);
        b += fuelTickPerSecond(23, 24);
    }
    TEST_ASSERT_EQUAL_UINT32(172, fuelMlFromTicks(a));
    TEST_ASSERT_EQUAL_UINT32(198, fuelMlFromTicks(b));
}

// The accumulator is session-scoped, but confirm the headroom: at the highest
// realistic rate it must not wrap during any plausible burn.
void test_fuel_accumulator_has_headroom(void) {
    uint32_t perSec = fuelTickPerSecond(23, 55);        // worst realistic case
    uint32_t week   = perSec * 7UL * 24UL * 3600UL;
    TEST_ASSERT_TRUE(week / perSec == 7UL * 24UL * 3600UL);   // no wrap
}

// ── Tank ──────────────────────────────────────────────────────────────────
//
// Dead reckoning with no sensor, so the arithmetic is all there is between
// the estimate and a drive to a heater that will not run.

void test_tank_debit_takes_fuel_out(void) {
    uint32_t left = 10000;
    tankDebit(left, 2500);
    TEST_ASSERT_EQUAL_UINT32(7500, left);
}

// An estimate that has drifted low must read empty, never wrap to a full tank.
void test_tank_debit_stops_at_empty(void) {
    uint32_t left = 300;
    tankDebit(left, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, left);
    tankDebit(left, 1);
    TEST_ASSERT_EQUAL_UINT32(0, left);
}

void test_tank_refill_adds_what_was_poured_in(void) {
    uint32_t left = 2000;
    tankRefill(left, 12000, 5000);
    TEST_ASSERT_EQUAL_UINT32(7000, left);
}

// "/filled" with no figure means the tank is full, which is also the moment
// the accumulated drift is wiped out.
void test_tank_refill_without_an_amount_means_full(void) {
    uint32_t left = 2000;
    tankRefill(left, 12000, 0);
    TEST_ASSERT_EQUAL_UINT32(12000, left);
}

void test_tank_refill_cannot_overfill(void) {
    uint32_t left = 10000;
    tankRefill(left, 12000, 5000);
    TEST_ASSERT_EQUAL_UINT32(12000, left);
}

void test_tank_warning_bands(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tankWarnLevel(6000, 12000));   // half
    TEST_ASSERT_EQUAL_UINT8(0, tankWarnLevel(3120, 12000));   // 26%
    TEST_ASSERT_EQUAL_UINT8(1, tankWarnLevel(3000, 12000));   // 25%
    TEST_ASSERT_EQUAL_UINT8(1, tankWarnLevel(1500, 12000));   // 12.5%
    TEST_ASSERT_EQUAL_UINT8(2, tankWarnLevel(1200, 12000));   // 10%
    TEST_ASSERT_EQUAL_UINT8(2, tankWarnLevel(0,    12000));
}

// Capacity of zero switches the whole feature off, and a device that has
// never been told its tank size must not start crying empty.
void test_tank_warning_is_silent_without_a_capacity(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tankWarnLevel(0, 0));
}

// Warning on a rise and only on a rise is what gives one message per
// crossing, and a refill rearms it without a latch to reset.
void test_tank_warning_rises_once_and_resets_on_a_refill(void) {
    uint32_t cap  = 12000;
    uint32_t left = 4000;
    uint8_t  last = tankWarnLevel(left, cap);
    TEST_ASSERT_EQUAL_UINT8(0, last);

    tankDebit(left, 1500);                       // down to 20%
    uint8_t now = tankWarnLevel(left, cap);
    TEST_ASSERT_TRUE(now > last);                // warns
    last = now;

    tankDebit(left, 500);                        // still in the same band
    TEST_ASSERT_FALSE(tankWarnLevel(left, cap) > last);

    tankRefill(left, cap, 0);
    TEST_ASSERT_EQUAL_UINT8(0, tankWarnLevel(left, cap));
}

// ── Consumption estimate ──────────────────────────────────────────────────

void test_fuel_rate_uses_real_history_once_there_is_some(void) {
    // Two hours of burning on one litre.
    TEST_ASSERT_EQUAL_UINT32(500, fuelRateMlPerHour(1000, 7200, 22));
}

// Ten minutes of history is mostly ignition and warm-up, which would read
// far higher than a steady burn.
void test_fuel_rate_falls_back_before_there_is_history(void) {
    uint32_t nominal = fuelRateMlPerHour(0, 0, 22);
    TEST_ASSERT_EQUAL_UINT32(237, nominal);                 // 22 ul at 3.0 Hz
    TEST_ASSERT_EQUAL_UINT32(nominal, fuelRateMlPerHour(50, 300, 22));
    TEST_ASSERT_EQUAL_UINT32(nominal, fuelRateMlPerHour(0, 99999, 22));
}

void test_fuel_rate_follows_the_dose_when_falling_back(void) {
    TEST_ASSERT_TRUE(fuelRateMlPerHour(0, 0, 30) > fuelRateMlPerHour(0, 0, 22));
}

void test_fuel_needed_for_a_planned_burn(void) {
    TEST_ASSERT_EQUAL_UINT32(500,  fuelNeededMl(500, 60));
    TEST_ASSERT_EQUAL_UINT32(750,  fuelNeededMl(500, 90));
    TEST_ASSERT_EQUAL_UINT32(0,    fuelNeededMl(500, 0));
    TEST_ASSERT_EQUAL_UINT32(1000, fuelNeededMl(500, 120));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_v2_registers_match_declared_frequency);
    RUN_TEST(test_v2_registers_decode_back);
    RUN_TEST(test_v1_registers_are_not_the_documented_frequency);
    RUN_TEST(test_frequency_roundtrip_is_stable);
    RUN_TEST(test_frequency_does_not_overflow_32bit);

    RUN_TEST(test_crc_reference_vector);
    RUN_TEST(test_crc_empty_is_initial_value);
    RUN_TEST(test_crc_detects_single_bit_flip);

    RUN_TEST(test_rssi_positive_half);
    RUN_TEST(test_rssi_negative_half);
    RUN_TEST(test_rssi_sign_boundary);
    RUN_TEST(test_rssi_truncation_at_the_top);
    RUN_TEST(test_rssi_stays_in_a_plausible_range);

    RUN_TEST(test_fuel_reference_hour_at_2_4_hz);
    RUN_TEST(test_fuel_reference_hour_at_5_hz);
    RUN_TEST(test_fuel_scales_with_the_pump_rate);
    RUN_TEST(test_fuel_is_zero_while_the_pump_is_idle);
    RUN_TEST(test_fuel_honours_a_different_dose);
    RUN_TEST(test_fuel_accumulator_has_headroom);

    RUN_TEST(test_tank_debit_takes_fuel_out);
    RUN_TEST(test_tank_debit_stops_at_empty);
    RUN_TEST(test_tank_refill_adds_what_was_poured_in);
    RUN_TEST(test_tank_refill_without_an_amount_means_full);
    RUN_TEST(test_tank_refill_cannot_overfill);
    RUN_TEST(test_tank_warning_bands);
    RUN_TEST(test_tank_warning_is_silent_without_a_capacity);
    RUN_TEST(test_tank_warning_rises_once_and_resets_on_a_refill);

    RUN_TEST(test_fuel_rate_uses_real_history_once_there_is_some);
    RUN_TEST(test_fuel_rate_falls_back_before_there_is_history);
    RUN_TEST(test_fuel_rate_follows_the_dose_when_falling_back);
    RUN_TEST(test_fuel_needed_for_a_planned_burn);

    RUN_TEST(test_state_names);
    RUN_TEST(test_error_names);
    RUN_TEST(test_error_none_and_on_are_both_normal);

    return UNITY_END();
}
