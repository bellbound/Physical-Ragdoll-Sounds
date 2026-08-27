#pragma once

// The seam between "the game is telling us what happened" and "a file is".
//
// This is the one interface that decides whether we ship one backend or two.
// FeedEvent is deliberately QuickModMenuNG's `RawEvent` with the CSV's resolved
// columns folded back in - same fields, same units, same sign conventions - so
// the live path fills it in the contact callback and the replay path fills it
// from a row of Research/NewRecordings, and Engine cannot tell which it got.
//
// See skse/QuickModMenuNG/src/debug/ImpactRecorder.cpp for the capture side and
// Research/02-Data-Dictionary.md for what each field is worth.

#include <cstdint>
#include <string>
#include <vector>

#include "rds/Types.h"

namespace rds {

enum class EventKind : std::uint8_t {
    kImpact = 0,   ///< a contact with a closing speed. The signal
    kTouch,        ///< manifold opened; carries no contact point
    kSeparate,     ///< manifold closed
    kState,        ///< ragdoll_start, knock_explode, session_stop, ...
    kLimbSample,   ///< a periodic pose/velocity sample of one limb
    kListener,     ///< where the player is and which way they face
    /// What vanilla's own impact system decided to play for one collision.
    ///
    /// Appended rather than slotted in beside kImpact so every value above stays
    /// what it was: the wire carries raw FeedEvent bytes and a testbench built a
    /// day apart from the DLL must not reinterpret an old stream's kinds.
    kVanillaSound,
};

[[nodiscard]] std::string_view ToString(EventKind k);

/// Which branch vanilla took, and what we did about it.
enum class VanillaSoundFlag : std::uint8_t {
    kNone = 0,
    /// The NAM1 branch fired rather than SNAM - vanilla's own light/heavy
    /// decision, which it makes on `sound2 != null && magnitude >= threshold`.
    kHeavy = 1 << 0,
    /// We dropped it. Off means it was heard, which is what the A/B switch does.
    kSuppressed = 1 << 1,
    /// The editor id did not fit `text` and is cut short. Recorded rather than
    /// silently truncated, because the testbench resolves files off that name
    /// and a quiet trim would look like a missing sound bank.
    kNameTruncated = 1 << 2,
    /// The game had no editor id for the descriptor at all.
    ///
    /// `BGSSoundDescriptorForm` does not implement `GetFormEditorID` - only about
    /// fifteen form types do - so the name exists at runtime only because
    /// po3_Tweaks keeps one. Without it the row still carries `descriptorFormId`,
    /// which identifies the sound perfectly well, but nothing can resolve it to a
    /// file. Flagged rather than left as an empty string, because "vanilla was
    /// quiet" and "this install cannot name its own sounds" must not look alike.
    kNameMissing = 1 << 3,
};

/// What vanilla would have played, for one collision.
///
/// Filled only on `kVanillaSound`. The point of it is stated in 08 section 5:
/// vanilla resolves 60 materials into 3 sounds and picks between an L and an H
/// pair, and none of that is reproducible from our own contact stream - so it is
/// observed at the moment it happens rather than modelled afterwards.
///
/// What is *not* here, deliberately: which of the descriptor's `fileCount` wavs
/// the audio engine drew, and the dB and frequency the variance rolled. Those
/// happen inside BSAudioManager after the handle is built and are not readable
/// without reverse engineering BSGameSound, whose layout CommonLib does not have.
/// `fileCount` and the variance figures are the honest substitute: they say what
/// the draw was *over*, and the testbench renders one of them rather than
/// pretending it knows which.
struct VanillaSoundInfo {
    std::uint32_t impactFormId{};       ///< the BGSImpactData the pair resolved to
    std::uint32_t descriptorFormId{};   ///< the descriptor that fired, SNAM or NAM1
    std::uint16_t staticAttenuation{};  ///< BNAM, the CK value x 100
    std::uint8_t soundLevel{};          ///< SOUND_LEVEL on the impact record
    std::uint8_t dbVariance{};          ///< +/- dB the audio engine rolls per play
    std::uint8_t frequencyShift{};
    std::uint8_t frequencyVariance{};
    std::uint8_t priority{};
    std::uint8_t fileCount{};  ///< how many wavs the descriptor picks between
    std::uint8_t flags{};      ///< VanillaSoundFlag
    std::uint8_t reserved[3]{};

