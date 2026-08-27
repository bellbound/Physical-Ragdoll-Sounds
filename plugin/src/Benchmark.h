#pragma once

// What the mod costs the frame it runs on, measured in the game.
//
// The testbench's benchmark is the better instrument for what it covers, but it
// times `RunOffline` and only that, so two of the three things the mod does every
// frame have never been measured:
//
//   feed    walking every tracked ragdoll's limbs to publish a tick (GameFeed)
//   engine  ingest, arbitration, the strategies - the half the testbench times
//   render  turning cues into voices in Skyrim's own sound manager
//
// The third is the one to watch: a knockdown can put hundreds of cues through it,
// each a call into game code we do not own.
//
// **It reports the worst frame, not the average.** A frame budget is missed by
// one frame, and the one that misses it is the frame with a fat manifold on it
// and a burst going out. Mean is reported too, since a max cannot be read alone.
//
// Game thread only, which is why nothing here is atomic and nothing takes a lock.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "rds/Config.h"
#include "rds/Engine.h"
#include "rds/Types.h"

namespace rds::game {

/// One timer's accumulated samples. Fixed size, no allocation, no history: a mean
/// and a max answer the question, and real percentiles are what you build *after*
/// a max says there is something to look at.
struct BenchSpan {
    std::uint32_t frames{};
    double sumUs{};
    double maxUs{};
    TimeMs maxAtMs{};

    /// The first sample of the run, held out of `sumUs` and `maxUs`.
    ///
    /// An early frame pays for untouched arena pages and a cold branch predictor
    /// and can sit orders of magnitude above the steady state. Thrown away it
    /// would hide a real cost; left in it would *be* the max on most runs. So it
    /// gets its own line.
    ///
    /// **Not a complete cold-start figure.** Sampling opens on the frame *after*
    /// the first actor is tracked, and the renderer's cold cost lands on whichever
    /// frame its first cue does. Both show up in `maxUs`, and should.
    double firstUs{};

    void Add(double us, TimeMs nowMs);
    [[nodiscard]] double MeanUs() const;
};

/// What the frame was carrying, sampled at the end of it. A microsecond count on
/// its own says a frame was slow and nothing about why; these are the quantities
/// the three spans scale with, so a slow frame reads as "the frame that started
/// forty voices".
///
/// The three counters are **cumulative** totals as those objects report them; the
/// benchmark differences them itself, which keeps the call site a straight read
/// of three getters with no state of its own to get wrong.
struct FrameLoad {
    std::uint32_t trackedActors{};
    std::uint32_t liveVoices{};
    std::uint32_t contactsIn{};
    std::uint32_t cuesOut{};
    std::uint32_t voicesOut{};
};

/// One frame worth keeping: the worst few of a window, with enough context to go
/// and look at what else the log says was happening at `atMs`.
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

    /// Read the config once, at data load. Nothing turns it on later, because a
    /// benchmark that starts mid-session has already missed the cold start it
    /// exists to report.
    ///
    /// Enabled means enabled for the session. A window ends on `iKnockdowns` or
    /// `iMaxFrames`, but that is the end of a *window*: the counters clear and the
    /// next window opens on the next knockdown, so a session yields a series of
    /// reports to compare rather than one.
    void Configure(const BenchmarkConfig& config);

    /// Whether a clock should be read this frame. One bool, and it is false in
    /// every shipping install.
    [[nodiscard]] bool Sampling() const noexcept { return m_sampling; }

    /// Fold one measured span into the frame in progress.
    void Add(Span span, double us) noexcept;

    /// Close the frame: the spans folded into it become one sample each, and the
    /// per-frame total one of its own. `frameSeconds` is the game's own delta, so
    /// the report can put its worst frame against the budget that frame actually
    /// had rather than an assumed 90 Hz.
    ///
    /// Takes the stats because the frame ceiling is reached here, and a report
    /// waiting for the next tracked/idle edge would never come for the ragdoll
    /// that tripped the ceiling by never letting go.
    void EndFrame(TimeMs nowMs, float frameSeconds, const EngineStats& stats,
                  const FrameLoad& load);

    /// Driven off the same tracked/idle edge `OnFrame` computes for its idle line.
    /// Sampling opens when the first actor is tracked, a knockdown is counted when
    /// the last lets go, and enough of them writes the report.
    ///
    /// **The window lags the knockdown by a frame at each end.** The tracked count
    /// is only known after the tick, so the first tracked frame closes before
    /// sampling opens and the frame that discovers the last actor has gone is
    /// sampled before this closes it. Both are real frames of the mod's work, so
    /// the count comes out right.
    void OnTracking(bool tracking, TimeMs nowMs, const EngineStats& stats);

private:
    void Report(const EngineStats& stats, const char* why);

    /// Clear the counters and open the next window. Everything the report is a
    /// function of has to go, or window two is window one plus a bit.
    ///
    /// `m_wasTracking` survives: it is the edge detector's memory of the world
    /// outside this class, and clearing it would manufacture a rising edge on the
    /// next frame of a knockdown that had already started.
    void BeginWindow();

    /// Slot one frame into `m_worst`, biggest first, dropping the smallest.
    void KeepIfWorst(const SlowFrame& frame);

    BenchmarkConfig m_config{};
    bool m_armed{};     ///< configured on; stays on for the session
    bool m_sampling{};  ///< armed, and an actor is being tracked right now
    bool m_wasTracking{};

    /// Which window is being measured, 1-based. In the log so two reports can be
    /// told apart, and because only window 1's held-out first frame is a cold one.
    std::uint32_t m_window{};

    BenchSpan m_spans[static_cast<std::size_t>(Span::kCount)]{};
    double m_frameUs[static_cast<std::size_t>(Span::kCount)]{};

    std::uint32_t m_knockdowns{};
    std::uint32_t m_slowFrames{};
    double m_frameSecondsSum{};

    /// The worst few frames of the window, biggest first. Five is enough to see
    /// whether the max is one freak frame or the top of a run; the real histogram
    /// is `m_buckets`.
    static constexpr std::size_t kWorstKept = 5;
    SlowFrame m_worst[kWorstKept]{};

    /// Fixed buckets of total frame cost, in us. A mean and a max cannot tell a
    /// clean 40 us frame that spikes once from one at 200 us all the time, which
    /// is the whole question when a stutter is being chased. Edges are the last
    /// value each bucket holds; the last bucket is everything above.
    static constexpr double kBucketEdgesUs[] = {50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0};
    static constexpr std::size_t kBucketCount = std::size(kBucketEdgesUs) + 1;
    std::uint32_t m_buckets[kBucketCount]{};

    /// Previous frame's cumulative counters, so a frame's own work is a difference
    /// rather than something the call site tracks.
    FrameLoad m_prevLoad{};
    bool m_havePrevLoad{};

    /// Peaks over the window, for the "what it was doing" line.
    std::uint32_t m_peakActors{};
    std::uint32_t m_peakVoices{};
    std::uint32_t m_peakCuesInFrame{};

    /// The worst frame already announced live, so the commentary is one line per
    /// new record rather than one per slow frame.
    double m_announcedUs{};

    /// The engine's counters when sampling started, so the report can say what
    /// work the measured window held. `EngineStats` is cumulative for the session,
    /// and a total including a knockdown from before the benchmark armed answers a
    /// different question.
    std::uint32_t m_contactsAtStart{};
    std::uint32_t m_cuesAtStart{};
    TimeMs m_startedMs{};
};

/// Times one span for as long as it is in scope. A scope rather than a pair of
/// calls so a span cannot be opened and not closed, and an early return cannot
/// drop a sample. One bool test at each end when the benchmark is off.
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
