// tcptap-unit — checks on the proxy's pure functions, run as a CTest case.
//
// No test framework: there are no dependencies, and the whole point of this
// project is to not reach for a wrapper around something small. CHECK records
// the failing file:line and keeps going; main exits non-zero if any fired.

#include "../proxy/options.h"

#include <cstdint>
#include <iostream>

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

    if (g_failures != 0)
    {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
