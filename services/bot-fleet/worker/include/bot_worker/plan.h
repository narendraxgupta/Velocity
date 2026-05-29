// =============================================================================
//  bot_worker/plan.h
//
//  In-memory representation of an active LoadPlan. We translate the
//  protobuf message into this denormalized struct once at the worker, then
//  every reactor thread reads from it lock-free (it's effectively const
//  during a run).
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace velocity::bot_worker {

enum class PersonaKind : std::uint8_t {
    MARKET_MAKER     = 0,
    AGGRESSIVE_TAKER = 1,
    CANCELLER        = 2,
    SPOOFER          = 3,
    NOISE            = 4,
    // ADAPTIVE delegates decision-making to an ONNX policy network
    // trained offline with PPO (see tools/rl-bot). The model is loaded
    // once per worker; per-bot inference is a single dense pass.
    ADAPTIVE         = 5,
};

struct PersonaSlice {
    PersonaKind kind;
    float       weight;   // fraction of total bot population, [0,1]
};

struct RampWaypoint {
    std::int64_t  offset_ns;   // since plan start
    std::uint64_t target_rps;
};

// One leg of a multi-venue plan. Mirrors bot.proto's VenueSpec but in
// the worker's native types so the hot loop doesn't dereference proto
// messages.
struct VenueLeg {
    std::string  venue_id;
    std::string  symbol;
    std::uint32_t weight{100};
    std::int64_t fair_value{0};
    std::int64_t tick_size{1};
    // Per-leg endpoint override; empty host ⇒ use the LoadPlan's primary
    // target_host/target_port.
    std::string  target_host;
    std::uint16_t target_port{0};
};

struct LoadPlan {
    std::string               submission_id;
    std::string               target_host;
    std::uint16_t             target_port{0};
    std::vector<PersonaSlice> personas;
    std::vector<RampWaypoint> ramp;
    std::uint32_t             bots_per_reactor{1024};
    // Default symbol — used when `venues` is empty (legacy single-venue
    // mode) and as the fallback for any leg that omits a symbol.
    std::string               symbol;
    std::int64_t              fair_value;   // fixed-point reference price
    std::int64_t              tick_size{1};

    // Multi-venue fan-out. When non-empty, bots are partitioned across
    // legs by `weight` and each bot stays on its assigned leg for the
    // run (so per-venue resting orders behave like real per-venue
    // traders). Empty list ⇒ legacy single-venue behaviour.
    std::vector<VenueLeg>     venues;
};

// Linearly interpolate the target RPS at `elapsed_ns` into the ramp.
[[nodiscard]] auto interp_rps(const std::vector<RampWaypoint>& ramp,
                              std::int64_t elapsed_ns) noexcept -> std::uint64_t;

}  // namespace velocity::bot_worker
