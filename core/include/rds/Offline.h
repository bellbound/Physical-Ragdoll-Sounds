#pragma once

// The testbench's entire view of the backend.
//
// Hand it a recording and a config, get back the cue list the engine would have
// produced in the game. Deterministic for a given seed, which is what makes an
// A/B between two configs honest - it compares the configs, not two dice rolls.
//
// This is also why the testbench can apply a config change without restarting
// playback: it runs the whole recording again in a millisecond, mixes the new
// cue list, and swaps the buffer at the current play position. "Call the backend
// twice with different configs and swap playback source" is one function called
// twice.

#include <cstdint>
#include <string>
#include <vector>

#include "rds/Config.h"
#include "rds/Cue.h"
#include "rds/Engine.h"
#include "rds/Recording.h"

namespace rds {

struct OfflineResult {
    std::vector<Cue> cues;  ///< sorted by timeMs
    EngineStats stats;
    std::vector<TraceRecord> trace;  ///< empty unless traced
    TimeMs durationMs{};             ///< the recording's, not the audio's

    /// The audio runs past the last cue by the longest tail any of them has.
    TimeMs audioDurationMs{};
};

struct OfflineOptions {
    std::uint32_t seed{1};
    bool trace{true};

    /// Replay only the window the ragdoll is actually in. The capture files open
    /// before the knockdown and close after it, and the dead air at each end is
    /// not worth auditioning. Zero means the whole take.
    TimeMs startMs{};
    TimeMs endMs{};
};

/// Run `recording` through a fresh Engine with `config`.
///
/// Ticks at the recording's own frame boundaries, so the engine steps the way it
/// did in the game rather than at whatever rate the testbench feels like.
[[nodiscard]] OfflineResult RunOffline(Recording& recording, const AlgorithmConfig& config,
                                       SoundBank& bank, const OfflineOptions& options = {});

/// A quick self-check the testbench's `--verify` mode runs over the whole
/// research folder: does the engine produce the shape the design predicts?
///
/// Not a unit test - it is the "did we build the right thing" check. Each
/// assertion is one of the design's own numbers, and a failure means the
/// algorithm has drifted off the references, not that the code crashed.
struct VerifyExpectation {
    std::string name;
    bool passed{};
    std::string detail;
};

struct VerifyReport {
    std::string recordingStem;
    std::vector<VerifyExpectation> checks;
    EngineStats stats;
    [[nodiscard]] bool Passed() const;
};

/// The checks, each traceable to a measurement:
///   - four to six audible moments per knockdown, not fifteen to thirty
///   - reduction ratio near 10:1 against the contacts that entered
///   - no two impact onsets closer than the configured rate cap
///   - bursts of three to five grains inside 200-400 ms, then 300 ms of quiet
///   - the top one to three cues within a decibel of each other, then a cliff
///   - the sub layer arrives 55-75 ms after its transient, and is the loudest
///   - every knockdown closes with exactly one settle cue
///   - determinism: the same seed and config twice give a byte-identical list
[[nodiscard]] VerifyReport Verify(Recording& recording, const AlgorithmConfig& config,
                                  SoundBank& bank, const OfflineOptions& options = {});

}  // namespace rds
