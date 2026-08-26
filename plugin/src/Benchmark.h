#pragma once

// What the mod costs the frame it runs on, measured in the game.
//
// The testbench has a benchmark already and it is the better instrument: it
// replays a take thousands of times, discards warmup runs and reports a best-of.
// This is not that, and it is not trying to be. It exists because the testbench
// times `RunOffline` and *only* `RunOffline`, so two of the three things this
// mod does every frame have never been measured at all:
//
//   feed    walking every tracked ragdoll's limbs to publish a tick (GameFeed)
//   engine  ingest, arbitration, the strategies - the half the testbench times
//   render  turning cues into voices in Skyrim's own sound manager
//
// The third is the one to watch. A knockdown can put hundreds of cues through
// it, each a call into game code we do not own, and no figure anywhere covered
// that until this.
//
// **The number it reports is the worst frame, not the average one.** A realtime
// factor or a mean per tick answers "is the engine fast", which was never in
// doubt; a frame budget is missed by one frame, and the one that misses it is
// the frame with a fat manifold on it and a burst going out. Mean is reported
// too, because a max with no mean beside it cannot be read.
//
// Game thread only. Every method is called from OnFrame and from nowhere else,
// which is why nothing here is atomic and nothing takes a lock.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "rds/Config.h"
#include "rds/Engine.h"
#include "rds/Types.h"

namespace rds::game {

/// One timer's accumulated samples. Fixed size, no allocation, no history: a
/// mean and a max answer the question, and the histogram that would give real
/// percentiles is what you build *after* a max says there is something to look
/// at.
struct BenchSpan {
    std::uint32_t frames{};
    double sumUs{};
    double maxUs{};
    TimeMs maxAtMs{};

    /// The first sample of the run, held out of `sumUs` and `maxUs` rather than
    /// mixed into them.
    ///
    /// An early frame of a knockdown pays for pages the engine's arena has not
    /// touched and a cold branch predictor, and can sit orders of magnitude
    /// above the steady state - the testbench answers the same problem by
    /// throwing three warmup runs away (`Bench.h`). Thrown away here it would
    /// hide a real cost; left in it would *be* the max on most runs and the
    /// steady state would be unreadable. So it is reported on its own line and
    /// excluded from the other two.
    ///
    /// **It is not a complete cold-start figure and must not be read as one.**
    /// Sampling opens on the frame *after* the first actor is tracked (see
    /// `OnTracking`), and the renderer's own cold cost lands on whichever frame
    /// its first cue does, which is later still. Both of those show up in
    /// `maxUs` - and should. What this isolates is one frame's worth of warmup,
    /// not all of it.
    double firstUs{};

    void Add(double us, TimeMs nowMs);
    [[nodiscard]] double MeanUs() const;
};

/// What the frame was carrying, sampled at the end of it.
///
/// A microsecond count on its own says a frame was slow and nothing about why.
/// These are the quantities the three spans actually scale with, so a slow frame
/// can be read as "the frame that started forty voices" instead of as a number.
///
/// The three counters are the engine's and the renderer's **cumulative** totals,
/// exactly as those objects report them; the benchmark differences them against
/// the previous frame itself. Passing cumulative values keeps the call site a
/// straight read of three getters with no state of its own to get wrong.
struct FrameLoad {
    std::uint32_t trackedActors{};
    std::uint32_t liveVoices{};
    std::uint32_t contactsIn{};
    std::uint32_t cuesOut{};
    std::uint32_t voicesOut{};
};

/// One frame worth keeping: the worst few of a window, with enough context to
/// go and look at what else the log says was happening at `atMs`.
struct SlowFrame {
    double totalUs{};
    double feedUs{};
    double engineUs{};
    double renderUs{};
    TimeMs atMs{};
    std::uint32_t trackedActors{};
    std::uint32_t liveVoices{};
    std::uint32_t contacts{};  ///< ingested *on this frame*, not cumulative
    std::uint32_t cues{};
    std::uint32_t voices{};
};

class Benchmark {
public:
    /// The three spans, plus the per-frame sum of them - which is not the sum of
    /// their maxima and has to be accumulated separately, because the frame that
    /// is worst overall is usually not worst in any one span.
    enum class Span : std::uint32_t { kFeed = 0, kEngine, kRender, kTotal, kCount };

    /// Read the config once, at data load. Anything but `enabled` leaves this
    /// inert for the session: there is no path that turns it on later, because a
    /// benchmark that starts mid-session has already missed the cold start it
    /// exists to report.
    ///
    /// Enabled means enabled for the session. A window still ends where it
    /// always did - `iKnockdowns` counted, or `iMaxFrames` reached - but the
    /// report is the end of a *window*, not of the benchmark: the counters clear
    /// and the next window opens on the next knockdown. So a session yields a
    /// series of reports to compare against each other rather than one.
    void Configure(const BenchmarkConfig& config);

    /// Whether a clock should be read this frame. One bool, and it is false in
    /// every shipping install.
    [[nodiscard]] bool Sampling() const noexcept { return m_sampling; }

    /// Fold one measured span into the frame in progress.
    void Add(Span span, double us) noexcept;

