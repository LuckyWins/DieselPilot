/*
 * Persistent counters and ring buffer tests.
 *
 * These blobs have to survive a nightly power cut and a reflash, so the
 * cases that matter are the unhappy ones: a blob from older firmware, a
 * truncated read, a ring that has wrapped several times.
 */

#include <unity.h>
#include <string.h>
#include "../../src/stats.h"

void setUp(void) {}
void tearDown(void) {}

// ── Ring buffer ───────────────────────────────────────────────────────────

#define CAP  4
#define RSZ  2

static uint8_t ring[RING_HEADER_BYTES + CAP * RSZ];

static void pushValue(uint8_t a, uint8_t b) {
    uint8_t rec[RSZ] = { a, b };
    ringPush(ring, CAP, RSZ, rec);
}

static int valueAt(uint8_t index) {
    const uint8_t* r = ringAt(ring, CAP, RSZ, index);
    return r ? r[0] : -1;
}

void test_ring_size_covers_header_and_records(void) {
    TEST_ASSERT_EQUAL(RING_HEADER_BYTES + CAP * RSZ, (int)ringBytes(CAP, RSZ));
}

void test_a_fresh_ring_is_empty(void) {
    ringInit(ring, CAP, RSZ, 1);
    TEST_ASSERT_EQUAL(0, ringCount(ring));
    TEST_ASSERT_NULL(ringAt(ring, CAP, RSZ, 0));
}

void test_records_come_back_newest_first(void) {
    ringInit(ring, CAP, RSZ, 1);
    pushValue(10, 0);
    pushValue(20, 0);
    pushValue(30, 0);

    TEST_ASSERT_EQUAL(3,  ringCount(ring));
    TEST_ASSERT_EQUAL(30, valueAt(0));
    TEST_ASSERT_EQUAL(20, valueAt(1));
    TEST_ASSERT_EQUAL(10, valueAt(2));
    TEST_ASSERT_NULL(ringAt(ring, CAP, RSZ, 3));
}

void test_a_full_ring_drops_the_oldest(void) {
    ringInit(ring, CAP, RSZ, 1);
    for(uint8_t i = 1; i <= 6; i++) pushValue(i * 10, 0);

    TEST_ASSERT_EQUAL(CAP, ringCount(ring));
    TEST_ASSERT_EQUAL(60, valueAt(0));
    TEST_ASSERT_EQUAL(50, valueAt(1));
    TEST_ASSERT_EQUAL(40, valueAt(2));
    TEST_ASSERT_EQUAL(30, valueAt(3));
    TEST_ASSERT_NULL(ringAt(ring, CAP, RSZ, CAP));
}

void test_the_whole_record_is_stored_not_just_its_first_byte(void) {
    ringInit(ring, CAP, RSZ, 1);
    pushValue(7, 99);
    const uint8_t* r = ringAt(ring, CAP, RSZ, 0);
    TEST_ASSERT_EQUAL(7,  r[0]);
    TEST_ASSERT_EQUAL(99, r[1]);
}

void test_a_valid_ring_is_recognised(void) {
    ringInit(ring, CAP, RSZ, 1);
    pushValue(1, 2);
    TEST_ASSERT_TRUE(ringValid(ring, sizeof(ring), CAP, RSZ, 1));
}

// A blob written by older firmware must be thrown away, not read at the
// wrong offsets.
void test_a_ring_of_another_version_is_rejected(void) {
    ringInit(ring, CAP, RSZ, 1);
    TEST_ASSERT_FALSE(ringValid(ring, sizeof(ring), CAP, RSZ, 2));
}

void test_a_ring_of_the_wrong_size_is_rejected(void) {
    ringInit(ring, CAP, RSZ, 1);
    TEST_ASSERT_FALSE(ringValid(ring, sizeof(ring) - 1, CAP, RSZ, 1));
    TEST_ASSERT_FALSE(ringValid(ring, sizeof(ring), CAP + 1, RSZ, 1));
    TEST_ASSERT_FALSE(ringValid(0, sizeof(ring), CAP, RSZ, 1));
}

