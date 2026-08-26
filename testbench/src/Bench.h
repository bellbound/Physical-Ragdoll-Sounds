#pragma once

// "Run this take as fast as it will go, and say how fast that was."
//
// The question this answers is not "how long does the engine take" in the
// abstract - the testbench is not the game, this take is not a battlefield, and
// a number off this machine is not a frame budget on anybody else's. The
// question is *does changing a config cost anything*: two configs over the same
// take, measured the same way, back to back, so a setting that quietly doubles
// the work shows up as a percentage instead of as a feeling.
//
// Three decisions follow from that, and all three are about making the two
// numbers comparable rather than making either one impressive:
//
//   - **Tracing off.** Engine::SetTracing allocates a TraceRecord per decision
//     and the game never turns it on. The testbench does, because the timeline
//     is drawn from it - so the transport's own "RunOffline + mix" figure is
//     dominated by instrumentation the mod will never run. Benchmarking that
//     would rank configs by how much trace they generate.
//
//   - **Best of many, not the mean.** The noise here is one-sided: a scheduler
//     slice, a page fault or the audio callback can only ever make a run
//     slower, never faster. The fastest run is the one with the least of
//     somebody else's work in it, so it is the cleanest estimate of what the
//     code costs. The median is reported beside it because a large gap between
//     the two means the machine was busy and the comparison is not to be
//     trusted.
//
//   - **A wall-clock budget, not a run count.** The takes range from about a
//     second to the better part of ten, so a fixed number of runs would spend
//     eight times as long on one take as another and give the short ones far
//     fewer samples. A budget spends the same time on each and takes whatever
//     sample size that buys.
//
// Nothing here touches the audio, the mixer or the sfx bank's contents.
// RunOffline reseeds the bank and rewinds the recording on entry, so a
// benchmark leaves both exactly as it found them and the cue list on screen
// stays the one you are listening to.

#include <cstdint>
#include <string>
#include <vector>

#include "rds/Config.h"
#include "rds/Offline.h"
#include "rds/Recording.h"
#include "rds/SlotManifest.h"

namespace tb {

struct BenchOptions {
    /// How long to spend measuring, per config. Longer is a tighter number and
    /// a longer freeze - the run is synchronous on purpose (see RunBench).
    double budgetMs{500.0};

    /// Runs thrown away before the clock starts, so first-touch page faults, a
    /// cold allocator and cold branch predictors land outside the sample.
    int warmupRuns{3};

    /// A ceiling so a pathologically short take cannot spin for the whole
    /// budget collecting a hundred thousand samples nobody reads.
    int maxRuns{50000};

    std::uint32_t seed{1};

    /// Off, and the reason is the header comment. Exposed only because the
    /// headless mode can then show what the instrumentation costs.
    bool trace{false};

    /// The replay the transport is showing: the trim window and anything being
    /// pretended. Carried rather than defaulted because a bench that measured
    /// the whole capture while the window played eight seconds of it was timing
    /// a run nobody was listening to - and reporting a tick count and a realtime
    /// factor off that other run. `seed` and `trace` above still win over
    /// whatever this carries, so the two settings that are the benchmark's own
    /// stay in one place.
    rds::OfflineOptions replay{};
};

/// What one config cost over one take.
struct BenchResult {
    bool valid{};

    int runs{};
    double wallMs{};  ///< time actually spent measuring, warmup excluded

    double bestMs{};
    double medianMs{};
    double meanMs{};
    double worstMs{};

    /// Per run, from the run itself - so the "is it faster because it did less"
    /// question is answerable off the same line.
    std::uint32_t ticks{};
    rds::TimeMs simulatedMs{};
    std::uint32_t contactsIn{};
    std::uint32_t emittedCues{};

    std::string label;  ///< the config's name, for the report line

    /// How many times faster than the wall clock the engine replayed the fall.
    /// The headline number: 2000x means a second of ragdoll cost half a
    /// millisecond of CPU.
    [[nodiscard]] double RealtimeFactor() const;

    /// The figure that transfers to the game, where a tick is a frame. Per run
    /// the take's length is in the way; per tick it is not.
    [[nodiscard]] double UsPerTick() const;

    /// A spread over about 1.15 means something else was running and the two
    /// configs were not measured under the same conditions.
    [[nodiscard]] double Spread() const;
};

/// Measure `cfg` over `rec`, synchronously.
///
/// Synchronous, and therefore a visible freeze for `budgetMs`, because the
/// alternative is worse in both directions: a background thread would have to
/// own its own copy of the recording and the bank (RunOffline mutates both),
/// and it would be measuring against a UI thread that is drawing at the same
/// time - which is exactly the interference the best-of-many is trying to
/// avoid. A button labelled "benchmark" is allowed to stop the world for half a
/// second.
[[nodiscard]] BenchResult RunBench(rds::Recording& rec, const rds::AlgorithmConfig& cfg,
                                   rds::SoundBank& bank, const BenchOptions& opt = {});

}  // namespace tb
