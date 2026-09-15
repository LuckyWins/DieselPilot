#include "json.h"

#include <cstdio>

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);

    for(size_t i = 0; i < value.size(); i++) {
        unsigned char c = (unsigned char)value[i];
        switch(c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if(c < 0x20) {
                    // No short form for the rest of the control range.
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Bytes above 0x7F pass through untouched: UTF-8 is valid
                    // inside a JSON string, and splitting it here would only
                    // corrupt multi-byte characters.
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}
