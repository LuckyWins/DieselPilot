#include "settings.h"

#include <cstdlib>
#include <cerrno>

std::string resolveField(bool present,
                         const std::string& incoming,
                         const std::string& current) {
    if(!present)                            return current;
    if(incoming.empty())                    return current;
    if(incoming == SETTINGS_CLEAR_TOKEN)    return std::string();
    return incoming;
}

bool resolveFlag(bool present, const std::string& incoming, bool current) {
    if(!present)         return current;
    if(incoming.empty()) return current;
    return incoming == "1";
}

long resolveNumber(bool present, const std::string& incoming, long current,
                   long minValue, long maxValue) {
    if(!present)         return current;
    if(incoming.empty()) return current;

    // strtol вместо atoi: нужно отличать "0" от нечислового мусора,
    // который иначе тоже дал бы ноль.
    errno = 0;
    char* end = nullptr;
    long parsed = std::strtol(incoming.c_str(), &end, 10);

    if(errno != 0)              return current;  // переполнение
    if(end == incoming.c_str()) return current;  // ни одной цифры
    if(end != nullptr && *end)  return current;  // хвостовой мусор
    if(parsed < minValue || parsed > maxValue) return current;

    return parsed;
}
