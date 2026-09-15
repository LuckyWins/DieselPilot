/*
 * Settings form parsing — pure logic with no hardware access.
 *
 * The core works on std::string so it builds on the host and can be
 * covered by tests. A thin Arduino String wrapper is provided for firmware.
 */

#pragma once

#include <string>

// Marker for explicitly clearing a field.
//
// An empty value means "keep unchanged" — otherwise a form submitted with
// a blank field wipes the stored setting. That is exactly how editing the
// hostname used to erase the SSID and password, after which the device
// fell back to AP mode.
//
// Clearing cannot simply be forbidden: mqttEnabled is derived from the
// length of the broker address, so without a reset there would be no way
// to turn MQTT off. Hence a dedicated marker.
#define SETTINGS_CLEAR_TOKEN "__CLEAR__"

// Decides what a settings field becomes after a form submission:
//   parameter missing        -> keep the current value
//   parameter empty          -> keep the current value
//   parameter == CLEAR_TOKEN -> clear it
//   otherwise                -> accept the new value
std::string resolveField(bool present,
                         const std::string& incoming,
                         const std::string& current);

// Checkboxes arrive as "1"/"0". A missing parameter must not silently
// switch the option off, so the current value is preserved.
bool resolveFlag(bool present, const std::string& incoming, bool current);

// Numeric fields: a non-empty, well-formed value inside the allowed range
// is accepted, anything else keeps the current one.
long resolveNumber(bool present, const std::string& incoming, long current,
                   long minValue, long maxValue);

#ifdef ARDUINO
#include <Arduino.h>

// Wrappers for firmware calls — all logic stays in the functions above.
inline String resolveField(bool present, const String& incoming, const String& current) {
    return String(resolveField(present,
                               std::string(incoming.c_str()),
                               std::string(current.c_str())).c_str());
}

inline bool resolveFlag(bool present, const String& incoming, bool current) {
    return resolveFlag(present, std::string(incoming.c_str()), current);
}

inline long resolveNumber(bool present, const String& incoming, long current,
                          long minValue, long maxValue) {
    return resolveNumber(present, std::string(incoming.c_str()), current, minValue, maxValue);
}
#endif