    /// Close the frame: the spans folded into it become one sample each, and the
    /// per-frame total becomes one of its own.
    ///
    /// `frameSeconds` is the game's own delta, so the report can put its worst
    /// frame against the budget that frame actually had rather than against an
    /// assumed 90 Hz.
    ///
    /// Takes the stats because this is one of the two places the run can end:
    /// the frame ceiling is reached here, and a report that had to wait for the
    /// next tracked/idle edge would be a report that never came for the ragdoll
    /// that tripped the ceiling by never letting go.
    void EndFrame(TimeMs nowMs, float frameSeconds, const EngineStats& stats,
                  const FrameLoad& load);

    /// Driven off the same tracked/idle edge `OnFrame` already computes for its
    /// idle line. Sampling opens when the first actor is tracked, a knockdown is
    /// counted when the last one lets go, and when enough have been counted the
    /// report is written and the next window opens.
    ///
    /// **The window lags the knockdown by a frame at each end**, and it is worth
    /// knowing which frames are in it. The tracked count is only known after the
    /// tick, so this runs at the end of a frame: the first tracked frame closes
    /// before sampling opens and is missed, and the frame that discovers the last
    /// actor has gone is sampled before this closes it. Both are real frames of
    /// the mod's work - one registering an actor, one releasing it - so the count
    /// comes out right even though neither end is the frame it looks like.
    void OnTracking(bool tracking, TimeMs nowMs, const EngineStats& stats);

private:
    void Report(const EngineStats& stats, const char* why);

    /// Clear the counters and open the next window. Everything the report is a
    /// function of has to go, or window two would be window one plus a bit.
    ///
    /// `m_wasTracking` deliberately survives: it is the edge detector's memory
    /// of the world outside this class, not part of the window, and clearing it
    /// would manufacture a rising edge on the next frame of a knockdown that had
    /// already started.
    void BeginWindow();

    /// Slot one frame into `m_worst`, biggest first, dropping the smallest.
    void KeepIfWorst(const SlowFrame& frame);

    BenchmarkConfig m_config{};
    bool m_armed{};     ///< configured on; stays on for the session
    bool m_sampling{};  ///< armed, and an actor is being tracked right now
    bool m_wasTracking{};

    /// Which window is being measured, 1-based. In the log so two reports can be
    /// told apart, and because only window 1's held-out first frame is a cold
    /// one - see `BenchSpan::firstUs`.
    std::uint32_t m_window{};

    BenchSpan m_spans[static_cast<std::size_t>(Span::kCount)]{};
    double m_frameUs[static_cast<std::size_t>(Span::kCount)]{};

    std::uint32_t m_knockdowns{};
    std::uint32_t m_slowFrames{};
    double m_frameSecondsSum{};

    /// The worst few frames of the window, biggest first. Five because the point
    /// is to see whether the max is one freak frame or the top of a run of them,
    /// and five answers that; a real histogram is `m_buckets`.
    static constexpr std::size_t kWorstKept = 5;
    SlowFrame m_worst[kWorstKept]{};

    /// Fixed buckets of total frame cost, in us. A mean and a max cannot tell a
    /// clean 40 us frame that spikes once from one that is at 200 us all the
    /// time, and that difference is the whole question when a stutter is being
    /// chased. Edges are the last value each bucket holds; the last bucket is
    /// everything above the last edge.
    static constexpr double kBucketEdgesUs[] = {50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0};
    static constexpr std::size_t kBucketCount = std::size(kBucketEdgesUs) + 1;
    std::uint32_t m_buckets[kBucketCount]{};

    /// Previous frame's cumulative counters, so a frame's own work is a
    /// difference rather than something the call site has to track.
    FrameLoad m_prevLoad{};
    bool m_havePrevLoad{};

    /// Peaks over the window, for the "what it was doing" line.
    std::uint32_t m_peakActors{};
    std::uint32_t m_peakVoices{};
    std::uint32_t m_peakCuesInFrame{};

    /// The worst frame already announced live, so the running commentary is one
    /// line per new record rather than one per slow frame.
    double m_announcedUs{};

    /// The engine's counters when sampling started, so the report can say what
    /// work the measured window actually held. `EngineStats` is cumulative for
    /// the session and a total that includes a knockdown before the benchmark
    /// armed would be answering a different question - the same "is it faster
    /// because it did less" check `Bench.h` keeps its counters for.
    std::uint32_t m_contactsAtStart{};
    std::uint32_t m_cuesAtStart{};
    TimeMs m_startedMs{};
};

/// Times one span for as long as it is in scope.
///
/// A scope rather than a pair of calls so a span cannot be opened and not
/// closed, and so an early return out of the frame path cannot silently drop a
/// sample. Costs one bool test at each end when the benchmark is off, which is
/// what a shipping install pays for having this compiled in.
class BenchScope {
public:
    BenchScope(Benchmark& bench, Benchmark::Span span) noexcept
        : m_bench(bench.Sampling() ? &bench : nullptr), m_span(span) {
        if (m_bench != nullptr) {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ~BenchScope() {
        if (m_bench != nullptr) {
            const auto elapsed = std::chrono::steady_clock::now() - m_start;
            m_bench->Add(m_span,
                         std::chrono::duration<double, std::micro>(elapsed).count());
        }
    }

    BenchScope(const BenchScope&) = delete;
    BenchScope& operator=(const BenchScope&) = delete;

private:
    Benchmark* m_bench;
    Benchmark::Span m_span;
    std::chrono::steady_clock::time_point m_start;
};

}  // namespace rds::game
