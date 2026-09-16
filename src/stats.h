/*
 * Counters and history that have to survive the night — pure logic, no
 * hardware and no NVS access of its own.
 *
 * Mains power at this site is cut every night, so anything kept only in RAM
 * is gone by morning: the fuel burnt, the errors seen at 21:50, whether the
 * heater was still lit when the power went. The firmware already measures all
 * of it and then throws each number away after using it once.
 *
 * This module owns the shapes those numbers are stored in and the arithmetic
 * over them. The caller does the actual `prefs.getBytes` / `putBytes`, which
 * is what keeps everything here buildable on the host and covered by
 * `pio test -e native`.
 *
 * Blobs are packed explicitly, little-endian, rather than written as raw
 * structs: struct padding is a compiler's business, and a blob that survives
 * reflashes should not depend on it. Every blob starts with a version byte,
 * so a future field is a version bump rather than a garbled read.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// ═══════════════════════════════════════════════════════════════════════════
// RING BUFFER
// ═══════════════════════════════════════════════════════════════════════════
//
// A fixed-capacity ring of equal-sized records inside one byte buffer, so it
// goes to NVS as a single blob and comes back as one. Used twice: for the
// ignition log and for the error history, which currently lives in RAM and
// is lost with everything else at 22:00.

#define RING_HEADER_BYTES 4

// Total blob size for a ring of `capacity` records of `recordSize` bytes.
size_t ringBytes(uint8_t capacity, uint8_t recordSize);

void ringInit(uint8_t* buf, uint8_t capacity, uint8_t recordSize,
              uint8_t version);

// True if the buffer really holds a ring of this shape and version. A blob
// written by older firmware fails this and is replaced rather than parsed.
bool ringValid(const uint8_t* buf, size_t len, uint8_t capacity,
               uint8_t recordSize, uint8_t version);

// Appends a record, overwriting the oldest once the ring is full.
void ringPush(uint8_t* buf, uint8_t capacity, uint8_t recordSize,
              const void* record);

uint8_t ringCount(const uint8_t* buf);

// Newest first: index 0 is the most recent record, 1 the one before it.
// Returns null past the end of what has been written.
const uint8_t* ringAt(const uint8_t* buf, uint8_t capacity, uint8_t recordSize,
                      uint8_t index);

// ═══════════════════════════════════════════════════════════════════════════
// LIFETIME COUNTERS
// ═══════════════════════════════════════════════════════════════════════════
//
// What wears a diesel heater out is not hours alone: the glow plug is worn by
// the number of starts, and decoking is due on accumulated running. Neither
// was counted anywhere. "Since the last service" is a subtraction from a
// snapshot rather than a second set of counters, so the two cannot drift.

#define COUNTERS_VERSION     2
#define COUNTERS_BLOB_BYTES  40

struct Counters {
    uint32_t burnSec;        // time not OFF, all time
    uint32_t fuelMl;         // burnt, all time
    uint32_t starts;         // ignitions commanded
    uint32_t startsFailed;   // of which never lit

    uint32_t svcBurnSec;     // the same four, snapshotted at the last service
    uint32_t svcFuelMl;
    uint32_t svcStarts;
    uint32_t svcEpoch;       // when that was, 0 if never

    // What a healthy start looked like when this burner was last serviced.
    // Frozen once, from the first few ignitions after a service, because the
    // ring buffer below is far too short to still hold them later on.
    uint16_t baseIgnSeconds;
    uint16_t baseIgnDropDv;
    uint8_t  baseIgnSamples;   // 0..IGN_BASELINE_SAMPLES, full means frozen

    // Set while the heater is lit and cleared once it reaches OFF cleanly, so
    // a blob still carrying it means the power went mid-burn.
    bool     wasBurning;

    // So a degrading burner is reported once rather than every morning after
    // the overnight power cut.
    bool     ignWarned;
};

void countersReset(Counters& c);

// Packs into `out`, returning the bytes written, or 0 if `cap` is too small.
size_t countersPack(const Counters& c, uint8_t* out, size_t cap);

// False when the blob is absent, truncated or of another version, leaving `c`
// reset rather than half-filled.
bool countersUnpack(Counters& c, const uint8_t* in, size_t len);

void countersMarkService(Counters& c, uint32_t epoch);

// Since the last service. Without a service ever recorded these equal the
// lifetime figures, which is the truth: nothing has been serviced yet.
uint32_t countersServiceBurnSec(const Counters& c);
uint32_t countersServiceFuelMl(const Counters& c);
uint32_t countersServiceStarts(const Counters& c);

// Failed starts as a percentage, scaled by ten so it prints one decimal
// without dragging in floating point. Zero starts give zero rather than a
// division by zero.
uint16_t countersFailureRateTenths(const Counters& c);

// ═══════════════════════════════════════════════════════════════════════════
// IGNITION LOG
// ═══════════════════════════════════════════════════════════════════════════
//
// A glow plug is worn out by ignitions, and it announces that long before it
// finally refuses to light: starts take longer, and the plug -- eight to ten
// amps of it -- drags the supply further down while it tries. The firmware
// already measures both and throws both away. Kept as a short ring so the
// trend can be compared against what this burner looked like when it was
// last cleaned.

#define IGN_LOG_VERSION      1
#define IGN_LOG_CAPACITY     16
#define IGN_RECORD_BYTES     14

// How many starts make a baseline, and how many recent ones it is judged
// against. Small, because a burner is serviced long before sixteen starts
// would accumulate any statistical weight -- and one slow start in a frost is
// noise, which is what taking a median is for.
#define IGN_BASELINE_SAMPLES 5
#define IGN_RECENT_SAMPLES   5
#define IGN_SUMMARY_MAX      8

// A start is called degraded once it takes half again as long as the baseline,
// or drags the supply half a volt further down.
#define IGN_SLOWER_NUMERATOR   3
#define IGN_SLOWER_DENOMINATOR 2
#define IGN_EXTRA_DROP_DV      5

#define IGN_OUTCOME_FAILED 0
#define IGN_OUTCOME_LIT    1

struct IgnitionRecord {
    uint32_t epoch;          // 0 when the clock had never synced
    uint16_t seconds;        // command to RUNNING
    uint16_t voltBeforeDv;   // supply just before the command went out
    uint16_t voltMinDv;      // lowest seen while it was lighting
    int8_t   ambientC;       // a cold start is slower, and that is not wear
    uint8_t  attempts;
    uint8_t  outcome;        // IGN_OUTCOME_*
};

size_t ignPack(const IgnitionRecord& r, uint8_t* out, size_t cap);
bool   ignUnpack(IgnitionRecord& r, const uint8_t* in);

struct IgnSummary {
    uint8_t  samples;        // 0 when there was nothing to summarise
    uint16_t medianSeconds;
    uint16_t medianDropDv;
};

// Medians over the most recent `want` successful starts in the ring, skipping
// failures: a start that never lit has no meaningful duration.
IgnSummary ignSummarise(const uint8_t* ring, uint8_t want);

// True when recent starts have drifted far enough from the baseline to be
// worth a word. False whenever either side is too thin to compare.
bool ignDegraded(const IgnSummary& recent, uint16_t baseSeconds,
                 uint16_t baseDropDv);

// ═══════════════════════════════════════════════════════════════════════════
// ERROR LOG
// ═══════════════════════════════════════════════════════════════════════════
//
// The same ring, for the errors the heater reports. These used to live in RAM
// and so were gone every morning: a fault at 21:50 left no trace at all,
// which is exactly the one worth seeing. Timestamps are absolute rather than
// millis for the same reason -- millis restarts with the power.

#define ERR_LOG_VERSION  1
#define ERR_LOG_CAPACITY 10
#define ERR_RECORD_BYTES 6

struct ErrorRecord {
    uint32_t epoch;    // 0 when the clock had never synced
    uint8_t  code;
};

size_t errPack(const ErrorRecord& r, uint8_t* out, size_t cap);
bool   errUnpack(ErrorRecord& r, const uint8_t* in);