// Garbage in the header would otherwise send ringAt() outside the buffer.
void test_a_corrupt_header_is_rejected(void) {
    ringInit(ring, CAP, RSZ, 1);
    ring[2] = CAP + 1;                 // count beyond capacity
    TEST_ASSERT_FALSE(ringValid(ring, sizeof(ring), CAP, RSZ, 1));

    ringInit(ring, CAP, RSZ, 1);
    ring[3] = CAP;                     // next past the last slot
    TEST_ASSERT_FALSE(ringValid(ring, sizeof(ring), CAP, RSZ, 1));
}

// ── Counters ──────────────────────────────────────────────────────────────

static Counters sample(void) {
    Counters c;
    countersReset(c);
    c.burnSec      = 461000;
    c.fuelMl       = 86000;
    c.starts       = 412;
    c.startsFailed = 7;
    c.wasBurning   = true;
    return c;
}

void test_a_reset_counter_set_is_all_zero(void) {
    Counters c = sample();
    countersReset(c);
    TEST_ASSERT_EQUAL_UINT32(0, c.burnSec);
    TEST_ASSERT_EQUAL_UINT32(0, c.starts);
    TEST_ASSERT_EQUAL_UINT32(0, c.svcEpoch);
    TEST_ASSERT_FALSE(c.wasBurning);
}

