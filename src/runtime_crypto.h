#pragma once
// Hashing helpers: SHA-256 into a byte array, and bytes to lowercase hex.
#include <windows.h>
#include <array>
#include <string>
#include "sha256.h"
namespace gc {
// SHA-256 of size bytes into out. Always returns true.
inline bool request_hash(const unsigned char* bytes, unsigned size, std::array<unsigned char, 32>& out) {
    out = sha256(bytes, size);
    return true;
}
inline std::string request_hex(const unsigned char* p, unsigned size) {
    const char* digits = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < size; ++i) {
        result += digits[p[i] >> 4];
        result += digits[p[i] & 15];
    }
    return result;
}
}
