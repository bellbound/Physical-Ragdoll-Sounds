#include "Benchmark.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <string>

namespace rds::game {
namespace {

constexpr auto kIndex = [](Benchmark::Span span) {
    return static_cast<std::size_t>(span);
};

/// One span's line in the report.
///
/// The max carries the timestamp it happened at so it can be lined up against
/// the rest of the log - the idle summary, the per-knockdown line the engine
/// writes - which is the difference between "something spiked" and "the spike
/// was the frame that started forty voices".
[[nodiscard]] std::string Line(const char* label, const BenchSpan& span) {
    if (span.frames == 0) {
        return std::format("  {:<8} no frames sampled", label);
    }
    return std::format("  {:<8} mean {:7.2f} us   max {:8.2f} us at {:.0f} ms   over {} frames",
                       label, span.MeanUs(), span.maxUs, span.maxAtMs, span.frames);
}

}  // namespace

void BenchSpan::Add(double us, TimeMs nowMs) {
    if (frames == 0) {
        firstUs = us;
        ++frames;
        return;
    }
    sumUs += us;
    if (us > maxUs) {
        maxUs = us;
        maxAtMs = nowMs;
    }
    ++frames;
}

double BenchSpan::MeanUs() const {
    // frames counts the held-out first sample, which contributes nothing to the
    // sum - so the divisor is one less, and a single-frame span has no mean at
    // all rather than a mean of zero.
    return frames > 1 ? sumUs / static_cast<double>(frames - 1) : 0.0;
}

void Benchmark::Configure(const BenchmarkConfig& config) {
    m_config = config;
    m_armed = config.enabled;
    if (m_armed) {
        m_window = 1;
        spdlog::info(
            "benchmark: armed - reporting every {} knockdown(s) (or {} frames, whichever comes "
            "first) for the rest of the session",
            m_config.knockdowns, m_config.maxFrames);
    }
}

void Benchmark::KeepIfWorst(const SlowFrame& frame) {
    if (frame.totalUs <= m_worst[kWorstKept - 1].totalUs) {
        return;
    }
    std::size_t at = kWorstKept - 1;
    while (at > 0 && m_worst[at - 1].totalUs < frame.totalUs) {
        m_worst[at] = m_worst[at - 1];
        --at;
    }
    m_worst[at] = frame;
}

void Benchmark::BeginWindow() {
    for (std::size_t i = 0; i < kIndex(Span::kCount); ++i) {
        m_spans[i] = BenchSpan{};
        m_frameUs[i] = 0.0;
    }
    for (auto& frame : m_worst) {
        frame = SlowFrame{};
    }
    for (auto& bucket : m_buckets) {
        bucket = 0;
    }
    m_knockdowns = 0;
    m_slowFrames = 0;
    m_frameSecondsSum = 0.0;
    m_contactsAtStart = 0;
    m_cuesAtStart = 0;
    m_startedMs = TimeMs{};
    m_peakActors = 0;
    m_peakVoices = 0;
    m_peakCuesInFrame = 0;
    m_announcedUs = 0.0;
    ++m_window;

    // Left armed, and left *not* sampling. If the ceiling tripped mid-knockdown
    // the actor is still tracked - and `OnFrame` calls `OnTracking` after
    // `EndFrame`, so the rising edge re-opens sampling on the same frame that
    // closed the window. No frame falls between two windows, and a body that
    // never lets go does not have to before measuring resumes.
    m_sampling = false;
}

void Benchmark::Add(Span span, double us) noexcept {
    m_frameUs[kIndex(span)] += us;
    m_frameUs[kIndex(Span::kTotal)] += us;
}

void Benchmark::EndFrame(TimeMs nowMs, float frameSeconds, const EngineStats& stats,
                         const FrameLoad& load) {
    if (!m_sampling) {
        // No baseline is kept across a gap. The call site only fills `load`
        // while sampling, so what arrives here is zeroed - and differencing a
        // session-cumulative counter against zero would hand the next window's
        // first frame the whole session's work. Dropping it costs the deltas on
        // exactly one frame, and that frame is the one `BenchSpan` holds out of
        // the figures anyway.
        m_havePrevLoad = false;
        return;
    }

    // What this frame did, as a difference against the last one we saw. Guarded
    // because the first frame of a window has nothing to difference against, and
    // subtracting a zeroed baseline from a session-cumulative counter would hand
    // that frame the entire session's work.
    SlowFrame frame{};
    frame.totalUs = m_frameUs[kIndex(Span::kTotal)];
    frame.feedUs = m_frameUs[kIndex(Span::kFeed)];
    frame.engineUs = m_frameUs[kIndex(Span::kEngine)];
    frame.renderUs = m_frameUs[kIndex(Span::kRender)];
    frame.atMs = nowMs;
    frame.trackedActors = load.trackedActors;
    frame.liveVoices = load.liveVoices;
    if (m_havePrevLoad) {
        frame.contacts = load.contactsIn - m_prevLoad.contactsIn;
        frame.cues = load.cuesOut - m_prevLoad.cuesOut;
        frame.voices = load.voicesOut - m_prevLoad.voicesOut;
    }
    m_prevLoad = load;
    m_havePrevLoad = true;

    m_peakActors = std::max(m_peakActors, load.trackedActors);
    m_peakVoices = std::max(m_peakVoices, load.liveVoices);
    m_peakCuesInFrame = std::max(m_peakCuesInFrame, frame.cues);

    // `BenchSpan::Add` holds the window's first sample out of its mean and its
    // max, so the table and the histogram below hold it out too. They are read
    // side by side with those figures, and a worst-frames table led by a frame
    // the max above says did not happen reads as a contradiction rather than as
    // the warmup it is. It keeps its own line in the report either way.
    const bool isFirstSample = m_spans[kIndex(Span::kTotal)].frames == 0;

    // Read before folding, because the slow count is a property of the frame
    // just closed and not of the running maximum - a threshold crossed on three
    // frames has to count three times, and a max only ever records the worst.
    const double frameTotalUs = frame.totalUs;
    if (m_config.slowFrameUs > 0.0f && frameTotalUs > static_cast<double>(m_config.slowFrameUs)) {
        ++m_slowFrames;

        // One line per new record, not one per slow frame. The last run had 529
        // frames over the threshold and 529 lines would bury the log it is meant
        // to be read against - whereas a new worst is rare, monotonic, and is
        // the frame somebody chasing a stutter actually wants to find. It goes
        // out live rather than waiting for the report so it lands *between* the
        // `placed:` and `knockdown:` lines for the moment it happened on.
        if (!isFirstSample && frameTotalUs > m_announcedUs) {
            m_announcedUs = frameTotalUs;
            spdlog::info("benchmark: slowest frame yet this window - {:.1f} us at {:.0f} ms "
                         "(feed {:.1f} / engine {:.1f} / render {:.1f}); this frame carried "
                         "{} contact(s) -> {} cue(s) -> {} voice(s), with {} actor(s) tracked "
                         "and {} voice(s) live",
                         frameTotalUs, static_cast<double>(nowMs), frame.feedUs, frame.engineUs,
                         frame.renderUs, frame.contacts, frame.cues, frame.voices,
                         frame.trackedActors, frame.liveVoices);
        }
    }

    if (!isFirstSample) {
        KeepIfWorst(frame);

        std::size_t bucket = kBucketCount - 1;
        for (std::size_t i = 0; i < std::size(kBucketEdgesUs); ++i) {
            if (frameTotalUs < kBucketEdgesUs[i]) {
                bucket = i;
                break;
            }
        }
        ++m_buckets[bucket];
    }

    for (std::size_t i = 0; i < kIndex(Span::kCount); ++i) {
        m_spans[i].Add(m_frameUs[i], nowMs);
        m_frameUs[i] = 0.0;
    }

    m_frameSecondsSum += frameSeconds > 0.0f ? static_cast<double>(frameSeconds) : 0.0;

    if (m_config.maxFrames > 0 &&
        m_spans[kIndex(Span::kTotal)].frames >= static_cast<std::uint32_t>(m_config.maxFrames)) {
        Report(stats, "frame ceiling reached mid-knockdown - a partial run");
    }
}

void Benchmark::OnTracking(bool tracking, TimeMs nowMs, const EngineStats& stats) {
    if (!m_armed) {
        m_wasTracking = tracking;
        return;
    }

    // Rising edge: the first body of this measured window has gone down.
    if (tracking && !m_sampling) {
        m_sampling = true;
        if (m_knockdowns == 0) {
            m_startedMs = nowMs;
            m_contactsAtStart = stats.contactsIn;
            m_cuesAtStart = stats.emittedCues;
        }
    }

    // Falling edge: the last one has let go. `OnFrame` writes its idle line off
    // this same transition, so the two land together in the log.
    if (!tracking && m_wasTracking && m_sampling) {
        m_sampling = false;
        ++m_knockdowns;
        if (m_knockdowns >= static_cast<std::uint32_t>(std::max(1, m_config.knockdowns))) {
            Report(stats, "complete");
        }
    }

    m_wasTracking = tracking;
}

void Benchmark::Report(const EngineStats& stats, const char* why) {
    m_sampling = false;

    const BenchSpan& total = m_spans[kIndex(Span::kTotal)];
    if (total.frames == 0) {
        spdlog::info("benchmark: nothing was sampled - no actor was ever tracked");
        BeginWindow();
        return;
    }

    const double meanFrameMs =
        total.frames > 1 ? m_frameSecondsSum * 1000.0 / static_cast<double>(total.frames) : 0.0;

    spdlog::info("benchmark #{}: {} knockdown(s), {} frames, {} contacts -> {} cues ({})", m_window,
                 m_knockdowns, total.frames, stats.contactsIn - m_contactsAtStart,
                 stats.emittedCues - m_cuesAtStart, why);
    spdlog::info("{}", Line("feed", m_spans[kIndex(Span::kFeed)]));
    spdlog::info("{}", Line("engine", m_spans[kIndex(Span::kEngine)]));
    spdlog::info("{}", Line("render", m_spans[kIndex(Span::kRender)]));
    spdlog::info("{}", Line("frame", total));

    // Only the first window's first frame is a cold one. Every window holds one
    // out the same way so the means stay comparable between reports, but calling
    // a hot frame "warmup" in window seven would be a lie.
    if (m_window <= 1) {
        spdlog::info("  first sampled frame {:.2f} us, held out of the figures above. It is one "
                     "frame's warmup and not the whole of it - the renderer's cold cost lands on "
                     "its first cue, which is later, and is in the maxima above",
                     total.firstUs);
    } else {
        spdlog::info("  first sampled frame {:.2f} us, held out of the figures above - held out "
                     "for consistency with window 1, not because it is cold; nothing here is cold "
                     "any more",
                     total.firstUs);
    }

    if (meanFrameMs > 0.0) {
        spdlog::info("  worst frame is {:.3f}% of the {:.2f} ms the game was actually spending "
                     "per frame; {} frame(s) went over {:.0f} us",
                     total.maxUs / (meanFrameMs * 1000.0) * 100.0, meanFrameMs, m_slowFrames,
                     m_config.slowFrameUs);
    }

    // What the window was actually doing. The spans say what it cost; without
    // this there is no way to tell a cheap window from a fast one, and "it got
    // quicker" is the same shape of number as "it did less".
    const std::uint32_t contacts = stats.contactsIn - m_contactsAtStart;
    const std::uint32_t cues = stats.emittedCues - m_cuesAtStart;
    spdlog::info("  work  {} contact(s) -> {} cue(s) ({:.2f} per contact) over {:.1f} s of frames; "
                 "peak {} actor(s) tracked, {} voice(s) live, {} cue(s) on one frame",
                 contacts, cues,
                 contacts != 0 ? static_cast<double>(cues) / static_cast<double>(contacts) : 0.0,
                 m_frameSecondsSum, m_peakActors, m_peakVoices, m_peakCuesInFrame);
    spdlog::info("  arbitration  dropped rate {} chain {} mask {} burst {} | rejected blowup {} "
                 "floor {} mirror {} | self {} | heroes {}",
                 stats.droppedRateCap, stats.droppedChainMerge, stats.droppedMasking,
                 stats.droppedBurstCap, stats.rejectedBlowup, stats.rejectedBelowFloor,
                 stats.droppedMirror, stats.droppedSelfContact, stats.heroes);

    // The shape the mean and the max between them cannot show: whether this is a
    // cheap frame that spikes occasionally or an expensive one throughout.
    {
        std::string line = "  distribution ";
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            if (i < std::size(kBucketEdgesUs)) {
                line += std::format("<{:.0f}us {}  ", kBucketEdgesUs[i], m_buckets[i]);
            } else {
                line += std::format(">={:.0f}us {}", kBucketEdgesUs[i - 1], m_buckets[i]);
            }
        }
        spdlog::info("{}", line);
    }

    // Where the stutters were. Five, so a lone freak frame can be told from the
    // top of a run of them, and each with the timestamp to go and read the
    // `placed:` and `knockdown:` lines around it.
    if (m_worst[0].totalUs > 0.0) {
        spdlog::info("  worst frames - us at ms, then feed/engine/render, then what that frame "
                     "carried:");
        for (const SlowFrame& frame : m_worst) {
            if (frame.totalUs <= 0.0) {
                break;
            }
            spdlog::info("    {:8.1f} us at {:>9.0f} ms   {:6.1f} / {:6.1f} / {:8.1f}   "
                         "{} contact(s) -> {} cue(s) -> {} voice(s), {} actor(s), {} live",
                         frame.totalUs, static_cast<double>(frame.atMs), frame.feedUs,
                         frame.engineUs, frame.renderUs, frame.contacts, frame.cues, frame.voices,
                         frame.trackedActors, frame.liveVoices);
        }
    }

    spdlog::info("  a steady_clock tick is ~100 ns here, so a single frame's figure carries about "
                 "that much noise and the means are the solid half. The three spans also pay for "
                 "the clock reads that bound them - roughly 0.1 us a frame, which is not nothing "
                 "against a 1 us span.");

    BeginWindow();
}

}  // namespace rds::game
