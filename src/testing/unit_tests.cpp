// tcptap-unit — checks on the proxy's pure functions, run as a CTest case.
//
// No test framework: there are no dependencies, and the whole point of this
// project is to not reach for a wrapper around something small. CHECK records
// the failing file:line and keeps going; main exits non-zero if any fired.

#include "../proxy/options.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
int g_failures{0};
}

#define CHECK(expr)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " << #expr << '\n';       \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

int main()
{
    uint16_t port{0};
    CHECK(tcptap::parse_port("9100", port) && port == 9100);
    CHECK(tcptap::parse_port("1", port) && port == 1);
    CHECK(tcptap::parse_port("65535", port) && port == 65535);
    CHECK(!tcptap::parse_port("0", port));
    CHECK(!tcptap::parse_port("65536", port));
    CHECK(!tcptap::parse_port("-1", port));
    CHECK(!tcptap::parse_port("", port));
    CHECK(!tcptap::parse_port("80x", port));
    CHECK(!tcptap::parse_port("12x", port));

    std::uint32_t seed{0};
    CHECK(tcptap::parse_seed("0", seed) && seed == 0);
    CHECK(tcptap::parse_seed("1", seed) && seed == 1);
    CHECK(tcptap::parse_seed("4294967295", seed) && seed == 0xffffffffUL);
    CHECK(!tcptap::parse_seed("4294967296", seed));
    CHECK(!tcptap::parse_seed("-1", seed));
    CHECK(!tcptap::parse_seed("", seed));
    CHECK(!tcptap::parse_seed("12x", seed));

    std::string host;
    std::string up_port;
    CHECK(tcptap::split_upstream("example.com:80", host, up_port) && host == "example.com" &&
          up_port == "80");
    CHECK(!tcptap::split_upstream(":80", host, up_port));
    CHECK(!tcptap::split_upstream("host", host, up_port));
    CHECK(!tcptap::split_upstream("host:", host, up_port));
    CHECK(!tcptap::split_upstream("", host, up_port));

    tcptap::FaultDirection direction{tcptap::FaultDirection::Both};
    CHECK(tcptap::parse_direction("c>u", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(tcptap::parse_direction("u>c", direction) && direction == tcptap::FaultDirection::UToC);
    CHECK(tcptap::parse_direction("both", direction) && direction == tcptap::FaultDirection::Both);

    // A rejected call must leave *out* alone: the early return happens before
    // any assignment, and a test that ignored this could miss a future bug
    // where a near-miss case falls through into a partial write.
    direction = tcptap::FaultDirection::CToU;
    CHECK(!tcptap::parse_direction("c<u", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("u<c", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("Both", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("BOTH", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("C>U", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("c>", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction(">u", direction) && direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("both ", direction) &&
          direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction(" both", direction) &&
          direction == tcptap::FaultDirection::CToU);
    CHECK(!tcptap::parse_direction("bothx", direction) &&
          direction == tcptap::FaultDirection::CToU);

    CHECK(tcptap::applies_to_c_to_u(tcptap::FaultDirection::CToU));
    CHECK(!tcptap::applies_to_c_to_u(tcptap::FaultDirection::UToC));
    CHECK(tcptap::applies_to_c_to_u(tcptap::FaultDirection::Both));
    CHECK(tcptap::applies_to_u_to_c(tcptap::FaultDirection::UToC));
    CHECK(!tcptap::applies_to_u_to_c(tcptap::FaultDirection::CToU));
    CHECK(tcptap::applies_to_u_to_c(tcptap::FaultDirection::Both));

    if (g_failures != 0)
    {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