void test_counters_survive_a_round_trip(void) {
    Counters in = sample();
    countersMarkService(in, 1772000000UL);
    in.burnSec += 3600;
    in.starts  += 5;

    uint8_t blob[COUNTERS_BLOB_BYTES];
    TEST_ASSERT_EQUAL(COUNTERS_BLOB_BYTES,
                      (int)countersPack(in, blob, sizeof(blob)));

    Counters out;
    TEST_ASSERT_TRUE(countersUnpack(out, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT32(in.burnSec,      out.burnSec);
    TEST_ASSERT_EQUAL_UINT32(in.fuelMl,       out.fuelMl);
    TEST_ASSERT_EQUAL_UINT32(in.starts,       out.starts);
    TEST_ASSERT_EQUAL_UINT32(in.startsFailed, out.startsFailed);
    TEST_ASSERT_EQUAL_UINT32(in.svcBurnSec,   out.svcBurnSec);
    TEST_ASSERT_EQUAL_UINT32(in.svcEpoch,     out.svcEpoch);
    TEST_ASSERT_TRUE(out.wasBurning);
}

// Values near the top of the range are the ones a hand-rolled packer gets
// wrong, and these counters only ever grow.
void test_packing_handles_the_full_range(void) {
    Counters in;
    countersReset(in);
    in.burnSec = 0xFFFFFFFFUL;
    in.fuelMl  = 0x80000000UL;
    in.starts  = 0x00FF00FFUL;

    uint8_t blob[COUNTERS_BLOB_BYTES];
    countersPack(in, blob, sizeof(blob));

    Counters out;
    TEST_ASSERT_TRUE(countersUnpack(out, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, out.burnSec);
    TEST_ASSERT_EQUAL_UINT32(0x80000000UL, out.fuelMl);
    TEST_ASSERT_EQUAL_UINT32(0x00FF00FFUL, out.starts);
}

void test_packing_refuses_a_buffer_that_is_too_small(void) {
    Counters c = sample();
    uint8_t  blob[COUNTERS_BLOB_BYTES - 1];
    TEST_ASSERT_EQUAL(0, (int)countersPack(c, blob, sizeof(blob)));
}

// An unreadable blob must leave the counters at zero rather than half filled,
// so a bad read looks like a fresh device instead of nonsense figures.
void test_an_unreadable_blob_leaves_counters_reset(void) {
    Counters c = sample();
    uint8_t  blob[COUNTERS_BLOB_BYTES];
    countersPack(sample(), blob, sizeof(blob));

    blob[0] = COUNTERS_VERSION + 1;     // written by other firmware
    TEST_ASSERT_FALSE(countersUnpack(c, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT32(0, c.burnSec);
    TEST_ASSERT_EQUAL_UINT32(0, c.starts);

    c = sample();
    TEST_ASSERT_FALSE(countersUnpack(c, blob, 10));     // truncated
    TEST_ASSERT_EQUAL_UINT32(0, c.burnSec);

    c = sample();
    TEST_ASSERT_FALSE(countersUnpack(c, 0, COUNTERS_BLOB_BYTES));   // absent
    TEST_ASSERT_EQUAL_UINT32(0, c.burnSec);
}

// ── Since the last service ────────────────────────────────────────────────

void test_before_any_service_the_totals_are_the_figures_since(void) {
    Counters c = sample();
    TEST_ASSERT_EQUAL_UINT32(461000, countersServiceBurnSec(c));
    TEST_ASSERT_EQUAL_UINT32(86000,  countersServiceFuelMl(c));
    TEST_ASSERT_EQUAL_UINT32(412,    countersServiceStarts(c));
}

void test_a_service_snapshot_starts_the_count_again(void) {
    Counters c = sample();
    countersMarkService(c, 1772000000UL);

    TEST_ASSERT_EQUAL_UINT32(0, countersServiceBurnSec(c));
    TEST_ASSERT_EQUAL_UINT32(0, countersServiceStarts(c));
    TEST_ASSERT_EQUAL_UINT32(1772000000UL, c.svcEpoch);

    c.burnSec += 3600 * 41;
    c.starts  += 98;
    TEST_ASSERT_EQUAL_UINT32(3600 * 41, countersServiceBurnSec(c));
    TEST_ASSERT_EQUAL_UINT32(98,        countersServiceStarts(c));

    // The lifetime figures keep counting through a service.
    TEST_ASSERT_EQUAL_UINT32(461000 + 3600 * 41, c.burnSec);
}

// A snapshot ahead of the totals means a blob written mid-update. Reporting
// the lifetime figure beats reporting a number that wrapped past zero.
void test_a_snapshot_ahead_of_the_totals_does_not_wrap(void) {
    Counters c;
    countersReset(c);
    c.burnSec    = 100;
    c.svcBurnSec = 500;
    TEST_ASSERT_EQUAL_UINT32(100, countersServiceBurnSec(c));
}

// ── Failure rate ──────────────────────────────────────────────────────────

void test_failure_rate_is_tenths_of_a_percent(void) {
    Counters c;
    countersReset(c);
    c.starts       = 412;
    c.startsFailed = 7;
    TEST_ASSERT_EQUAL_UINT16(16, countersFailureRateTenths(c));   // 1.6%

    c.starts       = 4;
    c.startsFailed = 1;
    TEST_ASSERT_EQUAL_UINT16(250, countersFailureRateTenths(c));  // 25.0%
}

void test_failure_rate_of_a_device_that_never_started(void) {
    Counters c;
    countersReset(c);
    TEST_ASSERT_EQUAL_UINT16(0, countersFailureRateTenths(c));
}

// ── Ignition log ──────────────────────────────────────────────────────────

static uint8_t ignRing[RING_HEADER_BYTES + IGN_LOG_CAPACITY * IGN_RECORD_BYTES];

static void logStart(uint16_t seconds, uint16_t beforeDv, uint16_t minDv,
                     uint8_t outcome) {
    IgnitionRecord r;
    r.epoch        = 1772000000UL;
    r.seconds      = seconds;
    r.voltBeforeDv = beforeDv;
    r.voltMinDv    = minDv;
    r.ambientC     = -4;
    r.attempts     = 1;
    r.outcome      = outcome;

    uint8_t packed[IGN_RECORD_BYTES];
    ignPack(r, packed, sizeof(packed));
    ringPush(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, packed);
}

static void logGoodStart(uint16_t seconds, uint16_t dropDv) {
    logStart(seconds, 129, (uint16_t)(129 - dropDv), IGN_OUTCOME_LIT);
}

void test_an_ignition_record_survives_a_round_trip(void) {
    IgnitionRecord in;
    in.epoch        = 1772000000UL;
    in.seconds      = 42;
    in.voltBeforeDv = 129;
    in.voltMinDv    = 118;
    in.ambientC     = -7;
    in.attempts     = 2;
    in.outcome      = IGN_OUTCOME_LIT;

    uint8_t packed[IGN_RECORD_BYTES];
    TEST_ASSERT_EQUAL(IGN_RECORD_BYTES, (int)ignPack(in, packed, sizeof(packed)));

    IgnitionRecord out;
    TEST_ASSERT_TRUE(ignUnpack(out, packed));
    TEST_ASSERT_EQUAL_UINT32(in.epoch,        out.epoch);
    TEST_ASSERT_EQUAL_UINT16(42,              out.seconds);
    TEST_ASSERT_EQUAL_UINT16(129,             out.voltBeforeDv);
    TEST_ASSERT_EQUAL_UINT16(118,             out.voltMinDv);
    TEST_ASSERT_EQUAL_INT8(-7,                out.ambientC);
    TEST_ASSERT_EQUAL_UINT8(2,                out.attempts);
    TEST_ASSERT_EQUAL_UINT8(IGN_OUTCOME_LIT,  out.outcome);
}

void test_a_record_fits_the_declared_size(void) {
    uint8_t small[IGN_RECORD_BYTES - 1];
    IgnitionRecord r;
    r.epoch = 0; r.seconds = 1; r.voltBeforeDv = 0; r.voltMinDv = 0;
    r.ambientC = 0; r.attempts = 1; r.outcome = IGN_OUTCOME_LIT;
    TEST_ASSERT_EQUAL(0, (int)ignPack(r, small, sizeof(small)));
}

void test_summary_of_an_empty_log(void) {
    ringInit(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, IGN_LOG_VERSION);
    IgnSummary s = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    TEST_ASSERT_EQUAL_UINT8(0, s.samples);
    TEST_ASSERT_EQUAL_UINT16(0, s.medianSeconds);
}

void test_summary_takes_the_median_not_the_worst(void) {
    ringInit(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, IGN_LOG_VERSION);
    logGoodStart(30, 8);
    logGoodStart(31, 8);
    logGoodStart(99, 8);          // one bad night in a frost
    logGoodStart(32, 8);
    logGoodStart(30, 8);

    IgnSummary s = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    TEST_ASSERT_EQUAL_UINT8(5, s.samples);
    TEST_ASSERT_EQUAL_UINT16(31, s.medianSeconds);   // not 44, the mean
}

// A start that never lit has no duration worth averaging.
void test_summary_skips_failed_starts(void) {
    ringInit(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, IGN_LOG_VERSION);
    logGoodStart(30, 8);
    logStart(300, 129, 100, IGN_OUTCOME_FAILED);
    logGoodStart(32, 8);

    IgnSummary s = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    TEST_ASSERT_EQUAL_UINT8(2, s.samples);
    TEST_ASSERT_EQUAL_UINT16(30, s.medianSeconds);
}

// Only the most recent ones, or a burner that improved after a clean would
// still be judged on how it lit before it.
void test_summary_looks_only_at_the_most_recent(void) {
    ringInit(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, IGN_LOG_VERSION);
    for(int i = 0; i < 6; i++) logGoodStart(90, 8);
    for(int i = 0; i < 5; i++) logGoodStart(30, 8);

    IgnSummary s = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    TEST_ASSERT_EQUAL_UINT8(5, s.samples);
    TEST_ASSERT_EQUAL_UINT16(30, s.medianSeconds);
}

void test_summary_measures_the_drop_not_the_voltage(void) {
    ringInit(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, IGN_LOG_VERSION);
    logGoodStart(30, 11);
    logGoodStart(30, 12);
    logGoodStart(30, 13);

    IgnSummary s = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    TEST_ASSERT_EQUAL_UINT16(12, s.medianDropDv);
}

// ── Degradation verdict ───────────────────────────────────────────────────

static IgnSummary summaryOf(uint8_t samples, uint16_t seconds, uint16_t dropDv) {
    IgnSummary s = { samples, seconds, dropDv };
    return s;
}

void test_a_healthy_burner_is_not_flagged(void) {
    TEST_ASSERT_FALSE(ignDegraded(summaryOf(5, 33, 9), 31, 8));
}

void test_a_start_half_again_as_slow_is_flagged(void) {
    TEST_ASSERT_FALSE(ignDegraded(summaryOf(5, 46, 8), 31, 8));   // 1.48x
    TEST_ASSERT_TRUE (ignDegraded(summaryOf(5, 47, 8), 31, 8));   // 1.51x
}

// The plug pulls eight to ten amps, so a deeper sag is the plug itself going.
void test_a_deeper_voltage_sag_is_flagged(void) {
    TEST_ASSERT_FALSE(ignDegraded(summaryOf(5, 31, 13), 31, 8));
    TEST_ASSERT_TRUE (ignDegraded(summaryOf(5, 31, 14), 31, 8));
}

// Two starts are not a trend, and a burner just serviced has no baseline yet.
void test_too_little_evidence_says_nothing(void) {
    TEST_ASSERT_FALSE(ignDegraded(summaryOf(4, 200, 40), 31, 8));
    TEST_ASSERT_FALSE(ignDegraded(summaryOf(5, 200, 40), 0, 0));
    TEST_ASSERT_FALSE(ignDegraded(summaryOf(0, 0, 0), 31, 8));
}

// A service means a different burner as far as the trend is concerned.
void test_a_service_clears_the_baseline_and_the_warning(void) {
    Counters c;
    countersReset(c);
    c.baseIgnSeconds = 31;
    c.baseIgnDropDv  = 8;
    c.baseIgnSamples = IGN_BASELINE_SAMPLES;
    c.ignWarned      = true;

    countersMarkService(c, 1772000000UL);

    TEST_ASSERT_EQUAL_UINT16(0, c.baseIgnSeconds);
    TEST_ASSERT_EQUAL_UINT8(0,  c.baseIgnSamples);
    TEST_ASSERT_FALSE(c.ignWarned);
}

void test_the_baseline_survives_a_round_trip(void) {
    Counters in;
    countersReset(in);
    in.baseIgnSeconds = 31;
    in.baseIgnDropDv  = 8;
    in.baseIgnSamples = IGN_BASELINE_SAMPLES;
    in.ignWarned      = true;

    uint8_t blob[COUNTERS_BLOB_BYTES];
    countersPack(in, blob, sizeof(blob));

    Counters out;
    TEST_ASSERT_TRUE(countersUnpack(out, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT16(31, out.baseIgnSeconds);
    TEST_ASSERT_EQUAL_UINT16(8,  out.baseIgnDropDv);
    TEST_ASSERT_EQUAL_UINT8(IGN_BASELINE_SAMPLES, out.baseIgnSamples);
    TEST_ASSERT_TRUE(out.ignWarned);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_ring_size_covers_header_and_records);
    RUN_TEST(test_a_fresh_ring_is_empty);
    RUN_TEST(test_records_come_back_newest_first);
    RUN_TEST(test_a_full_ring_drops_the_oldest);
    RUN_TEST(test_the_whole_record_is_stored_not_just_its_first_byte);
    RUN_TEST(test_a_valid_ring_is_recognised);
    RUN_TEST(test_a_ring_of_another_version_is_rejected);
    RUN_TEST(test_a_ring_of_the_wrong_size_is_rejected);
    RUN_TEST(test_a_corrupt_header_is_rejected);

    RUN_TEST(test_a_reset_counter_set_is_all_zero);
    RUN_TEST(test_counters_survive_a_round_trip);
    RUN_TEST(test_packing_handles_the_full_range);
    RUN_TEST(test_packing_refuses_a_buffer_that_is_too_small);
    RUN_TEST(test_an_unreadable_blob_leaves_counters_reset);

    RUN_TEST(test_before_any_service_the_totals_are_the_figures_since);
    RUN_TEST(test_a_service_snapshot_starts_the_count_again);
    RUN_TEST(test_a_snapshot_ahead_of_the_totals_does_not_wrap);

    RUN_TEST(test_an_ignition_record_survives_a_round_trip);
    RUN_TEST(test_a_record_fits_the_declared_size);
    RUN_TEST(test_summary_of_an_empty_log);
    RUN_TEST(test_summary_takes_the_median_not_the_worst);
    RUN_TEST(test_summary_skips_failed_starts);
    RUN_TEST(test_summary_looks_only_at_the_most_recent);
    RUN_TEST(test_summary_measures_the_drop_not_the_voltage);

    RUN_TEST(test_a_healthy_burner_is_not_flagged);
    RUN_TEST(test_a_start_half_again_as_slow_is_flagged);
    RUN_TEST(test_a_deeper_voltage_sag_is_flagged);
    RUN_TEST(test_too_little_evidence_says_nothing);
    RUN_TEST(test_a_service_clears_the_baseline_and_the_warning);
    RUN_TEST(test_the_baseline_survives_a_round_trip);

    RUN_TEST(test_failure_rate_is_tenths_of_a_percent);
    RUN_TEST(test_failure_rate_of_a_device_that_never_started);

    return UNITY_END();
}
