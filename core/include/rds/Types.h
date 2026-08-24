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
enum class SurfaceClass : std::uint8_t {
    kSoft = 0,  ///< carpet, cloth, dirt, grass, snow, mud, and everything unknown
    kWood,
    kStone,
    kMetal,
    kWater,
    kBody,  ///< skin on skin - another actor, or one of our own limbs
    kCount
};

[[nodiscard]] std::string_view ToString(SurfaceClass c);

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

/// Which limb chain a site hangs off, for the chain-merge rule (04 §4.2): a
/// strong hand impact silences the elbow and plays one arm flop.
enum class LimbChain : std::uint8_t { kNone = 0, kHead, kTorso, kLeftArm, kRightArm, kLeftLeg, kRightLeg };

[[nodiscard]] LimbChain ChainFromBoneName(std::string_view boneName);

/// What is on a body site. An axis of timbre only - never of physics (07 §11).
enum class Coverage : std::uint8_t { kBare = 0, kCloth, kLight, kHeavy };

[[nodiscard]] std::string_view ToString(Coverage c);

/// Our own nominal mass per site, in Havok mass units, before the actor's scale.
///
/// 07 §6: the vanilla ragdoll's own masses are asymmetric (R Forearm 6.0 against
/// L Forearm 2.0) and non-anatomical, and a KE-based loudness off them would be
/// three times louder on the right arm for identical movement. So the solver's
/// mass is read only to recover scale; this table is what loudness uses.
[[nodiscard]] float NominalMass(LimbSite site);

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
/// `Resting` means "no recent contacts". It carries the quiet budget - the last
/// twenty contacts of a knockdown are limbs flopping and should be nearly silent
/// (00-Design §4) - and it is left on a contact over the wake bar, never on any
/// contact at all.
enum class Motion : std::uint8_t {
    kLaunch = 0,
    kAirborne,
    kTumble,
    kSlide,
    kResting,
    kCount
};

[[nodiscard]] std::string_view ToString(Motion m);

/// How a slide ended, which is the only part of a slide the motion enum cannot
/// say. `Slide` names the state; this names the edge out of it, and the three
/// edges are three different sounds:
///
/// - `kRested`   the body ground to a halt. The loop fades out and the ordinary
///               closing cue does the rest.
/// - `kLaunched` the body left the ground. The loop fades out over
///               `ScrapeLoop:fLaunchFadeMs` instead, because a slide that ends
///               in flight ends faster than one that ends in friction.
/// - `kStruck`   neither of those, so the slide was stopped by something. That
///               is inferred rather than measured - a body that neither rested
///               nor launched had to have hit something - and it is what places
///               the slide-end impact.
///
/// Held on the crash state after the fact rather than being a transient, so the
/// timeline can mark the end of a span it has already drawn.
enum class SlideExit : std::uint8_t { kNone = 0, kRested, kLaunched, kStruck };

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
