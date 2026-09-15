#include "notify.h"

#include <cstdlib>
#include <cerrno>

std::vector<int64_t> parseChatList(const std::string& list) {
    std::vector<int64_t> out;
    std::string token;

    for(size_t i = 0; i <= list.size(); i++) {
        char c = (i < list.size()) ? list[i] : ',';
        if(c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if(!token.empty()) {
                errno = 0;
                char* end = nullptr;
                long long v = std::strtoll(token.c_str(), &end, 10);
                bool clean = (errno == 0) && end && *end == '\0' && end != token.c_str();
                if(clean && v != 0) out.push_back((int64_t)v);
                token.clear();
            }
        } else {
            token.push_back(c);
        }
    }
    return out;
}

std::string formatChatList(const std::vector<int64_t>& ids) {
    std::string out;
    char buf[24];
    for(size_t i = 0; i < ids.size(); i++) {
        if(i) out += ",";
        snprintf(buf, sizeof(buf), "%lld", (long long)ids[i]);
        out += buf;
    }
    return out;
}

bool isChatAllowed(const std::string& whitelist, int64_t chatId) {
    if(chatId == 0) return false;
    std::vector<int64_t> ids = parseChatList(whitelist);
    for(size_t i = 0; i < ids.size(); i++) {
        if(ids[i] == chatId) return true;
    }
    return false;
}

uint32_t nextBackoffMs(uint32_t currentMs) {
    if(currentMs < NOTIFY_BACKOFF_MIN_MS) return NOTIFY_BACKOFF_MIN_MS;
    if(currentMs >= NOTIFY_BACKOFF_MAX_MS) return NOTIFY_BACKOFF_MAX_MS;
    uint32_t next = currentMs * 2;
    return (next > NOTIFY_BACKOFF_MAX_MS) ? NOTIFY_BACKOFF_MAX_MS : next;
}

bool changedSince(int& stored, int current) {
    if(stored == current) return false;
    stored = current;
    return true;
}

bool voltageAlarm(bool alarming, uint16_t deciVolts,
                  uint16_t alarmBelow, uint16_t clearAbove) {
    // A zero reading means the heater has not reported yet; keep the current
    // verdict rather than inventing an alarm out of missing data.
    if(deciVolts == 0) return alarming;
    if(alarming)  return deciVolts < clearAbove;
    return deciVolts < alarmBelow;
}
