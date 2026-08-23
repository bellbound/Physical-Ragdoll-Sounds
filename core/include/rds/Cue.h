#pragma once

// The other clean seam: Stage 4 emits an abstract cue list, and something else
// turns it into voices. The game renders it through Skyrim's audio, the
// testbench through miniaudio, both implementing the same limited feature set.
//
// That feature set is deliberately what CommonLibVR actually offers, verified
// (§13): volume, continuous pitch, an updatable position, attachment to a bone,
// whole-file looping, fade in/out over N ms, and playing a raw wav off disk.
// Not available: per-voice filtering or EQ, reverb send control, and
// sample-accurate scheduling - voices start on frame boundaries, so the layer
// offsets quantise to 7-20 ms.
//
// Nothing richer belongs here. If the testbench can do something the game
// cannot, we will tune something the game cannot reproduce.

#include <cstdint>
#include <string_view>
#include <vector>

#include "rds/Types.h"
#include "rds/SlotManifest.h"

namespace rds {

enum class CueOp : std::uint8_t {
    kPlayOneShot = 0,
    kStartLoop,
    kUpdateLoop,  ///< new gain/pitch/position for a running loop
    kStopLoop,
};

/// Why this cue exists. Provenance only - the engine never branches on it - but
/// it is what makes the testbench's timeline readable and what turns a "that
/// sounded wrong" into a line in the log.
enum class CueReason : std::uint8_t {
    kImpactComposite = 0,
    kSurfaceSkin,
    kHeadImpact,
    kCrunch,
    kGore,
    kLimbTap,
    kScrape,
    kFoleyBed,
    kAirborneRise,
    kSettleClose,
};

[[nodiscard]] std::string_view ToString(CueReason r);

/// One voice instruction.
struct Cue {
    TimeMs timeMs{};  ///< absolute on the engine clock, including the layer offset
    CueOp op{};

    SlotId slot{};
    std::uint8_t variant{};  ///< which file of the slot, already resolved through the fallbacks

    float gainDb{};  ///< final, after intensity, layer balance, phase budget, distance and mix
    float pitch{1.0f};
    float fadeMs{};

    Vec3 position{};
    /// >= 0 attaches the voice to that limb index and the position is ignored.
    /// The player's own ragdoll uses this; NPCs collapse to a world point during
    /// a hero moment and spread back out during the tumble.
    std::int32_t boneIndex{-1};

    /// Identifies a loop across kStartLoop / kUpdateLoop / kStopLoop. Unused on
    /// one-shots.
    std::uint32_t voiceId{};

    // ── provenance ───────────────────────────────────────────────────────────
    ActorId actorId{};
    std::uint16_t limbIndex{};
    LimbSite site{};
    SurfaceClass surface{};
    CueReason reason{};
    Phase phase{};
    float intensity{};            ///< 0..1, what the loudness curve was fed
    std::uint32_t sourceSeq{};    ///< the FeedEvent this traces back to
};

/// Where cues go. The game implements it with BSSoundHandle; the testbench
/// collects them and mixes offline.
class ICueSink {
public:
    virtual ~ICueSink() = default;
    virtual void Emit(const Cue& cue) = 0;
};

/// A sink that just keeps them. The testbench's whole renderer input, and handy
/// in tests.
class CueCollector final : public ICueSink {
public:
    void Emit(const Cue& cue) override { m_cues.push_back(cue); }
    [[nodiscard]] const std::vector<Cue>& Cues() const { return m_cues; }
    void Clear() { m_cues.clear(); }

private:
    std::vector<Cue> m_cues;
};

}  // namespace rds
