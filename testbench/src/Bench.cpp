#include "Bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace tb {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double SinceMs(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

}  // namespace

double BenchResult::RealtimeFactor() const {
    if (!valid || bestMs <= 0.0) return 0.0;
    return simulatedMs / bestMs;
}

double BenchResult::UsPerTick() const {
    if (!valid || ticks == 0) return 0.0;
    return bestMs * 1000.0 / static_cast<double>(ticks);
}

double BenchResult::Spread() const {
    if (!valid || bestMs <= 0.0) return 0.0;
    return medianMs / bestMs;
}

BenchResult RunBench(rds::Recording& rec, const rds::ConfigSet& cfg, rds::SoundBank& bank,
                     const BenchOptions& opt) {
    BenchResult out;

    rds::OfflineOptions run = opt.replay;
    run.seed = opt.seed;
    run.trace = opt.trace;

    // Warmup, discarded. The first run of a session pays for every page the
    // engine's arena has not touched yet, and on a take that runs in under a
    // millisecond that one run is worth more than the whole rest of the sample.
    for (int i = 0; i < std::max(0, opt.warmupRuns); ++i) {
        const rds::OfflineResult r = rds::RunOffline(rec, cfg, bank, run);
        // Take the shape of the run off the warmup rather than off a timed one:
        // these fields are identical every run by construction (same seed, same
        // config, same take) and reading them here keeps the timed loop down to
        // the call and the clock.
        out.ticks = r.ticks;
        out.simulatedMs = r.simulatedMs;
        out.contactsIn = r.stats.contactsIn;
        out.emittedCues = r.stats.emittedCues;
    }

    std::vector<double> samples;
    samples.reserve(1024);

    const auto t0 = Clock::now();
    const int cap = std::max(1, opt.maxRuns);
    while (static_cast<int>(samples.size()) < cap) {
        const auto r0 = Clock::now();
        const rds::OfflineResult r = rds::RunOffline(rec, cfg, bank, run);
        samples.push_back(SinceMs(r0));

        // Read once, from the last timed run, so a warmup count of zero still
        // fills the shape fields.
        out.ticks = r.ticks;
        out.simulatedMs = r.simulatedMs;
        out.contactsIn = r.stats.contactsIn;
        out.emittedCues = r.stats.emittedCues;

        if (SinceMs(t0) >= opt.budgetMs) break;
    }
    out.wallMs = SinceMs(t0);

    if (samples.empty()) return out;

    out.runs = static_cast<int>(samples.size());
    double sum = 0.0;
    for (const double s : samples) sum += s;
    out.meanMs = sum / out.runs;

    std::ranges::sort(samples);
    out.bestMs = samples.front();
    out.worstMs = samples.back();
    out.medianMs = samples[samples.size() / 2];
    out.valid = true;
    return out;
}

}  // namespace tb