    [[nodiscard]] bool Has(VanillaSoundFlag f) const {
        return (flags & static_cast<std::uint8_t>(f)) != 0;
    }
};
static_assert(sizeof(VanillaSoundInfo) == 20, "VanillaSoundInfo rides the wire as raw bytes");

/// One row of what the solver saw.
///
/// Field grades are the data dictionary's. The two that matter most:
/// `impactSpeed` is A and is the signal; `normalSpeed` is B and exists only to
/// be checked against it - a disagreement between the two is a physics blow-up
/// detected on the arithmetic, needing no threshold (07 §2). Compare their
/// *magnitudes*: normalSpeed comes out exactly negated on 23 % of rows.
struct FeedEvent {
    TimeMs timeMs{};    ///< monotonic, since the session opened
    ActorId actorId{};  ///< FormID live, actor_id column on replay
    std::uint16_t limbIndex{};
    EventKind kind{};
    ActorPhase phase{};  ///< sampled by the tick, so accurate to one tick either side

    /// Whether the game says this actor is fighting or being fought, sampled the
    /// same way and on every row that carries a phase. It travels beside the phase
    /// because the two are one question downstream - `ModeFor` - and splitting
    /// them across two mechanisms would let a row answer one and not the other.
    bool inCombat{};

    /// Both re-fire on most frames of a persisting manifold and they do not
    /// pair up (244 rows carry `last` with no `first`). Kept for provenance;
    /// grade C as a bracket, and Ingest must not use them as one.
    bool manifoldFirst{};
    bool manifoldLast{};

    float impactSpeed{};   ///< solver's closing speed along the normal, units/s. THE signal
    float normalSpeed{};   ///< the same recomputed from both bodies' motion. Sign unreliable
    float tangentSpeed{};  ///< relative speed along the surface, units/s. Scrape versus thud
    float bodySpeed{};     ///< |linear velocity| of the limb at its centre of mass
    float angularSpeed{};  ///< |angular velocity|, rad/s
    float mass{};          ///< the limb's live Havok mass. Read to recover scale, not for loudness
    float limbRadius{};    ///< motionState.objectRadius, units. The honest input on an unknown skeleton

    Vec3 position{};  ///< contact point on kImpact; limb centre on kLimbSample; player on kListener
    Vec3 normal{};    ///< contact normal; player facing on kListener. |z| ~ 1 is floor, ~ 0 is wall
    Vec3 velocity{};  ///< the limb's linear velocity

    std::uint64_t otherBody{};      ///< raw body identity, for pairing a self-collision with its mirror
    std::uint32_t otherMaterial{};  ///< MATERIAL_ID, 0 when unresolved
    ColLayer otherLayer{};
    MaterialSource materialSource{};
    std::int32_t otherLimb{-1};  ///< index into ActorProfile::limbs when we hit ourselves, else -1

    /// State name on kState/kLimbSample/kListener rows, empty otherwise.
    /// Short and fixed so a FeedEvent stays trivially copyable and can live in
    /// the live path's lock-free ring.
    char text[24]{};

    /// Where this came from, so a cue can be traced back to a row of a CSV.
    std::uint32_t sourceSeq{};

    /// Filled on kVanillaSound rows, zero everywhere else. `text` carries the
    /// firing descriptor's editor id on those rows - it is the name the testbench
    /// resolves wav files from, so it is the descriptor's and not the impact's.
    VanillaSoundInfo vanilla{};

