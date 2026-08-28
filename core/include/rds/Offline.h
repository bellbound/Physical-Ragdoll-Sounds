#pragma once

// The testbench's entire view of the backend: hand it a recording and a config,
// get back the cue list the engine would have produced in the game. Deterministic
// for a given seed, which is what makes an A/B honest.
//
// It is also why a config change needs no restart: the testbench runs the whole
// recording again in a millisecond, mixes the new cue list, and swaps the buffer
// at the current play position.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rds/Config.h"
#include "rds/Cue.h"
#include "rds/Engine.h"
#include "rds/Recording.h"

namespace rds {

/// The measured body at one tick, read straight off the engine's own state.
/// Sampled rather than recomputed: every one of these is an input to a rule, so a
/// timeline that worked them out for itself could disagree with the engine and
/// would be answering a different question.
///
/// Only the offline runner fills these; the game drives Engine directly.
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

    /// The garment, before and after its envelope.
    ///
    /// Both, because the envelope is most of the tuning and one value cannot
    /// show it working. The timeline draws the smoothed level as a filled curve
    /// with the raw drive as a line over it, and the gap between the two *is*
    /// the attack and the release - which is the only way to set them by eye
    /// rather than by guessing at milliseconds.
    float rustleDriveRaw{};
    float rustleDrive{};
    /// The damage rule's violence window. Drawn beside the garment because they
    /// are the same measurement held two ways, and the difference between them
    /// is the thing worth seeing: the garment tracks every spike including the
    /// impacts, the violence average deliberately ignores them.
    float motionViolence{};
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

/// The coverage sites a take can be re-dressed on, matching what the game reads
/// per biped slot. Fewer than there are limb sites, because several sites share
/// one slot: torso, upper arm and thigh are all "the body piece".
enum class CoverageSite : std::uint8_t {
    kHead = 0,
    kHands,
    kForearms,
    kFeet,
    kCalves,
    kBody,
    kCount
};

[[nodiscard]] std::string_view ToString(CoverageSite site);

/// Which coverage site a limb site is dressed from. The engine-side twin of the
/// plugin's `SlotForSite`, and it has to agree with it: if they disagree, a take
/// re-dressed in the testbench is not the body the game would have built.
[[nodiscard]] CoverageSite CoverageSiteFor(LimbSite site);

/// A representative MATERIAL_ID for a surface class, for pretending a take
/// happened on a different floor.
///
/// Rewriting the material rather than the resolved class on purpose: the class
/// is what `SurfaceFromMaterial` decides, so going in through the material means
/// the pretend path and the real path run the same mapping. A pretend that
/// bypassed it could disagree with the game about what stone sounds like.
[[nodiscard]] std::uint32_t RepresentativeMaterial(SurfaceClass surface);

struct OfflineOptions {
    std::uint32_t seed{1};
    bool trace{true};

    // -- pretending ----------------------------------------------------------
    //
    // Session-only, and never written to a config: coming back tomorrow to a
    // testbench that is quietly pretending, and tuning under it, is the worst
    // thing this control could do.

    /// Force every *world* contact onto this floor. `kCount` replays the take as
    /// it was.
    ///
    /// World contacts only. Self- and body-contacts are routed by `otherLimb`
    /// and forcing one onto stone would move it into a different branch of
    /// Ingest, which is a change to behaviour that has nothing to do with the
    /// surface.
    SurfaceClass surfaceAs{SurfaceClass::kCount};

    /// Re-dress each site, or `kCount` at that site to leave it as recorded.
    /// Indexed by `CoverageSite`, so "heavy boots, naked otherwise" is two
    /// entries and is the only way to test the per-limb rule at all.
    Coverage coverageAs[static_cast<std::size_t>(CoverageSite::kCount)]{};
    bool coverageSet[static_cast<std::size_t>(CoverageSite::kCount)]{};

    /// Replay the take as if the actor were in this state, or `kCount` to replay
    /// it as recorded. The third pretend, and the one the mode columns need:
    /// every take in the corpus is a knockdown, so without it the gameplay and
    /// combat columns can only be tuned in the game.
    ///
    /// Goes in as a phase and a combat flag on every row, not as a mode: the
    /// engine decides the mode from those two exactly as it does in the game, so
    /// what is being auditioned is the rule and not a testbench shortcut around
    /// it. That includes the ingest gate - pretending an upright actor with
    /// `GameIntegration:bAnimatedMode` off is a silent take, because that is what
    /// the game would do with one.
    ActorMode modeAs{ActorMode::kCount};

    /// Whether anything is being pretended, for the warning chip.
    [[nodiscard]] bool Pretending() const {
        if (surfaceAs != SurfaceClass::kCount || modeAs != ActorMode::kCount) {
            return true;
        }
        for (const bool set : coverageSet) {
            if (set) {
                return true;
            }
        }
        return false;
    }

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
[[nodiscard]] OfflineResult RunOffline(Recording& recording, const ConfigSet& config,
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
[[nodiscard]] VerifyReport Verify(Recording& recording, const ConfigSet& config,
                                  SoundBank& bank, const OfflineOptions& options = {});

}  // namespace rds
