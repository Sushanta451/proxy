// Command-line value parsers shared by tap.cpp's option handling. Split out so
// a future test or a second binary can reach them without pulling in the rest
// of tap.cpp.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "faults.h"

namespace tcptap
{

inline bool applies_to_c_to_u(FaultDirection direction)
{
    return direction == FaultDirection::CToU || direction == FaultDirection::Both;
}

inline bool applies_to_u_to_c(FaultDirection direction)
{
    return direction == FaultDirection::UToC || direction == FaultDirection::Both;
}

// Everything a run needs, gathered in one place instead of a parameter list
// that grows with every flag. parse_args fills it directly; main just reads it.
struct Options
{
    uint16_t listen_port{0};
    std::string upstream_host;
    std::string upstream_port;
    bool once{false};
    bool help{false};
    FaultConfig faults;
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

inline bool parse_direction(const char* text, FaultDirection& out)
{
    std::string_view value{text};
    if (value == "c>u")
    {
        out = FaultDirection::CToU;
    }
    else if (value == "u>c")
    {
        out = FaultDirection::UToC;
    }
    else if (value == "both")
    {
        out = FaultDirection::Both;
    }
    else
    {
        return false;
    }
    return true;
}

inline void usage(std::ostream& outstream)
{
    outstream << "usage: tcptap --listen <port> --upstream <host>:<port> [--once] [--seed N]\n"
              << "              [--direction c>u|u>c|both]\n"
              << "\n"
              << "Accepts one connection on 127.0.0.1:<port>, dials the upstream,\n"
              << "and relays bytes between them, logging every chunk and its\n"
              << "direction (c>u downstream-to-upstream, u>c the other way).\n"
              << "--once serves a single connection, then exits.\n"
              << "--seed N seeds the fault RNG (0..4294967295); without it one is drawn\n"
              << "at random. The seed in use is logged either way, to replay a run.\n"
              << "--direction restricts faults to one direction (default both).\n";
}

// Fills *out* from argv, printing any complaint (and the usage banner) to
// stderr itself; the caller's only job on failure is to return 1.
inline bool parse_args(int argc, char* argv[], Options& out)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string_view flag{argv[i]};
        bool has_value{i + 1 < argc};

        if (flag == "--listen" && has_value)
        {
            if (!parse_port(argv[++i], out.listen_port))
            {
                std::cerr << "error: --listen wants a port in 1..65535\n";
                return false;
            }
        }
        else if (flag == "--upstream" && has_value)
        {
            if (!split_upstream(argv[++i], out.upstream_host, out.upstream_port))
            {
                std::cerr << "error: --upstream wants <host>:<port>\n";
                return false;
            }
        }
        else if (flag == "--seed" && has_value)
        {
            if (!parse_seed(argv[++i], out.faults.seed))
            {
                std::cerr << "error: --seed wants an integer in 0..4294967295\n";
                return false;
            }
            out.faults.seeded = true;
        }
        else if (flag == "--direction" && has_value)
        {
            if (!parse_direction(argv[++i], out.faults.direction))
            {
                std::cerr << "error: --direction wants c>u, u>c or both\n";
                return false;
            }
        }
        else if (flag == "--once")
        {
            out.once = true;
        }
        else if (flag == "--help" || flag == "-h")
        {
            usage(std::cout);
            out.help = true;
            return true;
        }
        else
        {
            std::cerr << "error: unexpected argument: " << flag << '\n';
            usage(std::cerr);
            return false;
        }
    }

    if (out.listen_port == 0 || out.upstream_host.empty())
    {
        usage(std::cerr);
        return false;
    }

    return true;
}

} // namespace tcptap
