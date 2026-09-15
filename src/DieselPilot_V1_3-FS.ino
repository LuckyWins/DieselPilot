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

#include "protocol.h"           // States, error codes, CRC, frequency maths
#include "settings.h"           // Settings form parsing
#include "scheduler.h"          // Shutdown timers

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

// Wi-Fi reconnect backoff bounds. The garage sits on the edge of town and
// the link can be down for hours, so retries back off instead of hammering.
#define WIFI_RETRY_MIN_MS 5000
#define WIFI_RETRY_MAX_MS 300000

// How often to check whether NTP has delivered a plausible date yet
#define NTP_CHECK_MS 5000

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

// Heater
uint32_t heaterAddress = 0x00000000;
uint8_t packetSeq = 0;
bool heaterPaired = false;

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

// Error History
struct ErrorHistoryEntry {
    uint8_t errorCode;
    unsigned long timestamp;
};
ErrorHistoryEntry errorHistory[10];
int errorHistoryIndex = 0;

// Display
String displayLine1 = "Diesel Pilot";
String displayLine2 = "Initializing...";
String displayLine3 = "";
String displayLine4 = "";

// Timing
unsigned long lastUpdate = 0;
unsigned long lastDisplay = 0;
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
    if(errorHistoryIndex > 0 && errorHistory[(errorHistoryIndex - 1) % 10].errorCode == errorCode) return;
    errorHistory[errorHistoryIndex % 10].errorCode = errorCode;
    errorHistory[errorHistoryIndex % 10].timestamp = millis();
    errorHistoryIndex++;
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
        return false;
    }
    Serial.printf("✅ CC1101 present: VERSION=0x%02X\n", version);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 INIT
// ═══════════════════════════════════════════════════════════════════════════

void cc1101_init() {
    cc1101_strobe(0x30); delay(100);
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
    Serial.println("✅ CC1101 initialized @ 433.937 MHz (V2)");
}

