/*
 * Тесты чистой логики протокола: расчёт частоты, CRC, декодеры.
 */

#include <unity.h>
#include <string.h>
#include "../../src/protocol.h"

void setUp(void) {}
void tearDown(void) {}

// ── Частота ────────────────────────────────────────────────────────────────

// Шаг перестройки: 26 МГц / 2^16. Точнее этого попасть невозможно.
static const uint32_t FREQ_STEP_HZ = 397;

// Набор регистров V2 из cc1101_init(): FREQ2=0x10, FREQ1=0xB0, FREQ0=0x9C.
void test_v2_registers_match_declared_frequency(void) {
    TEST_ASSERT_EQUAL_HEX32(0x10B09C, freqToRegisters(433937000UL));
}

void test_v2_registers_decode_back(void) {
    uint32_t hz = registersToFreq(0x10B09C);
    TEST_ASSERT_UINT32_WITHIN(FREQ_STEP_HZ, 433937000UL, hz);
}

// Набор регистров V1 из cc1101_init_V1(): FREQ2=0x10, FREQ1=0xB0, FREQ0=0x71.
//
// В коде и в выпадающем списке веб-морды подписано «433.892 MHz», но регистры
// дают ~433.920. Тест фиксирует реальное поведение железа, чтобы подпись
// исправлялась под него, а не наоборот.
void test_v1_registers_are_not_the_documented_frequency(void) {
    uint32_t hz = registersToFreq(0x10B071);

    TEST_ASSERT_UINT32_WITHIN(1000UL, 433920000UL, hz);
    TEST_ASSERT_TRUE(hz > 433900000UL);   // подпись «433.892» не соответствует
}

void test_frequency_roundtrip_is_stable(void) {
    const uint32_t probes[] = {433050000UL, 433920000UL, 433937000UL, 434790000UL};

    for(unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint32_t back = registersToFreq(freqToRegisters(probes[i]));
        TEST_ASSERT_UINT32_WITHIN(FREQ_STEP_HZ, probes[i], back);
    }
}

// Произведение freqHz * 65536 вылезает за 32 бита — проверяем,
// что промежуточные вычисления не переполняются.
void test_frequency_does_not_overflow_32bit(void) {
    TEST_ASSERT_EQUAL_HEX32(0x10B09C, freqToRegisters(433937000UL));
    TEST_ASSERT_TRUE(freqToRegisters(928000000UL) > freqToRegisters(433937000UL));
}

// ── CRC-16/MODBUS ──────────────────────────────────────────────────────────

// Эталонный вектор стандарта: "123456789" -> 0x4B37.
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

// ── Декодеры ───────────────────────────────────────────────────────────────

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

// Коды 0x00 и 0x01 оба означают штатную работу — на это опирается
// подавление уведомлений, поэтому фиксируем тестом.
void test_error_none_and_on_are_both_normal(void) {
    TEST_ASSERT_EQUAL_STRING("NORMAL", getErrorName(ERR_NONE));
    TEST_ASSERT_EQUAL_STRING("NORMAL", getErrorName(ERR_ON));
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

    RUN_TEST(test_state_names);
    RUN_TEST(test_error_names);
    RUN_TEST(test_error_none_and_on_are_both_normal);

    return UNITY_END();
}