    [[nodiscard]] bool IsContact() const {
        return kind == EventKind::kImpact || kind == EventKind::kTouch || kind == EventKind::kSeparate;
    }
};

static_assert(sizeof(FeedEvent) <= 160, "FeedEvent rides a lock-free ring in the live path");
static_assert(sizeof(FeedEvent) % 8 == 0, "and goes over the wire as raw bytes, so no tail padding");

/// One ragdoll body, resolved once when the ragdoll attaches.
///
/// Resolved on every attach rather than cached across them: `ragdoll_rebuilt`
/// fires on cell change, on 3D reload, and six times in three seconds on a
/// standing actor that gets disturbed (07 §7).
struct LimbInfo {
    std::string boneName;  ///< "NPC L Forearm [LLar]"
    LimbSite site{};       ///< resolved from boneName, never from the index
    LimbChain chain{};
    Coverage coverage{};      ///< what is on this site, from the armour addons
    float havokMass{};        ///< the solver's own, asymmetric and non-anatomical
    float radius{};           ///< objectRadius, units
    std::uint64_t bodyId{};   ///< raw body pointer, matched against FeedEvent::otherBody
};

/// Everything about the actor the per-contact stream does not carry.
///
/// Live this is built at ragdoll attach; on replay it is the YAML sidecar. The
/// two agree field for field, which is what lets one Engine serve both.
struct ActorProfile {
    ActorId actorId{};
    std::string name;
    bool isPlayer{};
    float scale{1.0f};  ///< the actor's scale. The mass vector is the skeleton table x this

    std::vector<LimbInfo> limbs;

    /// Present only when a recording carries it. The engine does not read the
    /// reverb block - the game applies the cell's acoustic space itself - but
    /// the testbench needs it to reproduce what a take sounded like in the room.
    struct Reverb {
        bool valid{};
        std::string acousticSpace;
        int decayTimeMs{};
        int hfReferenceHz{};
        int roomFilter{};
        int roomHfFilter{};
        int diffusionPct{};
        int densityPct{};
    } reverb;

    [[nodiscard]] const LimbInfo* Limb(std::size_t index) const {
        return index < limbs.size() ? &limbs[index] : nullptr;
    }
};

/// Where the player is. Published per tick from the game thread; on replay it is
/// the kListener rows. Distance is evaluated against this, per actor per tick.
struct ListenerState {
    Vec3 position{};
    Vec3 facing{};
    TimeMs timeMs{};
};

/// The source of events, however they are produced.
///
/// The live implementation drains a lock-free ring filled by the Havok contact
/// callback; the replay implementation walks a sorted vector. Engine holds one
/// of these and never asks which it is.
class IFeed {
public:
    virtual ~IFeed() = default;

    /// Move every event stamped at or before `untilMs` into `out`, in time
    /// order. Appends - the caller owns clearing. False once the feed is
    /// exhausted and will produce nothing further (a replay that ran out; a
    /// live feed never returns false).
    virtual bool Drain(TimeMs untilMs, std::vector<FeedEvent>& out) = 0;

    /// The profile for an actor, or nullptr if it is not tracked. Valid until
    /// the next Drain that carries a ragdoll_rebuilt for that actor.
    [[nodiscard]] virtual const ActorProfile* Profile(ActorId actor) const = 0;

    /// The most recent listener state. A live feed always has one; a replay may
    /// not until the first kListener row arrives.
    [[nodiscard]] virtual const ListenerState& Listener() const = 0;

    /// Seconds of wall time this frame, for the windows that scale with frame
    /// rate (07 §4). Live it is the real frame delta; on replay it is derived
    /// from the gap between contact batches, which the capture resolves to one
    /// frame. Zero means "unknown, use the configured floor".
    [[nodiscard]] virtual float FrameTimeSec() const { return 0.0f; }
};

}  // namespace rds
