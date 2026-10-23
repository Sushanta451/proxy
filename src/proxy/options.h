// Command-line value parsers shared by tap.cpp's option handling. Split out so
// a future test or a second binary can reach them without pulling in the rest
// of tap.cpp.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>

namespace tcptap
{

inline bool parse_port(const char* text, uint16_t& out)
{
    char* end{nullptr};
    errno = 0;
    unsigned long value{std::strtoul(text, &end, 10)};
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 65535)
    {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

// A seed is any 32-bit unsigned value. strtoul would quietly accept "-1" and
// leading spaces, so the first character has to be a digit.
inline bool parse_seed(const char* text, std::uint32_t& out)
{
    char* end{nullptr};
    errno = 0;
    unsigned long value{std::strtoul(text, &end, 10)};
    if (*text < '0' || *text > '9' || errno != 0 || *end != '\0' || value > 0xffffffffUL)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

} // namespace tcptap
