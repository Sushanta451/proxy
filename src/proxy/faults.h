// The fault injector's settings and per-connection state. Nothing injects a
// fault yet; this gathers what the flags already parse and the RNG that
// already runs, so the injector added later reads one struct instead of three
// loose Options fields and a local in relay().

#pragma once

#include <cstdint>
#include <random>

namespace tcptap
{

// Which way a fault is allowed to travel. Nothing consumes this yet; it is
// plumbed through so the fault injector added later has somewhere to read it
// from instead of growing its own flag.
enum class FaultDirection
{
    CToU,
    UToC,
    Both
};

// What --seed and --direction decided, parsed into Options by parse_args.
struct FaultConfig
{
    FaultDirection direction{FaultDirection::Both};
    std::uint32_t seed{0};
    bool seeded{false};
};

// Per-connection state a fault needs across the life of one relay. Every
// random decision a fault makes comes from this one generator, so a run is
// reproduced exactly by passing the logged seed back with --seed. Each
// connection starts from the same seed. Nothing draws from it yet; the faults
// on the roadmap will.
struct FaultState
{
    explicit FaultState(const FaultConfig& config) : rng{config.seed}
    {
    }

    std::mt19937 rng;
};

} // namespace tcptap
