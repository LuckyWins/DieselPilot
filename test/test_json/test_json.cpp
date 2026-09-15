/*
 * JSON escaping tests.
 *
 * The failure this guards against is quiet: a quote in a setting produced
 * malformed JSON, the settings page stopped loading, and nothing reported
 * why.
 */

#include <unity.h>
#include "../../src/json.h"

void setUp(void) {}
void tearDown(void) {}

void test_plain_text_is_untouched(void) {
    TEST_ASSERT_EQUAL_STRING("DieselPilot", jsonEscape("DieselPilot").c_str());
    TEST_ASSERT_EQUAL_STRING("", jsonEscape("").c_str());
}

// The case that actually broke the page: a quote in an SSID.
void test_quote_is_escaped(void) {
    TEST_ASSERT_EQUAL_STRING("my\\\"net", jsonEscape("my\"net").c_str());
}

void test_backslash_is_escaped(void) {
    TEST_ASSERT_EQUAL_STRING("C:\\\\path", jsonEscape("C:\\path").c_str());
}

void test_control_characters_use_short_forms(void) {
    TEST_ASSERT_EQUAL_STRING("a\\nb", jsonEscape("a\nb").c_str());
    TEST_ASSERT_EQUAL_STRING("a\\tb", jsonEscape("a\tb").c_str());
    TEST_ASSERT_EQUAL_STRING("a\\rb", jsonEscape("a\rb").c_str());
}

// Everything else below 0x20 has no short form and needs the \u sequence.
void test_other_control_characters_use_unicode_escape(void) {
    std::string in(1, (char)0x01);
    TEST_ASSERT_EQUAL_STRING("\\u0001", jsonEscape(in).c_str());
}

// Cyrillic settings are plausible here, and splitting UTF-8 would corrupt it.
void test_utf8_passes_through(void) {
    TEST_ASSERT_EQUAL_STRING("гараж", jsonEscape("гараж").c_str());
}

void test_a_hostile_value_cannot_inject_structure(void) {
    std::string out = jsonEscape("x\",\"admin\":true");
    TEST_ASSERT_EQUAL_STRING("x\\\",\\\"admin\\\":true", out.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_text_is_untouched);
    RUN_TEST(test_quote_is_escaped);
    RUN_TEST(test_backslash_is_escaped);
    RUN_TEST(test_control_characters_use_short_forms);
    RUN_TEST(test_other_control_characters_use_unicode_escape);
    RUN_TEST(test_utf8_passes_through);
    RUN_TEST(test_a_hostile_value_cannot_inject_structure);
    return UNITY_END();
}
