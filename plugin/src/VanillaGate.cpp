#include "VanillaGate.h"

#include <spdlog/spdlog.h>

#include <atomic>

namespace rds::game {
namespace {

/// Room for every actor that can claim at once. Claims come only from tracked
/// actors that are ragdolling or getting up, so this is a brawl's worth and then
/// some - `highActorHandles` runs to a few dozen and almost none of them are on
/// the floor. Overflow is not silent: it means an actor we are playing keeps
/// vanilla's impacts underneath ours, which is audible.
constexpr std::size_t kMaxClaims = 64;

struct Claim {
    std::atomic<float> x{};
    std::atomic<float> y{};
    std::atomic<float> z{};
};

Claim g_claims[kMaxClaims];

/// How many entries of `g_claims` the reader may look at. The publication order
/// is what makes this safe without a lock: zeroed first, filled, then raised.
std::atomic<std::size_t> g_count{0};

std::size_t g_filling = 0;

std::atomic<float> g_radiusSq{150.0f * 150.0f};

void WarnOnceAboutRoom() {
    static bool said = false;  // game thread only
    if (said) {
        return;
    }
    said = true;
    spdlog::warn("vanilla gate: more than {} actors are down at once, so the ones past that keep "
                 "vanilla's body impacts underneath ours",
                 kMaxClaims);
}

}  // namespace

void BeginVanillaGate() {
    g_filling = 0;
    // Release, so the reader that sees the zero cannot then see a stale claim
    // from before it.
    g_count.store(0, std::memory_order_release);
}

void AddVanillaGate(float x, float y, float z) {
    if (g_filling >= kMaxClaims) {
        WarnOnceAboutRoom();
        return;
    }
    Claim& claim = g_claims[g_filling];
    claim.x.store(x, std::memory_order_relaxed);
    claim.y.store(y, std::memory_order_relaxed);
    claim.z.store(z, std::memory_order_relaxed);
    ++g_filling;
}

void CommitVanillaGate() { g_count.store(g_filling, std::memory_order_release); }

void ClearVanillaGate() {
    g_filling = 0;
    g_count.store(0, std::memory_order_release);
}

void SetVanillaGateRadius(float units) {
    const float clamped = units > 0.0f ? units : 0.0f;
    g_radiusSq.store(clamped * clamped, std::memory_order_relaxed);
}

bool VanillaGateCovers(float x, float y, float z) {
    const std::size_t count = g_count.load(std::memory_order_acquire);
    if (count == 0) {
        return false;
    }
    const float radiusSq = g_radiusSq.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        const Claim& claim = g_claims[i];
        const float dx = x - claim.x.load(std::memory_order_relaxed);
        const float dy = y - claim.y.load(std::memory_order_relaxed);
        const float dz = z - claim.z.load(std::memory_order_relaxed);
        if (dx * dx + dy * dy + dz * dz <= radiusSq) {
            return true;
        }
    }
    return false;
}

}  // namespace rds::game
