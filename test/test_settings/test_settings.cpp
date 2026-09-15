/*
 * Тесты разбора форм настроек.
 *
 * Главное здесь — регрессия, из-за которой сохранение формы с пустыми
 * полями стирало SSID и пароль Wi-Fi. После перезагрузки устройство
 * уходило в AP-режим, и в удалённом гараже это означало поездку.
 */

#include <unity.h>
#include "../../src/settings.h"

void setUp(void) {}
void tearDown(void) {}

// ── resolveField ───────────────────────────────────────────────────────────

void test_field_absent_keeps_current(void) {
    TEST_ASSERT_EQUAL_STRING("MyWiFi",
        resolveField(false, "", "MyWiFi").c_str());
}

void test_field_empty_keeps_current(void) {
    TEST_ASSERT_EQUAL_STRING("MyWiFi",
        resolveField(true, "", "MyWiFi").c_str());
}

void test_field_value_replaces(void) {
    TEST_ASSERT_EQUAL_STRING("OtherWiFi",
        resolveField(true, "OtherWiFi", "MyWiFi").c_str());
}

void test_field_clear_token_empties(void) {
    TEST_ASSERT_EQUAL_STRING("",
        resolveField(true, SETTINGS_CLEAR_TOKEN, "MyWiFi").c_str());
}

void test_field_clear_token_on_empty_stays_empty(void) {
    TEST_ASSERT_EQUAL_STRING("",
        resolveField(true, SETTINGS_CLEAR_TOKEN, "").c_str());
}

// Значение, похожее на маркер, но не совпадающее с ним, должно записаться
// как обычный текст — иначе пароль вида "__CLEAR__x" повёл бы себя странно.
void test_field_clear_token_must_match_exactly(void) {
    TEST_ASSERT_EQUAL_STRING("__CLEAR__x",
        resolveField(true, "__CLEAR__x", "old").c_str());
}

// Та самая регрессия: в форме заполнен только hostname, поля Wi-Fi пусты.
// До правки этот сценарий стирал и SSID, и пароль.
void test_regression_saving_hostname_keeps_wifi_credentials(void) {
    std::string ssid = resolveField(true, "",        "HomeNet");
    std::string pass = resolveField(true, "",        "s3cret");
    std::string host = resolveField(true, "Garage",  "DieselPilot");

    TEST_ASSERT_EQUAL_STRING("HomeNet", ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("s3cret",  pass.c_str());
    TEST_ASSERT_EQUAL_STRING("Garage",  host.c_str());
}

// Выключить MQTT можно только очисткой адреса брокера: mqttEnabled
// выводится из его длины. Проверяем, что путь к выключению существует.
void test_regression_mqtt_can_still_be_disabled(void) {
    std::string broker = resolveField(true, SETTINGS_CLEAR_TOKEN, "mqtt.example.com");
    TEST_ASSERT_TRUE(broker.empty());
}

// ── resolveFlag ────────────────────────────────────────────────────────────

void test_flag_absent_keeps_current(void) {
    TEST_ASSERT_TRUE(resolveFlag(false, "", true));
    TEST_ASSERT_FALSE(resolveFlag(false, "", false));
}

void test_flag_empty_keeps_current(void) {
    TEST_ASSERT_TRUE(resolveFlag(true, "", true));
}

void test_flag_parses_one_and_zero(void) {
    TEST_ASSERT_TRUE(resolveFlag(true, "1", false));
    TEST_ASSERT_FALSE(resolveFlag(true, "0", true));
}

// ── resolveNumber ──────────────────────────────────────────────────────────

void test_number_absent_keeps_current(void) {
    TEST_ASSERT_EQUAL_INT(1883, resolveNumber(false, "", 1883, 1, 65535));
}

void test_number_valid_replaces(void) {
    TEST_ASSERT_EQUAL_INT(8883, resolveNumber(true, "8883", 1883, 1, 65535));
}

void test_number_out_of_range_keeps_current(void) {
    TEST_ASSERT_EQUAL_INT(1883, resolveNumber(true, "70000", 1883, 1, 65535));
    TEST_ASSERT_EQUAL_INT(1883, resolveNumber(true, "0",     1883, 1, 65535));
}

// Мусор не должен превращаться в ноль, как это делает atoi.
void test_number_garbage_keeps_current(void) {
    TEST_ASSERT_EQUAL_INT(1883, resolveNumber(true, "abc",  1883, 1, 65535));
    TEST_ASSERT_EQUAL_INT(1883, resolveNumber(true, "88x3", 1883, 1, 65535));
}

// А вот честный ноль в разрешённом диапазоне принимается —
// им, например, выключается относительный таймер.
void test_number_zero_is_valid_when_in_range(void) {
    TEST_ASSERT_EQUAL_INT(0, resolveNumber(true, "0", 180, 0, 1440));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_field_absent_keeps_current);
    RUN_TEST(test_field_empty_keeps_current);
    RUN_TEST(test_field_value_replaces);
    RUN_TEST(test_field_clear_token_empties);
    RUN_TEST(test_field_clear_token_on_empty_stays_empty);
    RUN_TEST(test_field_clear_token_must_match_exactly);
    RUN_TEST(test_regression_saving_hostname_keeps_wifi_credentials);
    RUN_TEST(test_regression_mqtt_can_still_be_disabled);

    RUN_TEST(test_flag_absent_keeps_current);
    RUN_TEST(test_flag_empty_keeps_current);
    RUN_TEST(test_flag_parses_one_and_zero);

    RUN_TEST(test_number_absent_keeps_current);
    RUN_TEST(test_number_valid_replaces);
    RUN_TEST(test_number_out_of_range_keeps_current);
    RUN_TEST(test_number_garbage_keeps_current);
    RUN_TEST(test_number_zero_is_valid_when_in_range);

    return UNITY_END();
}
