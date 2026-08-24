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

/// The measured body at one tick, read straight off the engine's own state.
///
/// Sampled rather than recomputed, and that is the whole point of it. Every
/// one of these numbers is an input to a rule - the free-fall gate reads
/// `verticalAccel`, the air-time rules read `airborne` - so a timeline that
/// worked them out for itself could disagree with the engine and would then be
/// answering a different question from the one being asked of it.
///
/// Only the offline runner fills these. The game drives Engine directly and
/// never allocates a row.
struct BodySample {
    TimeMs timeMs{};
    ActorId actorId{};
    Vec3 comPosition{};
    Vec3 comVelocity{};
    /// |comVelocity|, in units/s. Carried rather than left to the reader
    /// because it is the number actually looked at.
    float speed{};
    float verticalSpeed{};
    float verticalAccel{};
    float fallDropUnits{};
    bool airborne{};
    /// When the current unsupported stretch began. The tick a flight is first
    /// seen on is up to 20 ms after it started, and a lane drawn on ticks puts
    /// its edge visibly off the launch.
    TimeMs airborneSinceMs{};
    /// False on a take with no pose sidecar, where every field above is zero
    /// and zero is a lie rather than a measurement.
    bool haveBodySamples{};
    Motion motion{};
    /// When the motion state was entered. Carried for the same reason
    /// `airborneSinceMs` is: the tick a state is first seen on is up to a frame
    /// after it began, and a lane drawn on ticks puts its edge visibly off the
    /// event.
    TimeMs motionEnteredMs{};
    /// How the last slide ended - and therefore what the mark at the end of the
    /// slide lane means. A slide leaves the state three ways and they sound
    /// nothing like each other, so a lane that drew only the span would be
    /// showing the least interesting half of it.
    SlideExit slideExit{};
    Moment moment{};
    /// Which contact the open hero window is anchored on, and when it opened.
    ///
    /// Carried because `moment` alone cannot tell one long hero from two back
    /// to back: a re-anchor keeps the moment open and moves what it is built
    /// around, so the seq changing mid-span is the only thing that says the
    /// window found something bigger. That is the difference between "one
    /// moment with peers" and "two separate events", which is exactly what
    /// EngineStats::heroReanchors exists to count.
    std::uint32_t heroSeq{};
    TimeMs heroSinceMs{};
};

struct OfflineResult {
    std::vector<Cue> cues;  ///< sorted by timeMs
    EngineStats stats;
    std::vector<TraceRecord> trace;  ///< empty unless traced
    TimeMs durationMs{};             ///< the recording's, not the audio's

    /// The audio runs past the last cue by the longest tail any of them has.
    TimeMs audioDurationMs{};

    /// How many times Engine::Tick was called, and how much game time those
    /// ticks covered. The cost of a run divided by these is the number that
    /// means something in the game - a frame's worth of engine work - where the
    /// wall time of a whole run is a property of how long this take happens to
    /// be. Both include the tail the runner ticks past the last row.
    std::uint32_t ticks{};
    TimeMs simulatedMs{};

    /// One row per tracked actor per tick, in tick order.
    std::vector<BodySample> body;
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
