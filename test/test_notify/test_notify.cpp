/*
 * Notification logic tests.
 *
 * The whitelist cases matter most: anyone can find a Telegram bot by name
 * and message it, so this check is the only thing standing between a
 * stranger and the heater.
 */

#include <unity.h>
#include "../../src/notify.h"

void setUp(void) {}
void tearDown(void) {}

// -- Whitelist -------------------------------------------------------------

void test_allowed_chat_passes(void) {
    TEST_ASSERT_TRUE(isChatAllowed("123456789", 123456789LL));
}

void test_unknown_chat_is_rejected(void) {
    TEST_ASSERT_FALSE(isChatAllowed("123456789", 987654321LL));
}

// An unconfigured device must be inert, not open to the world.
void test_empty_whitelist_allows_nobody(void) {
    TEST_ASSERT_FALSE(isChatAllowed("", 123456789LL));
    TEST_ASSERT_FALSE(isChatAllowed("   ", 123456789LL));
}

void test_several_separators_are_accepted(void) {
    TEST_ASSERT_TRUE(isChatAllowed("111,222;333 444", 333LL));
    TEST_ASSERT_TRUE(isChatAllowed("111, 222 , 333", 222LL));
}

void test_negative_ids_work(void) {
    // Telegram group chats carry negative ids.
    TEST_ASSERT_TRUE(isChatAllowed("-1001234567890", -1001234567890LL));
}

// One malformed entry must not discard the rest, or a stray character in the
// settings would lock the owner out of their own heater.
void test_garbage_entry_does_not_drop_the_valid_ones(void) {
    TEST_ASSERT_TRUE(isChatAllowed("111,oops,222", 222LL));
    TEST_ASSERT_TRUE(isChatAllowed("111,oops,222", 111LL));
    TEST_ASSERT_FALSE(isChatAllowed("111,oops,222", 0LL));
}

void test_partial_number_is_not_accepted(void) {
    TEST_ASSERT_FALSE(isChatAllowed("123abc", 123LL));
}

void test_zero_is_never_allowed(void) {
    TEST_ASSERT_FALSE(isChatAllowed("0", 0LL));
}

void test_list_roundtrip(void) {
    std::vector<int64_t> ids = parseChatList(" 42 , -7 ; 1000 ");
    TEST_ASSERT_EQUAL_INT(3, (int)ids.size());
    TEST_ASSERT_EQUAL_STRING("42,-7,1000", formatChatList(ids).c_str());
}

// -- Backoff ---------------------------------------------------------------

void test_backoff_starts_at_the_minimum(void) {
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_BACKOFF_MIN_MS, nextBackoffMs(0));
}

void test_backoff_doubles(void) {
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_BACKOFF_MIN_MS * 2,
                             nextBackoffMs(NOTIFY_BACKOFF_MIN_MS));
}

void test_backoff_is_capped(void) {
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_BACKOFF_MAX_MS,
                             nextBackoffMs(NOTIFY_BACKOFF_MAX_MS));
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_BACKOFF_MAX_MS,
                             nextBackoffMs(NOTIFY_BACKOFF_MAX_MS - 1));
}

// -- Repeat suppression ----------------------------------------------------

void test_change_is_reported_once(void) {
    int stored = -1;
    TEST_ASSERT_TRUE(changedSince(stored, 6));    // OVERHEAT appears
    TEST_ASSERT_FALSE(changedSince(stored, 6));   // still there, stay quiet
    TEST_ASSERT_FALSE(changedSince(stored, 6));
    TEST_ASSERT_TRUE(changedSince(stored, 0));    // cleared
}

// -- Voltage alarm ---------------------------------------------------------

void test_voltage_alarm_trips_below_the_threshold(void) {
    TEST_ASSERT_TRUE(voltageAlarm(false, 114, 115, 120));
    TEST_ASSERT_FALSE(voltageAlarm(false, 115, 115, 120));
}

// Hysteresis: once alarming, it takes a clearly higher reading to recover.
void test_voltage_alarm_does_not_flap(void) {
    TEST_ASSERT_TRUE(voltageAlarm(true, 116, 115, 120));
    TEST_ASSERT_TRUE(voltageAlarm(true, 119, 115, 120));
    TEST_ASSERT_FALSE(voltageAlarm(true, 120, 115, 120));
}

// A missing reading is not evidence of a flat battery.
void test_voltage_alarm_ignores_a_missing_reading(void) {
    TEST_ASSERT_FALSE(voltageAlarm(false, 0, 115, 120));
    TEST_ASSERT_TRUE(voltageAlarm(true, 0, 115, 120));
}


// -- Debounce --──────────────────────────────────────────────────────────────

// The glow plug pulls the rail down for a few seconds at every ignition. That
// dip must not be reported; a sag that persists must be.
void test_debounce_ignores_a_brief_dip(void) {
    bool state = false; uint32_t since = 0;
    TEST_ASSERT_FALSE(debounceVerdict(state, since, true, 5000, 30000));
    TEST_ASSERT_FALSE(debounceVerdict(state, since, true, 20000, 30000));
    TEST_ASSERT_FALSE(debounceVerdict(state, since, false, 25000, 30000));
    TEST_ASSERT_FALSE(state);
}

void test_debounce_accepts_a_sustained_change(void) {
    bool state = false; uint32_t since = 0;
    TEST_ASSERT_FALSE(debounceVerdict(state, since, true, 29000, 30000));
    TEST_ASSERT_TRUE (debounceVerdict(state, since, true, 30000, 30000));
    TEST_ASSERT_TRUE(state);
}

// Recovery is debounced the same way, so a momentary rebound does not clear
// a real alarm.
void test_debounce_is_symmetric(void) {
    bool state = true; uint32_t since = 0;
    TEST_ASSERT_TRUE (debounceVerdict(state, since, false, 10000, 30000));
    TEST_ASSERT_FALSE(debounceVerdict(state, since, false, 40000, 30000));
}

// Agreement restarts the countdown, so alternating readings never accumulate
// into a change.
void test_debounce_flapping_never_settles(void) {
    bool state = false; uint32_t since = 0;
    for(uint32_t t = 0; t < 300000; t += 10000) {
        debounceVerdict(state, since, true,  t, 30000);
        debounceVerdict(state, since, false, t + 5000, 30000);
    }
    TEST_ASSERT_FALSE(state);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_allowed_chat_passes);
    RUN_TEST(test_unknown_chat_is_rejected);
    RUN_TEST(test_empty_whitelist_allows_nobody);
    RUN_TEST(test_several_separators_are_accepted);
    RUN_TEST(test_negative_ids_work);
    RUN_TEST(test_garbage_entry_does_not_drop_the_valid_ones);
    RUN_TEST(test_partial_number_is_not_accepted);
    RUN_TEST(test_zero_is_never_allowed);
    RUN_TEST(test_list_roundtrip);

    RUN_TEST(test_backoff_starts_at_the_minimum);
    RUN_TEST(test_backoff_doubles);
    RUN_TEST(test_backoff_is_capped);

    RUN_TEST(test_change_is_reported_once);

    RUN_TEST(test_voltage_alarm_trips_below_the_threshold);
    RUN_TEST(test_voltage_alarm_does_not_flap);
    RUN_TEST(test_voltage_alarm_ignores_a_missing_reading);

    RUN_TEST(test_debounce_ignores_a_brief_dip);
    RUN_TEST(test_debounce_accepts_a_sustained_change);
    RUN_TEST(test_debounce_is_symmetric);
    RUN_TEST(test_debounce_flapping_never_settles);

    return UNITY_END();
}
