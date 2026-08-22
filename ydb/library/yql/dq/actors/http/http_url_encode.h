#pragma once

#include <util/generic/string.h>

#include <cctype>

namespace NYql::NDq::NHttpEgress {

// URL-encode a string for safe inclusion in URLs.
// Safe characters: alphanumeric and -._~
inline TString UrlEncode(TStringBuf input) {
    TString result;
    result.reserve(input.size());
    for (auto c : input) {
        // Safe characters: alphanumeric and -._~
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            result += c;
        } else {
            // Encode as %XX
            result += '%';
            result += static_cast<char>("0123456789ABCDEF"[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            result += static_cast<char>("0123456789ABCDEF"[static_cast<unsigned char>(c) & 0xF]);
        }
    }
    return result;
}

} // namespace NYql::NDq::NHttpEgress
