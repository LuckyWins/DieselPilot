/*
 * Notification logic — pure, no hardware and no network.
 *
 * Covers the parts that are easy to get quietly wrong: who is allowed to
 * command the heater, how often a repeated condition may be announced, and
 * how fast to back off when Telegram is unreachable.
 */

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

// Backoff bounds for a failing Telegram connection. The garage link is poor,
// so retries slow down instead of burning the data plan on a dead endpoint.
#define NOTIFY_BACKOFF_MIN_MS 30000UL
#define NOTIFY_BACKOFF_MAX_MS 900000UL

// Parses a stored whitelist: chat ids separated by commas, spaces or
// semicolons. Malformed entries are skipped rather than aborting the parse,
// so one bad character cannot lock the owner out.
std::vector<int64_t> parseChatList(const std::string& list);

// Renders ids back into the stored form.
std::string formatChatList(const std::vector<int64_t>& ids);

// Anyone can find a bot by name and message it, so every incoming command
// must be checked against this. An empty whitelist allows nobody: failing
// closed means a misconfigured device is inert, not open to the world.
bool isChatAllowed(const std::string& whitelist, int64_t chatId);

uint32_t nextBackoffMs(uint32_t currentMs);

// True the first time a value differs from the one last accepted, updating
// the stored copy. The heater is polled every 3 seconds, so without this an
// error would produce twenty messages a minute.
bool changedSince(int& stored, int current);

// Battery voltage alarm with hysteresis, so a reading sitting on the
// threshold does not flap between states. Values are in tenths of a volt,
// matching heaterStatus.voltage.
bool voltageAlarm(bool alarming, uint16_t deciVolts,
                  uint16_t alarmBelow, uint16_t clearAbove);
