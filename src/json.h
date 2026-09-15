/*
 * JSON string escaping — pure, no hardware.
 *
 * The API responses are assembled by concatenation, and user-supplied values
 * go straight into them: device name, SSID, MQTT topic, chat ids. A single
 * quote in any of those produced malformed JSON, and the settings page then
 * failed to load with no diagnostic anywhere.
 */

#pragma once

#include <string>

// Escapes a value for embedding between double quotes. Handles the quote
// itself, the backslash, and the control characters below 0x20 that JSON
// forbids raw.
std::string jsonEscape(const std::string& value);

#ifdef ARDUINO
#include <Arduino.h>

inline String jsonEscape(const String& value) {
    return String(jsonEscape(std::string(value.c_str())).c_str());
}
#endif
