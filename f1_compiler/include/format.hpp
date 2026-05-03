#pragma once

// GCC 13 is the first version to fully support std::format.
// For older versions like GCC 11, we fall back to the {fmt} library and
// provide a compatibility alias in the std namespace.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 13
#include <fmt/core.h>
namespace std { using fmt::format; }
#else
#include <format>
#endif