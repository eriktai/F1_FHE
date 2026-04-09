#pragma once

#include <cstdint>

inline bool isPow2(uint64_t n) {
    return (n > 0) && ((n & (n - 1)) == 0);
}