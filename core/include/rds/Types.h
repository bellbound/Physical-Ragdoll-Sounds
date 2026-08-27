#pragma once

// Portable vocabulary for the whole engine.
//
// Nothing in core/ may include RE/ or SKSE/: the engine is compiled twice, into
// RagdollSounds.dll and into the testbench exe with no game running. Anything the
// game knows and the testbench does not arrives through Feed.h as plain data.

#include <cstdint>
#include <string>
#include <string_view>

namespace rds {

// ── units ────────────────────────────────────────────────────────────────────
//
// Game units throughout - what the solver reports and what the capture files
// carry. 69.99 units = 1 m; converting on the way in would only mean converting
// back to compare against a recording.

inline constexpr float kUnitsPerMetre = 69.99f;

inline constexpr float MetresToUnits(float m) { return m * kUnitsPerMetre; }
inline constexpr float UnitsToMetres(float u) { return u / kUnitsPerMetre; }

struct Vec3 {
    float x{}, y{}, z{};
};

[[nodiscard]] float Length(const Vec3& v);
[[nodiscard]] float Distance(const Vec3& a, const Vec3& b);

// ── what the contact hit ─────────────────────────────────────────────────────

/// Havok collision layer, the reliable half of "what did it hit" (07 §8; the
/// material is an enrichment that fails at the edges). Only the values the ragdoll
/// path actually meets are named; the rest read as kOther.
enum class ColLayer : std::uint8_t {
    kOther = 0,
    kStatic,
    kGround,
    kBiped,
    kDeadBip,
    kProps,
    kTrees,
    kWater,
};

/// The surface classes a sound is actually chosen for.
///
/// Skyrim tells 90 materials apart and vanilla's ragdoll path resolves 60 of them
/// into 3 sounds (08 §5). We sit in between: enough that stone and wood differ,
/// few enough that each can have a file behind it. Anything unresolved lands on
/// kSoft.
///
/// The first six keep their values: `SurfaceMatch` is stored in the sfx ini as an
/// index-adjacent enum, so a renumber would silently repoint every condition
/// already written down. New classes append.
///
/// Every class has a parent (`SurfaceParent`), and that one chain is both the
/// sound fallback and the config inheritance - so `ice` playing the stone skin and
/// `ice` reading stone's numbers can never disagree.
enum class SurfaceClass : std::uint8_t {
    kSoft = 0,  ///< carpet, cloth, sand, ash, grass, web, and everything unknown
    kWood,
    kStone,
    kMetal,
    kWater,
    kBody,  ///< skin on skin - another actor, or one of our own limbs
    kDirt,  ///< dirt and mud together: vanilla's footstep data merges them too
    kGravel,
    kSnow,
    kIce,
    kGlass,
    kWaterPuddle,  ///< a slap with something under it, not a body of water
    kBone,         ///< skeletons and draugr - the dry rattle under the flesh
    kCount
};

[[nodiscard]] std::string_view ToString(SurfaceClass c);

/// Parse a class by its `ToString` name. kCount for anything unrecognised, so a
/// caller can tell "not a surface" from "the first one".
[[nodiscard]] SurfaceClass SurfaceClassFrom(std::string_view name);

/// What this class falls back to for both sound and config, or kCount at a
/// root. The three classes with recorded files - soft, wood, stone - are roots;
/// everything else eventually reaches one of them.
[[nodiscard]] SurfaceClass SurfaceParent(SurfaceClass c);

/// Where the material came from, kept apart from the material itself: at a texture
/// seam a sampled answer and a measured one differ, and a system that cannot tell
/// which it had cannot debug the one time it sounded wrong (07 §8).
enum class MaterialSource : std::uint8_t {
    kNone = 0,
    kShape,    ///< the contact's own bhkShape - measured at the contact point
    kTerrain,  ///< the land record under the actor - sampled per tick, not at the contact
};

// ── the body ─────────────────────────────────────────────────────────────────

/// The body site a limb belongs to. Resolved from the bone *name*, never the limb
/// index: 18 bodies in a fixed order is the vanilla humanoid skeleton and nothing
/// else (07 §7). An unrecognised name resolves to kUnknown, which still sounds,
/// sized off limbRadius.
enum class LimbSite : std::uint8_t {
    kUnknown = 0,
    kHead,
    kNeck,
    kTorso,
    kUpperArm,
    kForearm,
    kHand,
    kThigh,
    kCalf,
    kFoot,
    kCount
};

[[nodiscard]] std::string_view ToString(LimbSite s);

/// Bone name to site. Case-insensitive substring match, e.g. "NPC L Forearm
/// [LLar]" -> kForearm. kUnknown for anything unrecognised, which is the honest
/// answer for a draugr or a modded skeleton.
[[nodiscard]] LimbSite SiteFromBoneName(std::string_view boneName);

/// The three bodies the damage rule is written for. Coarser than `LimbSite` on
/// purpose: "what broke and how badly" has three honest answers - a skull, the
/// column that carries the body, and a bone out on a limb.
///
/// `kNeck` is spine rather than head: a neck is the top of the column and breaks
/// like one, and putting it with the skull would give a whiplash the head's gate
/// and crunch file. `kUnknown` is a limb, the least dramatic of the three.
enum class DamageSite : std::uint8_t { kHead = 0, kSpine, kLimb, kCount };

[[nodiscard]] std::string_view ToString(DamageSite s);

/// Which of the three a contact's site answers to.
[[nodiscard]] DamageSite DamageSiteFor(LimbSite site);

/// Which limb chain a site hangs off, for the chain-merge rule (04 §4.2): a
/// strong hand impact silences the elbow and plays one arm flop.
enum class LimbChain : std::uint8_t { kNone = 0, kHead, kTorso, kLeftArm, kRightArm, kLeftLeg, kRightLeg };

[[nodiscard]] LimbChain ChainFromBoneName(std::string_view boneName);

/// What is on a body site. An axis of timbre only - never of physics (07 §11).
enum class Coverage : std::uint8_t { kBare = 0, kCloth, kLight, kHeavy };

[[nodiscard]] std::string_view ToString(Coverage c);

/// One half of a variant's condition: a specific class, or "any". `kAny` is a
/// value rather than a separate flag, because no opinion about the surface is the
/// ordinary case.
///
/// The specific values are `SurfaceClass + 1`, so `kAny` is zero and a
/// default-constructed condition matches everything - which keeps an
/// unconditioned pack byte-identical to one from before conditions existed.
/// `Matches` below relies on that offset, so a new class must be appended here in
/// the same order or conditions written against it match the wrong floor.
enum class SurfaceMatch : std::uint8_t {
    kAny = 0,
    kSoft,
    kWood,
    kStone,
    kMetal,
    kWater,
    kBody,
    kDirt,
    kGravel,
    kSnow,
    kIce,
    kGlass,
    kWaterPuddle,
    kBone,
};
enum class CoverageMatch : std::uint8_t { kAny = 0, kBare, kCloth, kLight, kHeavy };

[[nodiscard]] std::string_view ToString(SurfaceMatch m);
[[nodiscard]] std::string_view ToString(CoverageMatch m);

/// Parse a condition half by name. `any`, an empty string and anything
/// unrecognised all read as `kAny` - an ini nobody can typo into silence.
[[nodiscard]] SurfaceMatch SurfaceMatchFrom(std::string_view name);
[[nodiscard]] CoverageMatch CoverageMatchFrom(std::string_view name);

[[nodiscard]] inline bool Matches(SurfaceMatch m, SurfaceClass surface) {
    return m == SurfaceMatch::kAny || static_cast<std::uint8_t>(m) - 1 ==
                                          static_cast<std::uint8_t>(surface);
}

[[nodiscard]] inline bool Matches(CoverageMatch m, Coverage coverage) {
    return m == CoverageMatch::kAny || static_cast<std::uint8_t>(m) - 1 ==
                                           static_cast<std::uint8_t>(coverage);
}

/// What a variant asks of the contact before it will be picked. Two independent
/// halves and deliberately no third - keying on the limb site was considered and
/// dropped. A condition narrows *which files of a slot are candidates*; it never
/// changes which slot is asked for, never changes a level, and never silences
/// anything (see `SoundBank::Resolve`).
struct VariantCondition {
    SurfaceMatch surface{SurfaceMatch::kAny};
    CoverageMatch coverage{CoverageMatch::kAny};

