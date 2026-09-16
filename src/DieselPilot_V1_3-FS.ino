/*
 * ═══════════════════════════════════════════════════════════════════════════
 *                        DIESEL PILOT — FREE (open source)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Full-featured ESP32 controller for Chinese diesel heaters
 *
 * Features:
 * - WiFi AP mode (default) + STA mode
 * - Web GUI (dark theme)
 * - OLED SH1106 display (IP + status)
 * - Auto/Manual pairing
 * - Real-time heater control
 * - MQTT integration (Home Assistant ready)
 * - ERROR CODE DECODING (BYTE[7])
 * - OTA firmware updates (ArduinoOTA / espota — password protected)
 *
 * Hardware:
 * - ESP32
 * - CC1101 @ 433.937 MHz
 * - SH1106 OLED (I2C)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *                  Version: V1.3 - FREE
 * ═══════════════════════════════════════════════════════════════════════════
 * This is the open-source edition (no cloud). Remote access is provided via
 * local Web GUI, MQTT and on-network OTA updates.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>          // OTA firmware updates (espota protocol)
#include <esp_task_wdt.h>        // Hardware watchdog
#include <esp_ota_ops.h>         // Rollback of an update that does not come back

#include "protocol.h"           // States, error codes, CRC, frequency maths
#include "settings.h"           // Settings form parsing
#include "scheduler.h"          // Shutdown timers
#include "notify.h"             // Whitelist, repeat suppression, backoff
#include "stepper.h"            // Walking the power level up and down
#include "stats.h"              // Counters and history that outlive a reboot
#include "json.h"               // Response string escaping

#include <WiFiClientSecure.h>
#include <AsyncTelegram2.h>

// ═══════════════════════════════════════════════════════════════════════════
// HARDWARE CONFIG
// ═══════════════════════════════════════════════════════════════════════════

// CC1101 Pins
#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23
#define PIN_SS    5
#define PIN_GDO2  4

// I2C Pins (OLED)
#define PIN_SDA   21
#define PIN_SCL   22

// OLED Configuration
#define USE_OLED  true

// Watchdog. Generous timeout on purpose: connectivity at the garage is poor,
// a TLS handshake may legitimately take several seconds and connectMQTT()
// blocks for up to 4 s. The watchdog must catch hangs, not a slow network.
#define WDT_TIMEOUT_SEC 60

// How long to wait for CC1101 readiness after pulling CS low. Normally the
// module answers within microseconds (crystal startup is ~150 us), so 10 ms
// is more than enough while still preventing an endless hang.
#define CC1101_READY_TIMEOUT_US 10000

// Retry period for re-initialising the CC1101 after a fault
#define CC1101_RETRY_MS 30000

// Total time the V2 search listens for, and the length of one listening
// slice. The search used to hold loop() for the whole minute; slicing it
// keeps the web server, Telegram and the watchdog alive meanwhile.
#define PAIR_WINDOW_MS 60000
#define PAIR_SLICE_MS  400

// Heater poll rates. 3 s is what the firmware always used; the idle rate
// applies when the heater is off, which in a preheat-on-arrival installation
// is most of the time. A failed poll costs up to 3 s of loop time -- sending
// the wakeup plus a 2 s receive window -- so polling an unreachable heater
// every 3 s saturates the cycle and makes everything else sluggish.
#define HEATER_POLL_FAST_MS  3000
#define HEATER_POLL_IDLE_MS  10000
// After any command, poll fast for a while so the result appears without
// waiting out the idle interval.
#define HEATER_POLL_BOOST_MS 60000
// Beyond this the last reading is history, not the current state.
#define HEATER_DATA_FRESH_MS 20000

// Stepping the power level. One step is given two poll intervals to appear in
// the reading: the poll runs at HEATER_POLL_FAST_MS after a command, and a
// single dropped frame should not be read as a ladder that will not move.
#define STEP_SETTLE_MS   (HEATER_POLL_FAST_MS * 2)
// Enough for a walk down a six-rung ladder and back up it, with room to spare.
#define STEP_MAX_STEPS   16
#define STEP_DEADLINE_MS 180000
// Power levels on the panel, and the range a target temperature is accepted
// in. The heater's own limits are discovered by stepping into them; these
// only reject a typo before any command goes out.
#define LEVEL_MAX  6
#define TEMP_MIN_C 5
#define TEMP_MAX_C 35

// Telegram polls fast while someone is pressing buttons and slowly otherwise.
// A fixed rate has to choose between a sluggish keyboard and wasted data.
#define TG_POLL_ACTIVE_MS  3000
#define TG_POLL_BOOST_MS  60000

// Accepted range for a hand-entered frequency, in hertz. The CC1101 covers
// several bands; anything outside this is a typo, most often kilohertz.
#define FREQ_MIN_HZ 300000000UL
#define FREQ_MAX_HZ 928000000UL

// Wi-Fi reconnect backoff bounds. The garage sits on the edge of town and
// the link can be down for hours, so retries back off instead of hammering.
// How often to try the configured network while the fallback access point is
// up. The boot attempt gets ten seconds, and after the nightly power cut the
// controller is awake long before an LTE modem has finished registering, so
// it loses that race nearly every time.
// How long a freshly flashed image has to prove it can still be reached
// before the bootloader is allowed to put the old one back.
//
// Two thresholds, because the two channels are not equally trustworthy. The
// network is ours and its absence is the image's fault. Telegram is somebody
// else's service reached over a metered link: it was unreachable for ten
// seconds on the bench and took a perfectly good image down with it, which is
// a worse failure than the one this guards against.
#define OTA_VERIFY_BOT_WAIT_MS 180000UL
#define OTA_VERIFY_WINDOW_MS   300000UL

#define WIFI_AP_RETRY_MS 60000

#define WIFI_RETRY_MIN_MS 5000
#define WIFI_RETRY_MAX_MS 300000

// How often to check whether NTP has delivered a plausible date yet
#define NTP_CHECK_MS 5000

// Supply voltage thresholds, tenths of a volt, and how long a reading must
// hold before it is reported.
//
// This heater runs off a mains PSU, not a battery, so nothing drains the rail
// slowly. What does pull it down is the glow plug drawing eight to ten amps
// at ignition -- brief, normal, and not worth a message. A sag that persists
// means the supply is undersized, which is a common cause of hard starting,
// and the heater's own cutout only trips around 11.5 V (ERR_UNDERVOLTAGE),
// so the warning has to come earlier than that.
#define VOLT_ALARM_DECIV_DEFAULT 120
#define VOLT_CLEAR_DECIV_DEFAULT 125
#define VOLT_DEBOUNCE_SEC_DEFAULT 30

// Commands
#define CMD_WAKEUP 0x23
#define CMD_MODE   0x24
#define CMD_POWER  0x2B
#define CMD_UP     0x3C
#define CMD_DOWN   0x3E

// ═══════════════════════════════════════════════════════════════════════════
// GLOBAL OBJECTS
// ═══════════════════════════════════════════════════════════════════════════

WebServer server(80);
Preferences prefs;
WiFiClient espClient;
PubSubClient mqtt(espClient);

// VERSION CONTROL
String heaterVersion = "V2";
uint32_t customFrequency = 0;
bool autoMenuV1 = true;
unsigned long lastMenuChangeV1 = 0;
const unsigned long menuIntervalV1 = 3500;
const unsigned long commandLockTimeV1 = 5000;
unsigned long lastCommandTimeV1 = 0;
byte myAddrV1[3] = {0x19, 0x52, 0x4B};
// The SSD1315 is software compatible with the SSD1306. The SH1106 has a
// 132 px buffer with an offset of 2, so its driver would shift the image.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// False when nothing answered on the bus. Everything that draws checks it:
// the panel is a convenience, and the controller has to run without one.
bool displayPresent = false;

// ═══════════════════════════════════════════════════════════════════════════
// GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

String version = "1.3";

// WiFi
String apSSID = "Diesel-Pilot";
String apPassword = "12345678";
String staSSID = "";
String staPassword = "";
bool useAP = true;

// MQTT
String deviceName = "DieselPilot";
String mqttServer = "";
int mqttPort = 1883;
String mqttTopic = "diesel";
String mqttUser = "";
String mqttPassword = "";
bool mqttAuthEnabled = false;
bool mqttEnabled = false;

// CC1101: module does not answer over SPI (broken wiring, no power)
bool cc1101Fault = false;

// Wall clock. Needed for the blackout deadline and for timestamps; the ESP32
// boots believing it is 1970, so nothing may trust it before the first sync.
bool   timeValid   = false;
String ntpServer   = "pool.ntp.org";
int    tzOffsetMin = 180;              // UTC+3, Belarus, no DST

// Shutdown scheduling
uint16_t autoOffMin          = 180;    // runtime limit, 0 = disabled
bool     blackoutEnabled     = true;
int      blackoutMinutes     = 22 * 60;
uint16_t shutdownLeadMin     = 15;
uint16_t cooldownExpectedMin = 5;

// Scheduled start. Stored as an absolute epoch so it survives the overnight
// power cut -- a time of day could not say whether it meant today or tomorrow
// after a reboot.
bool     startArmed  = false;
uint32_t startTarget = 0;

// One-shot runtime limit for the current burn, overriding autoOffMin. The
// configured value is one number for every run, but a forty-minute preheat
// and an afternoon in the garage are different -- and the web GUI that holds
// it is only reachable on the local network, so from the road there was no
// way to change it at all. Zero means "use the configured default".
uint16_t sessionAutoOffMin = 0;

// Fuel used in the current burn. Accumulated once a second from the pump rate
// and reset when the heater reaches OFF, so it answers "how much did this
// preheat cost" rather than tracking a lifetime total.
//
// For V1 the figure is rough: that decoder derives pumpFreq from the power
// level rather than reading a real rate.
uint32_t fuelTicks  = 0;
uint16_t fuelDoseUl = FUEL_DOSE_UL_DEFAULT;

// Tank, by dead reckoning from the pump. The switch is separate from the
// capacity so turning the feature off does not throw the capacity away.
bool     tankEnabled     = false;
uint32_t tankCapacityMl  = 0;
uint32_t tankRemainingMl = 0;
static uint8_t tankWarnLast = 0;

// What each start looked like. The retry watcher gives up as soon as the
// heater acknowledges the command; what says something about the glow plug is
// what happens after that, so this is measured separately.
static uint8_t ignRing[RING_HEADER_BYTES + IGN_LOG_CAPACITY * IGN_RECORD_BYTES];
static bool     ignLogActive     = false;
static uint32_t ignLogStartMs    = 0;
static uint16_t ignLogVoltBefore = 0;
static uint16_t ignLogVoltMin    = 0;
static int8_t   ignLogAmbient    = 0;

// Lifetime counters, loaded from NVS at boot. Declared here because a start
// is counted in heaterEnsureOn(), long before the persistence code below.
Counters counters;
static bool     countersDirty   = false;
static uint32_t countersFlushMs = 0;

uint16_t voltAlarmDeciV = VOLT_ALARM_DECIV_DEFAULT;
uint16_t voltClearDeciV = VOLT_CLEAR_DECIV_DEFAULT;
uint16_t voltDebounceS  = VOLT_DEBOUNCE_SEC_DEFAULT;

// Progress reports while the heater burns. "Heater: RUNNING" says the state
// changed; it does not say whether anything is getting warmer. A heater can
// sit in RUNNING and produce no heat at all -- coked burner, fuel starvation,
// a blocked duct -- and the ambient trend is the only thing that shows it.
//
// Two numbers rather than two checkboxes, in the idiom already used by
// autoOffMin: zero on the first disables reports entirely, zero on the
// second sends one and no more. The delay is itself worth tuning, because
// the "not heating" warning hangs off it: too early and a cold heat
// exchanger raises false alarms, too late and the driver has already
// arrived.
// Hours of running between services, 0 = no reminder. Deliberately off by
// default: how long these heaters go between decokings depends on the model
// and on the fuel, and a figure invented here would be a notification built
// on a guess. Set it once there is enough of this device's own history.
uint16_t serviceHours = 0;

uint16_t reportFirstMin = 30;
uint16_t reportRptMin   = 0;

// Telegram. The bot is the only remote channel, so the chat whitelist is the
// single thing standing between a stranger and the heater.
bool   tgEnabled = false;
String tgToken   = "";
String tgChats   = "";     // comma-separated chat ids
String tgCaCert  = "";     // overrides the CA bundled with the library

// AsyncTelegram2 and WiFiClientSecure both keep the pointers they are handed
// rather than copying, so the token lives in a buffer that never moves.
char tgTokenBuf[64] = {0};

WiFiClientSecure tgClient;
AsyncTelegram2   tgBot(tgClient);
bool   tgReady   = false;
String tgPending = "";     // produced before the bot became reachable

// Idle polling interval for incoming commands. The bot drops to
// TG_POLL_ACTIVE_MS for a minute after each message, so this rate only
// governs the quiet hours -- which on a metered plan is where the data goes.
uint16_t tgPollSec = 60;

// Temporary window during which the bot answers /id to anyone, so the owner
// can discover their own chat id without a third-party bot. Started from the
// admin page, expires on its own.
uint32_t tgDiscoverUntilMs = 0;

// Interval currently handed to the library, so it is only reconfigured when
// the rate actually changes.
uint32_t tgCurrentPollMs = 0;

// Heater
uint32_t heaterAddress = 0x00000000;
uint8_t packetSeq = 0;
bool heaterPaired = false;

// Last command sent to the heater, and last message handled by the bot.
// Both open a window of faster polling.
uint32_t lastHeaterCommandMs = 0;
uint32_t lastTgActivityMs    = 0;

// Loop health. With physical access this rare, these are the difference
// between "it feels slow" and a number readable from a hundred kilometres
// away. The minimum free heap is the one that matters: a slow leak or heap
// fragmentation shows up there long before anything visibly breaks.
uint32_t loopMaxMs = 0;

// Ignition verification. Set when a start is commanded, cleared once the
// heater acknowledges by leaving OFF -- or once it has clearly failed to.
bool     ignWatching      = false;
uint8_t  ignAttempts      = 0;
uint32_t ignCommandedAtMs = 0;

// V2 pairing runs as a state machine in loop() rather than blocking the
// request handler for a minute.
enum PairState { PAIR_IDLE, PAIR_SEARCHING, PAIR_OK, PAIR_FAILED };
PairState pairState      = PAIR_IDLE;
uint32_t  pairDeadlineMs = 0;

// Status
struct {
    uint8_t state = 0;
    uint8_t power = 0;
    uint16_t voltage = 0;
    int8_t ambientTemp = 0;
    uint8_t caseTemp = 0;
    int8_t setpoint = 0;
    uint8_t pumpFreq = 0;
    bool autoMode = false;
    int16_t rssi = 0;
    uint8_t errorCode = 0;
    unsigned long lastUpdate = 0;
} heaterStatus;

// Error history. In NVS rather than RAM: the power goes every night, and a
// fault at 21:50 used to leave no trace by morning.
static uint8_t errRing[RING_HEADER_BYTES + ERR_LOG_CAPACITY * ERR_RECORD_BYTES];
static uint8_t errLastCode = ERR_NONE;

// Display
String displayLine1 = "Diesel Pilot";
String displayLine2 = "Initializing...";
String displayLine3 = "";
String displayLine4 = "";

// Timing
unsigned long lastMQTTRetry = 0;
const unsigned long mqttRetryInterval = 30000;

// ═══════════════════════════════════════════════════════════════════════════
// OTA VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

bool   otaEnabled  = false;             // Master ON/OFF switch (saved in NVS)
String otaPassword = "";                // Password required to flash (saved in NVS)
bool   otaRunning  = false;             // ArduinoOTA.begin() has been called

// ═══════════════════════════════════════════════════════════════════════════
// ERROR HISTORY
// ═══════════════════════════════════════════════════════════════════════════

void addErrorToHistory(uint8_t errorCode) {
    if(errorCode == ERR_NONE) return;
    // The heater is polled every few seconds, so without this one fault would
    // fill the whole ring in under a minute. It is also what keeps the NVS
    // write below down to one per distinct fault.
    if(errorCode == errLastCode) return;
    errLastCode = errorCode;

    ErrorRecord r;
    r.epoch = timeValid ? (uint32_t)time(nullptr) : 0;
    r.code  = errorCode;

    uint8_t packed[ERR_RECORD_BYTES];
    if(!errPack(r, packed, sizeof(packed))) return;
    ringPush(errRing, ERR_LOG_CAPACITY, ERR_RECORD_BYTES, packed);
    prefs.putBytes("errlog", errRing, sizeof(errRing));
}

// ═══════════════════════════════════════════════════════════════════════════
// WATCHDOG
// ═══════════════════════════════════════════════════════════════════════════

// Tell the watchdog the loop is alive. Called not only from loop() but also
// inside long yet legitimate waits — such as the heater search during
// pairing, which runs for up to 60 seconds.
inline void feedWatchdog() {
    esp_task_wdt_reset();
}

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 LOW-LEVEL FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

// The module signals readiness by pulling MISO low after CS is asserted.
//
// This wait must be bounded: without a limit, broken wiring or an unpowered
// module hangs the controller for good — and it happens inside setup(), in
// cc1101_init(), which the watchdog cannot cover because it is armed last.
//
// Returns false and releases CS if the module did not answer. Protocol
// semantics for a healthy module are unchanged: the loop still exits on
// exactly the same condition as before.
// The crystal has to start before the chip will answer anything, which takes
// far longer than an ordinary bus turnaround.
#define CC1101_RESET_TIMEOUT_US 50000

// Plain wait on SO, with none of waitReady()'s side effects: during a reset a
// chip that is not yet ready is the normal case, not a fault to record.
static bool cc1101_waitMisoLow(uint32_t timeoutUs) {
    uint32_t t0 = micros();
    while(digitalRead(PIN_MISO)) {
        if(micros() - t0 > timeoutUs) return false;
    }
    return true;
}

// One byte clocked out by hand, MSB first, SPI mode 0: data set while the
// clock is low, sampled by the chip on the rising edge.
static void cc1101_bitbangByte(uint8_t v) {
    for(int i = 7; i >= 0; i--) {
        digitalWrite(PIN_MOSI, (v >> i) & 1);
        delayMicroseconds(1);
        digitalWrite(PIN_SCK, HIGH);
        delayMicroseconds(1);
        digitalWrite(PIN_SCK, LOW);
        delayMicroseconds(1);
    }
}

// Manual reset, CC1101 datasheet section 19.1.
//
// The chip has an automatic power-on reset, but it only works when the supply
// rises quickly and cleanly. On a breadboard it often does not, and the chip
// comes up in a state where SO never falls -- so it never reports ready, and
// every ordinary strobe in this driver refuses to send anything, including
// the SRES that would have fixed it. This sequence breaks that circle: it
// runs on plain GPIO and does not wait for readiness before touching CSn.
static bool cc1101_manualReset() {
    SPI.end();

    pinMode(PIN_SCK,  OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_MISO, INPUT);
    pinMode(PIN_SS,   OUTPUT);

    // Step 1: SCLK high, SI low, so a chip that woke up in pin control mode
    // cannot mistake these lines for GDO outputs.
    digitalWrite(PIN_SS,   HIGH);
    digitalWrite(PIN_SCK,  HIGH);
    digitalWrite(PIN_MOSI, LOW);
    delayMicroseconds(10);

    // Steps 2 and 3: strobe CSn, then hold it high for at least 40 us.
    digitalWrite(PIN_SS, LOW);
    delayMicroseconds(10);
    digitalWrite(PIN_SS, HIGH);
    delayMicroseconds(50);

    // Step 4: select the chip and wait for it to say it is awake.
    digitalWrite(PIN_SS, LOW);
    digitalWrite(PIN_SCK, LOW);            // mode 0 idles the clock low
    bool ok = cc1101_waitMisoLow(CC1101_RESET_TIMEOUT_US);

    if(ok) {
        cc1101_bitbangByte(0x30);          // step 5: SRES
        // Step 6: the reset is complete when SO falls a second time.
        ok = cc1101_waitMisoLow(CC1101_RESET_TIMEOUT_US);
    }

    digitalWrite(PIN_SS, HIGH);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
    return ok;
}

static bool cc1101_waitReady() {
    uint32_t t0 = micros();
    while(digitalRead(PIN_MISO)) {
        if(micros() - t0 > CC1101_READY_TIMEOUT_US) {
            digitalWrite(PIN_SS, HIGH);
            cc1101Fault = true;
            return false;
        }
    }
    cc1101Fault = false;
    return true;
}

void cc1101_writeReg(uint8_t addr, uint8_t val) {
    digitalWrite(PIN_SS, LOW);
    if(!cc1101_waitReady()) return;
    SPI.transfer(addr);
    SPI.transfer(val);
    digitalWrite(PIN_SS, HIGH);
}

void cc1101_writeBurst(uint8_t addr, uint8_t len, uint8_t* bytes) {
    digitalWrite(PIN_SS, LOW);
    if(!cc1101_waitReady()) return;
    SPI.transfer(addr);
    for(int i = 0; i < len; i++) SPI.transfer(bytes[i]);
    digitalWrite(PIN_SS, HIGH);
}

void cc1101_strobe(uint8_t addr) {
    digitalWrite(PIN_SS, LOW);
    if(!cc1101_waitReady()) return;
    SPI.transfer(addr);
    digitalWrite(PIN_SS, HIGH);
}

uint8_t cc1101_readReg(uint8_t addr) {
    digitalWrite(PIN_SS, LOW);
    if(!cc1101_waitReady()) return 0;
    SPI.transfer(addr);
    uint8_t val = SPI.transfer(0xFF);
    digitalWrite(PIN_SS, HIGH);
    return val;
}

// Works out why the module is silent, which VERSION alone cannot say.
//
// cc1101_readReg() returns 0 both when the data coming back is genuinely
// zero and when the readiness wait timed out before a single bit moved, so a
// failed self-test on its own is ambiguous. This tells the two apart without
// a meter, by asking who wins the line: an idle CC1101 releases SO while CSn
// is high and drives it while CSn is low, so an internal pull that loses the
// argument means something out there is driving, and one that wins means
// nothing is.
static void cc1101_diagnose() {
    digitalWrite(PIN_SS, HIGH);
    delayMicroseconds(50);

    pinMode(PIN_MISO, INPUT_PULLDOWN);
    delayMicroseconds(200);
    bool idleWithPulldown = digitalRead(PIN_MISO);
    pinMode(PIN_MISO, INPUT_PULLUP);
    delayMicroseconds(200);
    bool idleWithPullup = digitalRead(PIN_MISO);

    // Selecting the chip is what makes a live CC1101 take the line over.
    digitalWrite(PIN_SS, LOW);
    delayMicroseconds(50);

    pinMode(PIN_MISO, INPUT_PULLDOWN);
    delayMicroseconds(200);
    bool selWithPulldown = digitalRead(PIN_MISO);
    pinMode(PIN_MISO, INPUT_PULLUP);
    delayMicroseconds(200);
    bool selWithPullup = digitalRead(PIN_MISO);

    digitalWrite(PIN_SS, HIGH);
    pinMode(PIN_MISO, INPUT);

    Serial.printf("   MISO idle: pulldown=%d pullup=%d | selected: pulldown=%d pullup=%d\n",
                  idleWithPulldown, idleWithPullup, selWithPulldown, selWithPullup);

    // Read VERSION again, this time ignoring the readiness signal. The chip's
    // SPI shift register is clocked by our SCK, not by its crystal, so a chip
    // whose oscillator never started should still answer here. That separates
    // a dead crystal from a data path that was never wired correctly.
    digitalWrite(PIN_SS, LOW);
    delayMicroseconds(20);
    SPI.transfer(0xF1);
    uint8_t forced = SPI.transfer(0xFF);
    digitalWrite(PIN_SS, HIGH);
    Serial.printf("   VERSION ignoring the ready signal: 0x%02X"
                  "  (0x14 is what a live CC1101 answers)\n", forced);

    bool floatsWhileSelected = (selWithPulldown == 0) && (selWithPullup == 1);

    if(floatsWhileSelected) {
        Serial.println("   -> nothing is driving MISO. The module is unpowered, "
                       "not connected, or dead.");
        Serial.println("      Check VCC and GND at the module, and the MISO wire "
                       "itself.");
    } else if(selWithPulldown == 1) {
        Serial.println("   -> the module is driving MISO high and never releases "
                       "it: it is powered and wired, but not ready.");
        Serial.println("      The reset above having failed too, that leaves "
                       "the crystal or the supply at the module itself.");
    } else {
        Serial.println("   -> the module drives MISO low, so it is alive and "
                       "ready. The fault is in the rest of the bus.");
        Serial.println("      Check SCK, MOSI and CSn.");
    }
}

// Presence check via the VERSION register.
//
// The readiness wait alone is not enough: it only catches the case where
// MISO is stuck high. With broken wiring the floating input may read as
// zeros instead, in which case every operation formally succeeds while the
// data is garbage. Reading a known register rules out both cases.
static bool cc1101_selfTest() {
    uint8_t version = cc1101_readReg(0xF1);   // VERSION, status register address
    if(version == 0x00 || version == 0xFF) {
        cc1101Fault = true;
        Serial.printf("❌ CC1101 self-test failed: VERSION=0x%02X\n", version);
        cc1101_diagnose();
        return false;
    }
    Serial.printf("✅ CC1101 present: VERSION=0x%02X\n", version);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 INIT
// ═══════════════════════════════════════════════════════════════════════════

void cc1101_init() {
    if(!cc1101_manualReset()) {
        Serial.println("⚠️ CC1101 did not answer the reset sequence");
    }
    delay(100);
    cc1101_writeReg(0x00, 0x07); cc1101_writeReg(0x02, 0x06);
    cc1101_writeReg(0x03, 0x47); cc1101_writeReg(0x07, 0x04);
    cc1101_writeReg(0x08, 0x05); cc1101_writeReg(0x0A, 0x00);
    cc1101_writeReg(0x0B, 0x06); cc1101_writeReg(0x0C, 0x00);
    cc1101_writeReg(0x0D, 0x10); cc1101_writeReg(0x0E, 0xB0);
    cc1101_writeReg(0x0F, 0x9C); cc1101_writeReg(0x10, 0xF8);
    cc1101_writeReg(0x11, 0x93); cc1101_writeReg(0x12, 0x13);
    cc1101_writeReg(0x13, 0x22); cc1101_writeReg(0x14, 0xF8);
    cc1101_writeReg(0x15, 0x26); cc1101_writeReg(0x17, 0x30);
    cc1101_writeReg(0x18, 0x18); cc1101_writeReg(0x19, 0x16);
    cc1101_writeReg(0x1A, 0x6C); cc1101_writeReg(0x1B, 0x03);
    cc1101_writeReg(0x1C, 0x40); cc1101_writeReg(0x1D, 0x91);
    cc1101_writeReg(0x20, 0xFB); cc1101_writeReg(0x21, 0x56);
    cc1101_writeReg(0x22, 0x17); cc1101_writeReg(0x23, 0xE9);
    cc1101_writeReg(0x24, 0x2A); cc1101_writeReg(0x25, 0x00);
    cc1101_writeReg(0x26, 0x1F); cc1101_writeReg(0x2C, 0x81);
    cc1101_writeReg(0x2D, 0x35); cc1101_writeReg(0x2E, 0x09);
    cc1101_writeReg(0x09, 0x00); cc1101_writeReg(0x04, 0x7E);
    cc1101_writeReg(0x05, 0x3C);
    uint8_t paTable[8] = {0x00, 0x12, 0x0E, 0x34, 0x60, 0xC5, 0xC1, 0xC0};
    cc1101_writeBurst(0x7E, 8, paTable);
    cc1101_strobe(0x31); cc1101_strobe(0x36); cc1101_strobe(0x3B);
    cc1101_strobe(0x36); cc1101_strobe(0x3A); delay(136);
    Serial.println("   CC1101 registers written @ 433.937 MHz (V2)");
}

void cc1101_init_V1() {
    if(!cc1101_manualReset()) {
        Serial.println("⚠️ CC1101 did not answer the reset sequence");
    }
    delay(100);
    const byte configRegsV1[] = {
        0x0B, 0x06, 0x0D, 0x10, 0x0E, 0xB0, 0x0F, 0x71,
        0x10, 0x86, 0x11, 0x83, 0x12, 0x12, 0x13, 0x22,
        0x04, 0x09, 0x05, 0x1A, 0x06, 0x0A, 0x08, 0x00,
        0x15, 0x40, 0x18, 0x18, 0x23, 0xFF
    };
    for(int i = 0; i < sizeof(configRegsV1); i += 2)
        cc1101_writeReg(configRegsV1[i], configRegsV1[i+1]);
    cc1101_strobe(0x36); delay(5); cc1101_strobe(0x34);
    Serial.println("   CC1101 registers written @ 433.920 MHz (V1)");
}

void cc1101_applyConfig() {
    if(customFrequency > 0) { cc1101_init(); cc1101_setFrequency(customFrequency); }
    else if(heaterVersion == "V1") cc1101_init_V1();
    else cc1101_init();
    cc1101_selfTest();
}

void cc1101_setFrequency(uint32_t freqHz) {
    uint32_t freq = freqToRegisters(freqHz);
    cc1101_writeReg(0x0D, (freq >> 16) & 0xFF);
    cc1101_writeReg(0x0E, (freq >> 8) & 0xFF);
    cc1101_writeReg(0x0F, freq & 0xFF);
    // Report the frequency after rounding to the tuning step (~397 Hz)
    // rather than the requested one — that is what the module tuned to.
    Serial.printf("✅ CC1101 custom frequency set: %u Hz\n", registersToFreq(freq));
}

// ═══════════════════════════════════════════════════════════════════════════
// V1 SPECIFIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void rxFlushV1() { cc1101_strobe(0x36); cc1101_strobe(0x3A); delay(5); }
void rxEnableV1() { cc1101_strobe(0x36); delay(5); cc1101_strobe(0x34); }

void sendFrame_V1(byte cmd, byte sub, byte d1, byte d2) {
    byte frame[10] = {0x2B, 0x01, myAddrV1[0], myAddrV1[1], myAddrV1[2], cmd, sub, d1, d2, 0x00};
    cc1101_strobe(0x36); cc1101_strobe(0x3B);
    for(int i = 0; i < 10; i++) cc1101_writeReg(0x3F, frame[i]);
    cc1101_strobe(0x35); delay(100);
    rxFlushV1(); rxEnableV1();
}

void decodePacket_V1(byte* data) {
    if(data[2] != myAddrV1[0] || data[3] != myAddrV1[1] || data[4] != myAddrV1[2]) return;
    uint8_t cmd = data[5], sub = data[6], b7 = data[7], b8 = data[8];
    Serial.printf("V1 PKT: CMD=%02X SUB=%02X B7=%02X B8=%02X\n", cmd, sub, b7, b8);

    if(cmd == 0x32 && sub == 0x20) {
        heaterStatus.state = STATE_RUNNING;
        heaterStatus.voltage = (b7 * 2 + (b8 & 0x01)) * 10;
        heaterStatus.errorCode = ERR_NONE;
    } else if(cmd == 0x31 && sub == 0x60) {
        heaterStatus.state = STATE_COOLING;
        heaterStatus.errorCode = (b7 == 0x64) ? ERR_EXTINGUISHED : ERR_NONE;
    } else if(cmd == 0x31 && sub == 0x00) {
        heaterStatus.state = STATE_OFF;
        if(b7 == 0x06)       heaterStatus.errorCode = ERR_NONE;
        else if(b7 == 0x61)  heaterStatus.errorCode = ERR_OIL_PUMP;
        else if(b7 == 0x62)  heaterStatus.errorCode = ERR_SPARK_PLUG;
        else if(b7 == 0x63)  heaterStatus.errorCode = ERR_SENSOR;
        else                  heaterStatus.errorCode = ERR_NONE;
    } else if(cmd == 0x31 && sub == 0x20) {
        heaterStatus.state = STATE_RUNNING;
        heaterStatus.errorCode = ERR_NONE;
        if(b7 <= 0x03) {
            heaterStatus.autoMode = false;
            if(b7 == 0x00 && (b8 & 0xF0) == 0x90) heaterStatus.power = 1;
            else if(b7 == 0x01 && (b8 & 0xF0) == 0x10) heaterStatus.power = 2;
            else if(b7 == 0x01 && (b8 & 0xF0) == 0x90) heaterStatus.power = 3;
            else if(b7 == 0x02 && (b8 & 0xF0) == 0x10) heaterStatus.power = 4;
            else if(b7 == 0x02 && (b8 & 0xF0) == 0x90) heaterStatus.power = 5;
            else if(b7 == 0x03 && (b8 & 0xF0) == 0x10) heaterStatus.power = 6;
            heaterStatus.pumpFreq = heaterStatus.power * 10;
            heaterStatus.setpoint = 0;
        } else {
            heaterStatus.autoMode = true;
            // The frame comes off the air, so a corrupt b7 can push this
            // far outside what an int8_t holds.
            int sp = (b7 - 32) * 2 + (b8 >> 7);
            heaterStatus.setpoint = (int8_t)constrain(sp, -128, 127);
            heaterStatus.power = 0;
        }
        heaterStatus.ambientTemp = (b8 & 0x0F) + 10;
    } else if(cmd == 0x31 && sub == 0xA0) {
        heaterStatus.caseTemp = b7 * 2;
    }
    heaterStatus.lastUpdate = millis();
}

void updateHeaterStatus_V1() {
    if(!heaterPaired) return;
    unsigned long now = millis();
    static unsigned long lastPoll = 0;
    if(now - lastPoll > 100) {
        lastPoll = now;
        uint8_t rxBytes = cc1101_readReg(0xBB) & 0x7F;
        if(rxBytes >= 10) {
            byte buf[10];
            for(int i = 0; i < 10; i++) buf[i] = cc1101_readReg(0xBF);
            decodePacket_V1(buf);
            // V1 frames are read as a fixed 10 bytes, so there is no appended
            // status byte to take the signal level from. The radio keeps the
            // last measurement in its RSSI status register instead.
            heaterStatus.rssi = rssiFromRaw(cc1101_readReg(0xF4));
            addErrorToHistory(heaterStatus.errorCode);
            rxFlushV1(); rxEnableV1();
        }
    }
    if(autoMenuV1 && heaterPaired) {
        if(now - lastCommandTimeV1 > commandLockTimeV1) {
            if(now - lastMenuChangeV1 >= menuIntervalV1) {
                lastMenuChangeV1 = now;
                sendFrame_V1(0x29, 0x00, 0x00, 0x00);
                delay(150);
                sendFrame_V1(0x2B, 0xAA, 0x80, 0x00);
            }
        }
    }
    if(mqttEnabled && mqtt.connected()) publishMQTT();
}

// ═══════════════════════════════════════════════════════════════════════════
// TX/RX FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void txFlush() { cc1101_strobe(0x36); cc1101_strobe(0x3B); delay(16); }

void txBurst(uint8_t len, uint8_t* bytes) {
    txFlush();
    cc1101_writeBurst(0x7F, len, bytes);
    cc1101_strobe(0x35);
}

void sendCommand(uint8_t cmd) {
    if(!heaterPaired) return;
    lastHeaterCommandMs = millis();
    if(heaterVersion == "V1") {
        lastCommandTimeV1 = millis();
        if(cmd == CMD_POWER) {
            if(heaterStatus.state == STATE_OFF || heaterStatus.state == 0)
                sendFrame_V1(0x2B, 0xAD, 0x80, 0x00);
            else
                sendFrame_V1(0x2B, 0xAE, 0x00, 0x00);
        } else if(cmd == CMD_UP)   sendFrame_V1(0x2B, 0xA9, 0x00, 0x00);
        else if(cmd == CMD_DOWN)   sendFrame_V1(0x2B, 0xA9, 0x80, 0x00);
        else if(cmd == CMD_MODE)   sendFrame_V1(0x2B, 0xAA, 0x00, 0x00);
        return;
    }
    uint8_t buf[10];
    buf[0] = 9; buf[1] = cmd;
    buf[2] = (heaterAddress >> 24) & 0xFF;
    buf[3] = (heaterAddress >> 16) & 0xFF;
    buf[4] = (heaterAddress >> 8) & 0xFF;
    buf[5] = heaterAddress & 0xFF;
    buf[6] = packetSeq++; buf[9] = 0;
    uint16_t crc = crc16_modbus(buf, 7);
    buf[7] = (crc >> 8) & 0xFF; buf[8] = crc & 0xFF;
    for(int i = 0; i < 10; i++) {
        txBurst(10, buf);
        unsigned long t = millis();
        while(cc1101_readReg(0xF5) != 0x01) {
            delay(1);
            if(millis() - t > 100) return;
        }
    }
}

void rxFlush() { cc1101_strobe(0x36); cc1101_readReg(0xBF); cc1101_strobe(0x3A); delay(16); }
void rxEnable() { cc1101_strobe(0x34); }

bool receivePacket(uint8_t* bytes, uint16_t timeout, uint8_t* outLen) {
    unsigned long t = millis();
    uint8_t rxLen;
    rxFlush(); rxEnable();
    while(1) {
        yield(); feedWatchdog();
        if(millis() - t > timeout) return false;
        // Pairing listens on air for up to 60 seconds. That is a legitimate
        // wait rather than a hang, so feed the watchdog here as well.
        while(!digitalRead(PIN_GDO2)) {
            yield(); feedWatchdog();
            if(millis() - t > timeout) return false;
        }
        delay(5);
        rxLen = cc1101_readReg(0xFB);
        if(rxLen >= 23 && rxLen <= 26) break;
        rxFlush(); rxEnable();
    }
    for(int i = 0; i < rxLen; i++) bytes[i] = cc1101_readReg(0xBF);
    rxFlush();
    if(outLen) *outLen = rxLen;
    uint16_t crc = crc16_modbus(bytes, 21);
    uint16_t rxCrc = (bytes[21] << 8) | bytes[22];
    return (crc == rxCrc);
}

// ═══════════════════════════════════════════════════════════════════════════
// HEATER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void updateHeaterStatus() {
    if(!heaterPaired) return;
    sendCommand(CMD_WAKEUP);
    uint8_t buf[32];
    uint8_t rxLen = 0;
    if(receivePacket(buf, 2000, &rxLen)) {
        uint32_t addr = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                        ((uint32_t)buf[4] << 8) | buf[5];
        if(addr == heaterAddress) {
            heaterStatus.state = buf[6];
            // BYTE[7] is the error code. It used to be copied into .power as
            // well, but nothing reads that for V2 -- only the V1 decoder
            // fills it meaningfully.
            heaterStatus.errorCode = buf[7];
            heaterStatus.voltage = buf[9];
            heaterStatus.ambientTemp = (int8_t)buf[10];
            heaterStatus.caseTemp = buf[12];
            heaterStatus.setpoint = (int8_t)buf[13];
            heaterStatus.autoMode = (buf[14] == 0x32);
            heaterStatus.pumpFreq = buf[15];
            // The radio appends RSSI and LQI to the frame, but only when it
            // was long enough to carry them: a 23-byte frame ends at index 22
            // and buf[23] would be uninitialised stack. Keep the previous
            // reading rather than report noise.
            if(rxLen >= 24) heaterStatus.rssi = rssiFromRaw(buf[23]);
            heaterStatus.lastUpdate = millis();
            addErrorToHistory(heaterStatus.errorCode);
            if(mqttEnabled && mqtt.connected()) publishMQTT();
        }
    }
}


// Listens in short slices so the rest of the loop keeps running. The window
// and the "first valid frame wins" rule are unchanged from the blocking
// version; only the waiting is broken up.
void updatePairing() {
    if(pairState != PAIR_SEARCHING) return;

    // A dead module would burn a whole slice per iteration waiting for a
    // frame that cannot arrive.
    if(cc1101Fault) return;

    if((int32_t)(pairDeadlineMs - millis()) <= 0) {
        pairState = PAIR_FAILED;
        displayLine2 = "Pair failed";
        Serial.println("⚠️ Pairing timed out");
        return;
    }

    uint8_t buf[32];
    uint8_t rxLen = 0;
    if(!receivePacket(buf, PAIR_SLICE_MS, &rxLen)) return;

    uint32_t addr = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                    ((uint32_t)buf[4] << 8) | buf[5];
    if(addr == 0) return;

    heaterAddress = addr;
    heaterPaired  = true;
    prefs.putUInt("heaterAddr", heaterAddress);
    pairState = PAIR_OK;
    Serial.printf("✅ Paired with 0x%08X\n", heaterAddress);
}

// Slow down only when there is nothing to watch. "Not OFF" covers the whole
// purge sequence, which the scheduler follows closely, and a heater that has
// gone silent stays on the idle rate rather than eating the loop.
static uint32_t heaterPollIntervalMs() {
    if(millis() - lastHeaterCommandMs < HEATER_POLL_BOOST_MS) return HEATER_POLL_FAST_MS;
    if(heaterStatus.state != STATE_OFF)                       return HEATER_POLL_FAST_MS;
    return HEATER_POLL_IDLE_MS;
}

// ═══════════════════════════════════════════════════════════════════════════
// EXPLICIT HEATER CONTROL
// ═══════════════════════════════════════════════════════════════════════════
//
// The protocol only offers a power toggle, which is unusable for anything
// automatic: a toggle sent to an already-off heater switches it on. These
// wrappers look at the reported state first and return whether a command
// was actually sent.

bool heaterEnsureOff() {
    if(!heaterPaired) return false;
    if(heaterIsOffOrStopping(heaterStatus.state)) return false;
    // A stop cancels any ignition still being verified, otherwise the retry
    // would fight the shutdown that was just ordered. It also abandons the
    // measurement: a start somebody interrupted says nothing about the plug.
    ignWatching  = false;
    ignLogActive = false;
    sendCommand(CMD_POWER);
    return true;
}

// Defined with the rest of the wall-clock helpers, further down.
int currentMinutesOfDay();

// Gathers the state the verdict is made from. Every start goes through here,
// whether it came from the chat, the web GUI or the schedule, so all three
// refuse on the same grounds.
StartVerdict heaterStartVerdict() {
    ManualStartInput in;
    in.heaterPaired    = heaterPaired;
    in.heaterState     = heaterStatus.state;
    in.timeValid       = timeValid;
    in.nowMinutes      = timeValid ? currentMinutesOfDay() : -1;
    in.blackoutEnabled = blackoutEnabled;
    in.blackoutMinutes = blackoutMinutes;
    in.shutdownLeadMin = shutdownLeadMin;
    return checkManualStart(in);
}

// A refusal is only useful if it says what to do about it, so the blackout
// case carries the number of minutes left rather than a bare no.
String startRefusalText(StartVerdict v) {
    switch(v) {
        case START_NO_HEATER:
            return "Heater is not paired.";
        case START_BUSY:
            return "Heater is already running or still purging";
        case START_TOO_LATE: {
            int left = minutesUntil(currentMinutesOfDay(), blackoutMinutes);
            return "Too late to start: mains power goes in " + String(left) +
                   " min. Lighting now would mean a shutdown before the burner "
                   "even settles, which is what cokes it up.";
        }
        default:
            return "";
    }
}

StartVerdict heaterEnsureOn() {
    StartVerdict v = heaterStartVerdict();
    if(v != START_ALLOWED) return v;
    sendCommand(CMD_POWER);
    ignWatching      = true;
    ignAttempts      = 1;
    ignCommandedAtMs = millis();
    counters.starts++;
    countersDirty = true;

    ignLogActive     = true;
    ignLogStartMs    = millis();
    ignLogVoltBefore = heaterStatus.voltage;
    ignLogVoltMin    = heaterStatus.voltage;
    ignLogAmbient    = heaterStatus.ambientTemp;

    return START_ALLOWED;
}

// ═══════════════════════════════════════════════════════════════════════════
// WALL CLOCK
// ═══════════════════════════════════════════════════════════════════════════

// Belarus sits at UTC+3 with no daylight saving, so a plain offset is enough
// and no POSIX timezone rule is needed.
void setupTime() {
    configTime(tzOffsetMin * 60, 0, ntpServer.c_str(), "time.google.com");
}

// Any year past 2024 means SNTP has answered; the 1970 epoch means it has not.
static bool clockLooksSynced() {
    struct tm t;
    if(!getLocalTime(&t, 0)) return false;
    return (t.tm_year + 1900) > 2024;
}

// Minutes since local midnight, or -1 while the clock is unusable.
int currentMinutesOfDay() {
    struct tm t;
    if(!getLocalTime(&t, 0)) return -1;
    return t.tm_hour * 60 + t.tm_min;
}

String currentTimeString() {
    struct tm t;
    if(!getLocalTime(&t, 0)) return String("--:--");
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    return String(buf);
}

// SNTP keeps polling on its own once it succeeds, so this only has to watch
// for the first sync.
void updateTimeSync() {
    if(timeValid) return;

    static unsigned long lastCheck = 0;
    if(millis() - lastCheck < NTP_CHECK_MS) return;
    lastCheck = millis();

    if(clockLooksSynced()) {
        timeValid = true;
        Serial.println("✅ Time synced: " + currentTimeString());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// TELEGRAM
// ═══════════════════════════════════════════════════════════════════════════

const char* resetReasonName(esp_reset_reason_t r) {
    switch(r) {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_EXT:       return "external reset";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "crash";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        default:                return "unknown";
    }
}

static void tgSendToAll(const String& text) {
    std::vector<int64_t> ids = parseChatList(std::string(tgChats.c_str()));
    for(size_t i = 0; i < ids.size(); i++) {
        if(!tgBot.sendTo(ids[i], text.c_str())) {
            tgReady = false;   // force a reconnect on the next cycle
        }
    }
}

// Queues the message when the bot is not up yet. The boot notice is produced
// before the network is ready, and it is the one worth keeping.
void notifyTelegram(const String& text) {
    Serial.println("TG: " + text);
    if(!tgEnabled) return;

    if(!tgReady) {
        if(tgPending.length() < 512) {
            if(tgPending.length()) tgPending += "\n\n";
            tgPending += text;
        }
        return;
    }
    tgSendToAll(text);
}

// Connects lazily from loop() rather than in setup(): a TLS handshake with no
// route out would stall startup, and the link here is unreliable by default.
void updateTelegram() {
    if(!tgEnabled || tgToken.length() == 0) return;
    if(WiFi.status() != WL_CONNECTED || tgReady) return;

    static uint32_t lastTry = 0;
    static uint32_t backoff = 0;
    if(lastTry != 0 && millis() - lastTry < backoff) return;
    lastTry = millis();

    strncpy(tgTokenBuf, tgToken.c_str(), sizeof(tgTokenBuf) - 1);

    // A custom CA overrides the one bundled with the library. That is the
    // escape hatch if Telegram ever changes issuer: with a hardcoded
    // certificate only, a stale one would need a trip to the garage.
    tgClient.setCACert(tgCaCert.length() > 0 ? tgCaCert.c_str() : telegram_cert);
    tgBot.setTelegramToken(tgTokenBuf);
    tgBot.setUpdateTime((uint32_t)tgPollSec * 1000UL);
    tgCurrentPollMs = (uint32_t)tgPollSec * 1000UL;

    tgReady = tgBot.begin();
    if(!tgReady) {
        backoff = nextBackoffMs(backoff);
        Serial.printf("⚠️ Telegram unreachable, retry in %u s\n", backoff / 1000);
        return;
    }

    backoff = 0;
    Serial.println("✅ Telegram connected");
    if(tgPending.length()) {
        tgSendToAll(tgPending);
        tgPending = "";
    }
}

// Watches for conditions worth a message. Everything goes through
// changedSince(), because the heater is polled every three seconds and an
// error would otherwise produce twenty messages a minute.
void updateNotifications() {
    if(!tgEnabled) return;

    static int  lastError  = -1;
    static int  lastState  = -1;
    static int  lastFault  = -1;
    static bool lowVoltage = false;
    static bool statePrimed = false;

    if(changedSince(lastFault, cc1101Fault ? 1 : 0) && cc1101Fault) {
        notifyTelegram("🔴 CC1101 not responding — check the wiring");
    }

    if(heaterStatus.lastUpdate == 0) return;   // nothing heard from the heater yet

    // Codes 0x00 and 0x01 both mean normal operation; STANDBY is not a fault.
    if(changedSince(lastError, heaterStatus.errorCode)) {
        if(heaterStatus.errorCode > ERR_ON && heaterStatus.errorCode != ERR_STANDBY) {
            notifyTelegram("🔴 Heater error: " +
                           String(getErrorName(heaterStatus.errorCode)));
        }
    }

    // The state at boot is the status quo, not an event -- prime it silently,
    // otherwise every restart would announce "heater is OFF".
    if(!statePrimed) {
        lastState   = heaterStatus.state;
        statePrimed = true;
    } else if(changedSince(lastState, heaterStatus.state)) {
        notifyTelegram("🔥 Heater: " + String(getStateName(heaterStatus.state)));
    }

    // The raw verdict is instantaneous; the debounce is what keeps the
    // ignition dip from being announced as a fault every single start.
    static uint32_t voltSinceMs = 0;
    bool raw = voltageAlarm(lowVoltage, heaterStatus.voltage,
                            voltAlarmDeciV, voltClearDeciV);
    bool was = lowVoltage;
    bool now = debounceVerdict(lowVoltage, voltSinceMs, raw, millis(),
                               (uint32_t)voltDebounceS * 1000UL);
    if(now != was) {
        notifyTelegram(now
            ? "🔴 Supply voltage sagging: " +
              String(heaterStatus.voltage / 10.0, 1) + " V — the PSU may be undersized"
            : "🟢 Supply voltage back to normal: " +
              String(heaterStatus.voltage / 10.0, 1) + " V");
    }
}

// ── Commands ───────────────────────────────────────────────────────────────

// Defined further down, with the rest of the scheduling code.
void     cancelScheduledStartPublic();
bool     scheduleStartAt(int hour, int minute, String& reply);
bool     scheduleStartIn(int minutes, String& reply);
extern bool     startArmed;
extern uint32_t startTarget;
String   clockOfPublic(uint32_t epoch);

// Defined further down, with the power level walk.
bool     startLevelChange(int64_t chatId, const String& cmd, String& reply);

// Defined further down, with the fuel counters.
String   mlText(uint32_t ml);
String   fuelWarningForRun();
bool     tankActive();
String   statsText();
String   ignText();
void     persistStats();
extern Counters counters;

static String tgStatusText() {
    String m = "🔥 Diesel Pilot\n\n";

    if(cc1101Fault) {
        m += "RF module is not responding.\nCheck the CC1101 wiring.\n";
        return m;
    }
    if(!heaterPaired) {
        m += "Heater is not paired.\nPair it from the web GUI.\n";
        return m;
    }
    if(heaterStatus.lastUpdate == 0) {
        m += "No reply from the heater yet.\n";
        return m;
    }

    m += "State:    " + String(getStateName(heaterStatus.state)) + "\n";
    if(heaterStatus.autoMode) {
        m += "Ambient:  " + String(heaterStatus.ambientTemp) + " C -> " +
             String(heaterStatus.setpoint) + " C\n";
    } else {
        m += "Ambient:  " + String(heaterStatus.ambientTemp) + " C\n";
        m += "Pump:     " + String(heaterStatus.pumpFreq / 10.0, 1) + " Hz\n";
    }
    m += "Case:     " + String(heaterStatus.caseTemp) + " C\n";
    m += "Battery:  " + String(heaterStatus.voltage / 10.0, 1) + " V\n";
    m += "Signal:   " + String(heaterStatus.rssi) + " dBm\n";
    m += "Error:    " + String(getErrorName(heaterStatus.errorCode)) + "\n";
    m += "Mode:     " + String(heaterStatus.autoMode ? "AUTO" : "MANUAL") + "\n";
    if(fuelTicks) {
        m += "Fuel:     ~" + String(fuelMlFromTicks(fuelTicks)) + " ml this burn\n";
    }
    if(tankActive()) {
        m += "Tank:     ~" + mlText(tankRemainingMl) + " of " +
             mlText(tankCapacityMl) + "\n";
    }
    uint32_t age = (millis() - heaterStatus.lastUpdate) / 1000;
    m += "\nUpdated:  " + String(age) + " s ago";
    if(age > 30) m += "  (stale)";
    if(startArmed) {
        m += "\nScheduled start: " + clockOfPublic(startTarget);
    }
    if(sessionAutoOffMin) {
        m += "\nRuntime limit: " + String(sessionAutoOffMin) + " min (this burn)";
    }
    m += "\nClock:    " + String(timeValid ? currentTimeString() : String("not synced"));
    m += "\nHeap:     " + String(ESP.getFreeHeap() / 1024) + " KB free, min " +
         String(ESP.getMinFreeHeap() / 1024) + " KB";
    m += "\nLoop max: " + String(loopMaxMs) + " ms";
    return m;
}

static void tgSendStatus(int64_t chatId) {
    InlineKeyboard kb;
    kb.addButton("🔥 On",   "on", KeyboardButtonQuery);
    kb.addButton("❄️ Off",  "off", KeyboardButtonQuery);
    kb.addButton("🔄",      "st", KeyboardButtonQuery);
    kb.addRow();
    kb.addButton("− 1",     "dn", KeyboardButtonQuery);
    kb.addButton("+ 1",     "up", KeyboardButtonQuery);
    kb.addButton("⚙️ Mode", "md", KeyboardButtonQuery);
    kb.addRow();
    kb.addButton("−30 min", "t-", KeyboardButtonQuery);
    kb.addButton("+30 min", "t+", KeyboardButtonQuery);
    tgBot.sendTo(chatId, tgStatusText(), kb.getJSON());
}

// Buttons that sit under the text field for good. Telegram keeps a reply
// keyboard until the bot sends a different one, so this goes out once with
// the greeting and is never attached to another message.
//
// Six is deliberate: the things worth reaching without thinking. Everything
// that takes an argument -- /at 06:30, /for 90 -- cannot be a button at all,
// which is what the help button is for.
static String tgKeyboardJson() {
    ReplyKeyboard kb;
    kb.addButton("🔥 On");
    kb.addButton("❄️ Off");
    kb.addButton("🔄 Status");
    kb.addRow();
    kb.addButton("⛽ Tank");
    kb.addButton("📊 Stats");
    kb.addButton("❓ Help");
    kb.enableResize();      // not one-time: it is meant to stay
    return kb.getJSON();
}

// A button arrives as its own label, so labels are folded into the commands
// they stand for before anything else looks at them.
static String tgCanonical(const String& text) {
    if(text == "🔥 On")     return "/on";
    if(text == "❄️ Off")    return "/off";
    if(text == "🔄 Status") return "/status";
    if(text == "⛽ Tank")   return "/tank";
    if(text == "📊 Stats")  return "/stats";
    if(text == "❓ Help")   return "/help";
    return text;
}

static void tgSendHelp(int64_t chatId) {
    tgBot.sendTo(chatId,
        "The buttons cover the everyday ones. These take a value, so they "
        "have to be typed:\n\n"
        "/at 06:30 - start at that time\n"
        "/in 2h - start after that delay\n"
        "/cancel - drop a pending scheduled start\n"
        "/for 90 - run for 90 minutes this time only\n"
        "/level 4 - set the power level (MANUAL)\n"
        "/temp 22 - set the target temperature (AUTO)\n"
        "/filled - tank filled up, /filled 10 - ten litres added\n"
        "/service done - reset the counters since the last service\n"
        "\nAnd the rest:\n\n"
        "/status - current readings and buttons\n"
        "/on - start the heater\n"
        "/off - stop it (a purge follows)\n"
        "/tank - what is left in the tank\n"
        "/stats - hours, fuel and starts\n"
        "/ign - how the last few starts went\n"
        "/id - show your chat id",
        tgKeyboardJson());
}

static void tgHandleCommand(int64_t chatId, const String& cmd) {
    if(cmd == "st" || cmd == "/status") {
        tgSendStatus(chatId);
        return;
    }

    // First contact, and the one moment the keyboard is guaranteed to be sent.
    if(cmd == "/start" || cmd == "/help") {
        tgSendHelp(chatId);
        return;
    }

    if(cmd == "/id") {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)chatId);
        tgBot.sendTo(chatId, "Your chat id: " + String(buf));
        return;
    }

    if(cmd.startsWith("/for ") || cmd == "t+" || cmd == "t-") {
        int base = sessionAutoOffMin ? sessionAutoOffMin : autoOffMin;
        int want = cmd.startsWith("/for ") ? cmd.substring(5).toInt()
                 : (cmd == "t+" ? base + 30 : base - 30);

        if(want < 0)    want = 0;
        if(want > 1440) want = 1440;
        sessionAutoOffMin = (uint16_t)want;

        tgBot.sendTo(chatId, want == 0
            ? "Runtime limit off for this burn"
            : "Runtime limit set to " + String(want) + " min for this burn");
        return;
    }

    if(cmd == "/stats") {
        tgBot.sendTo(chatId, statsText());
        return;
    }

    if(cmd == "/ign") {
        tgBot.sendTo(chatId, ignText());
        return;
    }

    // Spelt out rather than a bare /service, because one stray tap should not
    // silently wipe the only record of when the burner was last cleaned.
    if(cmd.startsWith("/service")) {
        if(cmd != "/service done") {
            tgBot.sendTo(chatId, "Send /service done to reset the counters since "
                                 "the last service");
            return;
        }
        countersMarkService(counters, timeValid ? (uint32_t)time(nullptr) : 0);
        countersDirty = true;
        persistStats();
        tgBot.sendTo(chatId, "🔧 Service recorded — counters since it start again");
        return;
    }

    // Litres in the chat, millilitres inside: nobody thinks about a tank in
    // millilitres, and the accumulator has to.
    if(cmd == "/filled" || cmd.startsWith("/filled ")) {
        if(!tankActive()) {
            tgBot.sendTo(chatId, "Tank tracking is off — switch it on in the "
                                 "Timers tab and give it a capacity");
            return;
        }
        uint32_t added = (uint32_t)(cmd.substring(7).toFloat() * 1000.0);
        tankRefill(tankRemainingMl, tankCapacityMl, added);
        tankWarnLast  = tankWarnLevel(tankRemainingMl, tankCapacityMl);
        countersDirty = true;
        persistStats();
        tgBot.sendTo(chatId, "⛽ Tank now ~" + mlText(tankRemainingMl) +
                             " of " + mlText(tankCapacityMl));
        return;
    }

    // The estimate drifts, so there has to be a way to correct it without
    // waiting for a full tank to reset it.
    if(cmd == "/tank" || cmd.startsWith("/tank ")) {
        if(!tankActive()) {
            tgBot.sendTo(chatId, "Tank tracking is off — switch it on in the "
                                 "Timers tab and give it a capacity");
            return;
        }
        if(cmd.length() > 5) {
            uint32_t want = (uint32_t)(cmd.substring(5).toFloat() * 1000.0);
            if(want > tankCapacityMl) want = tankCapacityMl;
            tankRemainingMl = want;
            tankWarnLast    = tankWarnLevel(tankRemainingMl, tankCapacityMl);
            countersDirty   = true;
            persistStats();
        }
        uint32_t rate = fuelRateMlPerHour(counters.fuelMl, counters.burnSec,
                                          fuelDoseUl);
        tgBot.sendTo(chatId,
            "⛽ Tank ~" + mlText(tankRemainingMl) + " of " + mlText(tankCapacityMl) +
            "\nBurn rate ~" + mlText(rate) + "/h" +
            "\nGood for roughly " + String(rate ? tankRemainingMl / rate : 0) +
            " h — an estimate from the pump, not a gauge");
        return;
    }

    if(cmd.startsWith("/level ") || cmd.startsWith("/temp ")) {
        String reply;
        startLevelChange(chatId, cmd, reply);
        tgBot.sendTo(chatId, reply);
        return;
    }

    if(cmd == "/cancel") {
        cancelScheduledStartPublic();
        tgBot.sendTo(chatId, "Scheduled start cancelled");
        return;
    }

    if(cmd.startsWith("/at ")) {
        String arg = cmd.substring(4); arg.trim();
        int colon = arg.indexOf(':');
        if(colon < 1) { tgBot.sendTo(chatId, "Use /at HH:MM"); return; }
        String reply;
        scheduleStartAt(arg.substring(0, colon).toInt(),
                        arg.substring(colon + 1).toInt(), reply);
        tgBot.sendTo(chatId, reply);
        return;
    }

    if(cmd.startsWith("/in ")) {
        String arg = cmd.substring(4); arg.trim();
        // Accepts "2h", "90m" or a bare number of minutes.
        long value = arg.toInt();
        if(arg.endsWith("h")) value *= 60;
        String reply;
        scheduleStartIn((int)value, reply);
        tgBot.sendTo(chatId, reply);
        return;
    }

    bool heaterCommand = (cmd == "on" || cmd == "/on" || cmd == "off" ||
                          cmd == "/off" || cmd == "up" || cmd == "dn" ||
                          cmd == "md");

    // Anything unrecognised gets the list. Silence after a typo reads as a
    // bot that has stopped working, which is the wrong thing to wonder about
    // from a hundred kilometres away.
    if(!heaterCommand) {
        tgSendHelp(chatId);
        return;
    }

    if(!heaterPaired) {
        tgBot.sendTo(chatId, "Heater is not paired.");
        return;
    }

    if(cmd == "on" || cmd == "/on") {
        StartVerdict v = heaterEnsureOn();
        tgBot.sendTo(chatId, v == START_ALLOWED
            ? "🔥 Ignition requested" + fuelWarningForRun()
            : startRefusalText(v));
    } else if(cmd == "off" || cmd == "/off") {
        tgBot.sendTo(chatId, heaterEnsureOff()
            ? "❄️ Shutdown requested, the purge will follow"
            : "Heater is already off or stopping");
    } else if(cmd == "up") {
        sendCommand(CMD_UP);
        tgSendStatus(chatId);
    } else if(cmd == "dn") {
        sendCommand(CMD_DOWN);
        tgSendStatus(chatId);
    } else if(cmd == "md") {
        sendCommand(CMD_MODE);
        tgSendStatus(chatId);
    }
}

// Polls for incoming messages. Runs only once the bot is up, so a dead link
// costs nothing here -- reconnection is updateTelegram()'s job.
void updateTelegramCommands() {
    if(!tgEnabled || !tgReady) return;

    TBMessage msg;
    if(tgBot.getNewMessage(msg) == MessageNoData) return;

    String text = (msg.messageType == MessageQuery) ? msg.callbackQueryData : msg.text;
    text.trim();

    // The whitelist is the only gate between a stranger and the heater:
    // anyone can find a bot by name and write to it. Unknown senders get
    // silence rather than a refusal -- no reason to confirm anything exists.
    if(!isChatAllowed(std::string(tgChats.c_str()), msg.chatId)) {
        bool discovering = tgDiscoverUntilMs != 0 &&
                           (int32_t)(tgDiscoverUntilMs - millis()) > 0;
        // While no chat is configured the device is inert anyway, and this is
        // the only way to learn your own id without a third-party bot.
        if(text == "/id" && (discovering || tgChats.length() == 0)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)msg.chatId);
            tgBot.sendTo(msg.chatId, "Your chat id: " + String(buf));
        }
        return;
    }

    lastTgActivityMs = millis();
    if(msg.messageType == MessageQuery) tgBot.endQuery(msg, "");
    tgHandleCommand(msg.chatId, tgCanonical(text));
}

// Keeps the keyboard responsive while it is being used without paying for
// that rate around the clock.
void updateTelegramPollRate() {
    if(!tgEnabled || !tgReady) return;

    // Chat id discovery is the one moment a fast reply actually matters, and
    // it used to be the slowest: the boost below only triggers on a message
    // from an allowed chat, and during discovery there is no allowed chat yet
    // by definition. So a minute would pass before /id came back, which reads
    // as a bot that is simply not working.
    bool discovering = tgDiscoverUntilMs != 0 &&
                       (int32_t)(tgDiscoverUntilMs - millis()) > 0;

    uint32_t want = (discovering ||
                     (lastTgActivityMs != 0 &&
                      millis() - lastTgActivityMs < TG_POLL_BOOST_MS))
                  ? TG_POLL_ACTIVE_MS
                  : (uint32_t)tgPollSec * 1000UL;

    if(want == tgCurrentPollMs) return;
    tgCurrentPollMs = want;
    tgBot.setUpdateTime(want);
}

// Verifies that a commanded start actually lit the heater. Without this the
// command went out over the air and was forgotten, so a failure to ignite
// stayed silent until somebody arrived to a cold garage.
// Integrates the pump rate once a second. Stale readings are skipped: the
// last known rate is not evidence the pump is still running at it.
void updateFuel() {
    static uint8_t lastState = STATE_OFF;

    bool fresh = heaterStatus.lastUpdate != 0 &&
                 (millis() - heaterStatus.lastUpdate) < HEATER_DATA_FRESH_MS;

    if(heaterStatus.state == STATE_OFF) {
        if(lastState != STATE_OFF) fuelTicks = 0;   // new burn starts at zero
    } else if(fresh) {
        fuelTicks += fuelTickPerSecond(fuelDoseUl, heaterStatus.pumpFreq);
    }
    lastState = heaterStatus.state;
}

// ═══════════════════════════════════════════════════════════════════════════
// PERSISTENT COUNTERS
// ═══════════════════════════════════════════════════════════════════════════
//
// RAM does not survive 22:00 here, so anything worth knowing in the morning
// has to reach NVS before the power goes. Writes are batched rather than
// continuous: one at the end of every burn, one every ten minutes while it
// runs, and one before a scheduled shutdown. That comes to a handful a day,
// which NVS wear levelling absorbs without noticing.

#define STATS_FLUSH_MS 600000UL

// Ticks already charged to the lifetime total. A mid-burn flush must not bill
// the same fuel twice, and the sub-millilitre remainder stays here rather
// than being rounded away once a second.
static uint32_t fuelTicksBilled = 0;

// Read from the stored flag at boot and resolved once the heater has actually
// reported: still burning means the controller restarted underneath it, off
// means the power went mid-burn.
static bool bootDirtyPending = false;

// Running time, in the units that suit its size. Hours for anything past a
// day of use, which is where these counters spend their life.
String hoursText(uint32_t seconds) {
    if(seconds < 3600) return String(seconds / 60) + " min";
    return String(seconds / 3600) + " h " + String((seconds % 3600) / 60) + " min";
}

// A date for the service log. Falls back when the clock had never synced at
// the moment it was recorded.
String dateOf(uint32_t epoch) {
    if(epoch == 0) return "never";
    time_t    t = (time_t)epoch;
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[16];
    // strftime rather than snprintf: the compiler cannot bound the tm fields
    // and warns about a truncation that cannot actually happen.
    strftime(buf, sizeof(buf), "%d.%m.%Y", &lt);
    return String(buf);
}

// Whether the tank estimate is running at all. Off by default, and off
// whenever there is no capacity to measure against.
bool tankActive() {
    return tankEnabled && tankCapacityMl > 0;
}

// Millilitres read as litres once there are enough of them. Always with a
// tilde at the call site: this is dead reckoning, not a gauge.
String mlText(uint32_t ml) {
    if(ml < 1000) return String(ml) + " ml";
    return String(ml / 1000.0, 1) + " l";
}

// How long the burn just started is expected to last, for the fuel estimate.
// Zero when nothing bounds it, in which case there is nothing to estimate.
static uint16_t plannedRunMinutes() {
    uint16_t limit = sessionAutoOffMin ? sessionAutoOffMin : autoOffMin;
    if(limit) return limit;

    if(blackoutEnabled && timeValid) {
        int deadline = blackoutMinutes - (int)shutdownLeadMin;
        while(deadline < 0) deadline += MINUTES_PER_DAY;
        deadline %= MINUTES_PER_DAY;
        return (uint16_t)minutesUntil(currentMinutesOfDay(), deadline);
    }
    return 0;
}

// Says so when the tank will not cover the burn that was just started. A
// warning rather than a refusal: the figure is dead reckoning, and refusing
// to light a heater on the strength of a guess is worse than being wrong.
String fuelWarningForRun() {
    if(!tankActive()) return "";

    uint16_t minutes = plannedRunMinutes();
    if(minutes == 0) return "";

    uint32_t rate   = fuelRateMlPerHour(counters.fuelMl, counters.burnSec,
                                        fuelDoseUl);
    uint32_t needed = fuelNeededMl(rate, minutes);
    if(needed <= tankRemainingMl) return "";

    return "\n⚠️ Tank holds ~" + mlText(tankRemainingMl) + ", a " +
           String(minutes) + " min burn needs ~" + mlText(needed);
}

// ── Ignition log ───────────────────────────────────────────────────────────

// A start that has not lit in ten minutes is not going to.
#define IGN_LOG_TIMEOUT_MS 600000UL

void loadErrLog() {
    size_t len = prefs.getBytes("errlog", errRing, sizeof(errRing));
    if(!ringValid(errRing, len, ERR_LOG_CAPACITY, ERR_RECORD_BYTES,
                  ERR_LOG_VERSION)) {
        ringInit(errRing, ERR_LOG_CAPACITY, ERR_RECORD_BYTES, ERR_LOG_VERSION);
    }
}

void loadIgnLog() {
    size_t len = prefs.getBytes("ignlog", ignRing, sizeof(ignRing));
    if(!ringValid(ignRing, len, IGN_LOG_CAPACITY, IGN_RECORD_BYTES,
                  IGN_LOG_VERSION)) {
        ringInit(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, IGN_LOG_VERSION);
    }
}

// Date and time in the form a log line wants it.
String stampOf(uint32_t epoch) {
    if(epoch == 0) return "  --   --  ";
    time_t    t = (time_t)epoch;
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[16];
    strftime(buf, sizeof(buf), "%d.%m %H:%M", &lt);
    return String(buf);
}

static void recordIgnition(uint8_t outcome, uint16_t seconds) {
    IgnitionRecord r;
    r.epoch        = timeValid ? (uint32_t)time(nullptr) : 0;
    r.seconds      = seconds;
    r.voltBeforeDv = ignLogVoltBefore;
    r.voltMinDv    = ignLogVoltMin;
    r.ambientC     = ignLogAmbient;
    r.attempts     = ignAttempts;
    r.outcome      = outcome;

    uint8_t packed[IGN_RECORD_BYTES];
    if(!ignPack(r, packed, sizeof(packed))) return;
    ringPush(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, packed);
    prefs.putBytes("ignlog", ignRing, sizeof(ignRing));

    if(outcome != IGN_OUTCOME_LIT) { persistStats(); return; }

    // Freeze what a healthy start looks like on this burner, from the first
    // few after it was cleaned. The ring is far too short to still hold them
    // by the time the comparison matters.
    if(counters.baseIgnSamples < IGN_BASELINE_SAMPLES) {
        counters.baseIgnSamples++;
        countersDirty = true;
        if(counters.baseIgnSamples == IGN_BASELINE_SAMPLES) {
            IgnSummary base = ignSummarise(ignRing, IGN_BASELINE_SAMPLES);
            counters.baseIgnSeconds = base.medianSeconds;
            counters.baseIgnDropDv  = base.medianDropDv;
        }
        persistStats();
        return;
    }

    IgnSummary recent = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    bool bad = ignDegraded(recent, counters.baseIgnSeconds, counters.baseIgnDropDv);

    // One message when it starts drifting, not one per start. A single slow
    // ignition in a frost is weather, which is what the median is there for.
    if(bad != counters.ignWarned) {
        counters.ignWarned = bad;
        countersDirty      = true;
        notifyTelegram(bad
            ? "🟠 Starts are getting worse: median " +
              String(recent.medianSeconds) + " s and " +
              String(recent.medianDropDv / 10.0, 1) + " V of sag, against " +
              String(counters.baseIgnSeconds) + " s and " +
              String(counters.baseIgnDropDv / 10.0, 1) + " V when it was last "
              "serviced. Glow plug or a coked burner — /ign for the detail."
            : "🟢 Starts are back to what they were after the last service");
    }
    persistStats();
}

void updateIgnitionLog() {
    if(!ignLogActive) return;

    bool fresh = heaterStatus.lastUpdate != 0 &&
                 (millis() - heaterStatus.lastUpdate) < HEATER_DATA_FRESH_MS;

    // The plug pulls eight to ten amps while it tries, and how far that drags
    // the rail down is the measurement worth keeping.
    if(fresh && heaterStatus.voltage > 0 &&
       heaterStatus.voltage < ignLogVoltMin) {
        ignLogVoltMin = heaterStatus.voltage;
    }

    bool lit     = fresh && heaterStatus.state == STATE_RUNNING;
    bool gaveUp  = fresh && !ignWatching &&
                   heaterIsOffOrStopping(heaterStatus.state);
    bool timeout = millis() - ignLogStartMs > IGN_LOG_TIMEOUT_MS;

    if(!lit && !gaveUp && !timeout) return;

    recordIgnition(lit ? IGN_OUTCOME_LIT : IGN_OUTCOME_FAILED,
                   (uint16_t)((millis() - ignLogStartMs) / 1000));
    ignLogActive = false;
}

// The trend, and the last few starts it was drawn from.
String ignText() {
    String m = "🔌 Recent starts\n";

    uint8_t count = ringCount(ignRing);
    if(count == 0) return m + "Nothing logged yet";

    uint8_t show = (count < 5) ? count : 5;
    for(uint8_t i = 0; i < show; i++) {
        IgnitionRecord r;
        if(!ignUnpack(r, ringAt(ignRing, IGN_LOG_CAPACITY, IGN_RECORD_BYTES, i))) {
            continue;
        }
        m += stampOf(r.epoch);
        m += "  " + String(r.seconds) + " s";
        m += "  " + String(r.voltBeforeDv / 10.0, 1) + "->" +
                    String(r.voltMinDv / 10.0, 1) + " V";
        m += "  " + String(r.ambientC) + " C";
        if(r.outcome != IGN_OUTCOME_LIT) m += "  failed";
        else if(r.attempts > 1)          m += "  " + String(r.attempts) + " tries";
        m += "\n";
    }

    IgnSummary recent = ignSummarise(ignRing, IGN_RECENT_SAMPLES);
    if(recent.samples) {
        m += "\nMedian of " + String(recent.samples) + ":  " +
             String(recent.medianSeconds) + " s / " +
             String(recent.medianDropDv / 10.0, 1) + " V";
    }

    if(counters.baseIgnSamples >= IGN_BASELINE_SAMPLES) {
        m += "\nBaseline:     " + String(counters.baseIgnSeconds) + " s / " +
             String(counters.baseIgnDropDv / 10.0, 1) + " V";
        if(ignDegraded(recent, counters.baseIgnSeconds, counters.baseIgnDropDv)) {
            m += "  ⚠️ worse";
        }
    } else {
        m += "\nBaseline:     building, " + String(counters.baseIgnSamples) +
             " of " + String(IGN_BASELINE_SAMPLES) + " starts since the service";
    }
    return m;
}

// What has been worn out so far. The number of starts is the figure that
// matters for the glow plug -- it is worn by ignitions, not by hours.
String statsText() {
    String m = "📊 All time\n";
    m += "Running:  " + hoursText(counters.burnSec) + "\n";
    m += "Fuel:     ~" + mlText(counters.fuelMl) + "\n";
    m += "Starts:   " + String(counters.starts);
    if(counters.starts) {
        uint16_t rate = countersFailureRateTenths(counters);
        m += "  (" + String(counters.startsFailed) + " failed, " +
             String(rate / 10) + "." + String(rate % 10) + "%)";
    }
    m += "\n";

    if(counters.svcEpoch || counters.svcBurnSec) {
        m += "\nSince service " + dateOf(counters.svcEpoch) + "\n";
    } else {
        m += "\nNever serviced\n";
    }
    m += "Running:  " + hoursText(countersServiceBurnSec(counters)) + "\n";
    m += "Fuel:     ~" + mlText(countersServiceFuelMl(counters)) + "\n";
    m += "Starts:   " + String(countersServiceStarts(counters)) + "\n";

    if(serviceHours) {
        uint32_t due   = (uint32_t)serviceHours * 3600UL;
        uint32_t doneS = countersServiceBurnSec(counters);
        m += doneS >= due
            ? "\n🔧 Service is due"
            : "\nNext service in " + hoursText(due - doneS);
    }

    if(tankActive()) {
        m += "\nTank:     ~" + mlText(tankRemainingMl) + " of " +
             mlText(tankCapacityMl);
    }
    return m;
}

void loadCounters() {
    uint8_t blob[COUNTERS_BLOB_BYTES];
    size_t  len = prefs.getBytes("counters", blob, sizeof(blob));
    if(!countersUnpack(counters, blob, len)) countersReset(counters);
    bootDirtyPending = counters.wasBurning;
    countersFlushMs  = millis();
}

void persistStats() {
    if(!countersDirty) return;
    uint8_t blob[COUNTERS_BLOB_BYTES];
    size_t  n = countersPack(counters, blob, sizeof(blob));
    if(n) prefs.putBytes("counters", blob, n);
    prefs.putUInt("tankRemMl", tankRemainingMl);
    countersDirty   = false;
    countersFlushMs = millis();
}

void updateStats() {
    static uint8_t lastState = STATE_OFF;

    bool burning = heaterStatus.state != STATE_OFF;
    bool fresh   = heaterStatus.lastUpdate != 0 &&
                   (millis() - heaterStatus.lastUpdate) < HEATER_DATA_FRESH_MS;

    // Decide what last night's flag meant, but only once the heater has been
    // heard from -- before that its state is a guess, not a reading.
    if(bootDirtyPending && heaterStatus.lastUpdate != 0) {
        bootDirtyPending = false;
        if(!burning) {
            notifyTelegram("⚠️ Last session ended with the heater still lit — "
                           "the power went before the purge could finish");
        }
        counters.wasBurning = burning;
        countersDirty       = true;
    }

    if(burning && fresh) {
        counters.burnSec++;
        countersDirty = true;

        // burnSec only ever rises, and by one at a time, so the threshold is
        // crossed exactly once per service. That is the whole latch: no flag
        // to persist, and no reminder repeated every morning after the
        // overnight power cut.
        if(serviceHours) {
            uint32_t due  = (uint32_t)serviceHours * 3600UL;
            uint32_t sinceSvc = countersServiceBurnSec(counters);
            if(sinceSvc == due) {
                notifyTelegram("🔧 " + String(serviceHours) + " h of running since "
                               "the last service — time to look at the burner. "
                               "Reset the count with /service done");
            }
        }
    }

    // updateFuel() resets its accumulator at the start of each burn, so a
    // reading below what has been billed means a new burn, not a rollback.
    if(fuelTicks < fuelTicksBilled) fuelTicksBilled = 0;
    uint32_t ml = fuelMlFromTicks(fuelTicks - fuelTicksBilled);
    if(ml) {
        counters.fuelMl += ml;
        fuelTicksBilled += ml * FUEL_TICKS_PER_ML;
        countersDirty    = true;
        // Only while the estimate is running. Debiting a switched-off tank
        // would leave a figure that silently drifted out of date, and looked
        // authoritative the day it was switched back on.
        if(tankActive()) tankDebit(tankRemainingMl, ml);
    }

    // Warning on a rise, and only on a rise, gives one message per crossing.
    // A refill lowers the band and rearms the warning by itself.
    uint8_t warn = tankActive()
                 ? tankWarnLevel(tankRemainingMl, tankCapacityMl) : 0;
    if(warn > tankWarnLast) {
        notifyTelegram(warn >= 2
            ? "🔴 Tank nearly empty: ~" + mlText(tankRemainingMl) + " left"
            : "🟡 Tank running low: ~" + mlText(tankRemainingMl) + " left");
    }
    tankWarnLast = warn;

    if(burning != (lastState != STATE_OFF)) {
        counters.wasBurning = burning;
        countersDirty       = true;
        // The end of a burn is the moment worth committing: the next thing to
        // happen may well be the power going out.
        if(!burning) persistStats();
    }
    lastState = heaterStatus.state;

    if(burning && millis() - countersFlushMs >= STATS_FLUSH_MS) persistStats();
}

// Sends progress while the heater burns, and turns the same message into a
// warning when the garage is not actually warming up.
void updateProgressReport() {
    static uint8_t  lastState    = STATE_OFF;
    static uint32_t burnStartMs  = 0;
    static int8_t   ambientStart = 0;
    static uint16_t reportsSent  = 0;

    if(heaterStatus.state == STATE_OFF) {
        lastState   = STATE_OFF;
        reportsSent = 0;
        return;
    }
    if(reportFirstMin == 0) return;

    if(lastState == STATE_OFF) {          // this burn just began
        burnStartMs  = millis();
        ambientStart = heaterStatus.ambientTemp;
        reportsSent  = 0;
    }
    lastState = heaterStatus.state;

    if(heaterStatus.lastUpdate == 0) return;

    if(reportsSent > 0 && reportRptMin == 0) return;   // first report only

    uint32_t dueMin  = reportFirstMin + (uint32_t)reportsSent * reportRptMin;
    uint32_t burnMin = (millis() - burnStartMs) / 60000UL;
    if(burnMin < dueMin) return;

    reportsSent++;
    int delta = heaterStatus.ambientTemp - ambientStart;

    if(delta <= 0) {
        notifyTelegram("⚠️ Running " + String(burnMin) + " min, garage still at " +
                       String(heaterStatus.ambientTemp) + " C — not heating");
        return;
    }

    String m = "🔥 Running " + String(burnMin) + " min\n";
    m += "Garage: " + String(ambientStart) + " -> " +
         String(heaterStatus.ambientTemp) + " C  (+" + String(delta) + ")\n";
    m += "Case:   " + String(heaterStatus.caseTemp) + " C";
    if(fuelTicks) m += "\nFuel:   ~" + String(fuelMlFromTicks(fuelTicks)) + " ml";
    notifyTelegram(m);
}

void updateIgnition() {
    if(!ignWatching) return;

    // Nothing can be judged, retried or reported through a dead radio.
    if(cc1101Fault) { ignWatching = false; return; }

    IgnitionInput in;
    in.watching      = ignWatching;
    in.attempts      = ignAttempts;
    in.nowMs         = millis();
    in.commandedAtMs = ignCommandedAtMs;
    in.heaterState   = heaterStatus.state;
    in.dataFresh     = heaterStatus.lastUpdate != 0 &&
                       (millis() - heaterStatus.lastUpdate) < HEATER_DATA_FRESH_MS;
    in.timeoutMs     = IGNITION_TIMEOUT_MS;
    in.maxAttempts   = IGNITION_MAX_TRIES;

    switch(checkIgnition(in)) {
        case IGN_CONFIRMED:
            // Silent on success: the state change is announced anyway.
            ignWatching = false;
            break;

        case IGN_RETRY:
            ignAttempts++;
            ignCommandedAtMs = millis();
            sendCommand(CMD_POWER);
            notifyTelegram("🟡 Heater did not light, trying once more");
            break;

        case IGN_FAILED:
            ignWatching = false;
            counters.startsFailed++;
            countersDirty = true;
            notifyTelegram("🔴 Heater failed to start — no acknowledgement "
                           "after " + String(ignAttempts) + " attempts");
            break;

        case IGN_NONE:
        default:
            break;
    }
}

// ── Power level ────────────────────────────────────────────────────────────
//
// Setting a level used to mean pressing +1 six times and waiting out a poll
// after each. The walk itself lives in stepper.h; what is walked depends on
// the heater, and the V2 frame carries no level number at all.

// Where a level change has got to. The V2 walk is two-phase: down to the
// bottom of the ladder, then up to the rung asked for.
enum LevelPhase { LVL_IDLE = 0, LVL_SEEK, LVL_FLOOR, LVL_CLIMB };

static StepperState levelStepper;
static LevelPhase   levelPhase  = LVL_IDLE;
static int          levelWanted = 0;
static int64_t      levelChat   = 0;   // who asked, and who gets the verdict

// The number the heater reports and that UP/DOWN move by one.
static int levelReading() {
    if(heaterStatus.autoMode) return heaterStatus.setpoint;
    if(heaterVersion == "V1") return heaterStatus.power;
    return heaterStatus.pumpFreq;      // V2 manual: the pump rate is all there is
}

// The same number in the units the user thinks in.
static String levelCurrentText() {
    if(heaterStatus.autoMode) return String(heaterStatus.setpoint) + " C";
    if(heaterVersion == "V1") return "level " + String(heaterStatus.power);
    return String(heaterStatus.pumpFreq / 10.0, 1) + " Hz";
}

static void levelReply(const String& text) {
    if(levelChat != 0 && tgEnabled && tgReady) tgBot.sendTo(levelChat, text);
    else                                       notifyTelegram(text);
}

static void levelAbandon(const String& why) {
    stepperStop(levelStepper);
    levelPhase = LVL_IDLE;
    levelReply(why);
}

// Starts a walk, or explains why it cannot. The reply to the command is
// immediate; the verdict arrives when the walk finishes, seconds later.
bool startLevelChange(int64_t chatId, const String& cmd, String& reply) {
    bool wantTemp = cmd.startsWith("/temp");
    int  space    = cmd.indexOf(' ');
    int  value    = (space > 0) ? cmd.substring(space + 1).toInt() : 0;

    if(!heaterPaired) { reply = "Heater is not paired."; return false; }
    if(cc1101Fault)   { reply = "RF module is not responding."; return false; }

    // Every step is judged against the reported reading, and a stopped heater
    // does not report one worth steering by.
    if(heaterStatus.state == STATE_OFF) {
        reply = "Heater is off — start it first. The level is walked against "
                "the heater's own reading, and a stopped heater has none.";
        return false;
    }
    if(levelPhase != LVL_IDLE) {
        reply = "A level change is already under way";
        return false;
    }
    if(wantTemp != heaterStatus.autoMode) {
        reply = heaterStatus.autoMode
            ? "The heater is in AUTO — /temp sets the target temperature"
            : "The heater is in MANUAL — /level sets the power level";
        return false;
    }

    if(wantTemp) {
        if(value < TEMP_MIN_C || value > TEMP_MAX_C) {
            reply = "Use /temp " + String(TEMP_MIN_C) + ".." + String(TEMP_MAX_C);
            return false;
        }
        levelPhase = LVL_SEEK;
        stepperStart(levelStepper, STEP_SEEK, value, 0, millis());
        reply = "Walking the setpoint to " + String(value) + " C from " +
                levelCurrentText();
    } else {
        if(value < 1 || value > LEVEL_MAX) {
            reply = "Use /level 1.." + String(LEVEL_MAX);
            return false;
        }
        levelWanted = value;
        if(heaterVersion == "V1") {
            // V1 reports the level outright, so it can be steered for.
            levelPhase = LVL_SEEK;
            stepperStart(levelStepper, STEP_SEEK, value, 0, millis());
        } else {
            // V2 does not, so the ladder is walked from its bottom instead.
            // Slower, but it needs no table of pump rates and cannot be wrong
            // about a heater whose ladder is not the one we guessed.
            levelPhase = LVL_FLOOR;
            stepperStart(levelStepper, STEP_FLOOR, 0, -1, millis());
        }
        reply = "Walking to level " + String(value) + " from " + levelCurrentText();
    }

    levelChat = chatId;
    return true;
}

void updateLevel() {
    if(levelPhase == LVL_IDLE) return;

    if(cc1101Fault) {
        levelAbandon("Level change abandoned — the RF module stopped responding");
        return;
    }
    // A heater that shut down mid-walk is no longer at a level worth setting,
    // and every further step would be a toggle sent into the purge.
    if(heaterStatus.state == STATE_OFF) {
        levelAbandon("Level change abandoned — the heater stopped");
        return;
    }

    StepperInput in;
    in.reading    = levelReading();
    in.dataFresh  = heaterStatus.lastUpdate != 0 &&
                    (millis() - heaterStatus.lastUpdate) < HEATER_DATA_FRESH_MS;
    in.nowMs      = millis();
    in.settleMs   = STEP_SETTLE_MS;
    in.deadlineMs = STEP_DEADLINE_MS;
    in.maxSteps   = STEP_MAX_STEPS;

    switch(stepperNext(levelStepper, in)) {
        case STEPPER_UP:   sendCommand(CMD_UP);   break;
        case STEPPER_DOWN: sendCommand(CMD_DOWN); break;

        case STEPPER_DONE:
            // Reaching the bottom is only half the V2 walk: the climb from it
            // is what actually selects the level.
            if(levelPhase == LVL_FLOOR) {
                levelPhase = LVL_CLIMB;
                stepperStart(levelStepper, STEP_COUNT, levelWanted - 1, +1, millis());
            } else {
                levelPhase = LVL_IDLE;
                levelReply("✅ Now at " + levelCurrentText());
            }
            break;

        case STEPPER_STUCK:
            levelPhase = LVL_IDLE;
            levelReply("Stopped at " + levelCurrentText() +
                       " — the heater would not step any further");
            break;

        case STEPPER_WRAPPED:
            levelPhase = LVL_IDLE;
            levelReply("The level rolls over at the end of its range instead of "
                       "stopping, so it cannot be walked to a fixed value. "
                       "Now at " + levelCurrentText() + " — use the ± buttons.");
            break;

        case STEPPER_EXHAUSTED:
            levelPhase = LVL_IDLE;
            levelReply("Gave up after " + String(STEP_MAX_STEPS) + " steps at " +
                       levelCurrentText());
            break;

        case STEPPER_WAIT:
        default:
            break;
    }
}

// ── Scheduled start ────────────────────────────────────────────────────────

static void cancelScheduledStart() {
    startArmed  = false;
    startTarget = 0;
    prefs.putBool("startArmed", false);
}

static void armScheduledStart(uint32_t epoch) {
    startArmed  = true;
    startTarget = epoch;
    prefs.putBool("startArmed", true);
    prefs.putULong("startAt", epoch);
}

static String clockOf(uint32_t epoch) {
    time_t t = (time_t)epoch;
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    return String(buf);
}

void cancelScheduledStartPublic() { cancelScheduledStart(); }
String clockOfPublic(uint32_t epoch) { return clockOf(epoch); }

// Turns a wall-clock request into an absolute target, taking the next
// occurrence of that time. Refuses anything that could not work rather than
// accepting it and behaving oddly later.
bool scheduleStartAt(int hour, int minute, String& reply) {
    if(!timeValid) {
        reply = "Cannot schedule: the clock is not synced yet";
        return false;
    }
    if(hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        reply = "Use /at HH:MM";
        return false;
    }
    if(!heaterPaired) {
        reply = "Cannot schedule: no heater is paired";
        return false;
    }

    int wanted = hour * 60 + minute;
    if(blackoutEnabled &&
       startCollidesWithShutdown(wanted, blackoutMinutes, shutdownLeadMin)) {
        reply = "That lands in the window before the power cut — it would be "
                "lit and stopped straight away";
        return false;
    }

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = hour; lt.tm_min = minute; lt.tm_sec = 0;
    time_t target = mktime(&lt);
    if(target <= now) target += 24 * 3600;   // the next occurrence

    armScheduledStart((uint32_t)target);
    long inMin = (long)(target - now) / 60;
    reply = "Scheduled for " + clockOf((uint32_t)target) + " — in " +
            String(inMin / 60) + " h " + String(inMin % 60) + " min";
    return true;
}

bool scheduleStartIn(int minutes, String& reply) {
    if(minutes < 1 || minutes > 24 * 60) {
        reply = "Use /in 2h or /in 90";
        return false;
    }
    if(!timeValid) {
        reply = "Cannot schedule: the clock is not synced yet";
        return false;
    }
    time_t target = time(nullptr) + (time_t)minutes * 60;
    struct tm lt;
    localtime_r(&target, &lt);
    return scheduleStartAt(lt.tm_hour, lt.tm_min, reply);
}

void updateScheduledStart() {
    StartInput in;
    in.armed        = startArmed;
    in.targetEpoch  = startTarget;
    in.nowEpoch     = timeValid ? (uint32_t)time(nullptr) : 0;
    in.timeValid    = timeValid;
    in.heaterState  = heaterStatus.state;
    in.heaterPaired = heaterPaired;
    in.graceMin     = START_GRACE_MIN;

    switch(decideStart(in)) {
        case START_FIRE: {
            cancelScheduledStart();
            StartVerdict v = heaterEnsureOn();
            notifyTelegram(v == START_ALLOWED
                ? "⏰ Scheduled start — igniting" + fuelWarningForRun()
                : "⚠️ Scheduled start refused: " + startRefusalText(v));
            break;
        }

        case START_MISSED:
            cancelScheduledStart();
            notifyTelegram("⚠️ Missed the start scheduled for " +
                           clockOf(in.targetEpoch) + " — no power at the time");
            break;

        case START_SKIP_RUNNING:
            cancelScheduledStart();
            notifyTelegram("⏰ Scheduled start skipped — already running");
            break;

        case START_NONE:
        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SHUTDOWN SCHEDULER
// ═══════════════════════════════════════════════════════════════════════════

// Thin wrapper around decideShutdown(): gathers state, executes the verdict.
// All the reasoning lives in scheduler.cpp so it can be tested on the host.
void updateScheduler() {
    static bool     shutdownRequested  = false;
    static uint32_t shutdownAtMs       = 0;
    static uint32_t heaterOnSinceMs    = 0;
    static bool     heaterOnSinceValid = false;
    static bool     warnedNoTime       = false;

    if(heaterStatus.state == STATE_OFF) {
        heaterOnSinceValid = false;
        sessionAutoOffMin  = 0;   // the override lasts one burn only
    } else if(!heaterOnSinceValid) {
        // Either the heater just started, or the controller rebooted while it
        // was already running. In the second case the runtime limit counts
        // from now -- conservative, but better than never firing at all.
        heaterOnSinceMs    = millis();
        heaterOnSinceValid = true;
    }

    SchedulerInput in;
    in.heaterState         = heaterStatus.state;
    in.heaterPaired        = heaterPaired;
    in.nowMs               = millis();
    in.heaterOnSinceMs     = heaterOnSinceMs;
    in.heaterOnSinceValid  = heaterOnSinceValid;
    in.timeValid           = timeValid;
    in.nowMinutes          = timeValid ? currentMinutesOfDay() : -1;
    in.shutdownRequested   = shutdownRequested;
    in.shutdownAtMs        = shutdownAtMs;
    in.autoOffMin          = sessionAutoOffMin ? sessionAutoOffMin : autoOffMin;
    in.blackoutEnabled     = blackoutEnabled;
    in.blackoutMinutes     = blackoutMinutes;
    in.shutdownLeadMin     = shutdownLeadMin;
    in.cooldownExpectedMin = cooldownExpectedMin;

    SchedulerDecision d = decideShutdown(in);

    switch(d.action) {
        case SCHED_SHUT_DOWN:
            if(heaterEnsureOff()) {
                shutdownRequested = true;
                shutdownAtMs      = millis();
                // Before a blackout shutdown this is the last chance to write
                // anything down -- the purge may outlast the mains.
                persistStats();
                notifyTelegram(d.reason == REASON_BLACKOUT
                    ? "⏱ Shutting down ahead of the mains cut"
                    : "⏱ Runtime limit reached, shutting down");
            }
            break;

        case SCHED_COOLDOWN_DONE:
            shutdownRequested = false;
            notifyTelegram("✅ Purge finished, heater is off");
            break;

        case SCHED_COOLDOWN_TIMEOUT:
            shutdownRequested = false;
            notifyTelegram("⚠️ Heater did not reach OFF within the purge window");
            break;

        case SCHED_NONE:
        default:
            break;
    }

    // The deadline silently does nothing without a synced clock, so say so.
    // This is the realistic morning case: mains returns before the modem does.
    if(blackoutEnabled && !timeValid) {
        if(!warnedNoTime) {
            warnedNoTime = true;
            Serial.println("⚠️ Blackout deadline inactive: clock not synced");
        }
    } else {
        warnedNoTime = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WIFI SUPERVISION
// ═══════════════════════════════════════════════════════════════════════════

// Keeps the station connection alive.
//
// Previously the link was only established in setup(), and loop() never
// looked at WiFi.status() again. Recovery relied entirely on the driver's
// built-in retry, which has no backoff and no reporting. Since Telegram is
// the only remote control channel, a link that fails to come back means the
// device is unreachable until someone drives out to the garage.
void superviseWiFi() {
    if(staSSID.length() == 0) return;            // nothing to reconnect to

    // Fell back to the access point because the network was not there at boot.
    // It may well turn up later -- a modem that takes a minute to register is
    // the normal case here, not the exception -- so keep asking rather than
    // waiting for somebody to drive out and power-cycle the board.
    //
    // The access point stays up while trying. The ESP32 can hold both at once,
    // and shutting the owner out while hunting for a network that may not even
    // be configured correctly would trade one lockout for another.
    if(useAP) {
        if(WiFi.status() == WL_CONNECTED) {
            useAP = false;
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            displayLine2 = "IP:";
            displayLine3 = WiFi.localIP().toString();
            Serial.println("✅ WiFi joined late: " + WiFi.localIP().toString());
            notifyTelegram("📶 Joined " + staSSID + " — " +
                           WiFi.localIP().toString());
            return;
        }

        static unsigned long lastApRetry = 0;
        if(millis() - lastApRetry < WIFI_AP_RETRY_MS) return;
        lastApRetry = millis();

        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(staSSID.c_str(), staPassword.c_str());
        return;
    }

    static unsigned long lastAttempt = 0;
    static unsigned long backoffMs   = WIFI_RETRY_MIN_MS;
    static bool wasConnected = true;

    if(WiFi.status() == WL_CONNECTED) {
        if(!wasConnected) {
            Serial.println("✅ WiFi reconnected: " + WiFi.localIP().toString());
            wasConnected = true;
        }
        backoffMs = WIFI_RETRY_MIN_MS;
        return;
    }

    if(wasConnected) {
        Serial.println("⚠️ WiFi lost, will retry");
        wasConnected = false;
        lastAttempt  = millis();   // give the driver its own chance first
        return;
    }

    if(millis() - lastAttempt < backoffMs) return;
    lastAttempt = millis();

    Serial.printf("WiFi reconnect attempt, next in %lu s\n", backoffMs / 1000);
    WiFi.disconnect();
    WiFi.begin(staSSID.c_str(), staPassword.c_str());

    backoffMs *= 2;
    if(backoffMs > WIFI_RETRY_MAX_MS) backoffMs = WIFI_RETRY_MAX_MS;
}

// ═══════════════════════════════════════════════════════════════════════════
// OTA FUNCTIONS  (ArduinoOTA / espota — local network firmware updates)
// ═══════════════════════════════════════════════════════════════════════════

// Initialize OTA subsystem (called from setup after WiFi, and on enable)
// ═══════════════════════════════════════════════════════════════════════════
// OTA ROLLBACK
// ═══════════════════════════════════════════════════════════════════════════
//
// The bootloader can put the previous firmware back when a new one fails to
// come up, and the support is compiled in -- but the Arduino core defeats it.
// initArduino() runs before setup(), and unless verifyRollbackLater() says
// otherwise it calls esp_ota_mark_app_valid_cancel_rollback() straight away,
// declaring any image that reached main() a good one. An image that boots and
// then cannot reach the network is exactly the one worth rolling back, and it
// passes that test with room to spare.
//
// Overriding the weak symbol defers the verdict to code that can actually
// judge it. C linkage: the core declares it in a .c file.
extern "C" bool verifyRollbackLater() { return true; }

// True while this image is on probation.
static bool     otaPendingVerify   = false;
static uint32_t otaBotDeadline     = 0;
static uint32_t otaVerifyDeadline  = 0;

void setupOtaVerify() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if(esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if(state != ESP_OTA_IMG_PENDING_VERIFY) return;

    otaPendingVerify  = true;
    otaBotDeadline    = millis() + OTA_VERIFY_BOT_WAIT_MS;
    otaVerifyDeadline = millis() + OTA_VERIFY_WINDOW_MS;
    Serial.println("OTA: new image on probation, must prove it is reachable");
}

// The test is reachability, because that is the whole point: firmware that
// runs but cannot be talked to is indistinguishable from a brick at a hundred
// kilometres. A device with no station network configured, or with no bot, is
// judged on what it does have -- otherwise it could never accept an update at
// all.
void updateOtaVerify() {
    if(!otaPendingVerify) return;

    bool networkUp = (staSSID.length() == 0) || (WiFi.status() == WL_CONNECTED);
    bool botUp     = !tgEnabled || tgReady;
    bool botGaveUp = (int32_t)(millis() - otaBotDeadline) >= 0;

    // Both channels answering is the clean case. Failing that, the network
    // alone is enough once the bot has had its three minutes: an image that
    // is on the network can be flashed again, which is what recovery needs.
    if(networkUp && (botUp || botGaveUp)) {
        esp_ota_mark_app_valid_cancel_rollback();
        otaPendingVerify = false;
        Serial.println(botUp
            ? "OTA: image confirmed, rollback cancelled"
            : "OTA: image confirmed on the network alone, the bot never answered");
        notifyTelegram(botUp
            ? "✅ New firmware confirmed — it came back and can be reached"
            : "⚠️ New firmware kept, but the bot did not answer within three "
              "minutes of the update. Worth a look.");
        return;
    }

    if((int32_t)(millis() - otaVerifyDeadline) < 0) return;

    // Reboot without confirming. The bootloader finds an image that never
    // vouched for itself and starts the previous one instead.
    Serial.println("OTA: never reached the network — rebooting to roll back");
    delay(100);
    ESP.restart();
}

void setupOTA() {
    if(!otaEnabled || otaRunning) return;

    ArduinoOTA.setHostname(deviceName.c_str());
    if(otaPassword.length() > 0) ArduinoOTA.setPassword(otaPassword.c_str());

    // ArduinoOTA.handle() does not return until the whole image has arrived,
    // so loop() -- and with it feedWatchdog() -- stops running for the length
    // of the transfer. A firmware image takes about a minute, the watchdog
    // fires at sixty seconds, and the board was being reset just as the last
    // block landed: the upload reported 100% and then timed out waiting for a
    // device that had already rebooted. The filesystem image is a tenth of
    // the size and slipped under the limit, which is what made this look like
    // a firmware-only problem.
    //
    // These callbacks are the only code that runs during the transfer, so the
    // watchdog gets fed from here.
    ArduinoOTA.onStart([]() {
        feedWatchdog();
        displayLine1 = "OTA UPDATE";
        displayLine2 = "Starting...";
        displayLine3 = "";
        displayLine4 = "";
        updateDisplay();
        Serial.println("OTA: Update started");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        feedWatchdog();

        // Redrawing costs a full framebuffer over I2C, some 25 ms, and this
        // fires once per block -- over a thousand times for a firmware image.
        // That was most of the transfer time. Once per percent is plenty for
        // something a human is watching.
        static int lastPct = -1;
        int pct = total ? (progress * 100) / total : 0;
        if(pct == lastPct) return;
        lastPct = pct;

        displayLine2 = "Flashing " + String(pct) + "%";
        updateDisplay();
    });
    ArduinoOTA.onEnd([]() {
        feedWatchdog();
        displayLine2 = "Done!";
        displayLine3 = "Rebooting...";
        updateDisplay();
        Serial.println("\nOTA: Update complete");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        feedWatchdog();
        displayLine2 = "OTA FAILED!";
        displayLine3 = "Err " + String(error);
        updateDisplay();
        Serial.printf("OTA: Error[%u]\n", error);
    });

    ArduinoOTA.begin();
    otaRunning = true;
    Serial.println("OTA: Enabled — hostname=" + deviceName +
                   ", password=" + String(otaPassword.length() > 0 ? "[set]" : "[none]"));
}

// Called from loop() — services OTA requests
void loopOTA() {
    if(otaEnabled && otaRunning) ArduinoOTA.handle();
}

// ═══════════════════════════════════════════════════════════════════════════
// MQTT FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for(int i = 0; i < length; i++) message += (char)payload[i];
    String topicStr = String(topic);
    if(topicStr == mqttTopic + "/cmd/power")      sendCommand(CMD_POWER);
    else if(topicStr == mqttTopic + "/cmd/up")    sendCommand(CMD_UP);
    else if(topicStr == mqttTopic + "/cmd/down")  sendCommand(CMD_DOWN);
    else if(topicStr == mqttTopic + "/cmd/mode")  sendCommand(CMD_MODE);
}

void connectMQTT() {
    if(!mqttEnabled || mqttServer.length() == 0) return;
    if(mqtt.connected()) { mqtt.disconnect(); delay(100); }
    mqtt.setServer(mqttServer.c_str(), mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(512);
    for(int attempt = 1; attempt <= 3; attempt++) {
        bool ok = mqttAuthEnabled ?
            mqtt.connect(deviceName.c_str(), mqttUser.c_str(), mqttPassword.c_str()) :
            mqtt.connect(deviceName.c_str());
        if(ok) { mqtt.subscribe((mqttTopic + "/cmd/#").c_str()); return; }
        if(attempt < 3) delay(2000);
    }
}

void publishMQTT() {
    if(!mqtt.connected()) return;
    mqtt.publish((mqttTopic + "/state").c_str(),   getStateName(heaterStatus.state));
    mqtt.publish((mqttTopic + "/voltage").c_str(), String(heaterStatus.voltage / 10.0, 1).c_str());
    mqtt.publish((mqttTopic + "/ambient").c_str(), String(heaterStatus.ambientTemp).c_str());
    mqtt.publish((mqttTopic + "/case").c_str(),    String(heaterStatus.caseTemp).c_str());
    mqtt.publish((mqttTopic + "/setpoint").c_str(),String(heaterStatus.setpoint).c_str());
    mqtt.publish((mqttTopic + "/pump").c_str(),    String(heaterStatus.pumpFreq / 10.0, 1).c_str());
    mqtt.publish((mqttTopic + "/mode").c_str(),    heaterStatus.autoMode ? "AUTO" : "MANUAL");
    mqtt.publish((mqttTopic + "/rssi").c_str(),    String(heaterStatus.rssi).c_str());
    mqtt.publish((mqttTopic + "/error").c_str(),   getErrorName(heaterStatus.errorCode));
}

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

// Finds the panel before handing it to U8g2.
//
// display.begin() blocks for ever on a bus nobody answers, and an unpowered
// module holds the lines down through its protection diodes rather than
// letting go of them. This runs before the watchdog is armed, so a display
// that died took the whole controller with it -- and a heater is attached to
// that controller. The same hole was closed for the CC1101 in d73f1d4; the
// display kept it.
//
// Probing both addresses also settles which one this panel uses, instead of
// leaving it as a thing to discover by seeing a blank screen.
void setupDisplay() {
#if USE_OLED
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setTimeOut(50);

    const uint8_t candidates[] = { 0x3C, 0x3D };
    for(uint8_t i = 0; i < sizeof(candidates); i++) {
        Wire.beginTransmission(candidates[i]);
        if(Wire.endTransmission() != 0) continue;

        display.setI2CAddress(candidates[i] << 1);   // U8g2 wants it shifted
        display.begin();
        display.setContrast(255);
        displayPresent = true;
        Serial.printf("✅ OLED at 0x%02X\n", candidates[i]);
        return;
    }
    Serial.println("⚠️ No OLED answered on I2C — carrying on without one");
#endif
}

void updateDisplay() {
#if USE_OLED
    if(!displayPresent) return;
    display.clearBuffer();
    display.drawFrame(0, 0, 128, 12);
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(4, 9, displayLine1.c_str());
    display.setFont(u8g2_font_7x13_tf);
    display.drawStr(2, 26, displayLine2.c_str());
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(2, 40, displayLine3.c_str());
    display.setFont(u8g2_font_5x8_tf);
    display.drawStr(2, 52, displayLine4.c_str());

    if(heaterPaired && heaterStatus.lastUpdate > 0) {
        String footer = useAP ? "AP " : "WiFi ";
        if(mqttEnabled && mqtt.connected())   footer += "| MQTT ";
        if(otaEnabled)                        footer += "| OTA";
        display.drawStr(2, 63, footer.c_str());
    }
    display.sendBuffer();
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
// WEB SERVER HANDLERS
// ═══════════════════════════════════════════════════════════════════════════

void handleRoot() {
    File file = LittleFS.open("/index.html", "r");
    if(!file) { server.send(404, "text/plain", "File not found!"); return; }
    server.streamFile(file, "text/html");
    file.close();
}

// ═══════════════════════════════════════════════════════════════════════════
// SETTINGS FORM PARSING
// ═══════════════════════════════════════════════════════════════════════════
//
// Wrappers around server.arg(). All logic lives in settings.cpp so it can be
// covered by tests: this is exactly where the bug that erased the Wi-Fi
// credentials used to live.

// A cross-origin page cannot set a custom header without a CORS preflight,
// which this server never answers. That alone stops a malicious site from
// reaching the device through the browser of someone sitting on the same
// network -- the realistic attack, now that the admin holds a bot token that
// grants remote control of the heater.
static bool csrfOk() {
    if(server.header("X-DieselPilot") == "1") return true;
    server.send(403, "text/plain", "Missing X-DieselPilot header");
    return false;
}

static String formField(const char* name, const String& current) {
    return resolveField(server.hasArg(name), server.arg(name), current);
}

static bool formFlag(const char* name, bool current) {
    return resolveFlag(server.hasArg(name), server.arg(name), current);
}

static long formNumber(const char* name, long current, long minValue, long maxValue) {
    return resolveNumber(server.hasArg(name), server.arg(name), current, minValue, maxValue);
}

void handleAPI_Status() {
    String json = "{";
    json.reserve(320);
    json += "\"state\":\"" + String(getStateName(heaterStatus.state)) + "\",";
    json += "\"voltage\":" + String(heaterStatus.voltage / 10.0, 1) + ",";
    json += "\"ambient\":" + String(heaterStatus.ambientTemp) + ",";
    json += "\"case\":" + String(heaterStatus.caseTemp) + ",";
    json += "\"setpoint\":" + String(heaterStatus.setpoint) + ",";
    json += "\"pump\":" + String(heaterStatus.pumpFreq / 10.0, 1) + ",";
    json += "\"mode\":\"" + String(heaterStatus.autoMode ? "AUTO" : "MANUAL") + "\",";
    json += "\"rssi\":" + String(heaterStatus.rssi) + ",";
    json += "\"addr\":\"" + (heaterPaired ? String(heaterAddress, HEX) : "Not paired") + "\",";
    json += "\"version\":\"" + jsonEscape(heaterVersion) + "\",";
    json += "\"errorCode\":" + String(heaterStatus.errorCode) + ",";
    json += "\"errorName\":\"" + String(getErrorName(heaterStatus.errorCode)) + "\",";
    // The device answering says nothing about the heater still being heard:
    // with a dead CC1101 these readings can be hours old and look live.
    // Cast before the ternary: the other branch is unsigned, and -1 would be
    // converted to 4294967295 instead of staying the "never heard" marker.
    // A silent radio and a heater that has not answered look identical from
    // here -- both give no readings -- but only one of them is the
    // controller's own fault, and only one is fixed by touching the wiring.
    json += "\"rfFault\":" + String(cc1101Fault ? "true" : "false") + ",";
    json += "\"paired\":" + String(heaterPaired ? "true" : "false") + ",";
    json += "\"fuelMl\":" + String(fuelMlFromTicks(fuelTicks)) + ",";
    json += "\"tankEn\":" + String(tankActive() ? "true" : "false") + ",";
    json += "\"tankLeft\":" + String(tankRemainingMl) + ",";
    json += "\"tankCap\":" + String(tankCapacityMl) + ",";
    json += "\"ageSec\":" + String(heaterStatus.lastUpdate
                ? (long)((millis() - heaterStatus.lastUpdate) / 1000) : -1L);
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_OTAStatus() {
    // Returns OTA state for GUI
    String json = "{";
    json.reserve(320);
    json += "\"enabled\":" + String(otaEnabled ? "true" : "false") + ",";
    json += "\"running\":" + String(otaRunning ? "true" : "false") + ",";
    json += "\"hasPassword\":" + String(otaPassword.length() > 0 ? "true" : "false") + ",";
    json += "\"hostname\":\"" + jsonEscape(deviceName) + "\",";
    json += "\"ip\":\"" + (useAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_OTAConfig() {
    if(!csrfOk()) return;
    // Toggle OTA ON/OFF and (optionally) set the flashing password.
    otaEnabled  = formFlag("enabled", otaEnabled);
    otaPassword = formField("password", otaPassword);

    prefs.putBool("otaEnabled", otaEnabled);
    prefs.putString("otaPass", otaPassword);

    // Reboot so ArduinoOTA starts/stops cleanly with the new settings.
    server.send(200, "text/plain", "OTA saved! Rebooting...");
    delay(1000); ESP.restart();
}

// The GUI used to send a bare CMD_POWER from here, bypassing the wrappers the
// chat goes through: no on/off distinction, no ignition watch, and no idea
// that the mains were about to be cut. Both paths now answer the same rules,
// and a refusal comes back as text the GUI can show.
void handleAPI_Command() {
    if(!csrfOk()) return;
    String cmd = server.arg("c");

    // "power" is kept for anything still sending the old toggle. It resolves
    // to an explicit start or stop from the reported state.
    if(cmd == "power") {
        cmd = heaterIsOffOrStopping(heaterStatus.state) ? "on" : "off";
    }

    if(cmd == "on") {
        StartVerdict v = heaterEnsureOn();
        if(v != START_ALLOWED) {
            server.send(409, "text/plain", startRefusalText(v));
            return;
        }
        server.send(200, "text/plain", "Ignition requested");
        return;
    }

    if(cmd == "off") {
        server.send(200, "text/plain", heaterEnsureOff()
            ? "Shutdown requested, the purge will follow"
            : "Heater is already off or stopping");
        return;
    }

    if(!heaterPaired) {
        server.send(409, "text/plain", "Heater is not paired.");
        return;
    }

    if(cmd == "up")         sendCommand(CMD_UP);
    else if(cmd == "down")  sendCommand(CMD_DOWN);
    else if(cmd == "mode")  sendCommand(CMD_MODE);
    else {
        server.send(400, "text/plain", "Unknown command");
        return;
    }
    server.send(200, "text/plain", "OK");
}

void handleAPI_PairAuto() {
    if(!csrfOk()) return;
    String ver = server.arg("version");
    String customFreqStr = server.arg("customFreq");
    if(ver.length() == 0) ver = "V2";

    uint32_t newCustomFreq = 0;
    if(customFreqStr.length() > 0) {
        long f = customFreqStr.toInt();
        if(f < (long)FREQ_MIN_HZ || f > (long)FREQ_MAX_HZ) {
            server.send(400, "text/plain",
                        "Frequency must be given in hertz, 300-928 MHz");
            return;
        }
        newCustomFreq = (uint32_t)f;
    }

    if(ver != heaterVersion || newCustomFreq != customFrequency) {
        heaterVersion = ver; customFrequency = newCustomFreq;
        prefs.putString("heaterVer", heaterVersion);
        prefs.putULong("customFreq", customFrequency);
        cc1101_applyConfig();
    }

    if(heaterVersion == "V1") {
        myAddrV1[0] = random(0x01, 0xFE); myAddrV1[1] = random(0x01, 0xFE); myAddrV1[2] = random(0x01, 0xFE);
        for(int i = 0; i < 20; i++) { sendFrame_V1(0x21, 0x00, 0x00, 0x00); delay(80); }
        prefs.putBytes("addrV1", myAddrV1, 3);
        heaterPaired = true; heaterAddress = 1; prefs.putUInt("heaterAddr", 1);
        char addrStr[16]; sprintf(addrStr, "%02X%02X%02X", myAddrV1[0], myAddrV1[1], myAddrV1[2]);
        server.send(200, "text/plain", "V1 Paired! ID: " + String(addrStr));
    } else {
        pairState      = PAIR_SEARCHING;
        pairDeadlineMs = millis() + PAIR_WINDOW_MS;
        displayLine2   = "Pairing...";
        server.send(200, "text/plain",
                    "Listening for 60 s — press and hold the pairing button");
    }
}

void handleAPI_PairManual() {
    if(!csrfOk()) return;
    String addrStr = server.arg("addr");
    String ver = server.arg("version");
    String customFreqStr = server.arg("customFreq");
    if(ver.length() == 0) ver = "V2";

    uint32_t newCustomFreq = 0;
    if(customFreqStr.length() > 0) {
        long f = customFreqStr.toInt();
        if(f < (long)FREQ_MIN_HZ || f > (long)FREQ_MAX_HZ) {
            server.send(400, "text/plain",
                        "Frequency must be given in hertz, 300-928 MHz");
            return;
        }
        newCustomFreq = (uint32_t)f;
    }

    if(ver != heaterVersion || newCustomFreq != customFrequency) {
        heaterVersion = ver; customFrequency = newCustomFreq;
        prefs.putString("heaterVer", heaterVersion);
        prefs.putULong("customFreq", customFrequency);
        cc1101_applyConfig();
    }

    if(heaterVersion == "V1") {
        unsigned long addr = strtoul(addrStr.c_str(), NULL, 0);
        myAddrV1[0] = (addr >> 16) & 0xFF; myAddrV1[1] = (addr >> 8) & 0xFF; myAddrV1[2] = addr & 0xFF;
        prefs.putBytes("addrV1", myAddrV1, 3);
        heaterPaired = true; heaterAddress = 1; prefs.putUInt("heaterAddr", 1);
        char addrOut[16]; sprintf(addrOut, "%02X%02X%02X", myAddrV1[0], myAddrV1[1], myAddrV1[2]);
        server.send(200, "text/plain", "V1 Paired! ID: " + String(addrOut));
    } else {
        heaterAddress = strtoul(addrStr.c_str(), NULL, 0);
        heaterPaired = true; prefs.putUInt("heaterAddr", heaterAddress);
        server.send(200, "text/plain", "V2 Paired: 0x" + String(heaterAddress, HEX));
    }
}

void handleAPI_WiFi() {
    if(!csrfOk()) return;
    deviceName  = formField("deviceName", deviceName);
    if(deviceName.length() == 0) deviceName = "DieselPilot";
    staSSID     = formField("ssid", staSSID);
    staPassword = formField("pass", staPassword);
    prefs.putString("deviceName", deviceName);
    prefs.putString("staSSID", staSSID); prefs.putString("staPass", staPassword);
    server.send(200, "text/plain", "WiFi saved! Rebooting...");
    delay(1000); ESP.restart();
}

void handleAPI_MQTT() {
    if(!csrfOk()) return;
    mqttServer      = formField("server", mqttServer);
    mqttPort        = formNumber("port", mqttPort, 1, 65535);
    mqttTopic       = formField("topic", mqttTopic);
    mqttAuthEnabled = formFlag("authEnabled", mqttAuthEnabled);
    mqttUser        = formField("user", mqttUser);
    mqttPassword    = formField("pass", mqttPassword);
    // MQTT stays enabled while a broker address is set. It is switched off
    // by clearing that field with __CLEAR__ — there is no other way.
    mqttEnabled     = (mqttServer.length() > 0);
    prefs.putString("mqttServer", mqttServer); prefs.putInt("mqttPort", mqttPort);
    prefs.putString("mqttTopic", mqttTopic); prefs.putBool("mqttAuthEn", mqttAuthEnabled);
    prefs.putString("mqttUser", mqttUser); prefs.putString("mqttPass", mqttPassword);
    prefs.putBool("mqttEnabled", mqttEnabled);
    server.send(200, "text/plain", "MQTT saved!");
    if(mqttEnabled) connectMQTT();
}

// One endpoint used to take every setting on this page, which meant saving
// in any tab wrote the values of all the others. Harmless while one person
// has the page open once, and wrong the moment that stops being true: two
// browsers open and the later save silently reinstates whatever the earlier
// one had changed. Each tab now writes what it shows and nothing else.
//
// Reading stays in one place: a single fetch fills every tab, and a read
// cannot clobber anything.

void handleAPI_Timers() {
    if(!csrfOk()) return;
    autoOffMin          = formNumber("autoOff", autoOffMin, 0, 1440);
    blackoutEnabled     = formFlag("blackoutEn", blackoutEnabled);
    blackoutMinutes     = formNumber("blackout", blackoutMinutes, 0, 1439);
    shutdownLeadMin     = formNumber("lead", shutdownLeadMin, 1, 240);
    cooldownExpectedMin = formNumber("cooldown", cooldownExpectedMin, 1, 60);

    prefs.putUShort("autoOffMin", autoOffMin);
    prefs.putBool("blackoutEn", blackoutEnabled);
    prefs.putInt("blackoutMin", blackoutMinutes);
    prefs.putUShort("shutdownLead", shutdownLeadMin);
    prefs.putUShort("cooldownMin", cooldownExpectedMin);

    server.send(200, "text/plain", "Timers saved");
}

// What this particular unit is: the pump it has, the tank it draws from,
// how often it wants looking at.
void handleAPI_HeaterCfg() {
    if(!csrfOk()) return;
    fuelDoseUl     = formNumber("fuelDose", fuelDoseUl, 5, 60);
    serviceHours   = formNumber("serviceHrs", serviceHours, 0, 2000);
    tankEnabled    = formFlag("tankEn", tankEnabled);
    tankCapacityMl = formNumber("tankCap", tankCapacityMl, 0, 200000);

    prefs.putUShort("fuelDoseUl", fuelDoseUl);
    prefs.putUShort("serviceHrs", serviceHours);
    prefs.putBool("tankEnabled", tankEnabled);
    prefs.putUInt("tankCapMl", tankCapacityMl);

    // A tank that shrank below what it is holding would sit permanently at
    // "full plus a bit", and the warning bands would never fire.
    if(tankRemainingMl > tankCapacityMl) {
        tankRemainingMl = tankCapacityMl;
        prefs.putUInt("tankRemMl", tankRemainingMl);
    }
    // Switched off and on again, the stored figure is however stale the gap
    // made it. Rearm from wherever it now sits rather than firing a warning
    // for a crossing that happened while nobody was counting.
    tankWarnLast = tankActive()
                 ? tankWarnLevel(tankRemainingMl, tankCapacityMl) : 0;

    server.send(200, "text/plain", "Heater settings saved");
}

// Thresholds that decide when the bot says something. They do nothing at all
// without it, which is why they live on its tab.
void handleAPI_Alerts() {
    if(!csrfOk()) return;
    voltAlarmDeciV = formNumber("voltAlarm", voltAlarmDeciV, 80, 160);
    voltClearDeciV = formNumber("voltClear", voltClearDeciV, 80, 170);
    voltDebounceS  = formNumber("voltDeb", voltDebounceS, 1, 600);
    reportFirstMin = formNumber("reportFirst", reportFirstMin, 0, 600);
    reportRptMin   = formNumber("reportRpt", reportRptMin, 0, 600);

    prefs.putUShort("voltAlarmDv", voltAlarmDeciV);
    prefs.putUShort("voltClearDv", voltClearDeciV);
    prefs.putUShort("voltDebSec", voltDebounceS);
    prefs.putUShort("reportFirst", reportFirstMin);
    prefs.putUShort("reportRpt", reportRptMin);

    server.send(200, "text/plain", "Alert settings saved");
}

// A property of the controller rather than of the heater.
void handleAPI_Clock() {
    if(!csrfOk()) return;
    ntpServer   = formField("ntpServer", ntpServer);
    tzOffsetMin = formNumber("tzOffset", tzOffsetMin, -720, 840);

    prefs.putString("ntpServer", ntpServer);
    prefs.putInt("tzOffsetMin", tzOffsetMin);

    setupTime();   // pick up a changed server or offset immediately
    server.send(200, "text/plain", "Clock saved");
}

void handleAPI_Schedule() {
    if(!csrfOk()) return;

    if(server.arg("cancel") == "1") {
        cancelScheduledStartPublic();
        server.send(200, "text/plain", "Scheduled start cancelled");
        return;
    }

    String at = server.arg("at");          // "HH:MM"
    int colon = at.indexOf(':');
    if(colon < 1) { server.send(400, "text/plain", "Use at=HH:MM"); return; }

    String reply;
    bool ok = scheduleStartAt(at.substring(0, colon).toInt(),
                              at.substring(colon + 1).toInt(), reply);
    server.send(ok ? 200 : 400, "text/plain", reply);
}

void handleAPI_Settings() {
    String json = "{";
    json.reserve(320);
    json += "\"timeValid\":" + String(timeValid ? "true" : "false") + ",";
    json += "\"now\":\"" + currentTimeString() + "\",";
    json += "\"ntpServer\":\"" + jsonEscape(ntpServer) + "\",";
    json += "\"tzOffset\":" + String(tzOffsetMin) + ",";
    json += "\"autoOff\":" + String(autoOffMin) + ",";
    json += "\"blackoutEn\":" + String(blackoutEnabled ? "true" : "false") + ",";
    json += "\"blackout\":" + String(blackoutMinutes) + ",";
    json += "\"lead\":" + String(shutdownLeadMin) + ",";
    json += "\"cooldown\":" + String(cooldownExpectedMin) + ",";
    json += "\"fuelDose\":" + String(fuelDoseUl) + ",";
    json += "\"serviceHrs\":" + String(serviceHours) + ",";
    json += "\"tankEn\":" + String(tankEnabled ? "true" : "false") + ",";
    json += "\"tankCap\":" + String(tankCapacityMl) + ",";
    json += "\"tankLeft\":" + String(tankRemainingMl) + ",";
    json += "\"voltAlarm\":" + String(voltAlarmDeciV) + ",";
    json += "\"voltClear\":" + String(voltClearDeciV) + ",";
    json += "\"voltDeb\":" + String(voltDebounceS) + ",";
    json += "\"reportFirst\":" + String(reportFirstMin) + ",";
    json += "\"reportRpt\":" + String(reportRptMin) + ",";
    json += "\"startArmed\":" + String(startArmed ? "true" : "false") + ",";
    json += "\"startAt\":\"" + (startArmed ? clockOfPublic(startTarget) : String("")) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_PairStatus() {
    const char* st = "idle";
    if(pairState == PAIR_SEARCHING) st = "searching";
    else if(pairState == PAIR_OK)   st = "paired";
    else if(pairState == PAIR_FAILED) st = "failed";

    int32_t left = (pairState == PAIR_SEARCHING)
                 ? (int32_t)(pairDeadlineMs - millis()) / 1000 : 0;
    if(left < 0) left = 0;

    String json = "{";
    json.reserve(320);
    json += "\"state\":\"" + String(st) + "\",";
    json += "\"secondsLeft\":" + String(left) + ",";
    json += "\"addr\":\"" + (heaterPaired ? String(heaterAddress, HEX) : String("")) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// Current settings for pre-filling the forms. Secrets are reported as a flag
// only: a blank field means "keep", so the page never needs their values.
void handleAPI_Config() {
    String json = "{";
    json.reserve(320);
    json += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
    json += "\"staSSID\":\"" + jsonEscape(staSSID) + "\",";
    json += "\"staPassSet\":" + String(staPassword.length() > 0 ? "true" : "false") + ",";
    json += "\"mqttServer\":\"" + jsonEscape(mqttServer) + "\",";
    json += "\"mqttPort\":" + String(mqttPort) + ",";
    json += "\"mqttTopic\":\"" + jsonEscape(mqttTopic) + "\",";
    json += "\"mqttAuth\":" + String(mqttAuthEnabled ? "true" : "false") + ",";
    json += "\"mqttUser\":\"" + jsonEscape(mqttUser) + "\",";
    json += "\"mqttPassSet\":" + String(mqttPassword.length() > 0 ? "true" : "false") + ",";
    json += "\"heaterVersion\":\"" + jsonEscape(heaterVersion) + "\",";
    json += "\"customFreq\":" + String(customFrequency);
    json += "}";
    server.send(200, "application/json", json);
}

// The ring buffer was filled but never read anywhere. Newest entry first.
void handleAPI_Errors() {
    uint8_t count = ringCount(errRing);
    String json = "[";
    json.reserve(512);
    for(uint8_t i = 0; i < count; i++) {
        ErrorRecord r;
        if(!errUnpack(r, ringAt(errRing, ERR_LOG_CAPACITY, ERR_RECORD_BYTES, i))) {
            continue;
        }
        if(i) json += ",";
        // An absolute stamp rather than an age: these outlive reboots now, and
        // "40 minutes ago" would mean 40 minutes since the last power-up.
        json += "{\"code\":" + String(r.code) +
                ",\"name\":\"" + jsonEscape(String(getErrorName(r.code))) + "\"" +
                ",\"when\":\"" + jsonEscape(stampOf(r.epoch)) + "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

// Forgetting the heater used to require a factory reset, which also wiped
// WiFi and MQTT.
void handleAPI_Unpair() {
    if(!csrfOk()) return;
    heaterAddress = 0;
    heaterPaired  = false;
    pairState     = PAIR_IDLE;
    heaterStatus.lastUpdate = 0;
    prefs.putUInt("heaterAddr", 0);
    server.send(200, "text/plain", "Heater forgotten");
}

void handleAPI_Telegram() {
    if(!csrfOk()) return;
    tgEnabled = formFlag("enabled", tgEnabled);
    tgToken   = formField("token", tgToken);
    tgChats   = formField("chats", tgChats);
    tgCaCert  = formField("caCert", tgCaCert);
    tgPollSec = formNumber("pollSec", tgPollSec, 5, 300);

    prefs.putBool("tgEnabled", tgEnabled);
    prefs.putString("tgToken", tgToken);
    prefs.putString("tgChats", tgChats);
    prefs.putString("tgCaCert", tgCaCert);
    prefs.putUShort("tgPollSec", tgPollSec);

    // Reboot rather than reconfigure in place: both the bot and the TLS
    // client hold raw pointers into the strings handed to them, and a
    // reassigned String can move.
    server.send(200, "text/plain", "Telegram saved! Rebooting...");
    delay(1000); ESP.restart();
}

void handleAPI_TelegramStatus() {
    String json = "{";
    json.reserve(320);
    json += "\"enabled\":" + String(tgEnabled ? "true" : "false") + ",";
    // The token grants remote control of the heater and is never returned.
    json += "\"tokenSet\":" + String(tgToken.length() > 0 ? "true" : "false") + ",";
    json += "\"customCa\":" + String(tgCaCert.length() > 0 ? "true" : "false") + ",";
    json += "\"connected\":" + String(tgReady ? "true" : "false") + ",";
    json += "\"pollSec\":" + String(tgPollSec) + ",";
    json += "\"discovering\":" + String(
        (tgDiscoverUntilMs != 0 && (int32_t)(tgDiscoverUntilMs - millis()) > 0)
            ? "true" : "false") + ",";
    json += "\"chats\":\"" + jsonEscape(tgChats) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// Opens a short window during which the bot answers /id to anyone, so the
// owner can discover their chat id without trusting a third-party bot.
// Time-limited on purpose: a permanent one would be a standing invitation.
void handleAPI_TelegramDiscover() {
    if(!csrfOk()) return;
    tgDiscoverUntilMs = millis() + 5UL * 60UL * 1000UL;
    server.send(200, "text/plain", "Write /id to the bot within 5 minutes");
}

void handleAPI_TelegramTest() {
    if(!csrfOk()) return;
    if(!tgEnabled)            { server.send(200, "text/plain", "Telegram is disabled"); return; }
    if(tgChats.length() == 0) { server.send(200, "text/plain", "No chat ids configured"); return; }
    if(!tgReady)              { server.send(200, "text/plain", "Bot not connected yet"); return; }
    tgSendToAll("✅ Test message from Diesel Pilot");
    server.send(200, "text/plain", "Test message sent");
}

// Resetting the service counters is the one action here that destroys a
// record, so it is POST behind the CSRF header like every other mutation.
void handleAPI_Service() {
    if(!csrfOk()) return;
    countersMarkService(counters, timeValid ? (uint32_t)time(nullptr) : 0);
    countersDirty = true;
    persistStats();
    server.send(200, "text/plain", "Service recorded");
}

void handleAPI_Info() {
    String json = "{";
    json.reserve(320);
    json += "\"hostname\":\"" + jsonEscape(deviceName) + "\",";
    json += "\"wifiMode\":\"" + String(useAP ? "AP" : "STA") + "\",";
    json += "\"ip\":\"" + (useAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
    json += "\"mqtt\":\"" + String(mqttEnabled && mqtt.connected() ? "Connected" : "Disconnected") + "\",";
    json += "\"ota\":\"" + String(otaEnabled ? "Enabled" : "Disabled") + "\",";
    json += "\"uptime\":\"" + String(millis() / 1000 / 60) + " min\",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) + ",";
    json += "\"loopMaxMs\":" + String(loopMaxMs) + ",";
    json += "\"time\":\"" + String(timeValid ? currentTimeString() : "not synced") + "\",";
    json += "\"version\":\"" + version + "\",";
    json += "\"stats\":\"" + jsonEscape(statsText()) + "\",";
    json += "\"ign\":\"" + jsonEscape(ignText()) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_Reboot() {
    if(!csrfOk()) return;
    server.send(200, "text/plain", "Rebooting...");
    delay(500); ESP.restart();
}

void handleAPI_Factory() {
    if(!csrfOk()) return;
    server.send(200, "text/plain", "Factory reset in progress...");
    Serial.println("\n⚠️ FACTORY RESET");
    prefs.clear();
    displayLine1 = "FACTORY RESET"; displayLine2 = "Clearing...";
    displayLine3 = "All settings"; displayLine4 = "erased!";
    updateDisplay(); delay(2000);
    ESP.restart();
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n═══════════════════════════════════════");
    Serial.println("    DIESEL PILOT V" + (String)version + " FREE");
    Serial.println("═══════════════════════════════════════\n");

    if(!LittleFS.begin(true)) {
        Serial.println("❌ LittleFS Mount Failed!");
    } else {
        Serial.println("✅ LittleFS mounted");
    }

#if USE_OLED
    setupDisplay();
    displayLine1 = "Diesel Pilot";
    displayLine2 = "V" + version + " Starting";
    displayLine3 = "Made by PPTG";
    displayLine4 = "Happy Heating :)";
    updateDisplay();
    delay(2000);
#endif

    // Load all preferences
    prefs.begin("diesel", false);
    heaterAddress   = prefs.getUInt("heaterAddr", 0);
    heaterPaired    = (heaterAddress != 0);
    heaterVersion   = prefs.getString("heaterVer", "V2");
    customFrequency = prefs.getULong("customFreq", 0);
    deviceName      = prefs.getString("deviceName", "DieselPilot");
    staSSID         = prefs.getString("staSSID", "");
    staPassword     = prefs.getString("staPass", "");
    mqttServer      = prefs.getString("mqttServer", "");
    mqttPort        = prefs.getInt("mqttPort", 1883);
    mqttTopic       = prefs.getString("mqttTopic", "diesel");
    mqttAuthEnabled = prefs.getBool("mqttAuthEn", false);
    mqttUser        = prefs.getString("mqttUser", "");
    mqttPassword    = prefs.getString("mqttPass", "");
    mqttEnabled     = prefs.getBool("mqttEnabled", false);
    // OTA preferences
    otaEnabled      = prefs.getBool("otaEnabled", false);
    otaPassword     = prefs.getString("otaPass", "");
    // Clock and shutdown scheduling
    ntpServer           = prefs.getString("ntpServer", "pool.ntp.org");
    tzOffsetMin         = prefs.getInt("tzOffsetMin", 180);
    autoOffMin          = prefs.getUShort("autoOffMin", 180);
    blackoutEnabled     = prefs.getBool("blackoutEn", true);
    blackoutMinutes     = prefs.getInt("blackoutMin", 22 * 60);
    shutdownLeadMin     = prefs.getUShort("shutdownLead", 15);
    cooldownExpectedMin = prefs.getUShort("cooldownMin", 5);
    startArmed          = prefs.getBool("startArmed", false);
    startTarget         = prefs.getULong("startAt", 0);
    fuelDoseUl          = prefs.getUShort("fuelDoseUl", FUEL_DOSE_UL_DEFAULT);
    voltAlarmDeciV      = prefs.getUShort("voltAlarmDv", VOLT_ALARM_DECIV_DEFAULT);
    voltClearDeciV      = prefs.getUShort("voltClearDv", VOLT_CLEAR_DECIV_DEFAULT);
    voltDebounceS       = prefs.getUShort("voltDebSec", VOLT_DEBOUNCE_SEC_DEFAULT);
    reportFirstMin      = prefs.getUShort("reportFirst", 30);
    reportRptMin        = prefs.getUShort("reportRpt", 0);
    serviceHours        = prefs.getUShort("serviceHrs", 0);
    tankEnabled         = prefs.getBool("tankEnabled", false);
    tankCapacityMl      = prefs.getUInt("tankCapMl", 0);
    tankRemainingMl     = prefs.getUInt("tankRemMl", 0);
    tankWarnLast        = tankActive()
                        ? tankWarnLevel(tankRemainingMl, tankCapacityMl) : 0;
    // Lifetime counters, and the flag saying how last night ended
    loadCounters();
    loadIgnLog();
    loadErrLog();
    // Telegram
    tgEnabled = prefs.getBool("tgEnabled", false);
    tgToken   = prefs.getString("tgToken", "");
    tgChats   = prefs.getString("tgChats", "");
    tgCaCert  = prefs.getString("tgCaCert", "");
    tgPollSec = prefs.getUShort("tgPollSec", 60);

    if(heaterVersion == "V1") {
        if(prefs.getBytes("addrV1", myAddrV1, 3) != 3) {
            myAddrV1[0] = 0x19; myAddrV1[1] = 0x52; myAddrV1[2] = 0x4B;
        }
    }

    // Without a seed, Arduino's random() returns the same sequence after every
    // boot -- which made V1 auto-pairing hand out one fixed address, identical
    // on every device and unchanged by re-pairing. The ESP32 has a hardware RNG.
    randomSeed(esp_random());

    // SPI + CC1101
    pinMode(PIN_SCK, OUTPUT); pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_MISO, INPUT); pinMode(PIN_SS, OUTPUT); pinMode(PIN_GDO2, INPUT);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);

    cc1101_applyConfig();

    WiFi.setHostname(deviceName.c_str());
    // Driver-level retry handles brief drops; superviseWiFi() covers the rest.
    WiFi.setAutoReconnect(true);

    // WiFi connect
    if(staSSID.length() > 0) {
        Serial.println("Connecting to WiFi: " + staSSID);
        displayLine2 = "WiFi: " + staSSID; updateDisplay();
        WiFi.begin(staSSID.c_str(), staPassword.c_str());
        int attempts = 0;
        while(WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); Serial.print("."); attempts++; }
        if(WiFi.status() == WL_CONNECTED) {
            useAP = false;
            Serial.println("\n✅ WiFi: " + WiFi.localIP().toString());
            displayLine2 = "IP:"; displayLine3 = WiFi.localIP().toString();
        } else {
            Serial.println("\n❌ WiFi failed, starting AP");
            useAP = true;
        }
    }

    if(useAP) {
        WiFi.softAP(apSSID.c_str(), apPassword.c_str());
        Serial.println("✅ AP: " + apSSID + " / " + WiFi.softAPIP().toString());
        displayLine2 = "AP: " + apSSID;
        displayLine3 = WiFi.softAPIP().toString();
    }

    displayLine4 = heaterPaired ? (heaterVersion + " Paired!") : (heaterVersion + " Not paired");
    updateDisplay();

    setupTime();

    if(mqttEnabled) connectMQTT();

    // OTA init (after WiFi — works in both AP and STA)
    setupOTA();

    // Register HTTP routes
    server.on("/", handleRoot);
    server.on("/api/status",       handleAPI_Status);
    server.on("/api/info",         handleAPI_Info);
    server.on("/api/cmd",          handleAPI_Command);
    server.on("/api/pair/auto",    handleAPI_PairAuto);
    server.on("/api/pair/manual",  handleAPI_PairManual);
    server.on("/api/pair/status",  handleAPI_PairStatus);
    server.on("/api/unpair",       handleAPI_Unpair);
    server.on("/api/config",       handleAPI_Config);
    server.on("/api/errors",       handleAPI_Errors);
    server.on("/api/service",      HTTP_POST, handleAPI_Service);
    server.on("/api/wifi",         handleAPI_WiFi);
    server.on("/api/mqtt",         handleAPI_MQTT);
    server.on("/api/factory",      handleAPI_Factory);
    server.on("/api/reboot",       handleAPI_Reboot);
    // OTA endpoints
    server.on("/api/telegram",       handleAPI_Telegram);
    server.on("/api/telegram/status",handleAPI_TelegramStatus);
    server.on("/api/telegram/test",  handleAPI_TelegramTest);
    server.on("/api/telegram/discover", handleAPI_TelegramDiscover);
    server.on("/api/timers",       handleAPI_Timers);
    server.on("/api/heater",       handleAPI_HeaterCfg);
    server.on("/api/alerts",       handleAPI_Alerts);
    server.on("/api/clock",        handleAPI_Clock);
    server.on("/api/schedule",     handleAPI_Schedule);
    server.on("/api/settings",     handleAPI_Settings);
    server.on("/api/ota/status",   handleAPI_OTAStatus);
    server.on("/api/ota/config",   handleAPI_OTAConfig);
    // WebServer discards any header not listed here.
    const char* collectedHeaders[] = { "X-DieselPilot" };
    server.collectHeaders(collectedHeaders, 1);

    server.begin();

    // Arm the watchdog last: the code above contains legitimate delays —
    // a 2 s splash screen and up to 10 s of Wi-Fi association.
    if(esp_task_wdt_init(WDT_TIMEOUT_SEC, true) == ESP_OK && esp_task_wdt_add(NULL) == ESP_OK) {
        Serial.printf("✅ Watchdog armed: %d s\n", WDT_TIMEOUT_SEC);
    } else {
        Serial.println("⚠️ Watchdog init failed");
    }

    setupOtaVerify();

    // Queued until the bot connects. esp_reset_reason() turns a useless
    // "I am up" into diagnostics: power back after the nightly cut is normal,
    // a watchdog reset means something is hanging.
    esp_reset_reason_t rr = esp_reset_reason();
    bool unexpected = (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT ||
                       rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT ||
                       rr == ESP_RST_BROWNOUT);
    notifyTelegram(String(unexpected ? "⚠️ Controller restarted"
                                     : "🔌 Controller started") +
                   "\nReason: " + resetReasonName(rr) +
                   "\nHeater: " + (heaterPaired ? heaterVersion + ", paired"
                                                 : String("not paired")));

    Serial.println("\n✅ Ready! V" + version);
}

// ═══════════════════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
    static unsigned long lastHeaterUpdate = 0;
    static unsigned long lastDisplayUpdate = 0;
    static unsigned long lastCC1101Retry = 0;
    static unsigned long lastSlowTick = 0;

    // Measured from the previous iteration, so it covers everything the loop
    // did -- including the blocking parts, which are what we want to see.
    static uint32_t loopPrevMs = 0;
    uint32_t loopNow = millis();
    if(loopPrevMs != 0) {
        uint32_t took = loopNow - loopPrevMs;
        if(took > loopMaxMs) loopMaxMs = took;
    }
    loopPrevMs = loopNow;

    feedWatchdog();
    yield();
    superviseWiFi();
    updateTimeSync();
    updatePairing();
    updateTelegram();
    updateTelegramCommands();

    // The scheduler works at minute granularity and the notifier compares
    // fields refreshed every few seconds. Running either on every iteration
    // is pure waste: currentMinutesOfDay() alone recomputes the timezone
    // thousands of times a second.
    if(millis() - lastSlowTick >= 1000) {
        lastSlowTick = millis();
        updateTelegramPollRate();
        updateFuel();
        updateStats();
        updateProgressReport();
        updateIgnition();
        updateIgnitionLog();
        updateLevel();
        updateScheduledStart();
        updateOtaVerify();
        updateNotifications();
        updateScheduler();
    }
    server.handleClient();
    // MQTT
    if(mqttEnabled && !mqtt.connected()) {
        if(millis() - lastMQTTRetry > mqttRetryInterval) {
            lastMQTTRetry = millis();
            connectMQTT();
        }
    }
    if(mqttEnabled && mqtt.connected()) mqtt.loop();

    // OTA
    loopOTA();

    // Do not poll a faulty module: every operation would hit the timeout and
    // eat up the loop. Instead retry initialisation every half minute —
    // the wiring may have been fixed in the meantime.
    if(cc1101Fault) {
        if(millis() - lastCC1101Retry > CC1101_RETRY_MS) {
            lastCC1101Retry = millis();
            Serial.println("⚠️ CC1101 not responding, re-init...");
            cc1101_applyConfig();
        }
    } else if(pairState == PAIR_SEARCHING) {
        // The search and the status poll would otherwise flush each other's
        // RX FIFO: both drive the same radio, and the poll runs every three
        // seconds. The blocking version could not collide because it held
        // the whole loop.
    } else if(millis() - lastHeaterUpdate > heaterPollIntervalMs() && heaterPaired) {
        lastHeaterUpdate = millis();
        if(heaterVersion == "V1") updateHeaterStatus_V1();
        else updateHeaterStatus();
    }

    // Display update
    if(millis() - lastDisplayUpdate > 1000) {
        lastDisplayUpdate = millis();

        if(cc1101Fault) {
            displayLine1 = "DIESEL PILOT " + version;
            displayLine2 = "RF FAULT";
            displayLine3 = "! CC1101 !";
            displayLine4 = "check wiring";
        } else if(heaterPaired && heaterStatus.lastUpdate > 0) {
            displayLine1 = "DIESEL PILOT " + version;
            displayLine2 = String(getStateName(heaterStatus.state));

            if(heaterStatus.errorCode > 1 && heaterStatus.errorCode != 12) {
                displayLine3 = "! " + String(getErrorName(heaterStatus.errorCode)) + " !";
            } else {
                if(heaterStatus.autoMode) {
                    displayLine3 = String(heaterStatus.ambientTemp) + "C -> " + String(heaterStatus.setpoint) + "C";
                } else {
                    displayLine3 = String(heaterStatus.ambientTemp) + "C  P:" + String(heaterStatus.pumpFreq / 10.0, 1) + "Hz";
                }
            }

            String modeIcon = heaterStatus.autoMode ? "A" : "M";
            displayLine4 = String(heaterStatus.voltage / 10.0, 1) + "V  [" + modeIcon + "]  " + String(heaterStatus.caseTemp) + "C";

        } else if(heaterPaired) {
            displayLine1 = "DIESEL PILOT";
            displayLine2 = "Waiting...";
            displayLine3 = "No data";
            displayLine4 = "";
        }

        updateDisplay();
    }
}
