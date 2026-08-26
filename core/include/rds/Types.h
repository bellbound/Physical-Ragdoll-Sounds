#pragma once

// Portable vocabulary for the whole engine.
//
// Nothing in core/ may include RE/ or SKSE/. The engine is compiled twice: once
// into RagdollSounds.dll inside the game process, once into the testbench exe
// with no game running. Anything the game knows and the testbench does not has
// to arrive through Feed.h as plain data.

#include <cstdint>
#include <string>
#include <string_view>

namespace rds {

// ── units ────────────────────────────────────────────────────────────────────
//
// Game units throughout, because that is what the solver reports and what the
// capture files carry. 69.99 units = 1 m; converting on the way in would only
// mean converting back to compare against a recording.

inline constexpr float kUnitsPerMetre = 69.99f;

inline constexpr float MetresToUnits(float m) { return m * kUnitsPerMetre; }
inline constexpr float UnitsToMetres(float u) { return u / kUnitsPerMetre; }

struct Vec3 {
    float x{}, y{}, z{};
};

[[nodiscard]] float Length(const Vec3& v);
[[nodiscard]] float Distance(const Vec3& a, const Vec3& b);

// ── what the contact hit ─────────────────────────────────────────────────────

/// Havok collision layer, the reliable half of "what did it hit".
///
/// 07 §8: the layer is trustworthy, the material is an enrichment that fails at
/// the edges. Only the values the ragdoll path actually meets are named; the
/// rest arrive as their raw number and read as kOther.
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
/// Skyrim can tell 90 materials apart and vanilla's ragdoll path resolves 60 of
/// them into 3 sounds (08 §5). We sit in between: enough classes that stone and
/// wood differ, few enough that every one can have a file behind it. Anything
/// unresolved lands on kSoft, which is the design's stated default.
///
/// The first six are the original set and keep their values, because
/// `SurfaceMatch` is stored in the sfx ini as an index-adjacent enum and a
/// renumber would silently repoint every condition already written down. New
/// classes append.
///
/// Every class has a parent (see `SurfaceParent`), and that one chain is used
/// twice: it is the sound fallback when a class has no file of its own, and it
/// is the config inheritance when a class has no block of its own. One
/// relationship, declared once - so `ice` playing the stone skin and `ice`
/// reading stone's numbers can never disagree.
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

/// Where the material came from, kept apart from the material itself because at
/// a texture seam a sampled answer and a measured one differ, and a system that
/// cannot tell which it had cannot debug the one time it sounded wrong (07 §8).
enum class MaterialSource : std::uint8_t {
    kNone = 0,
    kShape,    ///< the contact's own bhkShape - measured at the contact point
    kTerrain,  ///< the land record under the actor - sampled per tick, not at the contact
};

// ── the body ─────────────────────────────────────────────────────────────────

/// The body site a limb belongs to.
///
/// Resolved from the bone *name*, never from the limb index: 18 bodies in a
/// fixed order is the vanilla humanoid skeleton and nothing else (07 §7). An
/// unrecognised name resolves to kUnknown, which still sounds - sized off
/// limbRadius - rather than going silent.
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

/// Bone name to site. Case-insensitive substring match on the ragdoll bone
/// name, e.g. "NPC L Forearm [LLar]" -> kForearm. Returns kUnknown for anything
/// unrecognised, which is the honest answer for a draugr or a modded skeleton.
[[nodiscard]] LimbSite SiteFromBoneName(std::string_view boneName);

/// The three bodies the damage rule is written for.
///
/// Coarser than `LimbSite` on purpose. Damage asks one question - what broke and
/// how badly - and it has exactly three honest answers: a skull, the column that
/// carries the body, and a bone out on a limb. A tier per site would be nine
/// tunings of a thing nobody can tell apart by ear, and the two that differ
/// most already get their own slot.
///
/// `kNeck` is spine rather than head. A neck is the top of the column and it
/// breaks like one; putting it with the skull would give a whiplash the head's
/// gate and the head's crunch file, which is not the sound.
///
/// `kUnknown` is a limb, which is the least dramatic of the three - a draugr or a
/// modded skeleton we cannot name should not be handed the skull's tuning by
/// accident.
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

/// One half of a variant's condition: a specific class, or "any".
///
/// `kAny` is a value the enum holds rather than a separate flag, because a
/// condition with no opinion about the surface is the ordinary case - every file
/// in the shipping pack has one - and a bool beside every half would double the
/// storage to say "no" three quarters of the time.
///
/// The specific values are the class plus one, so `kAny` can be zero and a
/// default-constructed condition means "matches everything". That is what keeps
/// an unconditioned pack byte-identical to one from before conditions existed.
/// Deliberately `SurfaceClass + 1`, which is what `Matches` below relies on, so
/// a new class has to be appended here in the same order or conditions written
/// against it silently match the wrong floor.
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

/// What a variant asks of the contact before it will be picked.
///
/// Two independent halves, and deliberately no third: keying on the limb site
/// was considered and dropped. A condition narrows *which files of a slot are
/// candidates*; it never changes which slot is asked for, never changes a level,
/// and never silences anything - see `SoundBank::Resolve` for the ladder and for
/// why an unsatisfiable condition falls back rather than going quiet.
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
///
/// 07 §6: the vanilla ragdoll's own masses are asymmetric (R Forearm 6.0 against
/// L Forearm 2.0) and non-anatomical, and a KE-based loudness off them would be
/// three times louder on the right arm for identical movement. So the solver's
/// mass is read only to recover scale; this table is what loudness uses.
[[nodiscard]] float NominalMass(LimbSite site);

/// How much *garment* a site carries, which is a different question from how
/// much body it is.
///
/// `NominalMass` answers "how much of the actor is this limb", and the rustle
/// layer needs "how much cloth moves when this limb does". A hand is four
/// tenths of a unit of body and, bare, essentially no fabric at all; a torso is
/// most of a shirt. Weighting the rustle drive by mass would make the whole
/// measurement a story about the pelvis.
///
/// Coverage scales it, and that is what makes the axis worth having here:
/// gauntlets, boots and a helm put real fabric on three sites that are bare by
/// default, so a knight and a farmer do not merely play different files, they
/// have different limbs contributing to the drive in the first place.
[[nodiscard]] float FabricWeight(LimbSite site, Coverage coverage);

// ── the run of a fall ────────────────────────────────────────────────────────

/// Stage 2, first axis: what the *body* is doing.
///
/// Physics owns it, and it transitions freely: every edge is available from
/// every state that can reach it. A one-way edge here is a bug waiting to
/// happen - a state a fall can enter and not leave will eventually swallow the
/// loudest contacts of one.
///
/// What the *mix* should do is deliberately not here; that is `Moment`. Those
/// are two questions, and a single value that has to answer both must choose,
/// and whichever it chooses is wrong for the other reader.
///
/// No state here is quiet: nothing on this axis suppresses a contact, because a
/// phase cannot judge one. Keeping the flopping tail of a knockdown unobtrusive
/// belongs to intensity, temporal masking, the chain merge and burst shaping,
/// all of which read the contact in front of them.
enum class Motion : std::uint8_t {
    kLaunch = 0,
    kAirborne,
    kTumble,
    kSlide,
    kCount
};

[[nodiscard]] std::string_view ToString(Motion m);

/// How a slide ended, which is the only part of a slide the motion enum cannot
/// say. `Slide` names the state; this names the edge out of it:
///
/// - `kLaunched` the body left the ground. The loop fades out over
///               `ScrapeLoop:fLaunchFadeMs`, because a slide that ends in
///               flight ends faster than one that ends in friction.
/// - `kEnded`    the body stopped: the graze stream dried up and, where
///               `Motion:fSlideHoldSpeed` is set, the body had slowed under it.
///               The loop fades over `ScrapeLoop:fStopFadeMs`. Whatever
///               collision arrives in that frame is an ordinary contact and is
///               judged like one.
///
/// Held on the crash state after the fact rather than being a transient, so the
/// timeline can mark the end of a span it has already drawn.
enum class SlideExit : std::uint8_t { kNone = 0, kLaunched, kEnded };

[[nodiscard]] std::string_view ToString(SlideExit e);

/// Stage 2, second axis: what the *mix* is doing. Design owns it, and unlike
/// motion it is latched and windowed rather than a running description.
///
/// A fall may reach `Hero` late, more than once, or never - a gentle slump
/// crosses nothing. The old machine could express none of those, because it
/// fired on the first thing that touched.
enum class Moment : std::uint8_t { kOrdinary = 0, kHero, kCount };

[[nodiscard]] std::string_view ToString(Moment m);

/// Both axes as one label - "Tumble", or "Tumble+Hero" while the latch is open.
///
/// For the one place a single column used to show a single enum: a table row, a
/// timeline tooltip, an export line. It is a lookup into fixed storage rather
/// than a concatenation, so it costs nothing and can be handed straight to a
/// formatter. Anything making a *decision* should read the two fields.
[[nodiscard]] std::string_view ToString(Motion m, Moment moment);

/// Which side of the ragdoll handover a feed event is on, as the capture files
/// and the game's per-tick publisher both report it. Distinct from both axes
/// above: this is the engine's *gate*, they are its *state*.
enum class ActorPhase : std::uint8_t { kUnknown = 0, kAnimated, kRagdoll, kGetUp };

/// Distance tier, evaluated per actor per tick rather than per contact (§10).
enum class DistanceTier : std::uint8_t {
    kFull = 0,    ///< everything: composites, grains, loops, bed
    kSimplified,  ///< hero composites only
    kCulled       ///< nothing, and the actor stops being tracked
};

[[nodiscard]] std::string_view ToString(DistanceTier t);

// ── identity ─────────────────────────────────────────────────────────────────

/// A tracked actor. In the game this is the FormID; in a replay it is the
/// actor_id column, which is the same number. The player is whatever
/// ActorProfile::isPlayer says, never a hardcoded 0x14, because a recording of
/// somebody else's game may not agree.
using ActorId = std::uint32_t;

/// Monotonic engine clock, milliseconds since the session opened. Live it is
/// steady_clock; replaying it is the CSV's t_ms. Both are the same clock as far
/// as the engine is concerned, which is the point.
using TimeMs = double;

}  // namespace rds
