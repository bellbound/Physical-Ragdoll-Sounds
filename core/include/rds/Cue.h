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
    kArmorSkin,
    kHeadImpact,
    kCrunch,
    kGore,
    kLimbTap,
    kScrape,
    kAirborneRise,
    kRustle,
};

[[nodiscard]] std::string_view ToString(CueReason r);

struct CompressConfig;

/// Which line of `[Compress]` holds a layer down.
///
/// A band and not a reason, because the impact composite is four bands under one
/// reason: `imp_transient`, `imp_body`, `imp_sub` and `imp_body_limb` are the
/// frequency ranges of one hit, they arrive at levels 8 dB apart, and the one
/// that owns the composite's peak is not the one that owns its character. A
/// threshold per band is what lets the body be held while the transient is left
/// alone - which is the whole reason to hold anything.
///
/// Everything else a composite carries - the surface skin, the armour skin -
/// answers `kImpact`, the catch-all the section shipped with. Those two ride the
/// body layer and sit ~12 dB under it, so they are never the layer that clips,
/// and a line of their own would be a decision nobody would ever make.
enum class CompressBand : std::uint8_t {
    kTransient = 0,
    kBody,
    kBass,
    kBodyLimb,
    /// Every other layer of a composite: the surface skin and the armour skin.
    kImpact,
    kTap,
    kHead,
    kCrunch,
    kGore,
    kScrape,
    kAirborne,
};

/// The ini key this band is set by, for a tooltip that names the slider rather
/// than describing it. `"fBodyDb"`, not `"body"`.
[[nodiscard]] std::string_view ToString(CompressBand band);

/// Which band governs a layer. **The slot decides before the reason does**, and
/// that order is the whole of the impact split: all four impact layers carry
/// `kImpactComposite`, so a mapping that asked the reason first could only ever
/// give the four of them one answer.
///
/// Asked with the slot the *proposal* named, never the one the bank resolved to.
/// `imp_body_limb` falls back to `imp_body`'s recording until somebody records a
/// limb one, and keying on the resolution would make `fBodyLimbDb` inert until
/// that day - the same trap `SlotGain:fImpBodyLimb` already avoids by trimming
/// the file the limb composite falls back to.
[[nodiscard]] CompressBand CompressBandFor(SlotId slot, CueReason reason);

/// The compression threshold that governs a band, on the pre-trim scale
/// `Layer::gainDb` uses. See CompressConfig for what the number means.
///
/// Shared rather than switched twice: the engine applies it and the testbench
/// explains it, and a second copy of this mapping is how the two come to
/// disagree about which slider held a cue down. `Cue::compressBand` closes the
/// other half of that gap - the testbench is told which band was chosen rather
/// than re-deriving it from a slot that has since been through the bank.
[[nodiscard]] float CompressThresholdDb(const CompressConfig& cfg, CompressBand band);

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
    /// The player's own ragdoll uses this, because at arm's length a collapse to
    /// one point sounds like the audio is inside your head.
    std::int32_t boneIndex{-1};

    /// Stage 4 rule 5 fired: this cue is part of a hero moment and every layer
    /// of it belongs at ONE point rather than at its own contact.
    ///
    /// The renderer needs to be told rather than left to infer it. It follows
    /// the contact limb by default - a sound has to come from where the limb
    /// hit, which is the falsifiable half of the physics/design split - and
    /// following a limb ignores `position` entirely, so a collapse expressed
    /// only as "every cue got the same position" would be silently discarded.
    bool collapsed{};

    /// Identifies a loop across kStartLoop / kUpdateLoop / kStopLoop. Unused on
    /// one-shots.
    std::uint32_t voiceId{};

    // ── provenance ───────────────────────────────────────────────────────────
    ActorId actorId{};
    std::uint16_t limbIndex{};
    LimbSite site{};
    SurfaceClass surface{};
    /// What the limb that made this cue was wearing.
    ///
    /// Beside `surface` because it answers the same kind of question and the
    /// timeline needs both to explain a resolution: with only the surface, "why
    /// did the plate variant play?" is unanswerable from outside the engine.
    /// Carried on every cue, not only the armour skins, because the class also
    /// biases pitch and trims the composite - so a cue with no armour layer at
    /// all can still have been coloured by what the body was wearing.
    Coverage coverage{};
    CueReason reason{};
    /// Both Stage 2 axes at the moment this cue was emitted. Two fields rather
    /// than one, because "the body was tumbling" and "the mix was in a hero
    /// moment" are independent facts and a timeline that shows only their
    /// product cannot explain why a quiet-looking contact came out loud.
    Motion motion{};
    Moment moment{};
    float intensity{};            ///< 0..1, what the loudness curve was fed
    std::uint32_t sourceSeq{};    ///< the FeedEvent this traces back to

    /// How much the `[Compress]` threshold for this layer's band took off it,
    /// negative, or 0 when the layer was under it. Already inside `gainDb`.
    ///
    /// Carried rather than recomputed because from the level alone the two
    /// questions that matter cannot be told apart: "this is as loud as it wanted
    /// to be" and "this is as loud as it was allowed to be" land on the same
    /// number. It is also what lets the timeline draw the height a held cue
    /// would have had, which is the only way to see a compressor working.
    ///
    /// Per layer since the impact split, so the four layers of one composite
    /// routinely carry four different cuts. That is the point of the split and
    /// not a rounding difference: a body-heavy hit comes down by five decibels
    /// and its transient by none, so the hit gets quieter without getting dull.
    float compressCutDb{};

    /// Which line of `[Compress]` produced that cut. Provenance, and the only
    /// thing that can name the slider: `slot` here is what the *bank* resolved
    /// to, and `imp_body_limb` playing `imp_body`'s file was held by the limb's
    /// threshold whatever the slot now says.
    CompressBand compressBand{};
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
