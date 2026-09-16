#include "stats.h"

// ═══════════════════════════════════════════════════════════════════════════
// LITTLE-ENDIAN PACKING
// ═══════════════════════════════════════════════════════════════════════════

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ═══════════════════════════════════════════════════════════════════════════
// RING BUFFER
// ═══════════════════════════════════════════════════════════════════════════
//
// Header layout: version, capacity, count, next. Keeping the shape in the
// blob is what lets ringValid() reject one written by different firmware
// instead of reading a record at the wrong offset.

#define RING_VERSION_AT  0
#define RING_CAPACITY_AT 1
#define RING_COUNT_AT    2
#define RING_NEXT_AT     3

size_t ringBytes(uint8_t capacity, uint8_t recordSize) {
    return RING_HEADER_BYTES + (size_t)capacity * recordSize;
}

void ringInit(uint8_t* buf, uint8_t capacity, uint8_t recordSize,
              uint8_t version) {
    size_t total = ringBytes(capacity, recordSize);
    for(size_t i = 0; i < total; i++) buf[i] = 0;
    buf[RING_VERSION_AT]  = version;
    buf[RING_CAPACITY_AT] = capacity;
    buf[RING_COUNT_AT]    = 0;
    buf[RING_NEXT_AT]     = 0;
}

bool ringValid(const uint8_t* buf, size_t len, uint8_t capacity,
               uint8_t recordSize, uint8_t version) {
    if(buf == 0 || len != ringBytes(capacity, recordSize)) return false;
    if(buf[RING_VERSION_AT]  != version)  return false;
    if(buf[RING_CAPACITY_AT] != capacity) return false;
    if(buf[RING_COUNT_AT]    >  capacity) return false;
    if(buf[RING_NEXT_AT]     >= capacity) return false;
    return true;
}

void ringPush(uint8_t* buf, uint8_t capacity, uint8_t recordSize,
              const void* record) {
    uint8_t        next = buf[RING_NEXT_AT];
    uint8_t*       slot = buf + RING_HEADER_BYTES + (size_t)next * recordSize;
    const uint8_t* src  = (const uint8_t*)record;

    for(uint8_t i = 0; i < recordSize; i++) slot[i] = src[i];

    buf[RING_NEXT_AT] = (uint8_t)((next + 1) % capacity);
    if(buf[RING_COUNT_AT] < capacity) buf[RING_COUNT_AT]++;
}

uint8_t ringCount(const uint8_t* buf) {
    return buf[RING_COUNT_AT];
}

const uint8_t* ringAt(const uint8_t* buf, uint8_t capacity, uint8_t recordSize,
                      uint8_t index) {
    uint8_t count = buf[RING_COUNT_AT];
    if(index >= count) return 0;

    // `next` points at the slot the following record will take, so the newest
    // one sits immediately behind it.
    int slot = (int)buf[RING_NEXT_AT] - 1 - (int)index;
    while(slot < 0) slot += capacity;

    return buf + RING_HEADER_BYTES + (size_t)slot * recordSize;
}

// ═══════════════════════════════════════════════════════════════════════════
// LIFETIME COUNTERS
// ═══════════════════════════════════════════════════════════════════════════

void countersReset(Counters& c) {
    c.burnSec      = 0;
    c.fuelMl       = 0;
    c.starts       = 0;
    c.startsFailed = 0;
    c.svcBurnSec   = 0;
    c.svcFuelMl    = 0;
    c.svcStarts    = 0;
    c.svcEpoch     = 0;

    c.baseIgnSeconds = 0;
    c.baseIgnDropDv  = 0;
    c.baseIgnSamples = 0;

    c.wasBurning   = false;
    c.ignWarned    = false;
}

size_t countersPack(const Counters& c, uint8_t* out, size_t cap) {
    if(cap < COUNTERS_BLOB_BYTES) return 0;

    out[0] = COUNTERS_VERSION;
    put32(out + 1,  c.burnSec);
    put32(out + 5,  c.fuelMl);
    put32(out + 9,  c.starts);
    put32(out + 13, c.startsFailed);
    put32(out + 17, c.svcBurnSec);
    put32(out + 21, c.svcFuelMl);
    put32(out + 25, c.svcStarts);
    put32(out + 29, c.svcEpoch);
    out[33] = c.wasBurning ? 1 : 0;

    put16(out + 34, c.baseIgnSeconds);
    put16(out + 36, c.baseIgnDropDv);
    out[38] = c.baseIgnSamples;
    out[39] = c.ignWarned ? 1 : 0;

    return COUNTERS_BLOB_BYTES;
}

bool countersUnpack(Counters& c, const uint8_t* in, size_t len) {
    countersReset(c);

    if(in == 0 || len != COUNTERS_BLOB_BYTES) return false;
    if(in[0] != COUNTERS_VERSION)             return false;

    c.burnSec      = get32(in + 1);
    c.fuelMl       = get32(in + 5);
    c.starts       = get32(in + 9);
    c.startsFailed = get32(in + 13);
    c.svcBurnSec   = get32(in + 17);
    c.svcFuelMl    = get32(in + 21);
    c.svcStarts    = get32(in + 25);
    c.svcEpoch     = get32(in + 29);
    c.wasBurning   = in[33] != 0;

    c.baseIgnSeconds = get16(in + 34);
    c.baseIgnDropDv  = get16(in + 36);
    c.baseIgnSamples = in[38];
    c.ignWarned      = in[39] != 0;

    return true;
}

