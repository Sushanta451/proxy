// Command-line value parsers shared by tap.cpp's option handling. Split out so
// a future test or a second binary can reach them without pulling in the rest
// of tap.cpp.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace tcptap
{

// Everything a run needs, gathered in one place instead of a parameter list
// that grows with every flag. parse_args (ROADMAP) will fill this directly;
// for now main still does the filling.
struct Options
{
    uint16_t listen_port{0};
    std::string upstream_host;
    std::string upstream_port;
    bool once{false};
    bool seeded{false};
    std::uint32_t seed{0};
};

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

// v1 takes host:port and splits on the last colon, which is enough for IPv4
// and hostnames. The bracketed [::1]:port form is a ROADMAP item.
inline bool split_upstream(const std::string& spec, std::string& host, std::string& port)
{
    size_t colon{spec.rfind(':')};
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size())
    {
        return false;
    }
    host = spec.substr(0, colon);
    port = spec.substr(colon + 1);
    return true;
}

} // namespace tcptap