    [[nodiscard]] bool Unconditional() const {
        return surface == SurfaceMatch::kAny && coverage == CoverageMatch::kAny;
    }

    [[nodiscard]] bool operator==(const VariantCondition& other) const = default;

    /// How specific this is: 0, 1 or 2 halves pinned. The tier the ladder sorts
    /// on.
    [[nodiscard]] int Specificity() const {
        return (surface != SurfaceMatch::kAny ? 1 : 0) + (coverage != CoverageMatch::kAny ? 1 : 0);
    }
};

/// Our own nominal mass per site, in Havok mass units, before the actor's scale.
/// The vanilla ragdoll's own masses are asymmetric (R Forearm 6.0 against L
/// Forearm 2.0) and non-anatomical, so a KE-based loudness off them would be three
/// times louder on the right arm for identical movement (07 §6). The solver's mass
/// is read only to recover scale.
[[nodiscard]] float NominalMass(LimbSite site);

/// How much *garment* a site carries, which is a different question from how much
/// body it is. A hand is four tenths of a unit of body and, bare, essentially no
/// fabric; a torso is most of a shirt. Weighting the rustle drive by mass would
/// make the whole measurement a story about the pelvis.
///
/// Coverage scales it, which is what makes the axis worth having: gauntlets, boots
/// and a helm put real fabric on three sites that are bare by default, so a knight
/// and a farmer have different limbs contributing to the drive.
[[nodiscard]] float FabricWeight(LimbSite site, Coverage coverage);

// ── the run of a fall ────────────────────────────────────────────────────────

/// Stage 2, first axis: what the *body* is doing.
///
/// Physics owns it and it transitions freely - every edge is available from every
/// state that can reach it, because a state a fall can enter and not leave will
/// eventually swallow the loudest contacts of one. What the *mix* should do is
/// `Moment`; a single value answering both must choose, and whichever it chooses
/// is wrong for the other reader.
///
/// No state here is quiet: nothing on this axis suppresses a contact, because a
/// phase cannot judge one. Keeping the flopping tail of a knockdown unobtrusive
/// belongs to intensity, masking, the chain merge and burst shaping.
enum class Motion : std::uint8_t {
    kLaunch = 0,
    kAirborne,
    kTumble,
    kSlide,
    kCount
};

[[nodiscard]] std::string_view ToString(Motion m);

/// How a slide ended - `Slide` names the state, this names the edge out of it:
///
/// - `kLaunched` the body left the ground. Fades over
///               `ScrapeLoop:fLaunchFadeMs`, faster than friction would.
/// - `kEnded`    the body stopped: the graze stream dried up and, where
///               `Motion:fSlideHoldSpeed` is set, the body had slowed under it.
///               Fades over `ScrapeLoop:fStopFadeMs`.
///
/// Held on the crash state after the fact rather than being a transient, so the
/// timeline can mark the end of a span it has already drawn.
enum class SlideExit : std::uint8_t { kNone = 0, kLaunched, kEnded };

[[nodiscard]] std::string_view ToString(SlideExit e);

/// Stage 2, second axis: what the *mix* is doing. Design owns it, latched and
/// windowed rather than a running description. A fall may reach `Hero` late, more
/// than once, or never - a gentle slump crosses nothing.
enum class Moment : std::uint8_t { kOrdinary = 0, kHero, kCount };

[[nodiscard]] std::string_view ToString(Moment m);

/// Both axes as one label - "Tumble", or "Tumble+Hero" while the latch is open.
/// For a table row, a timeline tooltip, an export line. A lookup into fixed
/// storage rather than a concatenation, so it can be handed straight to a
/// formatter. Anything making a *decision* should read the two fields.
[[nodiscard]] std::string_view ToString(Motion m, Moment moment);

/// Which side of the ragdoll handover a feed event is on, as the capture files
/// and the game's per-tick publisher both report it. Distinct from both axes
/// above: this is the engine's *gate*, they are its *state*.
enum class ActorPhase : std::uint8_t { kUnknown = 0, kAnimated, kRagdoll, kGetUp };

/// Which of the three tuning columns an actor is read through. Decided per actor
/// and re-decided every tick, because it is a question about one body and not
/// about the scene: a guard swinging a sword and the man he just knocked down are
/// in the same fight and want opposite tuning.
///
/// The whole point of the axis is that most of the mod is *for* ragdolls. Upright
/// bodies want the same rules with the sensitivity taken off and the strategies
/// that only make sense for a loose body switched off - which is a column of the
/// same table, not a second algorithm. See ConfigSchema.h's `perMode`.
enum class ActorMode : std::uint8_t {
    kRagdoll = 0,  ///< down, getting up, or not yet classified
    kGameplay,     ///< upright and nobody is fighting them
    kCombat,       ///< upright, fighting or being fought
    kCount
};

[[nodiscard]] std::string_view ToString(ActorMode m);

/// Ragdoll wins, and that is the rule the whole axis rests on: an actor knocked
/// down mid-fight is a ragdoll and nothing else, so a fight cannot re-tune a body
/// that is already on the floor. Every phase that is not plainly `kAnimated`
/// answers ragdoll, so an actor we have not classified yet keeps the tuning the
/// mod has always had.
[[nodiscard]] constexpr ActorMode ModeFor(ActorPhase phase, bool inCombat) {
    if (phase != ActorPhase::kAnimated) {
        return ActorMode::kRagdoll;
    }
    return inCombat ? ActorMode::kCombat : ActorMode::kGameplay;
}

/// Distance tier, evaluated per actor per tick rather than per contact (§10).
enum class DistanceTier : std::uint8_t {
    kFull = 0,    ///< everything: composites, grains, loops, bed
    kSimplified,  ///< hero composites only
    kCulled       ///< nothing, and the actor stops being tracked
};

[[nodiscard]] std::string_view ToString(DistanceTier t);

// ── identity ─────────────────────────────────────────────────────────────────

/// A tracked actor: the FormID in the game, the actor_id column in a replay. The
/// player is whatever ActorProfile::isPlayer says, never a hardcoded 0x14, because
/// a recording of somebody else's game may not agree.
using ActorId = std::uint32_t;

/// Monotonic engine clock, milliseconds since the session opened: steady_clock
/// live, the CSV's t_ms on replay. The engine cannot tell them apart, which is the
/// point.
using TimeMs = double;

}  // namespace rds