void countersMarkService(Counters& c, uint32_t epoch) {
    c.svcBurnSec = c.burnSec;
    c.svcFuelMl  = c.fuelMl;
    c.svcStarts  = c.starts;
    c.svcEpoch   = epoch;

    // A cleaned burner starts a new baseline: comparing against how it lit
    // when it was dirty would report every service as a degradation.
    c.baseIgnSeconds = 0;
    c.baseIgnDropDv  = 0;
    c.baseIgnSamples = 0;
    c.ignWarned      = false;
}

// The snapshot can only ever be behind the running totals, but a blob written
// by a half-finished write is not worth a negative figure.
static uint32_t since(uint32_t total, uint32_t snapshot) {
    return (total >= snapshot) ? total - snapshot : total;
}

uint32_t countersServiceBurnSec(const Counters& c) {
    return since(c.burnSec, c.svcBurnSec);
}

uint32_t countersServiceFuelMl(const Counters& c) {
    return since(c.fuelMl, c.svcFuelMl);
}

uint32_t countersServiceStarts(const Counters& c) {
    return since(c.starts, c.svcStarts);
}

uint16_t countersFailureRateTenths(const Counters& c) {
    if(c.starts == 0) return 0;
    return (uint16_t)((uint64_t)c.startsFailed * 1000ULL / c.starts);
}

// ═══════════════════════════════════════════════════════════════════════════
// IGNITION LOG
// ═══════════════════════════════════════════════════════════════════════════

size_t ignPack(const IgnitionRecord& r, uint8_t* out, size_t cap) {
    if(cap < IGN_RECORD_BYTES) return 0;

    put32(out + 0, r.epoch);
    put16(out + 4, r.seconds);
    put16(out + 6, r.voltBeforeDv);
    put16(out + 8, r.voltMinDv);
    out[10] = (uint8_t)r.ambientC;
    out[11] = r.attempts;
    out[12] = r.outcome;
    out[13] = 0;                 // spare, so a new field is not a version bump

    return IGN_RECORD_BYTES;
}

bool ignUnpack(IgnitionRecord& r, const uint8_t* in) {
    if(in == 0) return false;

    r.epoch        = get32(in + 0);
    r.seconds      = get16(in + 4);
    r.voltBeforeDv = get16(in + 6);
    r.voltMinDv    = get16(in + 8);
    r.ambientC     = (int8_t)in[10];
    r.attempts     = in[11];
    r.outcome      = in[12];

    return true;
}

// Insertion sort: the arrays here are five elements long, and a median of
// five is the whole reason any of this is sorted.
static void sortSmall(uint16_t* v, uint8_t n) {
    for(uint8_t i = 1; i < n; i++) {
        uint16_t key = v[i];
        int      j   = (int)i - 1;
        while(j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
        v[j + 1] = key;
    }
}

static uint16_t medianOf(uint16_t* v, uint8_t n) {
    if(n == 0) return 0;
    sortSmall(v, n);
    // An even count takes the lower of the middle pair. With samples this few
    // an average would only invent precision that is not there.
    return v[(n - 1) / 2];
}

IgnSummary ignSummarise(const uint8_t* ring, uint8_t want) {
    IgnSummary out = { 0, 0, 0 };

    if(ring == 0 || want == 0) return out;
    if(want > IGN_SUMMARY_MAX) want = IGN_SUMMARY_MAX;

    uint16_t seconds[IGN_SUMMARY_MAX];
    uint16_t drops[IGN_SUMMARY_MAX];
    uint8_t  n     = 0;
    uint8_t  count = ringCount(ring);

    for(uint8_t i = 0; i < count && n < want; i++) {
        IgnitionRecord r;
        if(!ignUnpack(r, ringAt(ring, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, i))) {
            continue;
        }
        // A start that never lit has no duration worth averaging, and its
        // voltage trace belongs to a failure rather than to a healthy start.
        if(r.outcome != IGN_OUTCOME_LIT) continue;

        seconds[n] = r.seconds;
        drops[n]   = (r.voltBeforeDv > r.voltMinDv)
                   ? (uint16_t)(r.voltBeforeDv - r.voltMinDv) : 0;
        n++;
    }

    out.samples       = n;
    out.medianSeconds = medianOf(seconds, n);
    out.medianDropDv  = medianOf(drops, n);
    return out;
}

bool ignDegraded(const IgnSummary& recent, uint16_t baseSeconds,
                 uint16_t baseDropDv) {
    // Nothing to compare against, or too few starts to call it a trend.
    if(recent.samples < IGN_RECENT_SAMPLES) return false;
    if(baseSeconds == 0)                    return false;

    uint32_t slowerThan = (uint32_t)baseSeconds * IGN_SLOWER_NUMERATOR /
                          IGN_SLOWER_DENOMINATOR;
    if(recent.medianSeconds > slowerThan) return true;

    return recent.medianDropDv > (uint32_t)baseDropDv + IGN_EXTRA_DROP_DV;
}

// ═══════════════════════════════════════════════════════════════════════════
// ERROR LOG
// ═══════════════════════════════════════════════════════════════════════════

size_t errPack(const ErrorRecord& r, uint8_t* out, size_t cap) {
    if(cap < ERR_RECORD_BYTES) return 0;

    put32(out + 0, r.epoch);
    out[4] = r.code;
    out[5] = 0;                  // spare

    return ERR_RECORD_BYTES;
}

bool errUnpack(ErrorRecord& r, const uint8_t* in) {
    if(in == 0) return false;
    r.epoch = get32(in + 0);
    r.code  = in[4];
    return true;
}