void cc1101_init_V1() {
    cc1101_strobe(0x30); delay(100);
    const byte configRegsV1[] = {
        0x0B, 0x06, 0x0D, 0x10, 0x0E, 0xB0, 0x0F, 0x71,
        0x10, 0x86, 0x11, 0x83, 0x12, 0x12, 0x13, 0x22,
        0x04, 0x09, 0x05, 0x1A, 0x06, 0x0A, 0x08, 0x00,
        0x15, 0x40, 0x18, 0x18, 0x23, 0xFF
    };
    for(int i = 0; i < sizeof(configRegsV1); i += 2)
        cc1101_writeReg(configRegsV1[i], configRegsV1[i+1]);
    cc1101_strobe(0x36); delay(5); cc1101_strobe(0x34);
    Serial.println("✅ CC1101 initialized @ 433.892 MHz (V1)");
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
            heaterStatus.setpoint = (b7 - 32) * 2 + (b8 >> 7);
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

bool receivePacket(uint8_t* bytes, uint16_t timeout) {
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
    if(receivePacket(buf, 2000)) {
        uint32_t addr = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                        ((uint32_t)buf[4] << 8) | buf[5];
        if(addr == heaterAddress) {
            heaterStatus.state = buf[6];
            heaterStatus.power = buf[7];
            heaterStatus.errorCode = buf[7];
            heaterStatus.voltage = buf[9];
            heaterStatus.ambientTemp = (int8_t)buf[10];
            heaterStatus.caseTemp = buf[12];
            heaterStatus.setpoint = (int8_t)buf[13];
            heaterStatus.autoMode = (buf[14] == 0x32);
            heaterStatus.pumpFreq = buf[15];
            heaterStatus.rssi = (buf[23] - (buf[23] >= 128 ? 256 : 0)) / 2 - 74;
            heaterStatus.lastUpdate = millis();
            addErrorToHistory(heaterStatus.errorCode);
            if(mqttEnabled && mqtt.connected()) publishMQTT();
        }
    }
}

uint32_t findHeater(uint16_t timeout) {
    Serial.println("Searching for heater...");
    displayLine2 = "Pairing..."; updateDisplay();
    uint8_t buf[32];
    if(receivePacket(buf, timeout)) {
        return ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
               ((uint32_t)buf[4] << 8) | buf[5];
    }
    return 0;
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
    sendCommand(CMD_POWER);
    return true;
}

bool heaterEnsureOn() {
    if(!heaterPaired) return false;
    // Only from a full stop. Toggling mid-purge does not restart the heater
    // on these units, and the command would simply be lost.
    if(heaterStatus.state != STATE_OFF) return false;
    sendCommand(CMD_POWER);
    return true;
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
    in.autoOffMin          = autoOffMin;
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
                Serial.println(d.reason == REASON_BLACKOUT
                    ? "⏱ Shutting down ahead of the mains cut"
                    : "⏱ Runtime limit reached, shutting down");
            }
            break;

        case SCHED_COOLDOWN_DONE:
            shutdownRequested = false;
            Serial.println("✅ Purge finished, heater is off");
            break;

        case SCHED_COOLDOWN_TIMEOUT:
            shutdownRequested = false;
            Serial.println("⚠️ Heater did not reach OFF within the purge window");
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
    if(useAP || staSSID.length() == 0) return;   // nothing to reconnect to

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
void setupOTA() {
    if(!otaEnabled || otaRunning) return;

    ArduinoOTA.setHostname(deviceName.c_str());
    if(otaPassword.length() > 0) ArduinoOTA.setPassword(otaPassword.c_str());

    ArduinoOTA.onStart([]() {
        displayLine1 = "OTA UPDATE";
        displayLine2 = "Starting...";
        displayLine3 = "";
        displayLine4 = "";
        updateDisplay();
        Serial.println("OTA: Update started");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        int pct = total ? (progress * 100) / total : 0;
        displayLine2 = "Flashing " + String(pct) + "%";
        updateDisplay();
    });
    ArduinoOTA.onEnd([]() {
        displayLine2 = "Done!";
        displayLine3 = "Rebooting...";
        updateDisplay();
        Serial.println("\nOTA: Update complete");
    });
    ArduinoOTA.onError([](ota_error_t error) {
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

void updateDisplay() {
#if USE_OLED
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
    json += "\"state\":\"" + String(getStateName(heaterStatus.state)) + "\",";
    json += "\"voltage\":" + String(heaterStatus.voltage / 10.0, 1) + ",";
    json += "\"ambient\":" + String(heaterStatus.ambientTemp) + ",";
    json += "\"case\":" + String(heaterStatus.caseTemp) + ",";
    json += "\"setpoint\":" + String(heaterStatus.setpoint) + ",";
    json += "\"pump\":" + String(heaterStatus.pumpFreq / 10.0, 1) + ",";
    json += "\"mode\":\"" + String(heaterStatus.autoMode ? "AUTO" : "MANUAL") + "\",";
    json += "\"rssi\":" + String(heaterStatus.rssi) + ",";
    json += "\"addr\":\"" + (heaterPaired ? String(heaterAddress, HEX) : "Not paired") + "\",";
    json += "\"version\":\"" + heaterVersion + "\",";
    json += "\"errorCode\":" + String(heaterStatus.errorCode) + ",";
    json += "\"errorName\":\"" + String(getErrorName(heaterStatus.errorCode)) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_OTAStatus() {
    // Returns OTA state for GUI
    String json = "{";
    json += "\"enabled\":" + String(otaEnabled ? "true" : "false") + ",";
    json += "\"running\":" + String(otaRunning ? "true" : "false") + ",";
    json += "\"hasPassword\":" + String(otaPassword.length() > 0 ? "true" : "false") + ",";
    json += "\"hostname\":\"" + deviceName + "\",";
    json += "\"ip\":\"" + (useAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_OTAConfig() {
    // Toggle OTA ON/OFF and (optionally) set the flashing password.
    otaEnabled  = formFlag("enabled", otaEnabled);
    otaPassword = formField("password", otaPassword);

    prefs.putBool("otaEnabled", otaEnabled);
    prefs.putString("otaPass", otaPassword);

    // Reboot so ArduinoOTA starts/stops cleanly with the new settings.
    server.send(200, "text/plain", "OTA saved! Rebooting...");
    delay(1000); ESP.restart();
}

void handleAPI_Command() {
    String cmd = server.arg("c");
    if(cmd == "power")      sendCommand(CMD_POWER);
    else if(cmd == "up")    sendCommand(CMD_UP);
    else if(cmd == "down")  sendCommand(CMD_DOWN);
    else if(cmd == "mode")  sendCommand(CMD_MODE);
    server.send(200, "text/plain", "OK");
}

void handleAPI_PairAuto() {
    String ver = server.arg("version");
    String customFreqStr = server.arg("customFreq");
    if(ver.length() == 0) ver = "V2";

    uint32_t newCustomFreq = customFreqStr.length() > 0 ? customFreqStr.toInt() : 0;

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
        uint32_t addr = findHeater(60000);
        if(addr != 0) {
            heaterAddress = addr; heaterPaired = true; prefs.putUInt("heaterAddr", heaterAddress);
            server.send(200, "text/plain", "V2 Paired: 0x" + String(heaterAddress, HEX));
        } else {
            server.send(200, "text/plain", "V2 Pairing failed!");
        }
    }
}

void handleAPI_PairManual() {
    String addrStr = server.arg("addr");
    String ver = server.arg("version");
    String customFreqStr = server.arg("customFreq");
    if(ver.length() == 0) ver = "V2";
    uint32_t newCustomFreq = customFreqStr.length() > 0 ? customFreqStr.toInt() : 0;

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

void handleAPI_Timers() {
    ntpServer           = formField("ntpServer", ntpServer);
    tzOffsetMin         = formNumber("tzOffset", tzOffsetMin, -720, 840);
    autoOffMin          = formNumber("autoOff", autoOffMin, 0, 1440);
    blackoutEnabled     = formFlag("blackoutEn", blackoutEnabled);
    blackoutMinutes     = formNumber("blackout", blackoutMinutes, 0, 1439);
    shutdownLeadMin     = formNumber("lead", shutdownLeadMin, 1, 240);
    cooldownExpectedMin = formNumber("cooldown", cooldownExpectedMin, 1, 60);

    prefs.putString("ntpServer", ntpServer);
    prefs.putInt("tzOffsetMin", tzOffsetMin);
    prefs.putUShort("autoOffMin", autoOffMin);
    prefs.putBool("blackoutEn", blackoutEnabled);
    prefs.putInt("blackoutMin", blackoutMinutes);
    prefs.putUShort("shutdownLead", shutdownLeadMin);
    prefs.putUShort("cooldownMin", cooldownExpectedMin);

    setupTime();   // pick up a changed server or offset immediately
    server.send(200, "text/plain", "Timers saved!");
}

void handleAPI_TimerStatus() {
    String json = "{";
    json += "\"timeValid\":" + String(timeValid ? "true" : "false") + ",";
    json += "\"now\":\"" + currentTimeString() + "\",";
    json += "\"ntpServer\":\"" + ntpServer + "\",";
    json += "\"tzOffset\":" + String(tzOffsetMin) + ",";
    json += "\"autoOff\":" + String(autoOffMin) + ",";
    json += "\"blackoutEn\":" + String(blackoutEnabled ? "true" : "false") + ",";
    json += "\"blackout\":" + String(blackoutMinutes) + ",";
    json += "\"lead\":" + String(shutdownLeadMin) + ",";
    json += "\"cooldown\":" + String(cooldownExpectedMin);
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_Info() {
    String json = "{";
    json += "\"hostname\":\"" + deviceName + "\",";
    json += "\"wifiMode\":\"" + String(useAP ? "AP" : "STA") + "\",";
    json += "\"ip\":\"" + (useAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
    json += "\"mqtt\":\"" + String(mqttEnabled && mqtt.connected() ? "Connected" : "Disconnected") + "\",";
    json += "\"ota\":\"" + String(otaEnabled ? "Enabled" : "Disabled") + "\",";
    json += "\"uptime\":\"" + String(millis() / 1000 / 60) + " min\",";
    json += "\"time\":\"" + String(timeValid ? currentTimeString() : "not synced") + "\",";
    json += "\"version\":\"" + version + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleAPI_Reboot() {
    server.send(200, "text/plain", "Rebooting...");
    delay(500); ESP.restart();
}

void handleAPI_Factory() {
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
    Wire.begin(PIN_SDA, PIN_SCL);
    display.begin(); display.setContrast(255);
    displayLine1 = "Diesel Pilot";
    displayLine2 = "V" + version + " Starting";
    displayLine3 = "Made by PPTG";
    displayLine4 = "Happy Heating :)";
    updateDisplay();
    Serial.println("✅ OLED initialized");
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

    if(heaterVersion == "V1") {
        if(prefs.getBytes("addrV1", myAddrV1, 3) != 3) {
            myAddrV1[0] = 0x19; myAddrV1[1] = 0x52; myAddrV1[2] = 0x4B;
        }
    }

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
    server.on("/api/wifi",         handleAPI_WiFi);
    server.on("/api/mqtt",         handleAPI_MQTT);
    server.on("/api/factory",      handleAPI_Factory);
    server.on("/api/reboot",       handleAPI_Reboot);
    // OTA endpoints
    server.on("/api/timers",       handleAPI_Timers);
    server.on("/api/timers/status",handleAPI_TimerStatus);
    server.on("/api/ota/status",   handleAPI_OTAStatus);
    server.on("/api/ota/config",   handleAPI_OTAConfig);
    server.begin();

    // Arm the watchdog last: the code above contains legitimate delays —
    // a 2 s splash screen and up to 10 s of Wi-Fi association.
    if(esp_task_wdt_init(WDT_TIMEOUT_SEC, true) == ESP_OK && esp_task_wdt_add(NULL) == ESP_OK) {
        Serial.printf("✅ Watchdog armed: %d s\n", WDT_TIMEOUT_SEC);
    } else {
        Serial.println("⚠️ Watchdog init failed");
    }

    Serial.println("\n✅ Ready! V" + version);
}

// ═══════════════════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
    static unsigned long lastHeaterUpdate = 0;
    static unsigned long lastDisplayUpdate = 0;
    static unsigned long lastCC1101Retry = 0;

    feedWatchdog();
    yield();
    superviseWiFi();
    updateTimeSync();
    updateScheduler();
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
    } else if(millis() - lastHeaterUpdate > 3000 && heaterPaired) {
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
