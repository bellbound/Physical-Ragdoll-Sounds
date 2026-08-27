#include "rds/Engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

#include "rds/Log.h"
#include "rds/Pose.h"

namespace rds {
namespace {

constexpr TimeMs kLongAgo = -1.0e9;
constexpr float kSilentDb = -140.0f;
constexpr std::size_t kMaxLayers = 6;
constexpr std::size_t kChainCount = 7;

/// How many bookings the live list starts with room for. A hint to the
/// allocator and **not** a limit - see the voice budget below, which no longer
/// exists.
constexpr std::size_t kVoiceReserve = 64;

/// How many ragdoll bodies we keep per-limb bookkeeping for. The vanilla
/// humanoid ragdoll is eighteen; the headroom is for modded skeletons, and a
/// limb past the end is simply not counted rather than being a crash.
constexpr std::size_t kMaxLimbs = 32;

/// The limb chains that can carry a scrape loop of their own: two arms, two legs
/// and the head. Per *limb* rather than per bone, which is the grain a listener
/// hears - a forearm and the hand dragging together are one arm scraping.
///
/// The torso is absent: a spine on the floor is the body sliding, which is the
/// body loop's subject, so a limb loop there would play the same event twice.
constexpr std::size_t kScrapeChainCount = 5;

[[nodiscard]] int ScrapeChainOf(LimbChain chain) {
    switch (chain) {
        case LimbChain::kHead:     return 0;
        case LimbChain::kLeftArm:  return 1;
        case LimbChain::kRightArm: return 2;
        case LimbChain::kLeftLeg:  return 3;
        case LimbChain::kRightLeg: return 4;
        case LimbChain::kTorso:
        case LimbChain::kNone:
            break;
    }
    return -1;
}

[[nodiscard]] std::string_view ScrapeChainName(std::size_t index) {
    constexpr std::string_view kNames[kScrapeChainCount] = {"head", "left arm", "right arm",
                                                            "left leg", "right leg"};
    return index < kScrapeChainCount ? kNames[index] : "?";
}

// ── There is no voice budget ─────────────────────────────────────────────────
//
// Neither per actor nor global. A voice-limit run started 288 sounds with 224
// alive at once and the manager holding 257, refusing none: it ran out of sound
// to play before it found a ceiling. The per-actor cap - eight, then twelve, from
// the design's "six to eight per actor" (§14) - measured the same way.
//
// "No cap" is not "no limit on how much you will hear": the rate cap, the chain
// merge, temporal masking and burst shaping decide that, and they judge the
// contact in front of them. A ceiling on the count judges nothing - it takes
// sound from whoever asked last rather than from whoever mattered least. A
// composite is one voice however many layers it has, so the count was never
// measuring moments anyway.
//
// The bookkeeping survives as a leak detector: a non-zero `LiveVoices()` with
// nothing tracked is a loop booked and never given back.

/// A loop's voice does not end on a clock; it ends when it is stopped.
constexpr TimeMs kNever = 1.0e18;

/// One voice in flight. The id is 0 for a one-shot and the loop's own id for a
/// loop, which is how a stop gives the voice back.
///
/// `actorId` is carried so the list can be swept by owner: a loop books `kNever`
/// and is only ever given back by name, so an actor released mid-fall used to
/// strand its entry for the rest of the session. Harmless now the count is
/// uncapped, still a leak, and `LiveVoices()` is how it is seen.
struct Voice {
    TimeMs endsMs{};
    std::uint32_t voiceId{};
    ActorId actorId{};
};
constexpr std::size_t kSiteCount = static_cast<std::size_t>(LimbSite::kCount);

[[nodiscard]] bool Ancient(TimeMs t) { return t <= kLongAgo / 2.0; }

[[nodiscard]] float Lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Two thresholds into a clamped 0..1 position between them - config.md's ramp.
[[nodiscard]] float RampAt(float value, float lo, float hi) {
    return std::clamp((value - lo) / std::max(1.0f, hi - lo), 0.0f, 1.0f);
}

[[nodiscard]] float SemitonesToRatio(float semis) { return std::pow(2.0f, semis / 12.0f); }

/// Every window is max(k * frameTime, floor), never a fixed millisecond count
/// and never a frame count. The target range is 24-144 fps and a fixed window
/// behaves as a different system at each end (07 §4).
[[nodiscard]] float Window(float floorMs, float frameMs, float k) {
    return std::max(k * frameMs, floorMs);
}

class Rng {
public:
    void Seed(std::uint32_t seed) {
        m_state = seed == 0 ? 0x9E3779B97F4A7C15ULL : (0x9E3779B97F4A7C15ULL ^ seed);
    }
    [[nodiscard]] std::uint64_t Next() {
        m_state ^= m_state >> 12;
        m_state ^= m_state << 25;
        m_state ^= m_state >> 27;
        return m_state * 0x2545F4914F6CDD1DULL;
    }
    /// 0 .. 1
    [[nodiscard]] float Unit() {
        return static_cast<float>(static_cast<std::int64_t>(Next() >> 11)) / 9007199254740992.0f;
    }
    /// -1 .. 1
    [[nodiscard]] float Bipolar() { return Unit() * 2.0f - 1.0f; }

private:
    std::uint64_t m_state{0x9E3779B97F4A7C15ULL};
};

/// Mix a take-wide seed with one contact's row into a seed of its own. The same
/// avalanche the sound bank's variant picker uses, so the two agree about what
/// "this contact's randomness" means.
[[nodiscard]] std::uint32_t StableSeed(std::uint32_t seed, std::uint32_t token) {
    std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(seed) << 32) ^
                      (static_cast<std::uint64_t>(token) << 8);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return static_cast<std::uint32_t>(h) | 1u;
}

// ── Stage 0's output ─────────────────────────────────────────────────────────

/// An admitted, tagged contact: everything Stage 3 is allowed to see about one
/// collision.
struct Contact {
    TimeMs timeMs{};
    ActorId actorId{};
    std::uint16_t limbIndex{};
    std::uint32_t sourceSeq{};

    float impactSpeed{};
    float tangentSpeed{};
    float angularSpeed{};
    float bodySpeed{};  ///< the limb's own linear speed, which is what says the fall is still moving
    Vec3 position{};
    Vec3 normal{};

    LimbSite site{};
    LimbChain chain{};
    Coverage coverage{};
    SurfaceClass surface{};
    /// The radius the intensity curve was sized off - the profile's where the
    /// skeleton is known, the solver's shape otherwise. Carried because the
    /// slide-end impact is built without a feed row to look it up from.
    float limbRadius{};

    float intensity{};    ///< 0..1, what the loudness curve was fed
    float onsetGainDb{};  ///< the level before any layer balance
    /// Intensity before the glancing-landing rule touched it. Stage 2 and the
    /// leading-limb tally read this, not the reduced figure: quietening a landing
    /// must not walk the event into a different phase.
    float rawIntensity{};
    /// What the glancing-landing rule charged this contact, 1.0 for untouched.
    /// Kept on the contact so the crunch gate can optionally see it without
    /// recomputing the ratio.
    float glanceScale{1.0f};
    /// Every Shape-stage rule's post-arbitration half, summed: the glancing cut
    /// and the air-time lifts. Rides through to Emit so it changes loudness and
    /// nothing else. One field rather than one per rule - every `Shape()` call
    /// already traces its own delta, so attribution lives there.
    float modTrimDb{};
    std::uint64_t otherBody{};
    bool graze{};
    bool selfContact{};
    bool claimed{};  ///< the one cross-strategy mechanism there is
};

/// The order Stage 3 sees contacts in: strongest first, ties broken by source
/// row. Named rather than a lambda in Ingest because Stage 2 adds a contact of
/// its own and it has to land where a solver-reported one would have.
[[nodiscard]] bool StrongestFirst(const Contact& a, const Contact& b) {
    if (a.impactSpeed != b.impactSpeed) {
        return a.impactSpeed > b.impactSpeed;
    }
    return a.sourceSeq < b.sourceSeq;
}

struct Layer {
    SlotId slot{};
    float offsetMs{};
    float gainDb{};
    float pitch{1.0f};
    CueReason reason{};
};

/// What a strategy hands the arbitrator. Strategies propose; the arbitrator
/// disposes. No strategy touches another's cues.
struct Proposal {
    TimeMs timeMs{};
    float levelDb{kSilentDb};  ///< the loudest layer, which is what masking compares

    /// How much this proposal *matters*, which is not how loud it is -
    /// `config.md`'s level/rank split.
    ///
    /// `levelDb` plus the site's `[Arbitration]` weight, assembled once in
    /// `Arbitrate` so no strategy can reach it. Exactly four rules read it: the
    /// sort, the rate cap's override comparison, the chain merge, and what is
    /// stored in `lastOnsetDb`/`chainLastDb`. Masking and `maskCeilingDb` keep
    /// reading `levelDb` - "can this be heard under that" is about air, not
    /// importance. Every weight defaults to 0.
    float priorityDb{kSilentDb};

    ActorId actorId{};
    std::uint16_t limbIndex{};
    std::uint32_t sourceSeq{};
    LimbSite site{};
    LimbChain chain{};
    SurfaceClass surface{};
    Coverage coverage{};
    float intensity{};
    float impactSpeed{};  ///< carried purely so a dropped proposal can say what was lost
    Vec3 position{};

    Layer layers[kMaxLayers]{};
    int layerCount{};

    /// An onset counts against the rate cap, the chain merge and the burst
    /// shape. A ride-along is an accessory to one - a crunch on the impact it
    /// belongs to - and lives or dies with its parent. A bypass is neither: a
    /// loop or the closing cue is not an onset at all.
    bool rideAlong{};
    bool bypass{};

    /// Every rule's post-arbitration half, summed: the glancing cut, the lead
    /// halo, the head accent's voicing. Added at render, so it changes loudness
    /// without touching the sort, the class or the phase.
    float postTrimDb{};

    // ── the Budget stage's whole vocabulary ──────────────────────────────────
    //
    // Three fields, written by every rule that buys a proposal past the
    // arbitrator's budgets (the hero moment today), so the arbitrator stays a
    // list of fixed rules.
    //
    // A Budget-stage rule may move the gaps and the burst and may never touch a
    // level: a level before arbitration is also a rank, so reaching for one would
    // not buy a contact past the budget, it would make it outrank the frame.

    float burstGapScale{1.0f};
    float rateCapScale{1.0f};

    /// Close whatever burst was open and let this proposal start its own with the
    /// grain count at zero. The strongest waiver there is, and the only one that
    /// rescues a landing from a burst some scuff opened three hundred milliseconds
    /// earlier while the body was still in the air.
    bool resetsBurst{};

    CueOp op{CueOp::kPlayOneShot};
    std::uint32_t voiceId{};
    float fadeMs{};
    std::int32_t boneIndex{-1};
};

using ProposalList = std::vector<Proposal>;

// ── Stage 1, plus the arbitrator's per-actor memory ──────────────────────────

struct ActorRuntime {
    CrashState state{};
    bool inUse{};
    std::string name;
    bool isPlayer{};

    /// What the game said this actor was doing, off the last event that carried
    /// it - stamped rather than inferred.
    ///
    /// Only consulted through `Animated()`, and only in animated mode. kUnknown
    /// reads as "not animated", so an old take with no phase on its rows is
    /// judged the way it always was.
    ActorPhase phase{ActorPhase::kUnknown};

    /// Whether the game says this actor is fighting or being fought, off the same
    /// events as `phase` and stamped the same way. Meaningless while they are
    /// down, which is exactly what `ModeFor` says about it.
    bool inCombat{};

    /// Which of the three tuning columns this actor is read through, recomputed
    /// from the two above every time either is stamped. Held rather than derived
    /// at each read so one tick cannot resolve two ways.
    ActorMode mode{ActorMode::kRagdoll};

    /// Whether the player has this body in their hands, off the last
    /// `held_start`/`held_stop` row. Stamped like `phase`, never inferred: only
    /// the game thread can ask HIGGS, and only in VR is there anything to ask.
    /// Read by exactly one rule, `AccumDamage:bRequireHeld`.
    bool heldByPlayer{};

    TimeMs firstContactMs{kLongAgo};
    TimeMs lastAdmittedMs{kLongAgo};
    float energyRecent{};
    /// The same envelope before this tick's contacts were folded in. The hero
    /// rule's dominance clause needs it: `energyRecent` has already taken the
    /// maximum with this frame's contacts by the time Stage 2 runs, and a contact
    /// cannot be 1.3x itself.
    float energyRecentBeforeTick{};
    TimeMs energyStampMs{};
    float siteEnergy[kSiteCount]{};
    float groundZ{};
    bool haveGroundZ{};
    float headZ{};
    bool haveHeadZ{};

    /// The two geometric signals the head rules read, refreshed in UpdateState.
    /// `lastWorldContactMs` is stamped by any limb *but* the head and neck
    /// touching a non-body; `worldContactBeforeTickMs` is that value from before
    /// this tick's contacts were folded in, which lets an arm coming down with the
    /// head read as part of the same dive. The peer pair is this tick only.
    ///
    /// Hands keep their own stamp so the head half can discount a hand thrown out
    /// in front of a dive - see `headExcludeHands` and `headHandGraceMs`. The body
    /// half, the company rule and the hero arrival clause count them either way.
    TimeMs lastWorldContactMs{kLongAgo};
    TimeMs worldContactBeforeTickMs{kLongAgo};
    TimeMs lastHandContactMs{kLongAgo};
    TimeMs handContactBeforeTickMs{kLongAgo};
    std::int32_t worldPeers{};
    float worldPeerFastest{};

    /// A hero opened or re-anchored this tick and the burst is to start over. Set
    /// by `AnchorHero`, consumed by the arbitrator, cleared at the end of the tick
    /// - a moment resets the burst once, not on every contact inside its window.
    bool heroResetsBurst{};

    // ── the pose, folded in from this tick's kLimbSample rows ────────────────
    //
    // Accumulated in Ingest, consumed in UpdateState: a mass-weighted centre needs
    // every limb of the tick before it means anything.
    Vec3 poseSum{};        ///< sum of position x nominal mass
    Vec3 poseVelSum{};     ///< sum of velocity x nominal mass
    float poseMass{};      ///< the weights, so the sums can be divided
    int poseLimbs{};       ///< how many limbs this tick actually reported
    bool sawPoseEver{};    ///< distinguishes "no pose in this take" from "none this tick"
    /// Whether any collision at all reached this actor this tick, self-hits and
    /// unvoiced contacts included. Not about sound: it tells the driven test that
    /// the tick's acceleration already has a collision to explain it.
    bool sawContactThisTick{};

    /// Last tick's centre, and when it was taken. Vertical acceleration is
    /// differenced from the pair, and the stamp is why a dropped tick cannot
    /// silently turn into an enormous acceleration.
    Vec3 lastCom{};
    Vec3 lastComVel{};
    TimeMs lastComMs{kLongAgo};
    bool haveLastCom{};

    /// How long the acceleration has looked like free fall, and how long since
    /// it last did. The pair gives the flag hysteresis at both ends - see
    /// `fFreeFallMinMs` and `fFreeFallHoldMs`.
    float freeFallForMs{};
    TimeMs lastFreeFallMs{kLongAgo};
    /// When a collision that was not a self-hit last reached this actor - the
    /// motion machine's `touched`, written down so the next tick can see it.
    ///
    /// The other half of the hysteresis. `airborne` is a latch on the
    /// *acceleration* and a body bouncing along a floor is ballistic between
    /// bounces, so the latch holds through the whole bounce train and nothing
    /// remembered the body had just been hit. See `fLandedHoldMs`.
    TimeMs lastTouchedMs{kLongAgo};
    /// When something last pushed this body in a way gravity cannot explain.
    TimeMs lastDrivenMs{kLongAgo};
    /// Whether anything has pushed this body during the *current* flight. Per
    /// flight, not per frame or per edge: a gate high enough to make the flag
    /// flicker would otherwise report more driven flights than one that holds it
    /// steadily on.
    bool drivenThisFlight{};
    /// Where the body was when the current flight began, so the drop can be
    /// measured relative and survive a staircase.
    float airborneStartZ{};
    /// The flight that just ended, latched at the edge where the flag drops. A
    /// landing rule asks about a flight that is over; the live clock answers a
    /// different question and is wrong at both ends. Held for
    /// `MotionConfig::landingWindowMs` and gone with the runtime, so a fall cannot
    /// be paid for twice.
    float lastFlightMs{};
    float lastFlightDropUnits{};
    TimeMs lastFlightEndedMs{kLongAgo};
    /// How far the measured centre moved this tick. The honest answer to "how
    /// far has the body slid", and zero on a take with no pose.
    float comStepUnits{};

    // arbitration
    TimeMs lastOnsetMs{kLongAgo};
    float lastOnsetDb{kSilentDb};  ///< what the rate cap is asked to yield to
    TimeMs chainLastMs[kChainCount]{};
    float chainLastDb[kChainCount]{};
    TimeMs maskStampMs{};
    TimeMs burstStartMs{kLongAgo};
    TimeMs burstLastMs{kLongAgo};
    int burstGrains{};
    Vec3 collapsePoint{};
    TimeMs collapseUntilMs{kLongAgo};

    /// Where the body was last known to be, from the most recent admitted contact.
    /// What the continuous cues are placed at.
    ///
    /// `collapsePoint` cannot do this job: it is set only when a hero moment
    /// anchors and defaults to the origin, so a loop placed there before the first
    /// hero plays at the centre of the cell and one placed there afterwards stays
    /// nailed where the moment was anchored.
    Vec3 bodyPoint{};
    bool haveBodyPoint{};

    // strategy state, owned by the actor rather than by the strategy so the
    // strategies stay stateless and one instance can serve every actor

    /// What each damage tier has spent, one ledger per part per tier. Six counters
    /// rather than one shared, which let whichever rule reached a contact first
    /// decide which layer you heard.
    ///
    /// `lastMs` is the *proposal* time, not the emit time, like the count:
    /// spacing is about how close together the engine is willing to break bone, so
    /// a tier charging only for survivors would let a dropped crunch reopen the
    /// window immediately.
    struct DamageLedger {
        int count{};
        TimeMs lastMs{kLongAgo};
    };
    std::array<std::array<DamageLedger, 2>, static_cast<std::size_t>(DamageSite::kCount)> damage{};

    bool riseRunning{};
    std::uint32_t riseVoice{};
    float riseLastDb{kSilentDb};
    /// The anchor each running loop was last *told* about, which is not the anchor
    /// it should be on. The renderer re-attaches only when a cue arrives, so the
    /// engine has to know what it sent. See `EmitLoopProposal`.
    std::uint16_t riseAnchor{};
    /// The motion trim as the bed hears it: `fBedTrimGlideMs` of glide behind
    /// `MotionBudgetFor`. Seeded on the first tick rather than started at zero, so
    /// the first loop of a take does not fade up out of a trim it was never in.
    float bedTrimDb{};
    bool haveBedTrim{};

    /// The garment loop. One voice per actor, not one per limb: a shirt is one
    /// object.
    bool rustleRunning{};
    std::uint32_t rustleVoice{};
    float rustleLastDb{kSilentDb};
    std::uint16_t rustleAnchor{};
    /// The armour class the loop started on, pinned for its life like `scrapeSlot`:
    /// swapping the conditional variant under a running voice is a click.
    Coverage rustleCoverage{Coverage::kCloth};

    bool scrapeRunning{};
    std::uint32_t scrapeVoice{};
    float scrapeLastDb{kSilentDb};
    std::uint16_t scrapeAnchor{};
    /// The slot the body grind started on - the surface-coloured one where a file
    /// exists. Held for the loop's life: a body crossing from boards onto
    /// flagstone mid-skid must not swap files under a running voice.
    SlotId scrapeSlot{SlotId::kScrapeLoop};

    /// The armour rattle riding the body grind, as its own voice. Not a second
    /// layer on the grind's proposal: a loop proposal carries one voice id through
    /// start, update and stop, so two layers sharing it would be two sounds with
    /// one lifecycle. Slot pinned at start like `scrapeSlot`.
    bool armorSlideRunning{};
    std::uint32_t armorSlideVoice{};
    float armorSlideLastDb{kSilentDb};
    std::uint16_t armorSlideAnchor{};
    SlotId armorSlideSlot{SlotId::kArmorCloth};

    /// The mass under the grinds, as its own voice.
    ///
    /// **One per actor, not one per grind**: the floor is one object, as a shirt
    /// is. Five copies of the same bed would be five incoherent bass beds summing,
    /// which beats audibly down there.
    ///
    /// No slot field beside it: the bed has no surface variants to pin, because
    /// boards and flagstone colour the grit and mass sounds the same under any
    /// floor.
    bool scrapeBedRunning{};
    std::uint32_t scrapeBedVoice{};
    float scrapeBedLastDb{kSilentDb};
    std::uint16_t scrapeBedAnchor{};
    /// The bone the body grind hangs on. Nearest the contact, not the pelvis and
    /// not the lowest - see `bBodyFollowsContact`.
    std::uint16_t bodyAnchor{};
    bool haveBodyAnchor{};

    /// One limb scrape per chain. Several run at once, because several limbs can
    /// be scraping at once, and each is on its own bone.
    struct LimbLoop {
        bool running{};
        std::uint32_t voice{};
        float lastDb{kSilentDb};
        SlotId slot{SlotId::kScrapeLimb};

        /// The bone the loop is on, and the bone that has been leading. The second
        /// becomes the first only after `fLimbHoldMs` - the stickiness that stops
        /// a scrape smearing as the contact hops between the bones of one arm.
        std::uint16_t anchor{};
        bool haveAnchor{};
        std::uint16_t wantAnchor{};
        TimeMs wantSinceMs{kLongAgo};
        /// The anchor this loop was last told about, against `anchor`, which is
        /// where it should be. The two differ for exactly as long as a hop is
        /// waiting on a cue to carry it.
        std::uint16_t sentAnchor{};

        /// A decaying peak-hold over this chain's own graze tangent speed, on the
        /// same constant as the body's. What the chain's level and pitch track.
        float tangent{};
        float tickTangent{};
        TimeMs lastGrazeMs{kLongAgo};
        /// The feed event behind the most recent graze on this chain, so the entry
        /// scuff is attributed to the collision rather than the tick that noticed
        /// it. The renderer groups cues by `sourceSeq`, so a grain emitted with a
        /// zero would be mixed into whatever else on this actor had none.
        std::uint32_t lastGrazeSeq{};
        LimbSite site{};
        SurfaceClass surface{SurfaceClass::kSoft};
        Vec3 point{};
        bool havePoint{};
    };
    LimbLoop limbLoops[kScrapeChainCount]{};

    // -- how much body is on the floor ---------------------------------------
    //
    // The measurement the slide never had. Per limb, because the answer is a sum
    // over the limbs that are demonstrably rubbing and the contact stream is the
    // only thing that says which those are.

    struct LimbTrack {
        /// Our own anatomical mass for this limb's site, from the profile. Never
        /// the solver's: those are asymmetric enough that a right arm would count
        /// for three times a left one (07 §6).
        float mass{};
        /// When it last grazed the world. Held for `fContactHoldMs`: a fraction
        /// taken from one tick alone flickers at solver rate.
        TimeMs grazeMs{kLongAgo};
        /// Where the limb was, from the pose sidecar. Empty on a take with none,
        /// which makes the body anchor fall back on the heaviest limb that grazed.
        Vec3 pos{};
        bool havePos{};

        // -- what the garment reads ------------------------------------------
        //
        // Per limb because the rustle drive sums how each one moves *relative to
        // the body*, which is exactly what the mass-weighted centre averages away.

        /// How much cloth hangs here, from `FabricWeight(site, coverage)`. Seeded
        /// with the profile's mass, and 0 for a limb whose profile never arrived -
        /// which reads as "no garment", the quieter answer.
        float fabric{};
        /// This tick's velocity, and the previous tick's with its timestamp. Two
        /// fields because the fold writes the new value while the difference still
        /// needs the old one. Its own timestamp rather than the centre's, because
        /// a limb can miss a tick the centre does not.
        Vec3 tickVel{};
        bool haveTickVel{};
        Vec3 vel{};
        bool haveVel{};
        TimeMs velMs{kLongAgo};
        /// This tick's rotation, rad/s, straight off the pose sample.
        float angularSpeed{};
        /// The limb's radius, to turn radians into surface speed - what drags a
        /// sleeve is how fast the surface moves, not how many radians it turned.
        float radius{};

        /// This limb's own violence: a decaying peak-hold over its normalised
        /// thrash, 0 to 1, blended in by `DamageViolenceConfig::limbShare`. Not
        /// fabric-weighted, unlike the rest of this row - a bare arm breaks as
        /// well as a sleeved one.
        float violence{};
    };
    LimbTrack limbs[kMaxLimbs]{};

    /// Sum of `fabric` over the profile, seeded beside it. The test that tells
    /// "wearing nothing we can name" apart from "this tick carried no pose",
    /// which the garment's own per-tick sum cannot do - see `ConsumeRustle`.
    float fabricTotal{};

    /// Accumulated damage, one pool per limb. Its own array rather than more
    /// fields on `LimbTrack`: that row is derived from the *pose* stream and this
    /// from the *contact* stream, so they fill on different ticks and survive a
    /// missing sidecar differently.
    struct AccumTrack {
        /// How worked-over this limb is. Rises on every qualifying contact,
        /// heals exponentially, clamped at `fMaxPool`.
        float pool{};
        /// How far up the ladder this limb has climbed. Comes back down as the
        /// pool heals, so a limb left alone and then attacked again climbs a
        /// second time rather than being spent for ever.
        int stage{};
        /// Charged per knockdown, like the tier ledgers, and reset on the same
        /// edge.
        int fired{};
        TimeMs lastMs{kLongAgo};
        TimeMs lastFireMs{kLongAgo};
    };
    AccumTrack accum[kMaxLimbs]{};
    /// Breaks the accumulated ladder has produced on this body this knockdown,
    /// across every limb. The per-limb cap alone cannot bound a body: eighteen
    /// limbs times a per-limb budget is not a budget.
    int accumFired{};
    std::size_t limbCount{};
    /// The sum of `LimbTrack::mass` over the whole skeleton, or 0 when the
    /// profile never arrived. Zero means the fraction reads 0, which is the
    /// quieter answer - limb-only, never a guess at loud.
    float bodyMass{};
    /// The share of the body that is rubbing right now, 0 to 1.
    float contactFraction{};

    /// Mass-weighted centre of the grazing contacts, held between ticks. What
    /// the body loop's bone is chosen as the nearest to.
    Vec3 grazeCentre{};
    bool haveGrazeCentre{};
    /// The limb a whole-body loop hangs on: the torso, resolved from the profile's
    /// bone names by `SiteFromBoneName` and never from an index. (It used to
    /// default to 0, which on a humanoid ragdoll is `NPC COM [COM ]` by accident -
    /// but on a creature or modded skeleton the first body is a wing or a tail.)
    /// Falls back to 0 when no bone resolves to a torso.
    std::uint16_t bodyLimb{};

    /// What the *actor* is wearing, taken from the torso limb once at attach. The
    /// contactless cues (the airborne rise) have no limb to read a class off, and
    /// `Armor:bPerLimb = 0` wants this for the ones that do.
    Coverage bodyCoverage{};
    /// The heaviest limb that grazed, for a take with no pose to measure a
    /// nearest bone against.
    std::uint16_t heaviestGrazeLimb{};
    bool haveHeaviestGraze{};

    /// When a catch last fired, so the grain layer is a texture rather than a
    /// rattle.
    TimeMs lastGrainMs{kLongAgo};

    // ── the slide ────────────────────────────────────────────────────────────
    //
    // Owned by the actor and read by the motion axis, because when a slide starts
    // and stops is one decision. It used to be two - the strategy kept its own
    // gates alongside `[Motion]`'s - and they disagreed, so a knockdown could sit
    // in `Slide` with no loop under it or grind away in `Tumble`.

    TimeMs grazeSinceMs{kLongAgo};  ///< when the current run of grazes opened
    TimeMs grazeLastMs{kLongAgo};   ///< and when the last one arrived
    /// Decaying peak-hold over contact tangent speed, constant `fSlideGraceMs`. A
    /// running max (what this was) never comes down, so one fast skim held the
    /// entry test open for the rest of the knockdown.
    float slideTangent{};
    /// The fastest tangent this tick, for the distance fallback on a take with
    /// no pose. Cleared once it has been integrated.
    float slideTickTangent{};
    float slideDistance{};  ///< units covered since the run of grazes opened

    /// How fast the *body* was going while it was still grazing: a peak-hold that
    /// decays during the grazing and then stops, so afterwards it reports the
    /// speed at interruption rather than one a hundred milliseconds of
    /// deceleration has eaten.
    float slideSpeed{};

    /// The last graze's own tags. The slide-end impact is a contact the solver
    /// never reported, so it is coloured by the limb that was grinding along the
    /// floor a frame ago rather than by a guess.
    std::uint16_t scrapeLimb{};
    LimbSite slideSite{};
    LimbChain slideChain{};
    /// The feed event behind that graze, so the body grind's entry scuff is
    /// attributed to the collision it came from. The renderer groups cues by
    /// `sourceSeq`, and a grain carrying zero would be mixed into whatever else on
    /// this actor had none.
    std::uint32_t slideSeq{};
    Coverage slideCoverage{};
    SurfaceClass slideSurface{SurfaceClass::kSoft};
    float slideRadius{};


    EngineStats stats{};
};

/// Whether this actor is being heard *because* of animated mode: on their feet,
/// and only reaching the pipeline because the ragdoll gate was bypassed. The
/// three per-layer switches in `GameIntegration` ask about this and nothing else -
/// none may touch an actor who is actually ragdolling. kUnknown counts as
/// ragdolling, so a take recorded before the phase was stamped is judged as tuned.
[[nodiscard]] bool Animated(const AlgorithmConfig& cfg, const ActorRuntime& actor) {
    return cfg.game.animatedMode && actor.phase != ActorPhase::kRagdoll &&
           actor.phase != ActorPhase::kUnknown;
}

/// How fast the body is travelling, for everything the slide decides. The measured
/// centre of mass wherever the take carries pose - one limb's tangent speed is the
/// speed of a limb. Without pose it falls back on the held tangent rather than
/// `bodySpeed`, which is then the last contact's limb speed decaying to nothing.
[[nodiscard]] float SlideSpeed(const ActorRuntime& actor) {
    return actor.state.haveBodySamples ? actor.state.bodySpeed : actor.slideTangent;
}

// ── Stage 3: the pluggable layer ─────────────────────────────────────────────

/// Everything a strategy may see. Deliberately narrow: the crash state, the
/// config, a deterministic random source and the clock. No sink, no bank, no
/// other strategy.
struct StrategyContext {
    const AlgorithmConfig& cfg;
    ActorRuntime& actor;
    Rng& rng;
    TimeMs nowMs{};
    float frameMs{};
    std::uint32_t* nextVoiceId{};

    /// The run's totals, for the strategies that keep a counter of their own. The
    /// engine-wide twin of the per-actor stats reachable through `actor`.
    EngineStats* stats{};

    /// Read-only, and only ever asked `HasSound`. A strategy proposes; resolution
    /// happens once at Emit, so the cue list and the audio cannot disagree about
    /// which file played. But proposing a layer nothing can voice is not free: on
    /// the loop paths it costs a voice id and a stop cue for a sound that never
    /// started.
    const SoundBank* bank{};
};

/// Whether a layer on this slot could be heard. False for a slot nobody has
/// recorded yet, which is how the armour section stays silent until somebody
/// does - see `SoundBank::HasSound`.
[[nodiscard]] bool CanSound(const StrategyContext& ctx, SlotId slot) {
    return slot != SlotId::kCount && ctx.bank != nullptr && ctx.bank->HasSound(slot);
}

class IStrategy {
public:
    virtual ~IStrategy() = default;

    /// One admitted contact. Return true to *claim* it, which stops every later
    /// strategy from proposing for the same contact. Claiming is the only
    /// cross-strategy mechanism there is.
    virtual bool Propose(const StrategyContext&, const Contact&, ProposalList&) { return false; }

    /// Once per actor per tick, for the strategies that are continuous rather
    /// than per-contact.
    virtual void ProposeTick(const StrategyContext&, ProposalList&) {}

    [[nodiscard]] virtual const char* Name() const = 0;
};

/// The provenance every proposal carries, filled from the contact it came from.
[[nodiscard]] Proposal FromContact(const Contact& contact) {
    Proposal proposal{};
    proposal.timeMs = contact.timeMs;
    proposal.actorId = contact.actorId;
    proposal.limbIndex = contact.limbIndex;
    proposal.sourceSeq = contact.sourceSeq;
    proposal.site = contact.site;
    proposal.chain = contact.chain;
    proposal.surface = contact.surface;
    proposal.coverage = contact.coverage;
    proposal.intensity = contact.intensity;
    proposal.impactSpeed = contact.impactSpeed;
    proposal.postTrimDb = contact.modTrimDb;
    proposal.position = contact.position;
    return proposal;
}

/// Which armour class a contact's layers should be coloured by. Per limb by
/// default, which needs no mechanism - `contact.coverage` is already the class of
/// the limb that hit. Off, every contact reads the actor's cuirass instead.
[[nodiscard]] Coverage EffectiveCoverage(const StrategyContext& ctx, const Contact& contact) {
    return ctx.cfg.armor.perLimb ? contact.coverage : ctx.actor.bodyCoverage;
}

/// The class to hang on a cue that never came from a contact - the airborne rise
/// is the one such cue left. A cuirass is the honest default, with the last
/// graze's limb and "no opinion" as the alternatives. `kCloth` is "no opinion":
/// the class is defined as clothing *and anything we cannot decide*.
[[nodiscard]] Coverage ActorClassCoverage(const StrategyContext& ctx) {
    switch (ctx.cfg.armor.actorClassSource) {
        case 1:  return ctx.actor.slideCoverage;
        case 2:  return Coverage::kCloth;
        default: return ctx.actor.bodyCoverage;
    }
}

/// The per-class pitch bias, in semitones. Zero at the shipping defaults.
[[nodiscard]] float ArmorPitchSemis(const AlgorithmConfig& cfg, Coverage coverage) {
    if (!cfg.armor.enabled) {
        return 0.0f;
    }
    switch (coverage) {
        case Coverage::kBare:  return cfg.armor.barePitchSemis;
        case Coverage::kCloth: return cfg.armor.clothPitchSemis;
        case Coverage::kLight: return cfg.armor.lightPitchSemis;
        case Coverage::kHeavy: return cfg.armor.heavyPitchSemis;
    }
    return 0.0f;
}

/// The per-class trim over the whole stack, applied at render, zero by default.
/// Heavy armour is louder; this says so without touching which contact won.
[[nodiscard]] float ArmorCompositeTrimDb(const AlgorithmConfig& cfg, Coverage coverage) {
    if (!cfg.armor.enabled) {
        return 0.0f;
    }
    switch (coverage) {
        case Coverage::kBare:  return cfg.armor.bareCompositeTrimDb;
        case Coverage::kCloth: return cfg.armor.clothCompositeTrimDb;
        case Coverage::kLight: return cfg.armor.lightCompositeTrimDb;
        case Coverage::kHeavy: return cfg.armor.heavyCompositeTrimDb;
    }
    return 0.0f;
}

/// At arm's length the spatial collapse stops helping and starts sounding like
/// the audio is inside your head, so your own limbs carry their own voices.
[[nodiscard]] std::int32_t BoneFor(const StrategyContext& ctx, std::uint16_t limbIndex) {
    return ctx.actor.isPlayer && ctx.cfg.player.enabled && ctx.cfg.player.attachToBones
               ? static_cast<std::int32_t>(limbIndex)
               : -1;
}

/// A body that has touched nothing has the longest air time there is, not the
/// shortest. Every caller caps this against its own ramp or a threshold, so one
/// sentinel serves both.
constexpr float kAirTimeUnbounded = 1e9f;

/// How long the actor had been clear of the world when this contact arrived.
///
/// Contacts inside one frame arrive microseconds apart (measured median 1 us,
/// against 20 ms between batches - what `frameGapMs` separates). Vayne log_2's
/// faceplant lands its hand 3 microseconds before its skull, and reading that as
/// "the body was already down" made the head rule fire on every trivial head roll
/// and never on the dive. Nothing inside the frame bucket can break the air time.
///
/// `excludeHands` is the head half's forgiveness for an arm thrown out in front of
/// a dive, and is off for everything else.
///
/// The drop rides along because the hero test's arrival clause needs both off the
/// same flight; split, the two were read off different ones.
struct FlightMeasure {
    float airMs{};
    float dropUnits{};
};

[[nodiscard]] FlightMeasure FlightFor(const StrategyContext& ctx, const Contact& contact,
                                      bool excludeHands, float handGraceMs) {
    // Measured where the take carries pose: how long the body has actually been
    // unsupported, not how long since anything last touched - which is all the
    // fallback below has. On Vayne_impacts_log_2_cut_4 the old path scored the
    // opening 44.7 u/s scuff at the top of the ramp (nothing had ever touched)
    // while the body was being shoved *upward* at +229 u/s^2, and scored the real
    // fall at 0.26. `excludeHands`/`handGraceMs` mean nothing against a
    // measurement: a hand touching down ends the flight.

    // The only place air time is measured, so this one test covers the head
    // accent's lift, the hero arrival clause and the airborne rise together.
    if (Animated(ctx.cfg, ctx.actor) && !ctx.cfg.game.animatedAirTime) {
        return {};
    }
    if (ctx.actor.state.haveBodySamples) {
        const CrashState& state = ctx.actor.state;
        if (state.airborne) {
            // freeFlightSinceMs, not airborneSinceMs: a body hauled through the
            // air has been unsupported the whole time and falling for none of it,
            // and the rules downstream pay out for falling. See CrashState::driven.
            return {std::max(0.0f, static_cast<float>(contact.timeMs - state.freeFlightSinceMs)),
                    state.fallDropUnits};
        }
        // The flight is over. A landing is judged a frame or two after it arrives,
        // by which point the flag has cleared, so the live clock returned zero on
        // the one contact the rule exists for. The latched flight is what it wants.
        const float sinceLanding =
            Ancient(ctx.actor.lastFlightEndedMs)
                ? kAirTimeUnbounded
                : static_cast<float>(contact.timeMs - ctx.actor.lastFlightEndedMs);
        if (sinceLanding <= std::max(0.0f, ctx.cfg.motion.landingWindowMs)) {
            return {ctx.actor.lastFlightMs, ctx.actor.lastFlightDropUnits};
        }
        return {};
    }

    const float frameGap = std::max(0.0f, ctx.cfg.ingest.frameGapMs);
    const auto elapsedSince = [&](TimeMs stamp) {
        return Ancient(stamp) ? -1.0f : static_cast<float>(contact.timeMs - stamp);
    };

    float sincePeer = elapsedSince(ctx.actor.worldContactBeforeTickMs);
    if (sincePeer <= frameGap) {
        sincePeer = -1.0f;
    }
    // A hand stops counting while it is recent enough to belong to this strike.
    // Older than the grace window it is an ordinary peer, which keeps a
    // break-fall, roll and clipped head from reading as a dive.
    float sinceHand = elapsedSince(ctx.actor.handContactBeforeTickMs);
    if (sinceHand <= frameGap || (excludeHands && sinceHand <= handGraceMs)) {
        sinceHand = -1.0f;
    }

    // Against the most recent peer that still counts. None at all is unbounded,
    // not zero: the first limb to touch is as airborne as anything gets.
    float since = kAirTimeUnbounded;
    if (sincePeer >= 0.0f) {
        since = std::min(since, sincePeer);
    }
    if (sinceHand >= 0.0f) {
        since = std::min(since, sinceHand);
    }
    // No drop without pose. The one caller that reads it is gated on
    // `haveBodySamples` and never gets here.
    return {since, 0.0f};
}

/// The air time alone, for the callers that ramp on it and never ask the drop.
[[nodiscard]] float AirTimeMs(const StrategyContext& ctx, const Contact& contact,
                              bool excludeHands, float handGraceMs) {
    return FlightFor(ctx, contact, excludeHands, handGraceMs).airMs;
}

/// What kind of head strike a contact is.
///
/// Closing speed cannot tell a dive from a sprawl: Vayne log_2 has a 402 u/s head
/// that is the tip of a spine whip (Spine1 298, Spine2 355, Head 402, one frame)
/// and a 294 u/s head that is a genuine faceplant. What separates them is air time
/// (16 and 35 ms for the sprawls, 597 for the dive) and company in the same frame
/// (2 and 4 peers against 1).
///
/// Shared by the three strategies that care rather than passed between them.
struct HeadStrike {
    bool isHead{};
    float air{};     ///< 0..1 ramp over `AirTimeConfig::headClearMs`
    bool airFull{};  ///< the ramp reached the top: the body was fully clear
    bool crowded{};  ///< arrived inside a sprawl
    float gate{};    ///< the head gate after both rules have had their say
    float gainDb{};  ///< what to add to the accent's level before arbitration
    float trimDb{};  ///< ...and after it, where it is loudness alone
    float airDb{};   ///< the air-time rule's share of the gain, after the ceiling
    float airMs{};   ///< ms since the peer contact the air time was measured against
};

[[nodiscard]] HeadStrike ClassifyHead(const StrategyContext& ctx, const Contact& contact) {
    const HeadImpactConfig& head = ctx.cfg.strategies.head;
    const AirTimeConfig& air = ctx.cfg.strategies.airTime;
    HeadStrike strike{};
    if (contact.site != LimbSite::kHead) {
        return strike;
    }
    strike.isHead = true;

    // The gate as it was before either rule existed, so both default off means
    // exactly the old behaviour.
    strike.gate = head.gateFrac * ctx.cfg.intensity.speedRefHigh *
                  (ctx.actor.state.headDown ? 1.0f - head.headDownBonus : 1.0f);

    if (air.headEnabled) {
        const float clear = std::max(1.0f, air.headClearMs);
        const float since =
            std::min(clear, AirTimeMs(ctx, contact, air.headExcludeHands, air.headHandGraceMs));

        strike.airMs = since;
        strike.air = std::clamp(since / clear, 0.0f, 1.0f);
        strike.airFull = strike.air >= 1.0f;
        strike.gate *= 1.0f - air.headGateBonus * strike.air;

        // Before arbitration, where a level is also a rank, so an unbounded boost
        // makes the skull outrank the frame. `headMaxLevelDb` bounds where the
        // accent ends up, not how much is added. A negative `headGainDb` is a cut
        // and is left alone.
        float boost = air.headGainDb * strike.air;
        if (boost > 0.0f) {
            const float headroom = air.headMaxLevelDb - (contact.onsetGainDb + head.gainDb);
            boost = std::min(boost, std::max(0.0f, headroom));
        }
        strike.airDb = boost;
        strike.gainDb += boost;
        strike.trimDb += air.headTrimDb * strike.air;
    }

    if (head.companyEnabled) {
        strike.crowded =
            ctx.actor.worldPeers > head.companyMaxPeers ||
            (ctx.actor.worldPeerFastest > 0.0f &&
             contact.impactSpeed < head.companyLeadFrac * ctx.actor.worldPeerFastest);
        if (strike.crowded) {
            strike.gate *= 1.0f + head.companyGateFrac;
            strike.gainDb += head.companyDampDb;
            strike.trimDb += head.companyTrimDb;
        }
    }

    strike.gate = std::max(0.0f, strike.gate);
    return strike;
}

/// Which file a site's crunch plays. The head keeps `crunch_gran`; the other two
/// get a file of their own and fall back to it when nobody has recorded one.
///
/// Free rather than a member because both damage systems need it and neither owns
/// it: this is a fact about the sound bank, not a tuning decision.
[[nodiscard]] SlotId CrunchSlot(DamageSite site) {
    switch (site) {
        case DamageSite::kHead:  return SlotId::kCrunchGran;
        case DamageSite::kSpine: return SlotId::kSpineCrunch;
        default:                 return SlotId::kLimbCrunch;
    }
}

[[nodiscard]] Proposal RideAlongLayer(const StrategyContext& ctx, const Contact& contact,
                                      SlotId slot, float gainDb, CueReason reason,
                                      float offsetMs) {
    Proposal proposal = FromContact(contact);
    proposal.rideAlong = true;
    proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
    proposal.levelDb = gainDb;
    proposal.layerCount = 1;
    proposal.layers[0].slot = slot;
    proposal.layers[0].offsetMs = offsetMs;
    proposal.layers[0].gainDb = gainDb;
    proposal.layers[0].reason = reason;
    return proposal;
}

// The head refund and the air-time budget reset used to live here: two functions
// writing the same four proposal fields off different evidence, each with its own
// per-knockdown counter. Both were a hero moment bought per contact. Their
// evidence is now the hero test's dominance and arrival clauses, and what they
// bought is `HeroConfig`'s budget, granted once to the moment.

// ── the modifier pipeline ────────────────────────────────────────────────────
//
// See the contract in Config.h. Two functions, replacing four hand-rolled copies
// of each.

/// Shape one contact. The ONE place a rule may move `intensity` or `onsetGainDb`.
/// Three things every hand-rolled copy had to remember, and one forgot:
///
///  - the onset gain is **carried with the intensity delta** rather than
///    recomputed, so a later rule cannot undo what an earlier one charged. Since
///    `GainFromIntensity(i)` is `-range * (1 - i)`, that delta is
///    `range * (after - before)`;
///  - the level lift is **capped**, because a level before arbitration is also a
///    rank - the head halo added its lift with no cap at all;
///  - `rawIntensity` is **never touched**, so a lifted contact cannot walk the
///    actor into a different motion state and quieten everything after it.
///
/// `weight` is the ramp position, 0..1. A rule with no ramp passes 1.
void Shape(Contact& contact, const ShapeLift& lift, float weight, float dynamicRangeDb) {
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const float before = contact.intensity;
    contact.intensity = std::clamp(before + lift.intensity * w, 0.0f, 1.0f);
    contact.onsetGainDb += dynamicRangeDb * (contact.intensity - before);

    float boost = lift.gainDb * w;
    if (boost > 0.0f) {
        boost = std::min(boost, std::max(0.0f, lift.maxLevelDb - contact.onsetGainDb));
    }
    contact.onsetGainDb += boost;
    contact.modTrimDb += lift.trimDb * w;
}

/// Grant one proposal a budget waiver. The ONE place a rule may bend the
/// arbitrator's budgets, and it may not touch a level. Combining takes the
/// **smaller scale** - the more generous of what two rules asked - and the flags
/// are sticky.
void Grant(Proposal& proposal, const BudgetWaiver& waiver) {
    proposal.burstGapScale =
        std::min(proposal.burstGapScale, std::clamp(1.0f - waiver.burstGapFrac, 0.0f, 1.0f));
    proposal.rateCapScale =
        std::min(proposal.rateCapScale, std::clamp(1.0f - waiver.rateCapFrac, 0.0f, 1.0f));
    proposal.resetsBurst = proposal.resetsBurst || waiver.resetsBurst;
}

/// Whether a contact is a graze: sideways motion *instead of* a hit, not as well
/// as one. Above the ceiling the ratio stops meaning anything - log_4's head
/// arrives at 241 u/s with 445 u/s of tangent, which the ratio calls a scrape and
/// which is a skull hitting a floor at speed while sliding.
[[nodiscard]] bool IsGraze(const IngestConfig& ingest, float impactSpeed, float tangentSpeed) {
    return impactSpeed > 1.0f && impactSpeed < ingest.grazeMaxImpactSpeed &&
           tangentSpeed / impactSpeed > ingest.grazeRatio;
}

/// Hand a proposal what a hero moment bought it. Granted per proposal but
/// *decided* once, on the actor: a landing is five limbs arriving in one frame
/// with the same evidence behind each, so letting every one earn its own reset
/// turned one burst into five.
void ApplyHeroBudget(Proposal& proposal, const StrategyContext& ctx) {
    const HeroConfig& hero = ctx.cfg.hero;
    if (!hero.enabled || ctx.actor.state.moment != Moment::kHero) {
        return;
    }
    // Off the actor rather than the config: only the tick the moment was anchored
    // on restarts the burst. A later contact inside the window is a peer joining
    // it, not a reason to start again.
    Grant(proposal, BudgetWaiver{.burstGapFrac = hero.burstGapFrac,
                                 .rateCapFrac = hero.rateCapFrac,
                                 .resetsBurst = ctx.actor.heroResetsBurst});
}

/// Where a loop hangs. The limb is what matters - the renderer attaches the voice
/// to that bone's node, which moves with the body on its own. `position` is the
/// fallback for a node that does not resolve.
struct LoopAnchor {
    std::uint16_t limbIndex{};
    Vec3 position{};
};

/// The whole-body answer, for a loop that belongs to the actor rather than to one
/// limb. See `ActorRuntime::bodyLimb` for why it is resolved and not assumed.
[[nodiscard]] LoopAnchor BodyAnchor(const ActorRuntime& actor) {
    return LoopAnchor{actor.bodyLimb, actor.bodyPoint};
}

void EmitLoopProposal(const StrategyContext& ctx, ProposalList& out, bool& running,
                      std::uint32_t& voice, float& lastGainDb, std::uint16_t& lastAnchor,
                      SlotId slot, float gainDb, float pitch, CueReason reason, float fadeMs,
                      const LoopAnchor& anchor, Coverage coverage,
                      float deadbandDb = 0.75f) {
    // A running loop only needs a cue when something about it changed; without
    // this it emits an update every frame and buries the cue list in no-ops.
    //
    // Per caller, because a bed and a grind want different answers. 0.75 dB suits
    // a loop nobody is meant to notice moving and is most of what a breathing
    // scrape does - see `ScrapeLoopConfig::levelDeadbandDb`.
    //
    // A deadband on the *level* only. Where a loop hangs used to fall under it by
    // accident, since the callers patched the anchor on after this returned: the
    // renderer re-follows on a cue and at no other time, so a grind that hopped
    // hips while its level was steady went on sounding from the hip it left.
    const bool hopped = running && anchor.limbIndex != lastAnchor;
    if (running && !hopped && std::fabs(gainDb - lastGainDb) < std::max(0.0f, deadbandDb)) {
        return;
    }
    lastGainDb = gainDb;
    lastAnchor = anchor.limbIndex;

    Proposal proposal{};
    proposal.timeMs = ctx.nowMs;
    proposal.actorId = ctx.actor.state.actorId;
    proposal.limbIndex = anchor.limbIndex;
    proposal.boneIndex = BoneFor(ctx, anchor.limbIndex);
    proposal.position = anchor.position;
    proposal.surface = ctx.actor.state.surfaceUnder;
    proposal.coverage = coverage;
    proposal.bypass = true;
    proposal.op = running ? CueOp::kUpdateLoop : CueOp::kStartLoop;
    proposal.fadeMs = running ? 0.0f : fadeMs;
    if (!running) {
        voice = (*ctx.nextVoiceId)++;
        running = true;
    }
    proposal.voiceId = voice;
    proposal.levelDb = gainDb;
    proposal.layerCount = 1;
    proposal.layers[0].slot = slot;
    proposal.layers[0].gainDb = gainDb;
    proposal.layers[0].pitch = pitch;
    proposal.layers[0].reason = reason;
    out.push_back(proposal);
}

void StopLoopProposal(const StrategyContext& ctx, ProposalList& out, bool& running,
                      std::uint32_t voice, SlotId slot, CueReason reason, float fadeMs) {
    Proposal proposal{};
    proposal.timeMs = ctx.nowMs;
    proposal.actorId = ctx.actor.state.actorId;
    proposal.bypass = true;
    proposal.op = CueOp::kStopLoop;
    proposal.voiceId = voice;
    proposal.fadeMs = fadeMs;
    proposal.levelDb = kSilentDb;
    proposal.layerCount = 1;
    proposal.layers[0].slot = slot;
    proposal.layers[0].reason = reason;
    out.push_back(proposal);
    running = false;
}

/// The core. Every audible impact is a timed stack rather than a sample, and the
/// biggest part of it - the pitched sub at +65 ms - arrives late. That late sub is
/// what makes a hit read as *mass* rather than *contact*.
class ImpactCompositeStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ImpactComposite"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const ImpactCompositeConfig& impact = ctx.cfg.strategies.impact;
        if (!impact.enabled) {
            return false;
        }

        // Classified here as well as in HeadImpactStrategy rather than passed
        // between the two: it is a handful of floats, and it keeps the strategies
        // independent of each other's running order.
        const HeadStrike strike = ClassifyHead(ctx, contact);

        // The quiet nine of every ten contacts are burst filler. The job is not
        // which of thirty contacts to play; it is to spend what survives on a few
        // tight bursts with real silence between.
        if (contact.intensity < impact.tapBelowIntensity) {
            ProposeTap(ctx, contact, out, strike);
            return false;
        }

        Proposal proposal = FromContact(contact);
        ApplyHeroBudget(proposal, ctx);
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);

        // Pitch is free and continuous here and beats doubling the bank: scatter
        // per voice plus a downward bias with intensity, clamped so it never
        // sounds like a pitch trick.
        const Coverage coverage = EffectiveCoverage(ctx, contact);
        proposal.coverage = coverage;
        // The armour bias joins the intensity bias *before* the clamp, so plate
        // cannot push the composite past the pitch ceiling.
        const float bias = impact.pitchIntensityBiasSemis * contact.intensity +
                           ArmorPitchSemis(ctx.cfg, coverage);
        const float scatter = ctx.rng.Bipolar() * impact.pitchScatterSemis;
        const float semis = std::clamp(bias + scatter, -impact.pitchMaxSemis, impact.pitchMaxSemis);
        const float pitch = SemitonesToRatio(semis);
        // The bias is about *mass*, and the contact instant is not a mass. Hertzian
        // contact time goes as v^(-1/5), so a harder hit is fractionally
        // *brighter*; biasing the transient down with intensity darkens exactly the
        // hits that should read hardest, worst where the clamp catches - over two
        // takes half the composites landed on -3 semis exactly, so the scatter had
        // become a fixed transpose.
        //
        // The body and the sub keep it: a bigger effective mass does ring lower.
        // One `scatter` draw feeds both, so the layers move together and a second
        // Bipolar() cannot shift every downstream draw off a pinned seed.
        const float contactPitch =
            SemitonesToRatio(std::clamp(scatter, -impact.pitchMaxSemis, impact.pitchMaxSemis));

        // `ranks` keeps a layer out of `levelDb`. A floor is what a contact *hit*,
        // not how big it was, so `ProposeTap` has always excluded its skin while
        // the composite folded its own in - the same layer answering the same
        // question two ways.
        //
        // No-op at the shipping defaults (body -8..-2 against surface -12..-6 over
        // the same span, so the skin can never be the max), but that is arithmetic
        // nobody wrote down and it stops being true the moment either is re-voiced.
        //
        // `massPitch` is the same switch for the intensity pitch bias: on for the
        // layers carrying the body's weight, off for the ones about the contact.
        const auto layer = [&](SlotId slot, float offsetMs, float minDb, float maxDb,
                               CueReason reason, bool ranks = true, bool massPitch = true) {
            if (proposal.layerCount >= static_cast<int>(kMaxLayers)) {
                return;
            }
            Layer& out2 = proposal.layers[proposal.layerCount++];
            out2.slot = slot;
            // The onset itself does not move: the 46 ms rate cap was measured
            // against onsets, and scattering the first layer would let two land
            // inside it. Everything after scatters a few ms.
            out2.offsetMs =
                offsetMs + (offsetMs == 0.0f ? 0.0f : ctx.rng.Bipolar() * impact.offsetScatterMs);
            out2.gainDb = contact.onsetGainDb + Lerp(minDb, maxDb, contact.intensity);
            out2.pitch = massPitch ? pitch : contactPitch;
            out2.reason = reason;
            if (ranks) {
                proposal.levelDb = std::max(proposal.levelDb, out2.gainDb);
            }
        };

        // Loudness comes from layer balance, not tiers: a light contact is mostly
        // transient with almost no sub, a heavy one sub-dominant with the transient
        // on top. One continuum, no boundaries to hide.
        layer(SlotId::kImpTransient, impact.transientOffsetMs, impact.transientGainAtMinDb,
              impact.transientGainAtMaxDb, CueReason::kImpactComposite, true, false);
        const SurfaceConfig& surf = ctx.cfg.surfaces;
        if (surf.enabled) {
            // The floor's own offset and ramp, not the section's: glass ramps
            // steeply and carpet barely at all. A class with no block of its own is
            // already holding its parent's numbers, so this is a plain read.
            const SurfaceSkinConfig& skin = surf.Skin(contact.surface);
            layer(SurfaceSlot(contact.surface), skin.offsetMs, skin.gainAtMinDb,
                  skin.gainAtMaxDb, CueReason::kSurfaceSkin, false);
        }
        // What it was wearing, on the same terms as what it hit: after the strike,
        // before the mass, out of the rank. At offset 0 it would fuse with the
        // transient into one brighter click instead of reading as armour.
        //
        // The slot resolves to nothing until somebody records a file for this
        // class, and Emit skips a layer that resolves to nothing.
        const ArmorConfig& armor = ctx.cfg.armor;
        const SlotId armorSlot = ArmorSlot(coverage);
        if (armor.enabled && CanSound(ctx, armorSlot)) {
            layer(armorSlot, armor.offsetMs, armor.gainAtMinDb, armor.gainAtMaxDb,
                  CueReason::kArmorSkin, false);
        }
        // The torso's layer or the limb's - same ramp, offset and rank either way,
        // because which wav carries the mass is about timbre, not size. A bank with
        // no `imp_body_limb` resolves both back to `imp_body`.
        layer(BodySlot(contact.site), impact.bodyOffsetMs, impact.bodyGainAtMinDb,
              impact.bodyGainAtMaxDb, CueReason::kImpactComposite);
        layer(SlotId::kImpSub, impact.subOffsetMs, impact.subGainAtMinDb, impact.subGainAtMaxDb,
              CueReason::kImpactComposite);

        // Held under the stack whatever the ramp says, and necessarily after the
        // stack exists - `levelDb` is not final until the sub is added.
        ClampArmorSkin(proposal, armor.headroomDb);

        out.push_back(proposal);
        return false;
    }

    /// Hold every armour layer of `proposal` at most `headroomDb` under the
    /// proposal's own level. Negative headroom, always: 0 lets the colour tie
    /// with what it colours and positive lets it win.
    static void ClampArmorSkin(Proposal& proposal, float headroomDb) {
        for (int i = 0; i < proposal.layerCount; ++i) {
            if (proposal.layers[i].reason == CueReason::kArmorSkin) {
                proposal.layers[i].gainDb =
                    std::min(proposal.layers[i].gainDb, proposal.levelDb + headroomDb);
            }
        }
    }

private:
    static void ProposeTap(const StrategyContext& ctx, const Contact& contact, ProposalList& out,
                           const HeadStrike& strike) {
        const ImpactCompositeConfig& impact = ctx.cfg.strategies.impact;
        Proposal proposal = FromContact(contact);
        // A tap is what a landing sounds like when the gate said no, so it is
        // exactly what a hero moment's budget is worth spending on.
        ApplyHeroBudget(proposal, ctx);
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
        proposal.levelDb = contact.onsetGainDb + impact.transientGainAtMinDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = SlotId::kLimbTap;
        proposal.layers[0].gainDb = proposal.levelDb;
        // Heavily pitch-scattered: a tap is the one layer where a repeat is
        // audible as a repeat rather than as texture.
        proposal.layers[0].pitch = SemitonesToRatio(
            std::clamp(ctx.rng.Bipolar() * impact.pitchScatterSemis * 1.6f, -impact.pitchMaxSemis,
                       impact.pitchMaxSemis));
        proposal.layers[0].reason = CueReason::kLimbTap;

        // The colour, on what used to be the one cue that could not say what it
        // hit or wore. Same slots the composite's skins resolve to, only quieter,
        // tighter, and held under the grain they colour.
        //
        // Both stay out of `levelDb`, which `AddSkin` enforces by never touching
        // it: colouring a tap must not move it up the arbitrator's sort. The
        // headroom clamp keeps that true of the mix as well as of the sort.
        const SurfaceConfig& surf = ctx.cfg.surfaces;
        // `onTaps` is the floor's because of water: a splash on nine of every ten
        // contacts is absurd where a knock on boards is right. The offset and
        // headroom stay the section's - they describe the grain being coloured.
        const SurfaceSkinConfig& surfSkin = surf.Skin(contact.surface);
        if (surf.enabled && surfSkin.onTaps) {
            AddSkin(proposal, ctx, SurfaceSlot(contact.surface), surf.tapOffsetMs,
                    surfSkin.tapGainAtMinDb, surfSkin.tapGainAtMaxDb, surf.tapHeadroomDb,
                    contact.onsetGainDb, contact.intensity, CueReason::kSurfaceSkin);
        }
        const ArmorConfig& armor = ctx.cfg.armor;
        const SlotId armorSlot = ArmorSlot(EffectiveCoverage(ctx, contact));
        if (armor.enabled && armor.onTaps && CanSound(ctx, armorSlot)) {
            AddSkin(proposal, ctx, armorSlot, armor.tapOffsetMs, armor.tapGainAtMinDb,
                    armor.tapGainAtMaxDb, armor.tapHeadroomDb, contact.onsetGainDb,
                    contact.intensity, CueReason::kArmorSkin);
        }
        out.push_back(proposal);
    }

    /// One colour layer on a proposal: the ramp, the headroom clamp under the
    /// proposal's own level, and the ordinary pitch scatter.
    ///
    /// It never touches `levelDb` - a colour layer in that max would be a colour
    /// layer changing the *rank* the arbitrator sorts by, which is how a scuff on
    /// the floorboards gets ahead of a real impact. Enforced here so the next skin
    /// cannot get it wrong.
    ///
    /// `kCount` for the slot is "this class has no skin".
    static void AddSkin(Proposal& proposal, const StrategyContext& ctx, SlotId slot,
                        float offsetMs, float minDb, float maxDb, float headroomDb,
                        float onsetGainDb, float intensity, CueReason reason) {
        if (slot == SlotId::kCount || proposal.layerCount >= static_cast<int>(kMaxLayers)) {
            return;
        }
        const ImpactCompositeConfig& impact = ctx.cfg.strategies.impact;
        Layer& skin = proposal.layers[proposal.layerCount++];
        skin.slot = slot;
        skin.offsetMs = offsetMs;
        skin.gainDb = std::min(onsetGainDb + Lerp(minDb, maxDb, intensity),
                               proposal.levelDb + headroomDb);
        // The ordinary scatter, not the tap's 1.6x: a skin is colour and wants to
        // stay put.
        skin.pitch = SemitonesToRatio(std::clamp(ctx.rng.Bipolar() * impact.pitchScatterSemis,
                                                 -impact.pitchMaxSemis, impact.pitchMaxSemis));
        skin.reason = reason;
    }
};

/// Head contacts get their own layer and their own gate. It rides along with the
/// composite rather than claiming the contact, because a head landing is still a
/// body landing - the accent is what is extra about it.
class HeadImpactStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "HeadImpact"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const HeadImpactConfig& head = ctx.cfg.strategies.head;
        if (!head.enabled || contact.site != LimbSite::kHead) {
            return false;
        }
        // The only armour rule anywhere near the head, and the whole of it: one
        // read of the contact's class, before any classification, deciding whether
        // this strategy runs at all. No armour skin is layered on `head_impact` and
        // no armour condition runs through ClassifyHead.
        //
        // What goes away is the *accent*, the dull skull thud with the ring on it,
        // which is exactly wrong under a helm. The composite, both skins and damage
        // all still fire - damage through DamageStrategy, which this gate cannot
        // reach even by accident.
        //
        // A strategy-level gate, not a phase-level one, so it does not break "no
        // state suppresses a contact": that rule is about the motion axis, which
        // cannot judge a contact. This reads the contact in front of it.
        const ArmorConfig& armor = ctx.cfg.armor;
        if (armor.enabled &&
            ((armor.noHeadOnLight && contact.coverage == Coverage::kLight) ||
             (armor.noHeadOnHeavy && contact.coverage == Coverage::kHeavy))) {
            return false;
        }
        // Head-down attitude, air time before the head arrived, and how much
        // company it has all move the same gate - see ClassifyHead.
        const HeadStrike strike = ClassifyHead(ctx, contact);

        // Damage is `DamageStrategy`'s now. A gate raised for voicing reasons
        // should not quietly take the consequences with it, and keeping both here
        // meant the head and the body had two rules for one sound over one budget.

        if (contact.impactSpeed < strike.gate) {
            return false;
        }

        Proposal proposal = FromContact(contact);
        // A head impact that *is* the moment is the event, not an accent on one,
        // so it stops being an accessory that dies with whatever composite the
        // arbitrator dropped. Anything else still rides along.
        //
        // Specifically *this contact anchored it*: being merely inside somebody
        // else's hero window makes the accent a peer, not the event. (The trigger
        // used to be the head's own air-time ramp, which says nothing about
        // whether the mix considered this a moment.)
        proposal.rideAlong = !(ctx.cfg.strategies.head.claimsOnsetOnHero &&
                               ctx.actor.state.moment == Moment::kHero &&
                               ctx.actor.state.heroSeq == contact.sourceSeq);
        // An onset of its own has to pay its own way past the budgets, so it
        // carries what the moment is worth. Nothing is spent: the moment was
        // decided once, on the actor.
        if (!proposal.rideAlong) {
            ApplyHeroBudget(proposal, ctx);
        }
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
        proposal.levelDb = contact.onsetGainDb + head.gainDb + strike.gainDb;
        proposal.postTrimDb += head.trimDb + strike.trimDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = SlotId::kHeadImpact;
        proposal.layers[0].offsetMs = ctx.cfg.strategies.impact.bodyOffsetMs;
        proposal.layers[0].gainDb = proposal.levelDb;
        proposal.layers[0].reason = CueReason::kHeadImpact;
        out.push_back(proposal);
        return false;
    }
};

/// Crunch and gore, for every part of the body.
///
/// One rule where there were two - the head's deterministic rule inside
/// `HeadImpactStrategy` and everything else's probability gate here. Both shapes
/// survive as tunings, so what the merge removed is the second gate, the second
/// level ramp and the shared budget.
///
/// Discrete: you cannot have thirty percent of a bone break, and one played
/// quietly sounds like a bug. A tier that should not be certain is softened with
/// probability, never volume.
///
/// Everything it proposes is a ride-along, so damage dies with the onset under it.
class DamageStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "Damage"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const DamageConfig& dmg = ctx.cfg.strategies.damage;
        if (!dmg.enabled) {
            return false;
        }
        const DamageSite site = DamageSiteFor(contact.site);
        const DamagePartConfig& part = Part(dmg, site);
        if (!part.enabled) {
            return false;
        }

        // Every gate is a fraction of the loud anchor, so the tier structure moves
        // with the range rather than being re-derived whenever the anchor does.
        const float anchor = std::max(1.0f, ctx.cfg.intensity.speedRefHigh);

        // Optionally a glancing landing has to arrive proportionally faster to
        // break something - see GlancingImpactConfig::scaleCrunchGate.
        const float own = ctx.cfg.glancing.scaleCrunchGate
                              ? contact.impactSpeed * contact.glanceScale
                              : contact.impactSpeed;

        // Its own arrival, how hard the whole body is being dealt with, or a blend
        // - see DamagePartConfig::bodyForceShare. The envelope has already taken
        // the max with this frame's contacts, so the blend can only raise.
        const float speed =
            Lerp(own, std::max(own, ctx.actor.energyRecent),
                 std::clamp(part.bodyForceShare, 0.0f, 1.0f));

        // Past the obliterate point the limits loosen for this contact: budget
        // gains slots, spacing shrinks. It used to be a second gate *under* the
        // gore, which made the most extreme contacts the hardest to hear.
        const bool obliterate = speed >= ctx.cfg.intensity.obliterateFrac * anchor;

        // A head that led the body in is held to its own crunch gate: Vayne
        // log_2's 294 u/s faceplant crunches where its 402 u/s spine whip, faster
        // by a third, does not.
        float crunchGate = part.crunch.atFrac * anchor;
        float crunchProbability = -1.0f;  // <0 means "use the tier's own ramp"
        const AirTimeConfig& air = ctx.cfg.strategies.airTime;
        if (site == DamageSite::kHead && air.headEnabled && air.headCrunchGateFrac > 0.0f) {
            const float headGate = air.headCrunchGateFrac * anchor;
            if (headGate < crunchGate && ClassifyHead(ctx, contact).airFull) {
                crunchGate = headGate;
                crunchProbability = std::clamp(air.headCrunchProbability, 0.0f, 1.0f);
            }
        }

        // The two tiers are independent: gore is not nested inside crunch, so a
        // contact bad enough to be wet still sounds wet on a frame where the
        // crunch budget is gone.

        // What kind of fall this contact is happening inside, 0 to 1 - a different
        // question from how hard it hit. See DamageViolenceConfig for why it is a
        // window over the recent past and never this tick's reading.
        const float violence = Violence(ctx, contact);

        Fire(ctx, contact, out, site, kCrunchTier, part.crunch, anchor, speed, obliterate,
             crunchGate, crunchProbability, CrunchSlot(site), CueReason::kCrunch, violence);
        Fire(ctx, contact, out, site, kGoreTier, part.gore, anchor, speed, obliterate,
             part.gore.atFrac * anchor, -1.0f, SlotId::kGoreWet, CueReason::kGore, violence);
        return false;
    }

private:
    static constexpr std::size_t kCrunchTier = 0;
    static constexpr std::size_t kGoreTier = 1;

    /// How violent the fall this contact arrived in has been, 0 to 1. A blend of
    /// the body's window and the contact limb's, which come apart: a body sliding
    /// to a stop with one leg still cartwheeling is quiet on the first and loud on
    /// the second. Both halves rise only on contact-free ticks, so this cannot
    /// contain the collision it is about to judge.
    [[nodiscard]] static float Violence(const StrategyContext& ctx, const Contact& contact) {
        const DamageViolenceConfig& viol = ctx.cfg.strategies.ragdollDamage.violence;
        if (!viol.enabled) {
            return 0.0f;
        }
        const ActorRuntime& actor = ctx.actor;
        // No pose, no measurement, and the fallback is *off* rather than a guess.
        if (!actor.state.haveBodySamples) {
            return 0.0f;
        }
        float limb = 0.0f;
        if (contact.limbIndex < actor.limbCount && contact.limbIndex < kMaxLimbs) {
            limb = actor.limbs[contact.limbIndex].violence;
        }
        return std::clamp(
            Lerp(actor.state.motionViolence, limb, std::clamp(viol.limbShare, 0.0f, 1.0f)), 0.0f,
            1.0f);
    }

    [[nodiscard]] static const DamagePartConfig& Part(const DamageConfig& dmg, DamageSite site) {
        switch (site) {
            case DamageSite::kHead:  return dmg.head;
            case DamageSite::kSpine: return dmg.spine;
            default:                 return dmg.limb;
        }
    }

    /// One tier, start to finish: does it open, may it, does it, and how loud.
    /// `gate` is passed separately from `tier.atFrac` because the air-time rule can
    /// lower it for a led head. The *ramps* stay measured from the tier's own
    /// threshold either way, so a dive that only crunches because of that rule
    /// arrives at the quiet end.
    static void Fire(const StrategyContext& ctx, const Contact& contact, ProposalList& out,
                     DamageSite site, std::size_t tierIndex, const DamageTierConfig& tier,
                     float anchor, float speed, bool obliterate, float gate,
                     float flatProbability, SlotId slot, CueReason reason, float violence) {
        if (!tier.enabled) {
            return;
        }
        const DamageViolenceConfig& viol = ctx.cfg.strategies.ragdollDamage.violence;

        // A violent fall lowers the bar. It moves the **gate** and never the ramp,
        // as the air-time head rule does, so lowering the bar is not also a way of
        // making what comes through it loud. A fraction of the tier's own span, so
        // one number suits all six and can never invert the gate.
        if (violence > 0.0f && viol.gateDropFrac > 0.0f) {
            const float span = std::max(0.0f, tier.capFrac - tier.atFrac) * anchor;
            gate = std::max(0.0f,
                            gate - violence * std::clamp(viol.gateDropFrac, 0.0f, 1.0f) * span);
        }
        if (speed < gate) {
            return;
        }
        const DamageConfig& dmg = ctx.cfg.strategies.damage;
        // Ragdoll-only, and read through the actor's column anyway: a shared row
        // holds the same value in all three (see MirrorSharedRows), so this says
        // where the parameter lives rather than which column to look in.
        const RagdollDamageConfig& rag = ctx.cfg.strategies.ragdollDamage;
        auto& ledger = ctx.actor.damage[static_cast<std::size_t>(site)][tierIndex];

        // Budget and spacing, relaxed past the obliterate point rather than waived:
        // a raised budget is still a budget. Violence loosens the same two limits,
        // and is the only lever that moves occurrence audibly - these tiers are
        // budget-limited rather than threshold-limited by about eight to one, so
        // the gate drop above admits candidates into a ledger already spent. See
        // DamageViolenceConfig.
        //
        // Rounded, not truncated: violence peaks near 0.5 on real falls, so a
        // truncating bonus of 2 would grant nothing.
        const int violenceBudget =
            violence > 0.0f
                ? static_cast<int>(std::lround(violence * static_cast<float>(viol.budgetBonus)))
                : 0;
        const int budget =
            tier.budget + (obliterate ? rag.obliterateBudgetBonus : 0) + violenceBudget;
        if (ledger.count >= budget) {
            return;
        }
        float spacingMs =
            tier.spacingMs * (obliterate ? std::max(0.0f, rag.obliterateSpacingScale) : 1.0f);
        if (violence > 0.0f) {
            spacingMs *= Lerp(1.0f, std::max(0.0f, viol.spacingScale), violence);
        }
        if (spacingMs > 0.0f && ctx.nowMs - ledger.lastMs < spacingMs) {
            return;
        }

        // A cap at or under the threshold is how you ask for a step instead of a
        // ramp, and collapses to "full from the threshold up".
        const float at = tier.atFrac * anchor;
        const float span = tier.capFrac * anchor - at;
        const float ramp =
            span > 1.0f ? std::clamp((speed - at) / span, 0.0f, 1.0f) : 1.0f;

        // A tier left at 1/1 draws no random number at all, so switching one
        // part's damage on does not re-roll every variant and scatter after it.
        float probability =
            flatProbability >= 0.0f
                ? flatProbability
                : Lerp(std::clamp(tier.probAtGate, 0.0f, 1.0f),
                       std::clamp(tier.probAtCap, 0.0f, 1.0f), ramp);
        // The conservative half of occurrence: the gate drop changes *which*
        // contacts are eligible, this changes how often eligible ones fire.
        // Applied after the flat override too - a led head landing inside a violent
        // tumble is no less likely to break than one landing inside a calm one.
        if (violence > 0.0f && viol.chanceBonus > 0.0f) {
            probability = std::clamp(probability + violence * viol.chanceBonus, 0.0f, 1.0f);
        }
        if (probability < 1.0f && (probability <= 0.0f || ctx.rng.Unit() > probability)) {
            return;
        }

        // Charged at proposal rather than emit: spacing is about how often the
        // engine is willing to break bone, and a tier charging only for survivors
        // would reopen its window on every drop.
        ++ledger.count;
        ledger.lastMs = ctx.nowMs;

        // A tier is discrete, so violence never softens one into existence; it only
        // pushes a break that was already happening further forward.
        const float levelDb = Lerp(tier.quietDb, tier.loudDb, ramp) +
                              (violence > 0.0f ? violence * viol.levelBonusDb : 0.0f);

        out.push_back(RideAlongLayer(ctx, contact, slot, contact.onsetGainDb + levelDb, reason,
                                     ctx.cfg.strategies.impact.bodyOffsetMs + tier.delayMs));
        spdlog::debug("{} {} on seq {} at {:.0f} u/s: {:.1f} dB, {}/{} spent{}{}", ToString(site),
                      ToString(reason), contact.sourceSeq, speed, levelDb, ledger.count, budget,
                      obliterate ? " (obliterate)" : "",
                      violence > 0.0f ? std::format(" violence {:.2f}", violence) : "");
    }
};

/// The voicing of `Motion::kSlide`: grinding loops with catches riding on them.
/// It decides nothing about *when* a slide happens - that is the motion axis'.
///
/// Two kinds of loop, running together:
///
///  - **The body grind**, levelled on the body's measured speed *and* how much of
///    the body is on the surface. The second half is the fix: on speed alone a
///    corpse dragged by one ankle read as loud as the same corpse lying flat.
///  - **The limb grinds**, one per chain on the bone doing the most rubbing.
///    Light, dry, well under the body grind, and what carries the bottom of the
///    range - which is why the body grind may be silent there.
///
/// Blended rather than switched, on one continuous measurement: picking one would
/// put a boundary in the middle of a slide, and a boundary is what a listener
/// hears.
class ScrapeLoopStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ScrapeLoop"; }

    /// Unnamed `out` because this path proposes nothing: it decides whether the
    /// grind *keeps* a graze, and the layer that used to spend one here is now a
    /// tick-path event on the loop that opens. See `EmitEntryCatch`.
    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList&) override {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        if (!scrape.enabled || !contact.graze || !Allowed(ctx)) {
            return false;
        }

        // Claiming a graze keeps it off the impact path, which is right while a
        // slide is running and was being done all the time. Roughly half the
        // worthwhile contacts of an ordinary tumble classify as grazes, so half
        // were deleted from the mix with nothing put in their place - a single
        // glancing knock does not open a slide.
        //
        // Unconditional now rather than a switch: there is no reading in which a
        // graze with no grind under it should be silent. See
        // `ScrapeLoopConfig::slidesDontClaim`.
        if (ctx.actor.state.motion != Motion::kSlide) {
            return false;
        }

        // ...and inside a slide, a rub hard enough to also be a hit is not ours
        // either. The test above is a phase judging a contact (01 §3.1); this is
        // the contact judged in front of us. No grain for one that falls through:
        // the impact path is voicing it, and a grain on top would be a second
        // onset spending the same burst budget for one hit.
        if (scrape.slidesDontClaim && contact.intensity >= scrape.claimBelowIntensity) {
            return false;
        }

        // Claimed and spent on the grind, with nothing of its own. The catch layer
        // used to fire here on any graze harder than the slide's recent average;
        // sixty-five grit peaks a second is texture and belongs in the file, and
        // the same idea at cue rate is a rattle of little impacts over a grind.
        // What is left is the *entry*, a tick-path event - see `EmitEntryCatch`.
        return true;
    }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        // Full detail or not at all: nobody resolves which limb a grind is on at
        // fifteen metres, and the mod already strips loops past `fFullRadius`.
        const bool alive = scrape.enabled && Allowed(ctx) &&
                           actor.state.motion == Motion::kSlide && actor.haveBodyPoint &&
                           actor.state.tier == DistanceTier::kFull;

        // How much of the body is on the surface, and so how much body grind there
        // is. Off, the ramp is 1 and the level is speed alone.
        const float fracSpan = std::max(0.001f, scrape.bodyFracFull - scrape.bodyFracStart);
        const float weight =
            !scrape.fractionEnabled
                ? 1.0f
                : std::clamp((actor.contactFraction - scrape.bodyFracStart) / fracSpan, 0.0f, 1.0f);

        // The order is the mix. The grinds decide whether there is a slide to put
        // a bed under, so the bed is asked last and told what they did rather than
        // re-deriving their two `wants` tests.
        const bool bodyWants = BodyLoop(ctx, out, alive, weight);
        const int limbsWanted = LimbLoops(ctx, out, alive, weight);
        RumbleBed(ctx, out, alive, weight, bodyWants, limbsWanted);
    }

private:
    /// Whether a slide may be voiced on this actor at all. Asked *before* the loop
    /// is judged alive: `ProposeTick` turning it false is how a running grind gets
    /// its stop cue, so switching `bAnimatedSlide` off mid-slide ends the loop
    /// instead of stranding it.
    [[nodiscard]] static bool Allowed(const StrategyContext& ctx) {
        return !Animated(ctx.cfg, ctx.actor) || ctx.cfg.game.animatedSlide;
    }

    /// The speed the loops are levelled on: the body's, with a little of the
    /// contact's spikiness blended in. Body speed is smooth and a level following
    /// it alone reads as a constant; contact tangent speed is spiky as limbs load
    /// and unload. A wobble *around* the body speed, never a replacement.
    [[nodiscard]] static float LevelSpeed(const StrategyContext& ctx) {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        const float body = SlideSpeed(ctx.actor);
        const float blend = std::clamp(scrape.contactSpeedBlend, 0.0f, 1.0f);
        return blend <= 0.0f ? body : Lerp(body, ctx.actor.slideTangent, blend);
    }

    [[nodiscard]] static float Track(float speed, float lo, float hi) {
        return std::clamp((speed - lo) / std::max(1.0f, hi - lo), 0.0f, 1.0f);
    }

    /// Which file, given what is under the body. A surface with nothing recorded
    /// resolves back to the default grind inside the bank, so this can name a slot
    /// that does not exist yet without going silent.
    [[nodiscard]] static SlotId SurfaceOf(const ScrapeLoopConfig& scrape, SlotId base,
                                          SurfaceClass surface) {
        return scrape.surfaceVariants ? ScrapeSurfaceSlot(base, surface) : base;
    }

    /// Where the body grind hangs, and therefore where the bed does. Factored out
    /// because the two must agree: a bed anchored somewhere other than the grind it
    /// is under would not read as a placement error, it would just sound wrong.
    [[nodiscard]] static LoopAnchor BodyGrindAnchor(const StrategyContext& ctx) {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        const ActorRuntime& actor = ctx.actor;
        LoopAnchor anchor = BodyAnchor(actor);
        if (scrape.bodyFollowsContact && actor.haveBodyAnchor) {
            anchor.limbIndex = actor.bodyAnchor;
            if (actor.bodyAnchor < actor.limbCount && actor.limbs[actor.bodyAnchor].havePos) {
                anchor.position = actor.limbs[actor.bodyAnchor].pos;
            } else if (actor.haveGrazeCentre) {
                anchor.position = actor.grazeCentre;
            }
        }
        return anchor;
    }

    /// Returns whether the body grind is running, for the bed to read.
    bool BodyLoop(const StrategyContext& ctx, ProposalList& out, bool alive, float weight) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        const float speed = LevelSpeed(ctx);
        const float track = Track(speed, scrape.speedForMinGain, scrape.speedForMaxGain);
        const float gainDb = scrape.gainDb + Lerp(scrape.speedRangeDb, 0.0f, track) +
                             Lerp(scrape.bodyFracRangeDb, 0.0f, weight);
        const float pitch = 1.0f + scrape.pitchPerThousandUnits * speed / 1000.0f;

        // Stopped at the bottom of its own ramp on both axes, rather than at a
        // level. Below `fBodyFracStart` the limb loops are the whole of the slide,
        // and a body grind running at the bottom of its ramp is a voice held open
        // to play nothing.
        //
        // The fraction axis has always done this (`weight > 0`); the speed axis
        // never did, so ending a grind that had run out of speed fell to
        // `Mix:fVoiceFloorDb` - which worked only by coincidence, both ramps
        // bottoming out near -47 dB against a -48 floor. Move `fGainDb` up four
        // and the grind runs on at the bottom of its ramp emitting nothing (the
        // deadband holds every update back) until the slide state ends.
        // `fSpeedForMinGain` is the honest test: it is already where the ramp
        // bottoms out, in units per second a listener can check against what they
        // see. The limb loops have had `fLimbMinTangentSpeed` all along.
        //
        // The floor test stays as a backstop for the other failure: a loop
        // *started* under the floor is marked running while Stage 5 drops the cue,
        // so every update after it addresses a voice the renderer never opened.
        const bool wants = alive && scrape.bodyEnabled && weight > 0.0f &&
                           speed > scrape.speedForMinGain &&
                           gainDb >= ctx.cfg.mix.voiceFloorDb;

        if (!wants) {
            if (actor.scrapeRunning) {
                StopLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice,
                                 actor.scrapeSlot, CueReason::kScrape, StopFadeMs(ctx));
            }
            // The armour rides the grind and cannot outlive it. This exit used to
            // return early and leave the skin running with nothing to update or
            // stop it, stranded until the actor was released.
            if (actor.armorSlideRunning) {
                StopLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                                 actor.armorSlideSlot, CueReason::kArmorSkin, StopFadeMs(ctx));
            }
            return false;
        }

        const bool entering = !actor.scrapeRunning;
        if (entering) {
            actor.scrapeSlot = SurfaceOf(scrape, SlotId::kScrapeLoop, actor.slideSurface);
        }
        // Decided before the cue rather than patched on afterwards, which is what
        // let the level deadband swallow a hop. Nearest-to-contact rather than the
        // root: unlike "lowest" it survives a staircase, a wall and a ceiling.
        const LoopAnchor anchor = BodyGrindAnchor(ctx);
        EmitLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice, actor.scrapeLastDb,
                         actor.scrapeAnchor, actor.scrapeSlot, gainDb, pitch, CueReason::kScrape,
                         scrape.startFadeMs, anchor, actor.slideCoverage,
                         scrape.levelDeadbandDb);

        // The body's entry scuff, off by default: a torso arriving flat is a fall
        // the composite already voiced, so this is for a drag that starts out of
        // silence. See `bGrainOnBody`.
        if (entering && scrape.grainOnBody) {
            EmitEntryCatch(ctx, out, gainDb, anchor, actor.slideSeq, actor.slideSite,
                           actor.slideSurface);
        }

        // The armour riding it: same anchor, same fades, its own voice. Flat rather
        // than ramped - a slide has a duration, not an intensity, and the grind's
        // level already tracks how hard the body is pressing.
        const ArmorConfig& armor = ctx.cfg.armor;
        if (!armor.enabled || !armor.onSlide ||
            !CanSound(ctx, ArmorSlot(actor.slideCoverage))) {
            if (actor.armorSlideRunning) {
                StopLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                                 actor.armorSlideSlot, CueReason::kArmorSkin, StopFadeMs(ctx));
            }
            return true;
        }
        if (!actor.armorSlideRunning) {
            actor.armorSlideSlot = ArmorSlot(actor.slideCoverage);
        }
        const float armorDb = gainDb + armor.slideGainDb;
        if (armorDb < ctx.cfg.mix.voiceFloorDb) {
            if (actor.armorSlideRunning) {
                StopLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                                 actor.armorSlideSlot, CueReason::kArmorSkin, StopFadeMs(ctx));
            }
            return true;
        }
        EmitLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                         actor.armorSlideLastDb, actor.armorSlideAnchor, actor.armorSlideSlot,
                         armorDb, pitch, CueReason::kArmorSkin, scrape.startFadeMs, anchor,
                         actor.slideCoverage, scrape.levelDeadbandDb);
        return true;
    }

    /// Returns how many limb grinds are running, for the bed to read.
    int LimbLoops(const StrategyContext& ctx, ProposalList& out, bool alive, float weight) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        const auto hold = static_cast<float>(std::max(0.0f, scrape.contactHoldMs));
        const int budget = std::clamp(scrape.maxLimbLoops, 0, static_cast<int>(kScrapeChainCount));

        struct Candidate {
            std::size_t index{};
            float score{};
        };

        // The budget is handed out on the entry test and taken away on a cheaper
        // hold test - hysteresis, like the slide's own entry and exit speeds. A
        // chain with a loop keeps it while it clears `fLimbHoldTangentSpeed`; only
        // what is left is offered to newcomers, ranked by how hard they rub.
        // Without the split, two legs whose tangents cross swap one voice back and
        // forth every other tick.
        bool wanted[kScrapeChainCount]{};
        Candidate entrants[kScrapeChainCount]{};
        std::size_t entrantCount = 0;
        int held = 0;
        for (std::size_t i = 0; i < kScrapeChainCount; ++i) {
            const ActorRuntime::LimbLoop& loop = actor.limbLoops[i];
            const bool grazing = !Ancient(loop.lastGrazeMs) &&
                                 static_cast<float>(ctx.nowMs - loop.lastGrazeMs) <= hold;
            if (!alive || !scrape.limbEnabled || !grazing) {
                continue;
            }
            if (loop.running) {
                if (loop.tangent >= scrape.limbHoldTangentSpeed && held < budget) {
                    wanted[i] = true;
                    ++held;
                }
            } else if (loop.tangent >= scrape.limbMinTangentSpeed) {
                entrants[entrantCount++] = {i, loop.tangent};
            }
        }
        std::sort(entrants, entrants + entrantCount,
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        for (std::size_t i = 0; i < entrantCount && held < budget; ++i) {
            wanted[entrants[i].index] = true;
            ++held;
        }

        // The duck, scaled by the body grind's weight so it arrives with the body
        // rather than switching on. Deep enough is suppression rather than damping.
        const float duckDb =
            scrape.bodyDucksLimbs && scrape.bodyEnabled ? scrape.limbDuckDb * weight : 0.0f;

        for (std::size_t i = 0; i < kScrapeChainCount; ++i) {
            ActorRuntime::LimbLoop& loop = actor.limbLoops[i];
            if (!wanted[i]) {
                if (loop.running) {
                    StopLoopProposal(ctx, out, loop.running, loop.voice, loop.slot,
                                     CueReason::kScrape, StopFadeMs(ctx));
                }
                continue;
            }

            const bool head = i == 0 && scrape.headTint;
            const float track =
                Track(loop.tangent, scrape.limbSpeedForMinGain, scrape.limbSpeedForMaxGain);
            const float gainDb = scrape.limbGainDb + Lerp(scrape.limbSpeedRangeDb, 0.0f, track) +
                                 duckDb + (head ? scrape.headGainDb : 0.0f);

            // Ducked under the floor is not a quiet loop, it is a voice held open
            // to play nothing - and it is what asking the duck to *suppress* means.
            // Stopped, so the loop is never left marked running with no
            // `kStartLoop` behind it. See the same test on the body grind.
            if (gainDb < ctx.cfg.mix.voiceFloorDb) {
                if (loop.running) {
                    StopLoopProposal(ctx, out, loop.running, loop.voice, loop.slot,
                                     CueReason::kScrape, StopFadeMs(ctx));
                }
                continue;
            }

            const float pitch = (1.0f + scrape.limbPitchPerThousandUnits * loop.tangent / 1000.0f) *
                                (head ? std::max(0.05f, scrape.headPitchScale) : 1.0f);

            if (!loop.running) {
                loop.slot = SurfaceOf(scrape, SlotId::kScrapeLimb, loop.surface);
            }
            LoopAnchor anchor = BodyAnchor(actor);
            if (scrape.limbFollowsLimb && loop.haveAnchor) {
                anchor.limbIndex = loop.anchor;
                if (loop.havePoint) {
                    anchor.position = loop.point;
                }
            }
            const bool entering = !loop.running;
            const std::size_t before = out.size();
            EmitLoopProposal(ctx, out, loop.running, loop.voice, loop.lastDb, loop.sentAnchor,
                             loop.slot, gainDb, pitch, CueReason::kScrape, scrape.startFadeMs,
                             anchor, actor.slideCoverage, scrape.levelDeadbandDb);
            if (out.size() == before) {
                continue;
            }
            {
                // Identity rather than placement, so it stays here: the slot is
                // held for the loop's life and the renderer ignores a variant on
                // an update, which stops a file swapping under a running voice.
                //
                // Scoped, because the entry scuff below pushes onto `out` and a
                // reference into a vector does not survive that.
                Proposal& proposal = out.back();
                proposal.site = loop.site;
                proposal.surface = loop.surface;
            }

            // The scuff of this limb arriving: a foot catching, a hand slapping
            // down and dragging. On here and off for the body, because this is a
            // real edge and the torso's equivalent is a fall already voiced.
            if (entering) {
                EmitEntryCatch(ctx, out, gainDb, anchor, loop.lastGrazeSeq, loop.site,
                               loop.surface);
            }
        }
        return held;
    }

    /// The mass under the grinds: one bed voice per actor, at the body grind's
    /// anchor, for as long as anything is grinding. Levelled on the same speed the
    /// grinds are so the three move together. Its pitch does not track anything,
    /// which is the point of a separate voice: floor resonance does not move with
    /// how fast the body is going, and the references hold a static spectrum while
    /// the level swells.
    void RumbleBed(const StrategyContext& ctx, ProposalList& out, bool alive, float weight,
                   bool bodyWants, int limbsWanted) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        // **The bed is levelled exactly the way a limb grind is**, which is how
        // this layer got its movement.
        //
        // It used to read `LevelSpeed` through a ramp 30 dB deep with a v^2 curve.
        // Body speed is smooth by definition (the complaint `fContactSpeedBlend`
        // answers for the grinds), squaring a 0..1 track pulls the bottom of the
        // range down, and 30 dB over a curve leaves a layer that is inaudible or
        // fully on with nothing between. It read as a switch because it was one.
        //
        // `actor.slideTangent` is the actor-scope version of `loop.tangent`, a
        // decaying peak-hold over how fast the body is actually rubbing - spiky,
        // because limbs load and unload as a body tumbles - and unlike a scan over
        // the running loops it exists even when a torso is skidding on its spine
        // with no chain holding a grind.
        //
        // The ramp ends and depth are the limb grinds' own rather than a parallel
        // pair of keys, so `fRumbleSpeedCurve` and `fRumbleSpeedRangeDb` are gone;
        // `fRumbleGainDb` and `fRumbleLimbGainDb` remain the bed's.
        const float speed = actor.slideTangent;
        const float track =
            Track(speed, scrape.limbSpeedForMinGain, scrape.limbSpeedForMaxGain);

        // The limb/body difference is a trim, interpolated on the same fraction the
        // body grind's weight uses, and applied whole when no body grind runs at
        // all whatever the fraction says. `weight` is the *body's* contact
        // fraction, so a body lying flat and skidding with `bBodyEnabled` off
        // measured ~1, cancelled the trim, and played the bed at full body level
        // under nothing but limb grinds - mass with none of the grit.
        const float bodyWeight = bodyWants ? weight : 0.0f;
        const float gainDb = scrape.rumbleGainDb +
                             Lerp(scrape.limbSpeedRangeDb, 0.0f, track) +
                             Lerp(scrape.rumbleLimbGainDb, 0.0f, bodyWeight);
        const float pitch =
            scrape.rumblePitch + scrape.rumblePitchPerThousandUnits * speed / 1000.0f;

        // A bed under nothing is a hum in the room, so it needs a grind over it:
        // the body's, or a limb's if `bRumbleOnLimbs` is set. `CanSound` is why it
        // costs nothing until the file exists - the slot has no fallback, since
        // one to `scrape_loop` would play the grind twice.
        const bool wants = alive && scrape.rumbleEnabled &&
                           (bodyWants || (scrape.rumbleOnLimbs && limbsWanted > 0)) &&
                           speed > scrape.limbSpeedForMinGain &&
                           gainDb >= ctx.cfg.mix.voiceFloorDb &&
                           CanSound(ctx, SlotId::kScrapeLoopRumble);

        if (!wants) {
            if (actor.scrapeBedRunning) {
                StopLoopProposal(ctx, out, actor.scrapeBedRunning, actor.scrapeBedVoice,
                                 SlotId::kScrapeLoopRumble, CueReason::kScrape, StopFadeMs(ctx));
            }
            return;
        }

        EmitLoopProposal(ctx, out, actor.scrapeBedRunning, actor.scrapeBedVoice,
                         actor.scrapeBedLastDb, actor.scrapeBedAnchor,
                         SlotId::kScrapeLoopRumble, gainDb, pitch, CueReason::kScrape,
                         scrape.startFadeMs, BodyGrindAnchor(ctx), actor.slideCoverage,
                         scrape.levelDeadbandDb);
    }

    /// Which fade, from how the slide ended. A slide that ends in friction ends
    /// slowly; one that ends because the body launched ends the instant the
    /// surface does, and the ordinary fade drags a grinding rumble out behind a
    /// body that is already in the air.
    [[nodiscard]] static float StopFadeMs(const StrategyContext& ctx) {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        return ctx.actor.state.slideExit == SlideExit::kLaunched ? scrape.launchFadeMs
                                                                 : scrape.stopFadeMs;
    }

    /// The scuff of a grind arriving: one `scrape_grain` on the tick a loop opens,
    /// under the head of the loop it introduces.
    ///
    /// **What the catch layer became.** It used to fire through a whole slide on
    /// any graze harder than the slide's recent average. The references do put
    /// sixty-five grit peaks a second on the rumble, but that is texture, and
    /// texture at cue rate is a rattle of little impacts over a grind. That density
    /// belongs to the file; the layer keeps the one moment that is genuinely an
    /// event.
    ///
    /// A slide is not declared until 150 ms or 45 units into a grind that is
    /// already happening, so the loop opens into a body that has been scraping for
    /// a moment with nothing marking the arrival.
    ///
    /// Not an inference: a loop only opens on a chain that has really been grazing
    /// inside `fContactHoldMs`, and `seq` is that collision's, so it groups with
    /// the moment it belongs to.
    void EmitEntryCatch(const StrategyContext& ctx, ProposalList& out, float loopGainDb,
                        const LoopAnchor& anchor, std::uint32_t seq, LimbSite site,
                        SurfaceClass surface) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        if (!scrape.grainEnabled || !CanSound(ctx, SlotId::kScrapeGrain)) {
            return;
        }
        ctx.actor.lastGrainMs = ctx.nowMs;

        // Against the loop's level rather than a contact's onset: an entry is the
        // front of the sound it opens, so it scales with the grind - a limb barely
        // dragging scuffs quietly, a body arriving at speed scuffs hard.
        //
        // An onset of its own rather than a ride-along: a loop is not an onset, so
        // there is no parent to be an accessory to and a parentless ride-along is
        // dropped. Paid for out of `Slide`'s own grain budget.
        Proposal proposal{};
        proposal.timeMs = ctx.nowMs;
        proposal.actorId = ctx.actor.state.actorId;
        proposal.sourceSeq = seq;
        proposal.limbIndex = anchor.limbIndex;
        proposal.boneIndex = BoneFor(ctx, anchor.limbIndex);
        proposal.position = anchor.position;
        proposal.site = site;
        proposal.surface = surface;
        proposal.coverage = ctx.actor.slideCoverage;
        proposal.levelDb = loopGainDb + scrape.grainGainDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = SlotId::kScrapeGrain;
        proposal.layers[0].gainDb = proposal.levelDb;
        proposal.layers[0].reason = CueReason::kScrape;
        proposal.layers[0].pitch =
            1.0f + scrape.grainPitchScatter * (ctx.rng.Unit() * 2.0f - 1.0f);
        out.push_back(proposal);
    }
};

/// Damage from being worked on, rather than from one bad landing.
///
/// A separate strategy and not a fifth tier: the tiers judge *this contact*, this
/// judges *this limb's recent history*. The measured case is a head bashed against
/// a wall twenty-four times whose hardest contact is 371 u/s against a 432 gate -
/// every tier is right to refuse them, and the skull should still come apart.
///
/// It keeps its own pool per limb, its own ladder and its own budget, and borrows
/// only which slot a site's crunch plays.
class AccumDamageStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "AccumDamage"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const AccumDamageConfig& acc = ctx.cfg.strategies.accum;
        if (!acc.enabled || contact.limbIndex >= kMaxLimbs) {
            return false;
        }
        ActorRuntime& actor = ctx.actor;

        // Both scope switches read *here*, before the pool is touched. A pool that
        // fills on a contact the ladder can never fire on is damage banked against
        // the moment the switch stops applying - a body dropped out of the player's
        // hands would carry a full pool into the fall that follows.
        //
        // Healing still works across the gap: it is a function of elapsed time and
        // the stamp is only written on a contact that got past here.
        if (acc.headOnly && DamageSiteFor(contact.site) != DamageSite::kHead) {
            return false;
        }
        if (acc.requireHeld && !actor.heldByPlayer) {
            return false;
        }

        ActorRuntime::AccumTrack& track = actor.accum[contact.limbIndex];

        // Heal first, against this limb's own clock: a shared stamp would heal an
        // arm on the tick a leg was struck.
        if (!Ancient(track.lastMs)) {
            const auto dt = static_cast<float>(ctx.nowMs - track.lastMs) * 0.001f;
            if (dt > 0.0f) {
                track.pool *= std::exp(-dt / std::max(0.001f, acc.healMs * 0.001f));
            }
        }
        track.lastMs = ctx.nowMs;

        // What this contact was worth, on intensity rather than closing speed:
        // intensity already accounts for mass, radius and coupling. The floor keeps
        // a settling scrabble from eventually breaking a bone on its own.
        const float over = contact.intensity - acc.ignoreBelowIntensity;
        if (over > 0.0f) {
            track.pool = std::min(acc.maxPool, track.pool + over * acc.perHitScale);
        }
        actor.stats.AccumPeak(track.pool);
        if (ctx.stats != nullptr) {
            ctx.stats->AccumPeak(track.pool);
        }

        // Down the ladder as it heals, so a limb left alone and then attacked again
        // climbs a second time - but only once it has fallen *well* below the rung.
        // Without the margin a pool near a threshold steps down and back up on
        // alternate contacts and fires the same rung on a loop: 51 breaks on one
        // take before this was here.
        while (track.stage > 0 &&
               track.pool < StageAt(acc, track.stage - 1) * std::clamp(acc.rearmFrac, 0.0f, 1.0f)) {
            --track.stage;
        }

        // ...and up it, one rung per contact even when a single hit clears two:
        // each rung is a separate break, and two on one frame is a stack.
        const AccumDamageStageConfig* rung = Rung(acc, track.stage);
        if (rung == nullptr || rung->atDamage <= 0.0f || track.pool < rung->atDamage) {
            return false;
        }

        // **Reaching a rung arms the limb; it does not break it.** The pool waits
        // at the rung, taking more damage and doing nothing, until a blow arrives
        // with enough in it to finish the job. Without this the break lands on
        // whichever contact tipped the arithmetic over - very often the
        // twenty-fourth gentle scuff of a beating.
        //
        // Armed is a *property of the pool* rather than a flag, so it heals for
        // free: a limb left alone drops back under the rung and is no longer ready.
        if (contact.intensity < acc.breakIntensity) {
            return false;
        }

        if (track.fired >= acc.maxPerLimb || actor.accumFired >= acc.maxPerActor) {
            return false;
        }
        if (acc.minGapMs > 0.0f && !Ancient(track.lastFireMs) &&
            ctx.nowMs - track.lastFireMs < acc.minGapMs) {
            return false;
        }

        const DamageSite site = DamageSiteFor(contact.site);
        const SlotId slot = rung->gore ? SlotId::kGoreWet : CrunchSlot(site);
        if (!CanSound(ctx, slot)) {
            return false;
        }

        ++track.stage;
        ++track.fired;
        ++actor.accumFired;
        track.lastFireMs = ctx.nowMs;
        ++actor.stats.accumBreaks;
        if (ctx.stats != nullptr) {
            ++ctx.stats->accumBreaks;
        }

        out.push_back(RideAlongLayer(ctx, contact, slot, contact.onsetGainDb + rung->levelDb,
                                     rung->gore ? CueReason::kGore : CueReason::kCrunch,
                                     ctx.cfg.strategies.impact.bodyOffsetMs));
        spdlog::debug("accum {} rung {} on seq {}: pool {:.2f} -> {} at {:.1f} dB ({}/{} on limb)",
                      ToString(site), track.stage, contact.sourceSeq, track.pool,
                      rung->gore ? "gore" : "crunch", rung->levelDb, track.fired, acc.maxPerLimb);
        return false;
    }

private:
    [[nodiscard]] static const AccumDamageStageConfig* Rung(const AccumDamageConfig& acc,
                                                            int index) {
        switch (index) {
            case 0: return &acc.stage1;
            case 1: return &acc.stage2;
            case 2: return &acc.stage3;
            case 3: return &acc.stage4;
            default: return nullptr;
        }
    }

    [[nodiscard]] static float StageAt(const AccumDamageConfig& acc, int index) {
        const AccumDamageStageConfig* rung = Rung(acc, index);
        return rung != nullptr ? rung->atDamage : 0.0f;
    }
};

/// The garment: one continuous loop per actor, riding the whole knockdown.
///
/// A bypass like the other two loops: it never claims a contact, cannot suppress
/// one, and no arbitration rule sees it - so it can fill the gaps the arbitrator
/// makes without competing for them.
///
/// *When* it plays is read off the two axes rather than decided here: the level
/// is `CrashState::rustleDrive`, measured in Stage 1, and this strategy only adds
/// the slide duck.
class ClothRustleStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ClothRustle"; }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const RustleConfig& rustle = ctx.cfg.strategies.rustle;
        ActorRuntime& actor = ctx.actor;

        AirborneRise(ctx, out, rustle);

        const float drive = actor.state.rustleDrive;

        // Every reason there might be nothing to play, in one test - and every
        // clause has to be re-asked each tick, because a running loop is stopped
        // by `alive` going false and by nothing else.
        //
        // `haveBodySamples` is not optional: without a pose sidecar every field
        // the drive is built from is zero, and zero is a lie. `CanSound` matters
        // more here than most: a loop proposed for an unrecorded slot still books
        // a voice id and sends a stop cue, and `cloth_rustle` ships empty.
        const bool alive = rustle.enabled &&
                           (!Animated(ctx.cfg, actor) || ctx.cfg.game.animatedRustle) &&
                           actor.state.haveBodySamples && actor.haveBodyPoint &&
                           drive > rustle.silenceDrive &&
                           actor.state.tier == DistanceTier::kFull &&
                           CanSound(ctx, SlotId::kClothRustle);

        float gainDb = 0.0f;
        if (alive) {
            gainDb = rustle.gainDb + Lerp(rustle.driveRangeDb, 0.0f, drive) +
                     CoverageTrimDb(rustle, RustleCoverage(ctx, actor)) + SlideDuckDb(ctx) +
                     WanderDb(ctx, rustle) +
                     (actor.isPlayer ? rustle.playerTrimDb : 0.0f);
        }

        // Stopped at the bottom of its own ramp *and* under the voice floor - the
        // lesson the body grind paid for: a loop started under the floor has its
        // cue dropped by Stage 5 while the strategy has marked it running, and
        // every update after addresses a voice the renderer never opened.
        if (!alive || gainDb < ctx.cfg.mix.voiceFloorDb) {
            if (actor.rustleRunning) {
                StopLoopProposal(ctx, out, actor.rustleRunning, actor.rustleVoice,
                                 SlotId::kClothRustle, CueReason::kRustle, rustle.stopFadeMs);
            }
            return;
        }

        // Pinned at start, like the grind's slot. The class picks the conditional
        // variant, so letting it move mid-fall would swap the file under a running
        // voice.
        if (!actor.rustleRunning) {
            actor.rustleCoverage = RustleCoverage(ctx, actor);
        }

        const float pitch = Lerp(rustle.pitchAtFloor, rustle.pitchAtFull, drive);
        EmitLoopProposal(ctx, out, actor.rustleRunning, actor.rustleVoice, actor.rustleLastDb,
                         actor.rustleAnchor, SlotId::kClothRustle, gainDb, pitch,
                         CueReason::kRustle, rustle.startFadeMs, BodyAnchor(actor),
                         actor.rustleCoverage, rustle.levelDeadbandDb);
    }

private:
    /// The airborne anticipation rise: MotionFoley's last layer, moved here when
    /// that strategy was retired.
    ///
    /// It sits inside the garment rather than beside it because it answers the one
    /// question the garment already asks - what a body that is not touching
    /// anything is doing - and because a whole stage-3 object for a loop and a stop
    /// was most of a strategy's cost for none of its independence. It is still its
    /// own slot, its own voice and its own enable, so nothing about what is heard
    /// has changed.
    ///
    /// Independent of the rustle's own `bEnabled`: they are two layers, and an
    /// install with no `cloth_rustle` recorded still has an `air_whoosh`.
    static void AirborneRise(const StrategyContext& ctx, ProposalList& out,
                             const RustleConfig& rustle) {
        ActorRuntime& actor = ctx.actor;

        // Skipped for your own ragdoll: you are the one moving and the view tells
        // you.
        const bool wantsRise = rustle.airborneRise && actor.haveBodyPoint &&
                               actor.state.airborne &&
                               actor.state.tier == DistanceTier::kFull &&
                               !(actor.isPlayer && ctx.cfg.player.skipAirborneWhoosh);
        if (wantsRise) {
            EmitLoopProposal(ctx, out, actor.riseRunning, actor.riseVoice, actor.riseLastDb,
                             actor.riseAnchor, SlotId::kAirWhoosh, rustle.airborneRiseGainDb, 1.0f,
                             CueReason::kAirborneRise, 150.0f, BodyAnchor(actor),
                             ActorClassCoverage(ctx));
        } else if (actor.riseRunning) {
            StopLoopProposal(ctx, out, actor.riseRunning, actor.riseVoice, SlotId::kAirWhoosh,
                             CueReason::kAirborneRise, 200.0f);
        }
    }

    /// The class the whole actor answers to. The torso's `bodyCoverage`, whatever
    /// `[Armor] iActorClassSource` says about the other actor-level cues: a
    /// garment is what the body is wearing, and the slide's coverage is
    /// meaningless for a body that is not sliding.
    [[nodiscard]] static Coverage RustleCoverage(const StrategyContext& ctx,
                                                 const ActorRuntime& actor) {
        return ctx.cfg.armor.enabled ? actor.bodyCoverage : Coverage::kCloth;
    }

    [[nodiscard]] static float CoverageTrimDb(const RustleConfig& rustle, Coverage coverage) {
        switch (coverage) {
            case Coverage::kBare:  return rustle.bareTrimDb;
            case Coverage::kCloth: return rustle.clothTrimDb;
            case Coverage::kLight: return rustle.lightTrimDb;
            case Coverage::kHeavy: return rustle.heavyTrimDb;
        }
        return rustle.clothTrimDb;
    }

    /// How far the running grind pulls the garment down. Scaled by the grind's own
    /// weight rather than switched by the slide state, so it arrives with the body
    /// loop instead of stepping in when the motion axis changes its mind.
    [[nodiscard]] static float SlideDuckDb(const StrategyContext& ctx) {
        const ActorRuntime& actor = ctx.actor;
        if (actor.state.motion != Motion::kSlide || !actor.scrapeRunning) {
            return 0.0f;
        }
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        const float weight = scrape.fractionEnabled
                                 ? RampAt(actor.contactFraction, scrape.bodyFracStart,
                                          scrape.bodyFracFull)
                                 : 1.0f;
        return ctx.cfg.strategies.rustle.slideDuckDb * weight;
    }

    /// A slow deterministic wobble, so a two-second file does not read as one.
    /// Phased on the actor's own ragdoll start rather than the engine clock, so
    /// two bodies falling side by side do not breathe in unison and the wobble is
    /// a function of the take rather than of when it was replayed. Not a random
    /// walk: anything drawing per tick re-rolls every variant downstream.
    [[nodiscard]] static float WanderDb(const StrategyContext& ctx, const RustleConfig& rustle) {
        if (rustle.wanderDepthDb <= 0.0f || rustle.wanderHz <= 0.0f) {
            return 0.0f;
        }
        const auto since = static_cast<float>(ctx.nowMs - ctx.actor.state.ragdollStartMs) * 0.001f;
        constexpr float kTwoPi = 6.283185307f;
        return rustle.wanderDepthDb * std::sin(kTwoPi * rustle.wanderHz * since);
    }
};

}  // namespace

float EngineStats::ReductionRatio() const {
    // Contacts in against audible moments out. The design's 10:1.
    return bursts == 0 ? 0.0f : static_cast<float>(contactsIn) / static_cast<float>(bursts);
}

// ═════════════════════════════════════════════════════════════════════════════

struct Engine::Impl {
    /// All three columns. Everything an actor's own contacts are judged by is read
    /// through `For(actor)`; everything cross-actor - arbitration, the mix, the
    /// voice pool - is read through `cfg`.
    ConfigSet cfgs{};

    /// The ragdoll column, which is also where every shared value lives (see
    /// MirrorSharedRows). Kept as a plain member rather than a reference into the
    /// set so the hundred and sixty reads that do not care about modes read
    /// exactly as they always did, and so no read can be made stale by a swap of
    /// the set behind it.
    AlgorithmConfig cfg{};
    SoundBank* bank{};
    ICueSink* sink{};
    bool tracing{};
    Rng rng;
    std::uint32_t nextVoiceId{1};
    /// Sequence numbers for the contacts the engine makes up rather than receives.
    /// Its own range, well above anything a feed hands out, so a synthetic contact
    /// cannot collide with a real row in the shuffle, the accepted set or a trace.
    std::uint32_t nextSlideSeq{0x8000'0000u};
    std::vector<TraceRecord> trace;
    EngineStats stats{};

    // Every scratch buffer lives here and is reserved once: Tick runs on the game
    // thread every frame and must not allocate on the steady path.
    std::vector<FeedEvent> drained;
    std::vector<Contact> contacts;
    ProposalList proposals;
    std::vector<std::uint16_t> order;
    std::vector<std::uint32_t> acceptedSeqs;
    std::vector<ActorRuntime> actors;
    /// Every voice this engine believes is in flight, for `LiveVoices()`. Not a
    /// budget - a leak detector.
    std::vector<Voice> liveVoices;

    // Stage 3 in the order it runs. ScrapeLoop first because it is the only one
    // that claims; Damage after the composite because it only rides along. Order
    // no longer decides anything about damage - six budgets, one rule.
    ScrapeLoopStrategy scrape;
    HeadImpactStrategy head;
    ImpactCompositeStrategy composite;
    DamageStrategy damage;
    /// Last, and it does not matter that it is: it claims nothing, reads nothing
    /// another strategy writes, and bypasses arbitration.
    ClothRustleStrategy rustle;
    /// Its own strategy, not a fifth tier: the tiers judge one contact, this a
    /// limb's history. After `damage`, so a contact that breaks a bone on its own
    /// merits has already been judged on them.
    AccumDamageStrategy accum;
    std::array<IStrategy*, 6> strategies{};

    /// The column an actor is judged through. Every stage before arbitration goes
    /// through here; arbitration and the mix deliberately do not, because they sort
    /// one list of proposals from every actor into one voice pool and a rule that
    /// varied per proposal would not be a rule.
    [[nodiscard]] const AlgorithmConfig& For(ActorMode mode) const { return cfgs[mode]; }
    [[nodiscard]] const AlgorithmConfig& For(const ActorRuntime& actor) const {
        return cfgs[actor.mode];
    }

    Impl() {
        drained.reserve(256);
        contacts.reserve(128);
        proposals.reserve(128);
        order.reserve(128);
        acceptedSeqs.reserve(128);
        actors.reserve(8);
        liveVoices.reserve(kVoiceReserve);
        strategies = {&scrape, &head, &composite, &damage, &accum, &rustle};
        rng.Seed(1);
    }

    // ── actors ───────────────────────────────────────────────────────────────

    [[nodiscard]] ActorRuntime* Find(ActorId id) {
        for (auto& actor : actors) {
            if (actor.inUse && actor.state.actorId == id) {
                return &actor;
            }
        }
        return nullptr;
    }

    /// Our own anatomical mass per limb, and the body's total. From the profile,
    /// which resolved every bone name to a site, never from the solver's masses -
    /// those are asymmetric enough that the same movement on a right arm counts
    /// for three times a left one (07 §6).
    ///
    /// A body whose profile never arrived keeps a total of zero, so the contact
    /// fraction reads zero: limb-only, never a guess at loud.
    static void SeedLimbMasses(ActorRuntime& actor, const ActorProfile* profile) {
        if (profile == nullptr || profile->limbs.empty()) {
            return;
        }
        actor.limbCount = std::min(profile->limbs.size(), kMaxLimbs);
        actor.bodyMass = 0.0f;
        actor.fabricTotal = 0.0f;
        actor.bodyLimb = 0;
        bool haveBodyLimb = false;
        for (std::size_t i = 0; i < actor.limbCount; ++i) {
            const float mass = NominalMass(profile->limbs[i].site);
            actor.limbs[i].mass = mass;
            actor.bodyMass += mass;
            // How much garment, which is a different question from how much body -
            // see FabricWeight. From the profile, which carries both the site and
            // what is worn on it; a limb without one keeps 0.
            actor.limbs[i].fabric =
                FabricWeight(profile->limbs[i].site, profile->limbs[i].coverage);
            actor.fabricTotal += actor.limbs[i].fabric;
            // The first torso body - COM, pelvis or a spine - and the first only,
            // since a humanoid has four and they are all equally "the body".
            // Nothing indexes: `site` came off the bone name.
            if (!haveBodyLimb && profile->limbs[i].site == LimbSite::kTorso) {
                actor.bodyLimb = static_cast<std::uint16_t>(i);
                actor.bodyCoverage = profile->limbs[i].coverage;
                haveBodyLimb = true;
            }
        }
    }

    ActorRuntime& Acquire(ActorId id, const ActorProfile* profile) {
        if (ActorRuntime* existing = Find(id); existing != nullptr) {
            // A profile that arrived after the actor, or a ragdoll rebuilt onto a
            // different skeleton. Guarded so the ordinary path is one compare.
            if (existing->bodyMass <= 0.0f) {
                SeedLimbMasses(*existing, profile);
            }
            return *existing;
        }
        ActorRuntime* slot = nullptr;
        for (auto& actor : actors) {
            if (!actor.inUse) {
                slot = &actor;
                break;
            }
        }
        if (slot == nullptr) {
            actors.emplace_back();
            slot = &actors.back();
        }
        *slot = ActorRuntime{};
        slot->inUse = true;
        slot->state.actorId = id;
        slot->state.active = true;
        slot->state.motion = Motion::kLaunch;
        slot->name = profile != nullptr && !profile->name.empty() ? profile->name
                                                                  : std::format("{:08X}", id);
        slot->isPlayer = profile != nullptr && profile->isPlayer;
        SeedLimbMasses(*slot, profile);
        return *slot;
    }

    void Release(ActorRuntime& actor, TimeMs nowMs, std::string_view why) {
        if (!actor.inUse) {
            return;
        }
        // Before the summary, so a loop still running when the actor let go is
        // stopped rather than stranded. Only the motion axis' route to Resting
        // stops a loop, and an NPC who gets up mid-tumble never takes it:
        // `ragdoll_end` arrives in Ingest, before the strategies, so the actor is
        // gone by the time MotionFoley would have asked for the stop.
        StopActorLoops(actor, nowMs);

        if (actor.stats.contactsIn > 0) {
            log::Summary(actor.name, actor.stats,
                         actor.lastAdmittedMs - actor.firstContactMs);
        }
        spdlog::debug("actor {} dropped: {}", actor.name, why);

        // Belt and braces on the sweep above. Read before the state is cleared -
        // CrashState{} takes the actor id with it, and sweeping on a zeroed id
        // would drop somebody else's voices.
        const ActorId id = actor.state.actorId;
        std::erase_if(liveVoices, [id](const Voice& v) { return v.actorId == id; });

        actor.inUse = false;
        actor.state = CrashState{};
    }

    /// Every loop this actor still holds, stopped for real: a cue to the sink so
    /// the renderer lets its voice go, and the budget entry given back.
    void StopActorLoops(ActorRuntime& actor, TimeMs nowMs) {
        const AlgorithmConfig& c = For(actor);
        StopOneLoop(actor, nowMs, actor.riseRunning, actor.riseVoice, SlotId::kAirWhoosh,
                    CueReason::kAirborneRise, 200.0f);
        StopOneLoop(actor, nowMs, actor.scrapeRunning, actor.scrapeVoice, actor.scrapeSlot,
                    CueReason::kScrape, c.strategies.scrape.stopFadeMs);
        StopOneLoop(actor, nowMs, actor.armorSlideRunning, actor.armorSlideVoice,
                    actor.armorSlideSlot, CueReason::kArmorSkin,
                    c.strategies.scrape.stopFadeMs);
        // The bed, which has to be here and not only in `RumbleBed`: an NPC who
        // gets up mid-slide never reaches the route to Resting, so this sweep is
        // the only thing that would stop it. It is also the worst loop to strand -
        // pure low frequency, so it reads as the room having a hum in it.
        StopOneLoop(actor, nowMs, actor.scrapeBedRunning, actor.scrapeBedVoice,
                    SlotId::kScrapeLoopRumble, CueReason::kScrape,
                    c.strategies.scrape.stopFadeMs);
        StopOneLoop(actor, nowMs, actor.rustleRunning, actor.rustleVoice, SlotId::kClothRustle,
                    CueReason::kRustle, c.strategies.rustle.stopFadeMs);
        for (auto& loop : actor.limbLoops) {
            StopOneLoop(actor, nowMs, loop.running, loop.voice, loop.slot, CueReason::kScrape,
                        c.strategies.scrape.stopFadeMs);
        }
    }

    void StopOneLoop(ActorRuntime& actor, TimeMs nowMs, bool& running, std::uint32_t voiceId,
                     SlotId slot, CueReason reason, float fadeMs) {
        if (!running) {
            return;
        }
        running = false;
        ReleaseVoice(voiceId);
        if (sink == nullptr) {
            return;
        }
        // Straight to the sink rather than through a proposal: arbitration has
        // already run, and a stop is not a thing it may decline.
        Cue cue{};
        // The release, not the last contact it admitted. An actor who gets up
        // mid-slide was last hit some frames before `knock_get_up` while its grind
        // went on emitting updates. Stamping the stop with `lastAdmittedMs` put it
        // behind those updates: the game's renderer reads cues in arrival order and
        // never saw it, but the offline runner sorts by time, so the mixer - which
        // ends a loop on the last point it holds - found no stop there. One grind
        // and two limb scrapes ran for the remaining two minutes of the take.
        cue.timeMs = std::max(actor.lastAdmittedMs, nowMs);
        cue.op = CueOp::kStopLoop;
        cue.slot = slot;
        cue.gainDb = kSilentDb;
        cue.fadeMs = fadeMs;
        cue.voiceId = voiceId;
        cue.actorId = actor.state.actorId;
        cue.position = actor.bodyPoint;
        cue.boneIndex = -1;
        cue.reason = reason;
        cue.motion = actor.state.motion;
        cue.moment = actor.state.moment;
        sink->Emit(cue);
        spdlog::debug("actor {} let go with {} still running; stopped it", actor.name,
                      ToString(slot));
    }

    // ── intensity ────────────────────────────────────────────────────────────

    /// How much of the body is arriving through this contact, 0 to 1. A limb
    /// travelling with the body barely rotates; one whipping round a joint rotates
    /// hard and carries only its own mass. So compare surface turn
    /// (angular x radius) against centre speed: mostly turning is a flail, mostly
    /// moving is the body arriving. Every term is from this one contact.
    [[nodiscard]] float Coupling(float bodySpeed, float angularSpeed, float radius) const {
        const EffectiveMassConfig& lim = cfg.limbs;
        if (bodySpeed < lim.minBodySpeed) {
            return 0.0f;
        }
        const float surfaceTurn = angularSpeed * std::max(1.0f, radius);
        const float ratio = surfaceTurn / std::max(1.0f, bodySpeed);
        const float rotationShare = std::clamp(ratio / std::max(0.01f, lim.rotationRefRatio),
                                               0.0f, 1.0f);
        return std::clamp(1.0f - rotationShare, 0.0f, 1.0f);
    }

    [[nodiscard]] float Intensity(float impactSpeed, LimbSite site, float radius, float bodySpeed,
                                  float angularSpeed, ActorMode mode) const {
        const IntensityConfig& in = For(mode).intensity;
        const float span = std::max(1.0f, in.speedRefHigh - in.speedRefLow);
        float normalised = std::max(0.0f, (impactSpeed - in.speedRefLow) / span);

        // Soft-clip rather than reject: other mods' impulses arrive far outside
        // anything physical, and a silent obliterate is the worst outcome.
        if (normalised > in.softClipKnee) {
            const float room = std::max(0.05f, 1.0f - in.softClipKnee);
            normalised = in.softClipKnee + room * std::tanh((normalised - in.softClipKnee) / room);
        }
        float value = std::pow(std::clamp(normalised, 0.0f, 1.0f), in.curveExponent);

        // Size, from our own nominal mass table and the limb's bounding radius,
        // never the solver's masses - those would make the right arm three times
        // louder than the left for identical movement.
        constexpr float kReferenceMass = 2.5f;
        constexpr float kReferenceRadius = 14.5f;

        // Which mass is behind this contact - see EffectiveMassConfig. The radius
        // term is left alone either way: limb *size* is a legitimate timbre
        // difference worth about a decibel, not twenty.
        float mass = NominalMass(site);
        if (cfg.limbs.massDeterminesLoudness) {
            const float coupling =
                Coupling(bodySpeed, angularSpeed, radius) * cfg.limbs.couplingWeight;
            mass = Lerp(mass, cfg.limbs.bodyMass, std::clamp(coupling, 0.0f, 1.0f));
        }
        const float massTerm = std::pow(std::max(0.05f, mass / kReferenceMass), in.massWeight);
        const float radiusTerm =
            radius > 0.0f ? std::pow(std::max(0.05f, radius / kReferenceRadius), in.radiusWeight)
                          : 1.0f;
        value *= std::clamp(massTerm * radiusTerm, 0.4f, 2.2f);
        return std::clamp(value, 0.0f, 1.0f);
    }

    /// What the glancing-landing rule charges a contact - see
    /// GlancingImpactConfig. Neutral for anything the rule does not judge.
    struct GlanceCut {
        float ramp{};                 ///< 0 a square landing, 1 fully glancing
        float intensityScale{1.0f};   ///< class and rank
        float gainDb{};               ///< level and rank, before arbitration
        float trimDb{};               ///< level only, after arbitration
    };

    [[nodiscard]] GlanceCut Glance(LimbSite site, float impactSpeed, float tangentSpeed,
                                   float bodySpeed, ActorMode mode) const {
        const GlancingImpactConfig& g = For(mode).glancing;
        GlanceCut cut{};
        if (!g.enabled || bodySpeed < g.minBodySpeed) {
            return cut;
        }
        const bool landingLimb = site == LimbSite::kFoot || site == LimbSite::kCalf ||
                                 (g.includeThigh && site == LimbSite::kThigh);
        if (!landingLimb) {
            return cut;
        }

        const float transfer = std::clamp(impactSpeed / std::max(1.0f, bodySpeed), 0.0f, 1.0f);
        const float span = std::max(0.01f, g.fullTransferFrac - g.noTransferFrac);
        float ramp = std::clamp((g.fullTransferFrac - transfer) / span, 0.0f, 1.0f);

        // ...but back off as it stops looking like a landing and starts looking
        // like a slide: only a slide keeps running sideways at many times its
        // closing speed.
        const float ratio = tangentSpeed / std::max(1.0f, impactSpeed);
        const float slideSpan = std::max(0.01f, g.slideRatioFull - g.slideRatioStart);
        ramp *= 1.0f - std::clamp((ratio - g.slideRatioStart) / slideSpan, 0.0f, 1.0f);

        cut.ramp = ramp;
        cut.intensityScale = Lerp(1.0f, std::clamp(g.maxIntensityScale, 0.0f, 1.0f), ramp);
        cut.gainDb = g.maxGainCutDb * ramp;
        cut.trimDb = g.maxTrimCutDb * ramp;
        return cut;
    }

    /// The whole range onto about 35 dB. The references span 13-17 dB across their
    /// onsets with the bed 30-36 dB under the hero hit; a naive log curve gives 60
    /// and sounds wrong at both ends.
    [[nodiscard]] float GainFromIntensity(float intensity, ActorMode mode) const {
        return -For(mode).intensity.dynamicRangeDb * (1.0f - intensity);
    }

    /// The Stage 5 half of the same curve - see PostIntensityConfig. Returns a
    /// *difference*: the level under the post numbers minus what
    /// GainFromIntensity already charged, which is what makes it addable at the
    /// end and neutral defaults cost exactly nothing.
    [[nodiscard]] float PostShapeDb(float intensity, ActorMode mode) const {
        const PostIntensityConfig& post = For(mode).intensity.post;
        const float raw = std::clamp(intensity, 0.0f, 1.0f);

        float shaped = raw;
        if (post.softClipKnee < 1.0f && shaped > post.softClipKnee) {
            const float room = std::max(0.05f, 1.0f - post.softClipKnee);
            shaped = post.softClipKnee + room * std::tanh((shaped - post.softClipKnee) / room);
        }
        shaped = std::pow(std::clamp(shaped, 0.0f, 1.0f), post.curveExponent);

        const float range = std::max(0.0f, For(mode).intensity.dynamicRangeDb + post.extraRangeDb);
        return -range * (1.0f - shaped) - GainFromIntensity(raw, mode);
    }

    /// What this actor may spend right now, from both Stage 2 axes. Motion owns
    /// the trim and the grain count; the hero latch overrides both while open.
    /// This function is the entire coupling between the two axes, which is why
    /// they could be split at all.
    [[nodiscard]] const PhaseBudget& BudgetFor(const CrashState& state) const {
        if (state.moment == Moment::kHero) {
            return cfg.hero.budget;
        }
        return MotionBudgetFor(state);
    }

    /// The motion half on its own, with the hero latch not consulted - what a bed
    /// is levelled by. A body grinding along the floor is grinding along the floor
    /// whether or not the hit that just landed was the event of the fall.
    [[nodiscard]] const PhaseBudget& MotionBudgetFor(const CrashState& state) const {
        switch (state.motion) {
            case Motion::kLaunch: return cfg.motion.launch;
            case Motion::kAirborne: return cfg.motion.airborne;
            case Motion::kTumble: return cfg.motion.tumble;
            case Motion::kSlide: return cfg.motion.slide;
            case Motion::kCount:
                break;
        }
        return cfg.motion.tumble;
    }

    /// The role trim - every surface skin together, every grain together: the
    /// balance between kinds of layer. Which kind a slot is comes from
    /// `SlotDesc::family` rather than a hand-kept list of names. The three
    /// composite layers keep a trim each, because the balance *between* them is
    /// what the composite is.
    [[nodiscard]] float RoleTrimDb(SlotId slot, bool isPlayer) const {
        switch (slot) {
            case SlotId::kImpTransient:
                return cfg.mix.transientTrimDb;
            // Both body layers: the role trim balances the body against the
            // transient and the sub, which does not change with which wav carries
            // the mass. Missing the second case is silent - the family switch
            // below returns 0 for kImpact, so a limb composite would quietly lose
            // the whole body trim.
            case SlotId::kImpBody:
            case SlotId::kImpBodyLimb:
                return cfg.mix.bodyTrimDb;
            case SlotId::kImpSub:
                // A 30 Hz boom at zero distance through headphones is
                // overwhelming; in VR low frequency is felt as much as heard.
                return cfg.mix.subTrimDb + (isPlayer ? cfg.player.subTrimDb : 0.0f);
            default:
                break;
        }
        switch (Slot(slot).family) {
            case SlotFamily::kSurface:
                return cfg.surfaces.trimDb;
            case SlotFamily::kArmor:
                return cfg.armor.trimDb;
            // An accent is a grain that arrives on its own, so it takes the grain
            // trim.
            case SlotFamily::kGrain:
            case SlotFamily::kAccent:
                return cfg.mix.grainTrimDb;
            case SlotFamily::kLoop:
                return cfg.mix.loopTrimDb;
            case SlotFamily::kImpact:
            case SlotFamily::kVoice:
                break;
        }
        return 0.0f;
    }

    /// How much rank a contact on this site is given over its own level - the only
    /// writer of `Proposal::priorityDb`'s weight half. See the priority block in
    /// `ArbitrationConfig`.
    ///
    /// Binned through `DamageSiteFor`, the engine's one site->part mapping, so the
    /// neck counts as the column everywhere and an unreadable skeleton as a limb.
    [[nodiscard]] float SiteWeightDb(LimbSite site) const {
        switch (DamageSiteFor(site)) {
            case DamageSite::kHead:  return cfg.arb.headWeightDb;
            case DamageSite::kSpine: return cfg.arb.torsoWeightDb;
            case DamageSite::kLimb:  break;
        }
        return cfg.arb.limbWeightDb;
    }

    /// The per-file trim - see SlotGainConfig. The two declared-and-unfilled slots
    /// have none, for the same reason they have no mute: nothing resolves to them.
    [[nodiscard]] float SlotTrimDb(SlotId slot) const {
        const SlotGainConfig& g = cfg.slotGains;
        // A variant of another slot takes that slot's trim - see
        // `SlotDesc::trimsWith`. The thirteen surface skins come out of the
        // surfaces list rather than SlotGainConfig, so they are a table lookup
        // ahead of the switch, exactly as their mutes are in `LayerMute`.
        if (const SurfaceClass surface = SurfaceOfSlot(TrimOwner(slot));
            surface != SurfaceClass::kCount) {
            return cfg.surfaces.Skin(surface).trimDb;
        }
        switch (TrimOwner(slot)) {
            case SlotId::kImpTransient: return g.impTransient;
            case SlotId::kImpBody:      return g.impBody;
            // A trim of its own - two recordings at two levels - but `imp_body`'s
            // *mute*, since silencing the body layer is one decision. The split
            // `mutesWith` and `trimsWith` are two columns for.
            case SlotId::kImpBodyLimb:  return g.impBodyLimb;
            case SlotId::kImpSub:       return g.impSub;
            case SlotId::kArmorBare:    return cfg.armor.bareTrimDb;
            case SlotId::kArmorCloth:   return cfg.armor.clothTrimDb;
            case SlotId::kArmorLight:   return cfg.armor.lightTrimDb;
            case SlotId::kArmorHeavy:   return cfg.armor.heavyTrimDb;
            case SlotId::kLimbTap:      return g.limbTap;
            // One mute between them, but a trim each: three separate recordings
            // arrive at three different levels.
            case SlotId::kCrunchGran:   return g.crunchGran;
            case SlotId::kSpineCrunch:  return g.spineCrunch;
            case SlotId::kLimbCrunch:   return g.limbCrunch;
            case SlotId::kGoreWet:      return g.goreWet;
            case SlotId::kScrapeGrain:  return g.scrapeGrain;
            // The surface-coloured grinds arrive here as the loop they colour;
            // balancing them is what the surface panel's own trims are for.
            case SlotId::kScrapeLoop:   return g.scrapeLoop;
            case SlotId::kScrapeLimb:   return g.scrapeLimb;
            // Its own trim, not the grind's: the balance between them is the whole
            // of this layer.
            case SlotId::kScrapeLoopRumble: return g.scrapeLoopRumble;
            case SlotId::kAirWhoosh:    return g.airWhoosh;
            case SlotId::kHeadImpact:   return g.headImpact;
            case SlotId::kSettleRest:   return g.settleRest;
            default:                    return 0.0f;
        }
    }

    /// The per-part balance - see CompositeBalanceConfig. Which body part made this
    /// cue, crossed with which of the composite's four layers it is.
    ///
    /// Binned through `DamageSiteFor`, the same bin `BodySlot` uses to choose
    /// between `imp_body` and `imp_body_limb`, so the layer a limb *plays* and the
    /// trim a limb *gets* cannot disagree about what counts as a limb.
    ///
    /// Silent for everything else: the taps, crunches, gore, head accent and loops
    /// are per-part somewhere better, and the armour skin is a different axis.
    [[nodiscard]] float SiteBalanceDb(SlotId slot, LimbSite site) const {
        const CompositeBalanceConfig& balance = cfg.balance;
        if (!balance.enabled) {
            return 0.0f;
        }
        const LayerBalanceConfig& part = [&]() -> const LayerBalanceConfig& {
            switch (DamageSiteFor(site)) {
                case DamageSite::kHead:  return balance.head;
                case DamageSite::kSpine: return balance.spine;
                default:                 return balance.limb;
            }
        }();
        // Both body slots, as `RoleTrimDb` takes both: which wav carries the mass
        // is about timbre, this is about how much mass the part gets.
        switch (slot) {
            case SlotId::kImpTransient: return part.transientTrimDb;
            case SlotId::kImpBody:
            case SlotId::kImpBodyLimb:  return part.bodyTrimDb;
            case SlotId::kImpSub:       return part.subTrimDb;
            default:                    break;
        }
        // By family rather than name, so a fourth surface is a manifest row.
        return Slot(slot).family == SlotFamily::kSurface ? part.surfaceTrimDb : 0.0f;
    }

    /// Every trim that depends on the *layer* rather than the proposal: its kind,
    /// which file it resolved to, and which body part made it. One term rather
    /// than three, because `Emit` must not grow a summand each time a rule learns
    /// to trim (01 §5).
    [[nodiscard]] float LayerTrimDb(SlotId slot, LimbSite site, bool isPlayer) const {
        return RoleTrimDb(slot, isPlayer) + SlotTrimDb(slot) + SiteBalanceDb(slot, site);
    }


    /// Takes the whole crash state rather than one enum: Stage 2 has two axes, and
    /// a trace recording only one could not explain why a quiet-looking contact
    /// came out loud.
    void TraceLine(TimeMs timeMs, ActorId actorId, std::uint16_t limbIndex, std::uint32_t seq,
                   float impactSpeed, float intensity, const CrashState& state,
                   std::string_view outcome) {
        if (!tracing) {
            return;
        }
        TraceRecord record{};
        record.timeMs = timeMs;
        record.actorId = actorId;
        record.limbIndex = limbIndex;
        record.sourceSeq = seq;
        record.impactSpeed = impactSpeed;
        record.intensity = intensity;
        record.motion = state.motion;
        record.moment = state.moment;
        const std::size_t length = std::min(outcome.size(), sizeof(record.outcome) - 1);
        std::memcpy(record.outcome, outcome.data(), length);
        trace.push_back(record);
    }

    // ── voices ───────────────────────────────────────────────────────────────

    void ExpireVoices(TimeMs nowMs) {
        std::erase_if(liveVoices, [nowMs](const Voice& v) { return v.endsMs <= nowMs; });
    }

    /// Record a voice as being in flight. It cannot fail - see "There is no voice
    /// budget" above. A one-shot books for as long as its file runs; a loop books
    /// `kNever` and is given back by name in ReleaseVoice, because a loop past its
    /// own file length has wrapped rather than finished.
    void TakeVoice(ActorRuntime& actor, TimeMs endsMs, std::uint32_t voiceId) {
        liveVoices.push_back(Voice{endsMs, voiceId, actor.state.actorId});
    }

    /// Undo the booking made most recently. A one-shot's booking has no id - only
    /// loops carry one - and needs none: nothing else books between taking a
    /// proposal's voice and finding out whether it made a sound.
    void UntakeLastVoice() {
        if (!liveVoices.empty()) {
            liveVoices.pop_back();
        }
    }

    void ReleaseVoice(std::uint32_t voiceId) {
        if (voiceId == 0) {
            return;
        }
        std::erase_if(liveVoices, [voiceId](const Voice& v) { return v.voiceId == voiceId; });
    }

    // ── Stage 0: ingest ──────────────────────────────────────────────────────

    void Ingest(IFeed& feed) {
        contacts.clear();
        for (auto& actor : actors) {
            actor.sawContactThisTick = false;
        }
        for (const FeedEvent& event : drained) {
            ++stats.eventsIn;

            if (event.kind == EventKind::kState) {
                HandleState(event, feed);
                continue;
            }
            // Pose, not signal: it never becomes a Contact and never makes a sound,
            // but it is the only measurement of where the body actually is.
            //
            // Tick samples only. Older captures carry a limb_sample per limb at
            // ragdoll_start and ragdoll_end - a launch pose, not a signal - and
            // taking those would be worse than having none: two frames 1.5 seconds
            // apart set the "we have measurements" flag, turning off every fallback
            // the take needs, then difference a velocity across a gap that is not
            // a frame.
            if (event.kind == EventKind::kLimbSample) {
                if (pose::IsTickSample(event)) {
                    HandlePose(event, feed);
                }
                continue;
            }
            if (event.kind != EventKind::kImpact) {
                // Touch and separate carry no contact point and re-fire on most
                // frames of a persisting manifold; listener rows are state.
                continue;
            }
            // The gate. The ragdoll bodies exist and collide the whole time,
            // keyframed while the actor is animated, so without this the mod plays
            // impacts while NPCs walk around (07 §1). `bAnimatedMode` opens it on
            // purpose; the feed has its own copy in the contact callback, and both
            // must be open for a walking actor's contacts to arrive.
            if (event.phase != ActorPhase::kRagdoll && !cfg.game.animatedMode) {
                continue;
            }

            const ActorProfile* profile = feed.Profile(event.actorId);
            ActorRuntime& actor = Acquire(event.actorId, profile);
            actor.phase = event.phase;
            actor.inCombat = event.inCombat;
            actor.mode = ModeFor(event.phase, event.inCombat);

            // This actor's own column, and from here to the end of the contact
            // every ingest decision is read through it. The ragdoll bodies collide
            // the whole time a character is animated, so the floor that admits a
            // knockdown's small taps would admit every footstep as well: this is
            // the seam the whole mode axis exists for.
            const AlgorithmConfig& c = For(actor);

            // The floor, with the slide relief 01 §5's Admit stage is for. Asked of
            // the contact's own tangent speed, never the motion state: ingest runs
            // before Stage 2, so the state here is last tick's (01 §7.4).
            float floorSpeed = c.ingest.minImpactSpeed;
            if (c.ingest.slideFloorFrac < 1.0f) {
                const float from = c.ingest.minImpactSpeed;
                const float span =
                    std::max(1.0f, c.ingest.slideFloorAtTangent - from);
                const float ramp =
                    std::clamp((event.tangentSpeed - from) / span, 0.0f, 1.0f);
                floorSpeed *= Lerp(1.0f, std::clamp(c.ingest.slideFloorFrac, 0.0f, 1.0f), ramp);
            }
            if (event.impactSpeed < floorSpeed) {
                ++stats.rejectedBelowFloor;
                ++actor.stats.rejectedBelowFloor;
                continue;
            }
            if (IsBlowup(event, actor.mode)) {
                ++stats.rejectedBlowup;
                ++actor.stats.rejectedBlowup;
                spdlog::debug("seq {} rejected: blow-up, {:.0f} against {:.0f} u/s",
                              event.sourceSeq, event.impactSpeed, std::fabs(event.normalSpeed));
                continue;
            }

            // Above every rule about whether this collision is worth *hearing*,
            // because the driven test asks something else: whether the tick's
            // acceleration has a collision to explain it. A self-hit explains one
            // just as well - the centre is mass-weighted off our nominal table, so
            // a limb swinging into the body's own torso does not cancel out of it.
            // On devbench_5 that leak reads 554 u/s^2 at 6226 ms, over the driven
            // gate with nothing outside the body touching it.
            actor.sawContactThisTick = true;

            const LimbInfo* limb = profile != nullptr ? profile->Limb(event.limbIndex) : nullptr;
            if (limb == nullptr) {
                // An unrecognised skeleton still sounds, sized off limbRadius,
                // rather than going silent (07 §7).
                spdlog::debug("seq {} has no limb {} in the profile; sizing off radius",
                              event.sourceSeq, event.limbIndex);
            }

            // Every self-collision fires twice - 624 of 624 ordered pairs had their
            // mirror in the same frame. Keeping the copy whose own body sorts first
            // is a decision each end can make alone.
            if (c.ingest.dropMirroredSelfContacts && event.otherLimb >= 0 && limb != nullptr &&
                limb->bodyId != 0 && event.otherBody != 0 && limb->bodyId > event.otherBody) {
                ++stats.droppedMirror;
                ++actor.stats.droppedMirror;
                continue;
            }

            Contact contact{};
            contact.timeMs = event.timeMs;
            contact.actorId = event.actorId;
            contact.limbIndex = event.limbIndex;
            contact.sourceSeq = event.sourceSeq;
            contact.impactSpeed = event.impactSpeed;
            contact.tangentSpeed = event.tangentSpeed;
            contact.angularSpeed = event.angularSpeed;
            contact.bodySpeed = event.bodySpeed;
            contact.position = event.position;
            contact.normal = event.normal;
            contact.site = limb != nullptr ? limb->site : LimbSite::kUnknown;
            contact.chain = limb != nullptr ? limb->chain : LimbChain::kNone;
            contact.coverage = limb != nullptr ? limb->coverage : Coverage::kBare;
            contact.limbRadius =
                limb != nullptr && limb->radius > 0.0f ? limb->radius : event.limbRadius;
            // The collision layer is the reliable input and the material an
            // enrichment that fails at the edges (07 §8), so the material is
            // preferred when there is one and the layer stands in otherwise.
            contact.surface = event.otherMaterial != 0 ? SurfaceFromMaterial(event.otherMaterial)
                                                       : SurfaceFromLayer(event.otherLayer);
            contact.otherBody = event.otherBody;
            contact.selfContact = event.otherLimb >= 0;
            contact.graze = IsGraze(c.ingest, event.impactSpeed, event.tangentSpeed);
            contact.intensity = Intensity(event.impactSpeed, contact.site, contact.limbRadius,
                                          event.bodySpeed, event.angularSpeed, actor.mode);
            // A landing limb that only clipped the surface can be charged on
            // intensity (which also demotes it through the composite/tap branch),
            // on level, or both - but never on how big the fall itself looks, so
            // the untouched figure is kept.
            contact.rawIntensity = contact.intensity;
            const GlanceCut glance =
                Glance(contact.site, event.impactSpeed, event.tangentSpeed, event.bodySpeed,
                       actor.mode);
            contact.glanceScale = glance.intensityScale;
            contact.modTrimDb += glance.trimDb;
            contact.intensity = std::clamp(contact.intensity * glance.intensityScale, 0.0f, 1.0f);
            contact.onsetGainDb = GainFromIntensity(contact.intensity, actor.mode) + glance.gainDb;

            // All contact points of one manifold collapse to their max, grouped by
            // (limb, other body) within the frame rather than bracketed by
            // manifold_first/manifold_last: 244 rows carry `last` with no `first`,
            // and both flags re-fire across a persisting manifold's frames.
            if (c.ingest.collapseManifolds) {
                bool merged = false;
                for (Contact& existing : contacts) {
                    // (this body, other body) - 07 §3's x1.07 over-count. By the
                    // limb alone it would merge a hand hitting the floor with the
                    // same hand hitting a thigh, which is two collisions.
                    if (existing.actorId != contact.actorId ||
                        existing.limbIndex != contact.limbIndex ||
                        existing.otherBody != contact.otherBody) {
                        continue;
                    }
                    merged = true;
                    ++stats.collapsedManifold;
                    ++actor.stats.collapsedManifold;
                    // The fastest member wins outright, graze flag included.
                    // Carrying the flag over made it sticky: a manifold that opened
                    // with a 217 u/s skim and then took a 444 u/s slam stayed a
                    // graze, so the scrape path claimed it and the slam was silent.
                    if (contact.impactSpeed > existing.impactSpeed) {
                        existing = contact;
                    }
                    break;
                }
                if (merged) {
                    continue;
                }
            }

            ++stats.contactsIn;
            ++actor.stats.contactsIn;
            spdlog::debug("contact seq {} {} {:.0f} u/s tan {:.0f} -> intensity {:.2f} ({} {}{})",
                          event.sourceSeq, ToString(contact.site), contact.impactSpeed,
                          contact.tangentSpeed, contact.intensity, ToString(contact.surface),
                          contact.graze ? "graze" : "thud", contact.selfContact ? " self" : "");
            contacts.push_back(contact);
        }

        // Half of all contacts are one limb touching another of the same body, and
        // an arm brushing your own thigh makes no impact sound.
        std::size_t write = 0;
        for (std::size_t i = 0; i < contacts.size(); ++i) {
            const Contact& contact = contacts[i];
            // A second pass over every contact of every actor, so the column comes
            // from the contact rather than from the loop above it. Falling back to
            // the shared column for an actor already released is the same answer
            // for a row that is not per mode, and the safe one for a row that is.
            ActorRuntime* owner = Find(contact.actorId);
            const AlgorithmConfig& c = owner != nullptr ? For(*owner) : cfg;
            if (contact.selfContact && contact.impactSpeed < c.ingest.selfContactThreshold) {
                ++stats.droppedSelfContact;
                if (owner != nullptr) {
                    ++owner->stats.droppedSelfContact;
                }
                continue;
            }
            contacts[write++] = contacts[i];
        }
        contacts.resize(write);

        // Strongest first, ties broken by source row: an order that is a function
        // of the input and not of the container.
        std::ranges::stable_sort(contacts, StrongestFirst);
    }

    /// Reject blow-ups on the arithmetic, not on a threshold: `impactSpeed` is the
    /// solver's closing speed and `normalSpeed` the same quantity recomputed from
    /// both bodies' motion state, so a row where they disagree cannot be
    /// reproduced by rigid-body arithmetic (07 §2).
    [[nodiscard]] bool IsBlowup(const FeedEvent& event, ActorMode mode) const {
        const AlgorithmConfig& cfg = For(mode);
        if (event.normalSpeed != 0.0f) {
            // Magnitudes: normalSpeed comes out exactly negated on 23% of good
            // rows, so compared signed a quarter of the dataset looks broken.
            const float ours = std::fabs(event.normalSpeed);
            const float theirs = std::fabs(event.impactSpeed);
            return std::fabs(ours - theirs) / std::max(1.0f, theirs) >
                   cfg.ingest.blowupDisagreeFrac;
        }
        // Backstops, only when there is no reconstruction to check against. The old
        // guards were far too tight: 700 u/s rejects two clean takes, 25 rad/s
        // rejects 8.3% of good rows.
        return event.impactSpeed > cfg.ingest.blowupSpeedCeiling ||
               event.angularSpeed > cfg.ingest.blowupAngularCeiling;
    }

    /// Fold one limb's pose into this tick's running centre.
    ///
    /// Mass-weighted rather than "whatever the body called COM reports": that COM
    /// body is one rigid body among eighteen and whips about on its constraints
    /// like any other, while the weighted centre of the whole set follows a
    /// ballistic arc. Our nominal table is the weight, never the solver's masses,
    /// which would drag the centre sideways (07 §6).
    ///
    /// An unrecognised skeleton lands on kUnknown for every limb and weights them
    /// equally - a plain average, which is a perfectly good centre.
    void HandlePose(const FeedEvent& event, IFeed& feed) {
        const ActorProfile* profile = feed.Profile(event.actorId);
        ActorRuntime& actor = Acquire(event.actorId, profile);
        // The pose is the only row an actor who never collides carries, so in
        // animated mode it is what tells the rustle which world this actor is in -
        // and, since the modes, which column it is judged through.
        actor.phase = event.phase;
        actor.inCombat = event.inCombat;
        actor.mode = ModeFor(event.phase, event.inCombat);
        const LimbInfo* limb = profile != nullptr ? profile->Limb(event.limbIndex) : nullptr;
        const float mass = NominalMass(limb != nullptr ? limb->site : LimbSite::kUnknown);

        actor.poseSum.x += event.position.x * mass;
        actor.poseSum.y += event.position.y * mass;
        actor.poseSum.z += event.position.z * mass;
        actor.poseVelSum.x += event.velocity.x * mass;
        actor.poseVelSum.y += event.velocity.y * mass;
        actor.poseVelSum.z += event.velocity.z * mass;
        actor.poseMass += mass;
        ++actor.poseLimbs;
        actor.sawPoseEver = true;

        // Per limb as well as summed, because the body grind has to hang on *a*
        // bone and the only way to say which is nearest the contact is to know
        // where each is. One copy per limb per tick, off data already here.
        if (event.limbIndex < kMaxLimbs) {
            ActorRuntime::LimbTrack& track = actor.limbs[event.limbIndex];
            track.pos = event.position;
            track.havePos = true;
            if (track.mass <= 0.0f) {
                track.mass = mass;
            }
            // Rotation, kept rather than dropped: the garment wants it because a
            // limb spinning at a *steady* rate drags its sleeve continuously with
            // no acceleration in it at all.
            //
            // The velocity lands in `tickVel`; ConsumePose differences it against
            // `vel` and then promotes it, so the fold cannot overwrite its input.
            track.angularSpeed = event.angularSpeed;
            track.radius = event.limbRadius;
            track.tickVel = event.velocity;
            track.haveTickVel = true;
            actor.limbCount = std::max<std::size_t>(actor.limbCount, event.limbIndex + 1u);
        }
    }

    void HandleState(const FeedEvent& event, IFeed& feed) {
        const std::string_view state{event.text};
        if (state == "ragdoll_start") {
            // Only for an actor that is not already falling: a second
            // `ragdoll_start` mid-tumble is the game re-arming a knockdown that
            // never finished, and restarting on it would reset the burst budget
            // and hero count mid-fall. A fresh `Acquire` is already in `Launch`.
            ActorRuntime* existing = Find(event.actorId);
            const bool fresh = existing == nullptr;
            ActorRuntime& actor = Acquire(event.actorId, feed.Profile(event.actorId));
            if (fresh) {
                actor.state.ragdollStartMs = event.timeMs;
                EnterMotion(actor, Motion::kLaunch, event.timeMs);
                spdlog::debug("actor {} ragdoll_start -> Launch", actor.name);
            }
            return;
        }
        if (state == "combat_start" || state == "combat_stop") {
            // Find rather than Acquire, like a hold: entering a fight is not a
            // reason to start tracking a body. An actor acquired later gets their
            // combat flag off the next contact or pose row instead, both of which
            // carry it.
            if (ActorRuntime* actor = Find(event.actorId); actor != nullptr) {
                actor->inCombat = state == "combat_start";
                actor->mode = ModeFor(actor->phase, actor->inCombat);
                spdlog::debug("actor {} {} -> {}", actor->name, state, ToString(actor->mode));
            }
            return;
        }
        if (state == "held_start" || state == "held_stop") {
            // Find rather than Acquire: a hold is not a reason to open a knockdown.
            // The feed only publishes these for an actor it already tracks, and
            // re-publishes a live hold on the far side of a `ragdoll_start`, which
            // is the one edge that gives the actor a fresh runtime.
            if (ActorRuntime* actor = Find(event.actorId); actor != nullptr) {
                actor->heldByPlayer = state == "held_start";
                spdlog::debug("actor {} {}", actor->name, state);
            }
            return;
        }
        if (state == "ragdoll_end" || state == "knock_get_up" || state == "actor_gone" ||
            state == "session_stop") {
            if (ActorRuntime* actor = Find(event.actorId); actor != nullptr) {
                Release(*actor, event.timeMs, state);
            }
        }
    }

    // ── Stage 1 and 2: crash state, then the two axes ────────────────────────

    /// How much the clothes are moving, 0 to 1, and the envelope over it.
    ///
    /// Two signals summed over the limbs, weighted by how much *garment* each
    /// carries: `thrash` (acceleration relative to the body, u/s^2) and `tumble`
    /// (surface speed from rotation, u/s). Each is normalised through its own ramp
    /// before they blend - they are not commensurable, so adding them raw would
    /// bury a unit conversion inside a weight.
    ///
    /// Called from ConsumePose with the body's own acceleration, the reference the
    /// relative term subtracts, which is not stored anywhere else.
    void ConsumeRustle(ActorRuntime& actor, TimeMs nowMs, float dt, const Vec3& bodyAccel) {
        const RustleConfig& rust = For(actor).strategies.rustle;
        // Ragdoll-only, so it is read through the column that holds every shared
        // value rather than through the actor's. See RagdollDamageConfig.
        const DamageViolenceConfig& viol = cfg.strategies.ragdollDamage.violence;
        // **Two measurements, not one shared one.** The garment asks how much
        // *cloth* is moving and weights by what hangs on each limb; violence asks
        // how much *body* is being thrown about and weights by anatomical mass.
        // The first build had damage reading the garment's figure, which was wrong
        // twice over - a naked body breaks as well as a clothed one, and tuning the
        // rustle silently moved how often bones broke.
        //
        // The per-limb acceleration is shared because it is one number however it
        // is later weighted; everything downstream is computed twice.
        if (!rust.enabled && !viol.enabled) {
            // Off means nothing is measured, not merely nothing played.
            return;
        }

        // A moving average and **not** a peak-hold: measured on the corpus, a
        // peak-hold saturated at 1.00 on twelve takes of thirteen. A knockdown
        // always contains one very violent instant, so "the worst moment of the
        // last half-second" is nearly always the top of the range - a constant
        // offset on the thresholds rather than a term.
        //
        // The question is "has this been violent", not "did something violent
        // happen", so a fall that was bad and has calmed reads as calm.
        const float holdTau = std::max(1.0f, viol.holdMs) * 0.001f;
        const float decay = dt > 0.0f ? std::exp(-dt / holdTau) : 1.0f;
        const float rise = 1.0f - decay;
        // **The asymmetry that makes this measurement mean anything.** A limb
        // hitting stone has its velocity reversed inside one solver step, so on a
        // contact tick the thrash *is* the collision restated - and a violence
        // figure allowed to rise there would hand every contact its own impact
        // speed back as evidence. See 01 §7.4 on the `driven` gate.
        const bool mayRise = !actor.sawContactThisTick;

        // The garment's sums, weighted by cloth.
        float thrashSum = 0.0f;
        float tumbleSum = 0.0f;
        float fabricSum = 0.0f;
        // Violence's sums, weighted by anatomical mass. Separate accumulators
        // rather than one scaled afterwards: the weights differ per limb.
        float violThrashSum = 0.0f;
        float violTumbleSum = 0.0f;
        float massSum = 0.0f;

        for (std::size_t i = 0; i < actor.limbCount && i < kMaxLimbs; ++i) {
            ActorRuntime::LimbTrack& track = actor.limbs[i];
            const bool sampled = track.haveTickVel;
            // Consumed either way: a limb that misses a tick must not have last
            // tick's velocity read as this one's.
            track.haveTickVel = false;
            // Fades whenever there is no clean evidence to average in - a tick
            // carrying a collision, or a limb that missed a sample.
            if (!mayRise || !sampled) {
                track.violence *= decay;
            }
            if (!sampled) {
                continue;
            }

            const auto limbDt = static_cast<float>(nowMs - track.velMs) * 0.001f;
            // The same guard the centre uses: a gap far longer than a frame is a
            // rebuilt ragdoll or a resumed replay, not an enormous acceleration.
            const bool usable = track.haveVel && limbDt > 0.0f && limbDt <= 0.5f;

            if (usable) {
                Vec3 a{(track.tickVel.x - track.vel.x) / limbDt,
                       (track.tickVel.y - track.vel.y) / limbDt,
                       (track.tickVel.z - track.vel.z) / limbDt};
                // Violence is always relative - free fall is not violence - and the
                // garment has a switch only because it is worth an A/B there.
                Vec3 rel{a.x - bodyAccel.x, a.y - bodyAccel.y, a.z - bodyAccel.z};
                const float absLen = Length(a);
                const float relLen = Length(rel);

                // Clamped per limb and before the sum. Pose has no blow-up
                // rejection, so a teleported limb would dominate the body; and in
                // the ordinary case it keeps one limb striking stone from
                // saturating what the other seventeen contribute to. Two ceilings,
                // since the consumers may want different tolerances.
                thrashSum +=
                    track.fabric * std::min(rust.relativeToBody ? relLen : absLen,
                                            rust.thrashCeiling);

                const float violThrash = std::min(relLen, viol.thrashCeiling);
                violThrashSum += track.mass * violThrash;

                // This limb's own violence, off the raw acceleration rather than a
                // weighted share: a bare arm breaks as well as a sleeved one. The
                // only unweighted figure here, deliberately - `limbShare` asks
                // about *this limb*, not its contribution to the body.
                if (mayRise) {
                    const float sample = RampAt(violThrash, viol.thrashFloor, viol.thrashFull);
                    track.violence += (sample - track.violence) * rise;
                }
            }

            // Surface speed, not radians - the same product `Coupling` takes. What
            // drags a sleeve is how fast the surface travels, and for the other
            // consumer how much the limb is being wrung about its own axis.
            const float surface = track.angularSpeed * std::max(1.0f, track.radius);
            tumbleSum += track.fabric * surface;
            fabricSum += track.fabric;
            violTumbleSum += track.mass * surface;
            massSum += track.mass;

            track.vel = track.tickVel;
            track.haveVel = true;
            track.velMs = nowMs;
        }

        CrashState& state = actor.state;
        if (fabricSum <= 0.0f) {
            // "Nothing sampled this tick" is two different questions. An actor
            // wearing nothing we can name has no measurement and stays at zero. An
            // actor whose next pose frame has not landed yet must keep the last
            // one: zeroing it drew a sawtooth at the pose rate into the raw drive,
            // and through the near-instant attack into the level.
            if (actor.fabricTotal <= 0.0f) {
                state.rustleDriveRaw = 0.0f;
            }
        } else {
            const float inv = 1.0f / fabricSum;
            const float thrashMean = thrashSum * inv;
            const float tumbleMean = tumbleSum * inv;
            // The two raw means at their peak, before either ramp - the measurement
            // the ramps are guesses for. See EngineStats.
            const auto tally = [&](EngineStats& into) {
                into.rustleThrashPeak = std::max(into.rustleThrashPeak, thrashMean);
                into.rustleTumblePeak = std::max(into.rustleTumblePeak, tumbleMean);
                // A running mean rather than a sum, so the field means the same
                // thing mid-take as at the end and cannot overflow on a long one.
                ++into.rustleTicks;
                const float n = static_cast<float>(into.rustleTicks);
                into.rustleThrashMean += (thrashMean - into.rustleThrashMean) / n;
                into.rustleTumbleMean += (tumbleMean - into.rustleTumbleMean) / n;
            };
            tally(stats);
            tally(actor.stats);

            const float thrash = RampAt(thrashMean, rust.thrashFloor, rust.thrashFull);
            const float tumble = RampAt(tumbleMean, rust.tumbleFloor, rust.tumbleFull);

            float raw = std::clamp(rust.thrashWeight * thrash + rust.tumbleWeight * tumble,
                                   0.0f, 1.0f);
            // Speed multiplies and air adds, and the asymmetry is the argument: a
            // body drifting fast and limply must stay silent, so speed only scales
            // what the limbs are already doing; a free fall makes the relative term
            // read nothing, so it has to come from outside that product.
            raw *= 1.0f + rust.speedWeight * RampAt(state.bodySpeed, 0.0f, rust.speedForFull);
            if (rust.airWeight > 0.0f && state.airborne) {
                raw += rust.airWeight *
                       RampAt(std::fabs(state.verticalSpeed), 0.0f, rust.airSpeedForFull);
            }
            state.rustleDriveRaw = std::clamp(raw, 0.0f, 1.0f);
        }

        // The body-wide violence, off its own mass-weighted sums and ramps. It
        // shares the pose stream with the garment and nothing else - no fabric
        // weighting, no speed multiplier, no airborne term.
        if (massSum > 0.0f && mayRise) {
            const float inv = 1.0f / massSum;
            const float thrashMean = violThrashSum * inv;
            const float tumbleMean = violTumbleSum * inv;
            stats.violenceThrashPeak = std::max(stats.violenceThrashPeak, thrashMean);
            actor.stats.violenceThrashPeak =
                std::max(actor.stats.violenceThrashPeak, thrashMean);
            ++stats.violenceTicks;
            stats.violenceThrashMean +=
                (thrashMean - stats.violenceThrashMean) / static_cast<float>(stats.violenceTicks);
            ++actor.stats.violenceTicks;
            actor.stats.violenceThrashMean +=
                (thrashMean - actor.stats.violenceThrashMean) /
                static_cast<float>(actor.stats.violenceTicks);

            const float sample = std::clamp(
                viol.thrashWeight * RampAt(thrashMean, viol.thrashFloor, viol.thrashFull) +
                    viol.tumbleWeight * RampAt(tumbleMean, viol.tumbleFloor, viol.tumbleFull),
                0.0f, 1.0f);
            state.motionViolence += (sample - state.motionViolence) * rise;
        } else {
            state.motionViolence *= decay;
        }
        stats.violencePeak = std::max(stats.violencePeak, state.motionViolence);
        actor.stats.violencePeak = std::max(actor.stats.violencePeak, state.motionViolence);

        // The envelope, asymmetric on purpose: fast up because cloth responds
        // immediately, slow down because a garment carries on moving - which fills
        // the gaps between bounces and leaves a fabric tail behind every impact.
        const float target = state.rustleDriveRaw;
        const float tauMs = target > state.rustleDrive ? rust.attackMs : rust.releaseMs;
        const float tau = std::max(1.0f, tauMs) * 0.001f;
        // A time constant and not a per-tick fraction, so the envelope is the same
        // shape at 24 fps and at 144.
        const float alpha = dt > 0.0f ? 1.0f - std::exp(-dt / tau) : 1.0f;
        state.rustleDrive += (target - state.rustleDrive) * std::clamp(alpha, 0.0f, 1.0f);
    }

    /// Turn this tick's accumulated limb poses into a centre, a velocity, a
    /// vertical acceleration, and the question all of it exists to answer: is
    /// anything holding this body up.
    ///
    /// Acceleration and not height, because height needs a ground to be above and
    /// the only one we have is the last floor contact - stale the moment the body
    /// starts travelling, and on a staircase a step it left three bounces ago.
    void ConsumePose(ActorRuntime& actor, TimeMs nowMs) {
        CrashState& state = actor.state;
        state.haveBodySamples = actor.sawPoseEver;
        if (actor.poseLimbs == 0 || actor.poseMass <= 0.0f) {
            return;
        }

        const float inv = 1.0f / actor.poseMass;
        const Vec3 com{actor.poseSum.x * inv, actor.poseSum.y * inv, actor.poseSum.z * inv};
        const Vec3 vel{actor.poseVelSum.x * inv, actor.poseVelSum.y * inv,
                       actor.poseVelSum.z * inv};
        actor.poseSum = {};
        actor.poseVelSum = {};
        actor.poseMass = 0.0f;
        actor.poseLimbs = 0;

        // How far the centre moved since the last tick, before `lastCom` is
        // overwritten. The honest answer to "how far has the body slid": a contact
        // point hops between manifold points and limbs, so its displacement is
        // mostly jitter.
        actor.comStepUnits =
            actor.haveLastCom ? Distance(com, actor.lastCom) : 0.0f;

        state.comPosition = com;
        state.comVelocity = vel;
        state.verticalSpeed = vel.z;
        // Measured, not held: `bodySpeed` used to be the last contact's, decayed
        // exponentially, because there was no periodic measurement to have.
        state.bodySpeed = Length(vel);

        const auto dt = static_cast<float>(nowMs - actor.lastComMs) * 0.001f;
        // A gap far longer than a frame is a rebuilt ragdoll or a resumed replay,
        // not an enormous acceleration. Re-seed rather than difference across it.
        if (!actor.haveLastCom || dt <= 0.0f || dt > 0.5f) {
            actor.lastCom = com;
            actor.lastComVel = vel;
            actor.lastComMs = nowMs;
            actor.haveLastCom = true;
            return;
        }
        const Vec3 accel{(vel.x - actor.lastComVel.x) / dt, (vel.y - actor.lastComVel.y) / dt,
                         (vel.z - actor.lastComVel.z) / dt};
        state.verticalAccel = accel.z;
        actor.lastCom = com;
        actor.lastComVel = vel;
        actor.lastComMs = nowMs;

        // The garment, measured here because the per-limb pose is already here and
        // the relative term needs `accel`, which is kept nowhere else. `dt` is
        // passed rather than recovered - `lastComMs` has just moved on.
        ConsumeRustle(actor, nowMs, dt, accel);

        // What gravity cannot account for. Free fall is straight down at exactly g,
        // so |a - g| is zero for a body nothing is touching and grows with whatever
        // pushes it. No direction is privileged: a leash hauls sideways, a shout
        // backwards, a blast upward, and all three are the same measurement.
        const Vec3 residual{accel.x, accel.y,
                            accel.z + std::max(0.0f, cfg.motion.gravityUnitsPerSec2)};
        state.drivenResidual = Length(residual);

        // Steeper than free fall still counts: a body driven into the ground has
        // nothing holding it up either. The other sign is excluded - a shove pushes
        // the body *up*, and reading that as flight is the bug this replaces.
        const float gate = -cfg.motion.freeFallFrac * std::max(1.0f, cfg.motion.gravityUnitsPerSec2);
        const bool ballistic = state.verticalAccel <= gate;
        if (ballistic) {
            actor.freeFallForMs += dt * 1000.0f;
            actor.lastFreeFallMs = nowMs;
        } else if (Ancient(actor.lastFreeFallMs) ||
                   (nowMs - actor.lastFreeFallMs) > cfg.motion.freeFallHoldMs) {
            // Only once the hold has run out, so a body clipping something on the
            // way down does not land and take off repeatedly.
            actor.freeFallForMs = 0.0f;
        }

        const bool wasAirborne = state.airborne;
        state.airborne = actor.freeFallForMs >= cfg.motion.freeFallMinMs;

        // Only asked while the body is otherwise unsupported: a body on the floor
        // is held up by the floor, which reads as a residual of a full g - true and
        // useless. The question is narrower: *this flight*, fall or throw.
        //
        // And never on a frame the body is hitting the world. A collision is the
        // largest unexplained acceleration in a fall by an order of magnitude - the
        // leash yank this gate was measured on reads 900-1600 u/s^2, while
        // Proventus_Avenicci_devbench_5's arrival at 2919 ms reads 5110. So every
        // landing declared itself driven, pushing the clock below onto the frame of
        // impact, and every flight in that take reported 20-23 ms and 0 units
        // against a real 61-331 ms and 4-43.
        const bool detect = cfg.motion.drivenEnabled && cfg.motion.drivenResidual > 0.0f;
        const bool wasDriven = state.driven;
        if (detect && state.airborne && !actor.sawContactThisTick &&
            state.drivenResidual > cfg.motion.drivenResidual) {
            actor.lastDrivenMs = nowMs;
        }
        state.driven = detect && state.airborne && !Ancient(actor.lastDrivenMs) &&
                       (nowMs - actor.lastDrivenMs) <= cfg.motion.drivenHoldMs;

        if (state.airborne && !wasAirborne) {
            state.airborneSinceMs = nowMs;
            state.freeFlightSinceMs = nowMs;
            actor.airborneStartZ = com.z;
            state.fallDropUnits = 0.0f;
            actor.drivenThisFlight = false;
            spdlog::debug("actor {} left support at {:.0f} ms, {:.0f} u/s^2 down", actor.name, nowMs,
                          -state.verticalAccel);
        } else if (!state.airborne && wasAirborne) {
            // Latched: this is the edge every landing rule was written for and the
            // only place the whole flight is still known. A frame later `airborne`
            // is false and the live clock reads zero. Kept for `landingWindowMs`.
            actor.lastFlightMs =
                std::max(0.0f, static_cast<float>(nowMs - state.freeFlightSinceMs));
            actor.lastFlightDropUnits = state.fallDropUnits;
            actor.lastFlightEndedMs = nowMs;
            spdlog::debug("actor {} landed at {:.0f} ms after {:.0f} ms and {:.0f} units",
                          actor.name, nowMs, actor.lastFlightMs, actor.lastFlightDropUnits);
        } else if (state.airborne) {
            // Relative, and therefore stair-proof: how far this body has come down
            // since it left support, never how high it is above anything.
            state.fallDropUnits = std::max(0.0f, actor.airborneStartZ - com.z);
        }

        if (state.driven) {
            // The clock air time is read off, pushed forward while something is
            // pushing, so it reports how long the body has been falling *on its
            // own*. The drop goes with it: units covered under power were not
            // fallen either.
            state.freeFlightSinceMs = nowMs;
            actor.airborneStartZ = com.z;
            state.fallDropUnits = 0.0f;
            if (!actor.drivenThisFlight) {
                actor.drivenThisFlight = true;
                ++stats.drivenFlights;
                ++actor.stats.drivenFlights;
            }
            if (!wasDriven) {
                spdlog::debug("actor {} is being driven at {:.0f} ms: {:.0f} u/s^2 that gravity "
                              "does not explain",
                              actor.name, nowMs, state.drivenResidual);
            }
        }
    }

    void UpdateState(ActorRuntime& actor, TimeMs nowMs, const ListenerState& listener,
                     float frameMs) {
        CrashState& state = actor.state;

        // An envelope follower over contact speed, not a running total: "how hard
        // has this fall been hitting lately" is a question about the last few
        // hundred milliseconds, and units/s keeps it comparable with
        // minImpactSpeed and the 355-543 of an ordinary shove. Read by the hero
        // test's dominance clause.
        const auto elapsed = static_cast<float>(nowMs - actor.energyStampMs);
        if (elapsed > 0.0f) {
            actor.energyRecent *= std::exp(-elapsed / 300.0f);
            // The old guess, kept for takes with no pose sidecar: the only
            // measurement was the last contact's, and in the air a body speeds up
            // rather than slowing, so it is released slowly rather than dropped.
            // ConsumePose overwrites this where a take carries pose.
            if (!actor.sawPoseEver) {
                state.bodySpeed *= std::exp(-elapsed / 1500.0f);
            }
            // A running maximum never comes down, and this used to be one: a single
            // fast skim held the slide's entry test open for the rest of the
            // knockdown. Decayed on the grace window instead.
            actor.slideTangent *=
                std::exp(-elapsed / std::max(20.0f, cfg.motion.slideGraceMs));
            // Each chain's own hold, on the same constant and for the same reason.
            for (auto& loop : actor.limbLoops) {
                loop.tangent *= std::exp(-elapsed / std::max(20.0f, cfg.motion.slideGraceMs));
            }
            actor.energyStampMs = nowMs;
        }
        const auto maskElapsed = static_cast<float>(nowMs - actor.maskStampMs);
        if (maskElapsed > 0.0f) {
            state.maskCeilingDb = std::max(
                kSilentDb, state.maskCeilingDb - cfg.arb.maskDecayDbPerSec * maskElapsed * 0.001f);
            actor.maskStampMs = nowMs;
        }

        // The pose this tick's kLimbSample rows built, before anything reads it.
        ConsumePose(actor, nowMs);

        // Snapshot the world-contact stamp before this tick folds its own contacts
        // in: an arm landing in the *same* frame as the head is part of that
        // landing, not evidence the body was already down.
        actor.worldContactBeforeTickMs = actor.lastWorldContactMs;
        actor.handContactBeforeTickMs = actor.lastHandContactMs;
        actor.worldPeers = 0;
        actor.worldPeerFastest = 0.0f;
        // Same idea for the hero rule's dominance clause: it asks whether this
        // contact stands out from the recent peak, and the loop below is about to
        // fold this contact into that peak.
        actor.energyRecentBeforeTick = actor.energyRecent;
        // A moment resets the burst on the tick it is anchored and not after.
        actor.heroResetsBurst = false;

        float frameBodySpeed = 0.0f;
        bool grazedThisTick = false;
        // The grazing contacts' own mass-weighted centre, built as they arrive -
        // what the body grind's bone is chosen as nearest to. Not "lowest": a body
        // can grind along a wall, down a staircase, or across a ceiling.
        Vec3 grazeSum{};
        float grazeMass = 0.0f;
        float heaviestGraze = 0.0f;
        for (const Contact& contact : contacts) {
            if (contact.actorId != state.actorId) {
                continue;
            }
            // A peer is any limb but the head and neck touching something that is
            // not a body, so a self-hit or brushing another ragdoll never counts
            // as the body having reached the floor.
            if (contact.surface != SurfaceClass::kBody && contact.site != LimbSite::kHead &&
                contact.site != LimbSite::kNeck) {
                if (contact.site == LimbSite::kHand) {
                    actor.lastHandContactMs = contact.timeMs;
                } else {
                    actor.lastWorldContactMs = contact.timeMs;
                }
                ++actor.worldPeers;
                actor.worldPeerFastest = std::max(actor.worldPeerFastest, contact.impactSpeed);
            }
            if (Ancient(actor.firstContactMs)) {
                actor.firstContactMs = contact.timeMs;
            }
            actor.lastAdmittedMs = contact.timeMs;
            state.lastContactMs = contact.timeMs;
            ++state.contactCount;
            state.peakSpeed = std::max(state.peakSpeed, contact.impactSpeed);
            actor.stats.peakSpeed = std::max(actor.stats.peakSpeed, contact.impactSpeed);
            stats.peakSpeed = std::max(stats.peakSpeed, contact.impactSpeed);
            state.energyAccum += contact.rawIntensity;
            actor.energyRecent = std::max(actor.energyRecent, contact.impactSpeed);
            actor.siteEnergy[static_cast<std::size_t>(contact.site)] += contact.rawIntensity;
            // A fresh measurement replaces the held one rather than taking a max
            // with it: a frame where every limb reports 30 u/s is the body having
            // stopped, which is when the event should be allowed to close.
            frameBodySpeed = std::max(frameBodySpeed, contact.bodySpeed);

            // A near-vertical normal is the ground, which the height and the
            // surface underneath are both measured against.
            if (std::fabs(contact.normal.z) > 0.7f) {
                actor.groundZ = actor.haveGroundZ ? std::min(actor.groundZ, contact.position.z)
                                                  : contact.position.z;
                actor.haveGroundZ = true;
                state.surfaceUnder = contact.surface;
            }
            if (contact.site == LimbSite::kHead) {
                actor.headZ = contact.position.z;
                actor.haveHeadZ = true;
            }
            // The slide's tallies, here rather than in ScrapeLoopStrategy because
            // the motion axis reads them and Stage 2 runs before Stage 3.
            //
            // World grazes only: an arm brushing your own thigh is not the body
            // travelling along a surface, and letting self-contacts open a slide is
            // how a corpse folding up on itself starts grinding.
            if (contact.graze && !contact.selfContact) {
                grazedThisTick = true;
                // A run of grazes with a hole in it is two runs. Without this a
                // body that bounced, flew for a fifth of a second and landed again
                // kept credit for the grazing from before the bounce, so a skipping
                // body was in Slide continuously.
                const bool lapsed = (contact.timeMs - actor.grazeLastMs) >
                                    Window(cfg.motion.slideGraceMs, frameMs, cfg.arb.frameScaleK);
                if (Ancient(actor.grazeSinceMs) || lapsed) {
                    actor.grazeSinceMs = contact.timeMs;
                    actor.slideDistance = 0.0f;
                    actor.slideSpeed = 0.0f;
                }
                actor.grazeLastMs = contact.timeMs;
                actor.slideTangent = std::max(actor.slideTangent, contact.tangentSpeed);
                actor.slideTickTangent = std::max(actor.slideTickTangent, contact.tangentSpeed);
                actor.scrapeLimb = contact.limbIndex;
                actor.slideSite = contact.site;
                actor.slideChain = contact.chain;
                actor.slideSeq = contact.sourceSeq;
                actor.slideCoverage = contact.coverage;
                actor.slideSurface = contact.surface;
                actor.slideRadius = contact.limbRadius;

                // Which limb, so the contact fraction can be a sum over the ones
                // actually rubbing rather than "is anything rubbing".
                const float mass = contact.limbIndex < kMaxLimbs
                                       ? actor.limbs[contact.limbIndex].mass
                                       : 0.0f;
                if (contact.limbIndex < kMaxLimbs) {
                    actor.limbs[contact.limbIndex].grazeMs = contact.timeMs;
                }
                const float weight = mass > 0.0f ? mass : NominalMass(contact.site);
                grazeSum.x += contact.position.x * weight;
                grazeSum.y += contact.position.y * weight;
                grazeSum.z += contact.position.z * weight;
                grazeMass += weight;
                if (weight > heaviestGraze) {
                    heaviestGraze = weight;
                    actor.heaviestGrazeLimb = contact.limbIndex;
                    actor.haveHeaviestGraze = true;
                }

                // And which chain, so a dragging foot and a trailing hand each
                // get a loop of their own instead of sharing the body's.
                if (const int chain = ScrapeChainOf(contact.chain); chain >= 0) {
                    ActorRuntime::LimbLoop& loop =
                        actor.limbLoops[static_cast<std::size_t>(chain)];
                    loop.lastGrazeMs = contact.timeMs;
                    loop.lastGrazeSeq = contact.sourceSeq;
                    loop.tangent = std::max(loop.tangent, contact.tangentSpeed);
                    loop.surface = contact.surface;
                    // The bone in the chain doing the most rubbing this tick. It
                    // only becomes the one the loop hangs on after `fLimbHoldMs`.
                    if (contact.tangentSpeed >= loop.tickTangent) {
                        loop.tickTangent = contact.tangentSpeed;
                        loop.wantAnchor = contact.limbIndex;
                        loop.site = contact.site;
                        loop.point = contact.position;
                        loop.havePoint = true;
                    }
                }
            }
            actor.bodyPoint = contact.position;
            actor.haveBodyPoint = true;
            if (listener.timeMs > 0.0) {
                state.distanceToListener = Distance(contact.position, listener.position);
            }
        }

        // Only when there is nothing better: a contact reports the speed of one
        // limb, and a limb whipping on its constraints is not the body.
        if (frameBodySpeed > 0.0f && !actor.sawPoseEver) {
            state.bodySpeed = frameBodySpeed;
        }

        // Ground covered since the run of grazes opened, and how fast the body was
        // going while covering it. Both are held rather than decayed once the
        // grazing stops: what the slide-end impact needs is the speed at
        // interruption, not that figure minus however long it took to notice.
        if (grazedThisTick) {
            actor.slideDistance += state.haveBodySamples
                                       ? actor.comStepUnits
                                       : actor.slideTickTangent * frameMs * 0.001f;
            actor.slideSpeed = std::max(actor.slideSpeed, SlideSpeed(actor));
        }
        actor.slideTickTangent = 0.0f;

        if (grazeMass > 0.0f) {
            const float inv = 1.0f / grazeMass;
            actor.grazeCentre = {grazeSum.x * inv, grazeSum.y * inv, grazeSum.z * inv};
            actor.haveGrazeCentre = true;
        }
        UpdateContactFraction(actor, nowMs);
        UpdateScrapeAnchors(actor, nowMs);

        float best = 0.0f;
        for (std::size_t i = 0; i < kSiteCount; ++i) {
            if (actor.siteEnergy[i] > best) {
                best = actor.siteEnergy[i];
                state.leadingLimb = static_cast<LimbSite>(i);
            }
        }
        state.headDown = actor.haveHeadZ && actor.haveGroundZ && actor.headZ < actor.groundZ + 30.0f;

        // Distance, per actor per tick rather than per contact.
        state.tier = listener.timeMs <= 0.0                                        ? DistanceTier::kFull
                     : state.distanceToListener > cfg.distance.simplifiedRadius     ? DistanceTier::kCulled
                     : state.distanceToListener > cfg.distance.fullRadius           ? DistanceTier::kSimplified
                                                                                    : DistanceTier::kFull;

        // ConsumePose has already set the flag from the body's own acceleration and
        // nothing here may overrule it. The fallback is the old inference, kept for
        // takes recorded before the sidecar and wrong in the ways that motivated
        // replacing it: a body that has merely stopped touching anything reads as
        // flying, maximally so at ragdoll_start and on a body lying still.
        if (!actor.sawPoseEver) {
            state.airborne =
                !Ancient(actor.lastAdmittedMs) &&
                (nowMs - actor.lastAdmittedMs) >
                    Window(cfg.motion.airborneMinTimeMs, frameMs, cfg.arb.frameScaleK);
        }

        // Motion first, then moment: the moment axis reads what the body is doing,
        // and the budget the arbitrator asks for is a function of both.
        AdvanceMotion(actor, nowMs, frameMs);
        AdvanceMoment(actor, nowMs, frameMs);
        AdvanceBedTrim(actor, frameMs);
    }

    /// The motion trim the bed is levelled by, glided rather than stepped.
    ///
    /// Once per actor per tick, after the motion axis has settled and before any
    /// strategy proposes, so every cue of a tick is levelled by one number. Per
    /// proposal, a loop and the impact beside it would disagree about what the
    /// body was doing.
    ///
    /// A time constant, not a per-tick fraction, so the glide is the same shape at
    /// 24 fps and at 144.
    void AdvanceBedTrim(ActorRuntime& actor, float frameMs) {
        const float target = MotionBudgetFor(actor.state).gainTrimDb;
        // Seeded, never faded into: a take opens with the body already in some
        // motion, and gliding up from 0 dB would be an eight-decibel swell at the
        // top of every fall with nothing behind it.
        if (!actor.haveBedTrim || cfg.motion.bedTrimGlideMs <= 0.0f) {
            actor.bedTrimDb = target;
            actor.haveBedTrim = true;
            return;
        }
        const float tau = std::max(1.0f, cfg.motion.bedTrimGlideMs) * 0.001f;
        const float dt = std::max(0.0f, frameMs) * 0.001f;
        const float alpha = dt > 0.0f ? 1.0f - std::exp(-dt / tau) : 0.0f;
        actor.bedTrimDb += (target - actor.bedTrimDb) * std::clamp(alpha, 0.0f, 1.0f);
    }

    /// How much of the body is on the surface, 0 to 1: our own anatomical masses
    /// summed over the limbs that have grazed inside `fContactHoldMs`, over the
    /// whole body's. One foot ~1.5%, both feet and shins ~13%, a body flat on its
    /// back and skidding 60%+. Without it the grind's level stood on speed alone,
    /// so a corpse dragged by one ankle read as loud as one lying flat.
    ///
    /// The hold turns a stream of collisions into a state: a fraction from one
    /// tick's contacts flickers between 45% and nothing at solver rate.
    ///
    /// **This shapes the loop and nothing else** - no inferred state may silence a
    /// contact.
    void UpdateContactFraction(ActorRuntime& actor, TimeMs nowMs) const {
        if (actor.bodyMass <= 0.0f) {
            // No profile, so no sites, so no honest sum. Zero is the quieter
            // answer: the limb loops play and the body grind does not.
            actor.contactFraction = 0.0f;
            return;
        }
        const auto hold =
            static_cast<float>(std::max(0.0f, For(actor).strategies.scrape.contactHoldMs));
        float touching = 0.0f;
        for (std::size_t i = 0; i < actor.limbCount && i < kMaxLimbs; ++i) {
            const ActorRuntime::LimbTrack& limb = actor.limbs[i];
            if (!Ancient(limb.grazeMs) && static_cast<float>(nowMs - limb.grazeMs) <= hold) {
                touching += limb.mass;
            }
        }
        actor.contactFraction = std::clamp(touching / actor.bodyMass, 0.0f, 1.0f);
    }

    /// Which bone each loop hangs on. The body grind takes the limb nearest the
    /// grazing contacts' centre where the take carries pose, and the heaviest limb
    /// that grazed where it does not. A limb loop takes whichever bone in its chain
    /// has been rubbing hardest for `fLimbHoldMs` - the hold is the cure for a
    /// scrape hopping between bones and smearing rather than tracking.
    void UpdateScrapeAnchors(ActorRuntime& actor, TimeMs nowMs) const {
        if (actor.haveGrazeCentre) {
            float best = 0.0f;
            bool found = false;
            for (std::size_t i = 0; i < actor.limbCount && i < kMaxLimbs; ++i) {
                if (!actor.limbs[i].havePos) {
                    continue;
                }
                const float d = Distance(actor.limbs[i].pos, actor.grazeCentre);
                if (!found || d < best) {
                    best = d;
                    found = true;
                    actor.bodyAnchor = static_cast<std::uint16_t>(i);
                }
            }
            if (found) {
                actor.haveBodyAnchor = true;
            }
        }
        if (!actor.haveBodyAnchor && actor.haveHeaviestGraze) {
            // No pose to measure a distance against. The heaviest limb that was
            // grinding is still much closer to the truth than the pelvis.
            actor.bodyAnchor = actor.heaviestGrazeLimb;
            actor.haveBodyAnchor = true;
        }

        const auto hold =
            static_cast<float>(std::max(0.0f, For(actor).strategies.scrape.limbHoldMs));
        for (std::size_t i = 0; i < kScrapeChainCount; ++i) {
            ActorRuntime::LimbLoop& loop = actor.limbLoops[i];
            loop.tickTangent = 0.0f;
            if (!loop.havePoint) {
                continue;
            }
            if (!loop.haveAnchor) {
                loop.anchor = loop.wantAnchor;
                loop.haveAnchor = true;
                loop.wantSinceMs = nowMs;
                continue;
            }
            if (loop.wantAnchor == loop.anchor) {
                loop.wantSinceMs = nowMs;
                continue;
            }
            if (Ancient(loop.wantSinceMs) || static_cast<float>(nowMs - loop.wantSinceMs) >= hold) {
                spdlog::debug("actor {} {} scrape moved from limb {} to {}", actor.name,
                              ScrapeChainName(i), loop.anchor, loop.wantAnchor);
                loop.anchor = loop.wantAnchor;
                loop.wantSinceMs = nowMs;
            }
        }
    }

    /// Stage 2, axis one. What the body is doing.
    ///
    /// Every edge is available from every state that can reach it. The machine this
    /// replaces could only reach `Airborne` from `Launch`, so a body that left the
    /// ground again halfway down a staircase was never airborne twice.
    void AdvanceMotion(ActorRuntime& actor, TimeMs nowMs, float frameMs) {
        CrashState& state = actor.state;
        const Motion previous = state.motion;

        // The hardest contact this actor took this tick, on the untouched figure.
        // Reading the adjusted intensity here let a modifier walk the actor into a
        // different state and quieten everything that followed.
        float frameSpeed = 0.0f;
        for (const Contact& contact : contacts) {
            if (contact.actorId == state.actorId && !contact.selfContact) {
                frameSpeed = std::max(frameSpeed, contact.impactSpeed);
            }
        }
        const bool touched = frameSpeed > 0.0f;
        if (touched) {
            actor.lastTouchedMs = nowMs;
        }

        // Has the floor stopped arguing about it. The Airborne edge drops to Tumble
        // on `touched` and the Tumble edge returns on `airborne`, a latch on the
        // acceleration that survives the contact - so without this the two take
        // turns one per tick for as long as a body bounces along a floor. A contact
        // does not merely move the state once, it holds the body out of flight
        // until the contacts stop.
        //
        // Asked of the same `touched` the other edge fires on, and stamped above
        // rather than read off `lastAdmittedMs` - that one counts self-hits, and a
        // corpse elbowing itself is not the floor.
        const bool clearOfContacts =
            cfg.motion.landedHoldMs <= 0.0f || Ancient(actor.lastTouchedMs) ||
            (nowMs - actor.lastTouchedMs) >
                Window(cfg.motion.landedHoldMs, frameMs, cfg.arb.frameScaleK);

        // Sliding is sustained tangential motion, asked the same way from Tumble
        // and from Slide. Two ways in, because a slide has two shapes: a slow grind
        // earns it by lasting, a fast skid by covering ground - devbench_3 crosses
        // two metres of floor in less time than the duration gate wants.
        //
        // The grace window keeps it alive between contacts: a couple of quiet
        // frames mid-grind is a solver artefact, but a stretch with no grazing at
        // all in it is not a slide any more.
        const bool grazing =
            !Ancient(actor.grazeLastMs) &&
            (nowMs - actor.grazeLastMs) <=
                Window(cfg.motion.slideGraceMs, frameMs, cfg.arb.frameScaleK);
        const bool longEnough =
            !Ancient(actor.grazeSinceMs) &&
            (nowMs - actor.grazeSinceMs) >=
                Window(cfg.motion.slideMinDurationMs, frameMs, cfg.arb.frameScaleK);
        const bool farEnough =
            !Ancient(actor.grazeSinceMs) && actor.slideDistance >= cfg.motion.slideMinDistance;
        // What *opens* a slide. Speed and either duration or distance, so a
        // single glancing blow is not one and a slow shuffle is not one either.
        const bool slideOpens = grazing &&
                                actor.slideTangent >= cfg.motion.slideMinTangentSpeed &&
                                (longEnough || farEnough);

        // What *keeps* one open is a different question: re-applying the entry
        // speed every tick ends slides on the tangent hold's decay curve rather
        // than on anything the body did - a slide would open at 200 u/s and close
        // seventy milliseconds later at 119, with the body still grinding.
        //
        // The two ways out are going airborne and the graze stream drying up, so
        // what is asked here is whether the body is still grazing - or, where the
        // take carries pose, whether it is still *travelling*.
        //
        // That second clause is a different measurement, not the entry gate asked
        // twice: the contacts are right for opening a slide and wrong for holding
        // one, since a long low grind loses its graze stream while the body is
        // still moving.
        //
        // `bodySpeed` is the body itself, so it cannot decay out from under a
        // slide the way a contact hold does. Gated on pose for that reason, and
        // bounded by `slideHoldMaxMs` so a drifting pose stream cannot hold a
        // grind open indefinitely. Off by default.
        const bool speedHolds =
            cfg.motion.slideHoldSpeed > 0.0f && state.haveBodySamples &&
            state.bodySpeed >= cfg.motion.slideHoldSpeed && !Ancient(actor.grazeLastMs) &&
            (nowMs - actor.grazeLastMs) <=
                Window(cfg.motion.slideHoldMaxMs, frameMs, cfg.arb.frameScaleK);
        const bool slideHolds = grazing || speedHolds;

        switch (state.motion) {
            case Motion::kLaunch:
                if (touched) {
                    EnterMotion(actor, Motion::kTumble, nowMs);
                } else if (state.airborne) {
                    EnterMotion(actor, Motion::kAirborne, nowMs);
                }
                break;
            case Motion::kAirborne:
                // A landing is a contact. A body that merely stopped falling -
                // caught on a slope, or dropped into water - goes back to tumbling
                // rather than staying in a flight it is no longer in.
                if (touched || !state.airborne) {
                    EnterMotion(actor, Motion::kTumble, nowMs);
                }
                break;
            case Motion::kTumble:
            case Motion::kSlide:
                // Two ways out of a slide. Going airborne is *measured*; the other
                // is the graze stream drying up, which is a statement about the
                // contact data and nothing more.
                if (state.airborne && state.haveBodySamples && clearOfContacts) {
                    // The edge the old machine did not have: `airborne` was
                    // consulted only in Launch, so a body that left the ground
                    // halfway down a staircase was never airborne twice.
                    //
                    // `clearOfContacts` is the hysteresis it needed. `airborne`
                    // alone cannot *re-enter* flight, because a bouncing body keeps
                    // the latch topped up while the floor is still hitting it - see
                    // `fLandedHoldMs` and the 300 ms of tick-rate alternation in
                    // Eldawyn_devbench_1.
                    //
                    // Gated on pose: without it `airborne` is inferred from the gap
                    // since the last contact, so a corpse lying still would be
                    // flown for the rest of the take.
                    LeaveSlide(actor, nowMs, SlideExit::kLaunched);
                    EnterMotion(actor, Motion::kAirborne, nowMs);
                } else if (state.motion == Motion::kTumble && slideOpens) {
                    EnterMotion(actor, Motion::kSlide, nowMs);
                } else if (state.motion == Motion::kSlide && !slideHolds) {
                    LeaveSlide(actor, nowMs, SlideExit::kEnded);
                    EnterMotion(actor, Motion::kTumble, nowMs);
                }
                break;
            case Motion::kCount:
                break;
        }

        if (state.motion != previous) {
            spdlog::debug("actor {} motion {} -> {} at {:.0f} ms", actor.name, ToString(previous),
                          ToString(state.motion), nowMs);
        }
    }

    /// Close the slide, on whichever edge is leaving it. One function for both
    /// because the bookkeeping is the same and only the consequence differs: the
    /// exit chooses the loop's fade. Left on the crash state rather than being a
    /// transient - the strategy reads it on the tick it stops the loop, and the
    /// timeline reads it to mark the end of a span it has drawn.
    void LeaveSlide(ActorRuntime& actor, TimeMs nowMs, SlideExit why) {
        if (actor.state.motion != Motion::kSlide) {
            return;
        }
        actor.state.slideExit = why;
        // `slideSpeed` is a peak over the whole slide, a diagnostic only.
        spdlog::debug(
            "actor {} slide ended {} at {:.0f} ms after {:.0f} units, peak {:.0f} u/s",
            actor.name, ToString(why), nowMs, actor.slideDistance, actor.slideSpeed);
        actor.grazeSinceMs = kLongAgo;
        actor.grazeLastMs = kLongAgo;
        actor.slideDistance = 0.0f;
        actor.slideTangent = 0.0f;
        actor.slideSpeed = 0.0f;
    }

    /// A hero floor from a fraction of the loud anchor. One function so the
    /// ordinary floor and the head's relieved one cannot disagree about what a
    /// fraction means, and so the clamp at zero is written once.
    [[nodiscard]] float HeroFloor(float frac, ActorMode mode) const {
        return std::max(0.0f, frac * For(mode).intensity.speedRefHigh);
    }

    /// How much of the loud anchor `[HeadImpact]`'s relief takes off that floor.
    /// Negative is refused rather than passed through: a "relief" that raised the
    /// gate is the one thing the name promises it cannot do.
    [[nodiscard]] float HeadRelief(ActorMode mode) const {
        return std::max(0.0f, For(mode).strategies.head.heroFloorReliefFrac);
    }

    /// Whether a contact earns that relief: a head, hard enough in its own right,
    /// with both the accent and the relief switched on.
    ///
    /// A plain threshold, deliberately *not* `ClassifyHead`'s gate - that one folds
    /// in head-down attitude, air time and company, so reusing it would move this
    /// rule whenever any of the three were tuned. This asks something simpler: was
    /// the skull the thing that hit.
    [[nodiscard]] bool HeadFloorRelieved(const Contact& contact, ActorMode mode) const {
        const AlgorithmConfig& c = For(mode);
        const HeadImpactConfig& headCfg = c.strategies.head;
        return headCfg.enabled && headCfg.heroFloorRelief && contact.site == LimbSite::kHead &&
               HeadRelief(mode) > 0.0f &&
               contact.impactSpeed >= headCfg.heroFloorReliefAtFrac * c.intensity.speedRefHigh;
    }

    /// Stage 2, axis two. What the mix is doing.
    ///
    /// Latched and windowed rather than a running description, and measured on raw
    /// `impactSpeed` throughout. The test it replaces read intensity against a
    /// running energy total: the total only ever grew, so past 1/0.35 it could
    /// never fire again, and intensity clamps at 1.0, so above `speedRefHigh` it
    /// was blind at the top of its own range.
    void AdvanceMoment(ActorRuntime& actor, TimeMs nowMs, float frameMs) {
        CrashState& state = actor.state;
        const HeroConfig& hero = cfg.hero;
        if (!hero.enabled) {
            state.moment = Moment::kOrdinary;
            return;
        }

        // Expire first, so a re-anchor decides about a window still open rather
        // than one that had already closed.
        if (state.moment == Moment::kHero &&
            (nowMs - state.heroSinceMs) > Window(hero.windowMs, frameMs, cfg.arb.frameScaleK)) {
            state.moment = Moment::kOrdinary;
            state.heroPeakSpeed = 0.0f;
            state.heroSeq = 0;
            spdlog::debug("actor {} hero window closed at {:.0f} ms", actor.name, nowMs);
        }

        const float baseFloor = HeroFloor(hero.floorFrac, actor.mode);

        for (const Contact& contact : contacts) {
            if (contact.actorId != state.actorId || contact.selfContact) {
                continue;
            }

            // Per contact rather than per tick, because `[HeadImpact]`'s relief
            // makes the floor a function of which limb arrived.
            const bool relieved = HeadFloorRelieved(contact, actor.mode);
            const float floor =
                relieved ? HeroFloor(hero.floorFrac - HeadRelief(actor.mode), actor.mode)
                         : baseFloor;
            if (contact.impactSpeed < floor) {
                continue;
            }

            // The envelope *before* this tick's contacts were folded in: a contact
            // cannot be 1.3x itself, so reading the live value would make the
            // dominance clause dead.
            //
            // Floored at the hero floor, which is doing real work. The envelope
            // decays with a 300 ms constant, so a few hundred milliseconds of quiet
            // take it to nothing - and against nothing everything is dominant.
            // Unfloored, this clause fired on every contact over the floor that
            // followed a gap: twenty-one hero moments in one long take, and a burst
            // budget that reset so often it stopped being a budget.
            //
            // It is also the second half of what the head's relief buys: a relieved
            // contact is measured against a lower clamp as well as a lower gate, so
            // it can be dominant over a quiet envelope. Judging it against the
            // unrelieved clamp would move the number without moving the outcome.
            const float recent = std::max(floor, actor.energyRecentBeforeTick);

            // Dominance: it stands out from the last few hundred milliseconds.
            const bool dominant = contact.impactSpeed >= hero.dominanceRatio * recent;

            // Arrival: it landed out of a real, measured flight. Unavailable
            // without pose - the old inference scores a take's first contact at the
            // top of its ramp, which is what spent the hero moment on a 44.7 u/s
            // scuff at 696 ms.
            //
            // Both halves off one `FlightFor`, so they are two questions about the
            // same flight: reading the contact's air time and the live state's drop
            // let them come from different ones.
            bool arrived = false;
            if (state.haveBodySamples) {
                StrategyContext ctx{For(actor), actor,        rng,   nowMs,
                                    frameMs,      &nextVoiceId, &stats, bank};
                const FlightMeasure flight = FlightFor(ctx, contact, false, 0.0f);
                arrived = flight.airMs >= hero.arrivalMinAirMs &&
                          flight.dropUnits >= hero.arrivalMinDropUnits;
            }

            if (!dominant && !arrived) {
                continue;
            }

            // Did the relief actually change the answer? Both halves can, so both
            // are re-asked at the unrelieved floor. Counted rather than inferred:
            // from the hero count alone, a relief firing on contacts that would
            // have anchored anyway looks like one doing real work.
            bool creditRelief = false;
            if (relieved) {
                const bool baseDominant =
                    contact.impactSpeed >=
                    hero.dominanceRatio * std::max(baseFloor, actor.energyRecentBeforeTick);
                creditRelief =
                    !(contact.impactSpeed >= baseFloor && (baseDominant || arrived));
            }

            if (state.moment == Moment::kHero) {
                // Re-anchor rather than open a second moment: a landing is five
                // limbs arriving over a couple of hundred milliseconds and should
                // read as one event with peers. What moves is which of them the
                // moment is *about*.
                if (contact.impactSpeed >= hero.reanchorRatio * state.heroPeakSpeed) {
                    AnchorHero(actor, contact, nowMs, true, creditRelief);
                }
                continue;
            }
            if (hero.maxPerEvent > 0 &&
                state.heroCount >= static_cast<std::uint32_t>(hero.maxPerEvent)) {
                continue;
            }
            AnchorHero(actor, contact, nowMs, false, creditRelief);
        }
    }

    /// Open a hero moment on this contact, or move an open one onto it. Both
    /// restart the window, mark the burst for a reset and move the collapse point,
    /// which is why they are one function. The collapse point belongs on the
    /// moment's own peak rather than its first grain, so a landing whose 608 u/s
    /// slam arrives 132 ms after the 294 u/s head collapses onto the slam.
    void AnchorHero(ActorRuntime& actor, const Contact& contact, TimeMs nowMs, bool reanchor,
                    bool headRelief) {
        CrashState& state = actor.state;
        state.moment = Moment::kHero;
        state.heroSinceMs = nowMs;
        state.heroPeakSpeed = contact.impactSpeed;
        state.heroSeq = contact.sourceSeq;
        if (reanchor) {
            ++stats.heroReanchors;
            ++actor.stats.heroReanchors;
        } else {
            ++state.heroCount;
            ++stats.heroes;
            ++actor.stats.heroes;
        }
        if (headRelief) {
            ++stats.heroHeadRelief;
            ++actor.stats.heroHeadRelief;
        }
        // Consumed by the arbitrator this tick. It absorbs the old air-time budget
        // reset: what a long fall most often lost to was a burst some scuff opened
        // and filled while the body was still in the air.
        actor.heroResetsBurst = cfg.hero.resetsBurst;
        actor.collapseUntilMs = nowMs + cfg.arb.spatialCollapseWindowMs;
        actor.collapsePoint = contact.position;
        spdlog::debug("actor {} hero {} on seq {} at {:.0f} ms, {:.0f} u/s{}", actor.name,
                      reanchor ? "re-anchored" : "opened", contact.sourceSeq, nowMs,
                      contact.impactSpeed, headRelief ? " (head floor relief)" : "");
    }

    void EnterMotion(ActorRuntime& actor, Motion motion, TimeMs nowMs) {
        actor.state.motion = motion;
        actor.state.motionEnteredMs = nowMs;
        if (motion == Motion::kSlide) {
            // Cleared as the span opens, never as it closes: the strategy reads it
            // on the tick after the one that set it, and the timeline for as long
            // as the span it marks is on screen.
            actor.state.slideExit = SlideExit::kNone;
            ++stats.slides;
            ++actor.stats.slides;
        }
        if (motion == Motion::kLaunch) {
            actor.energyRecent = 0.0f;
            actor.energyRecentBeforeTick = 0.0f;
            // All six damage ledgers at once. A body being launched again is the
            // only event boundary the engine has.
            actor.damage = {};
            // The accumulated ladder's per-knockdown budget goes back with them,
            // and only the budget - the pools keep healing on their own clock. A
            // body relaunched mid-beating has not stopped being beaten, and wiping
            // what it took would let the ladder be climbed from the bottom again
            // by anything that re-arms a knockdown.
            for (auto& track : actor.accum) {
                track.fired = 0;
            }
            actor.accumFired = 0;
            actor.state.energyAccum = 0.0f;
            actor.state.heroCount = 0;
            actor.state.moment = Moment::kOrdinary;
            actor.state.heroPeakSpeed = 0.0f;
            actor.state.heroSeq = 0;
            actor.burstStartMs = kLongAgo;
            actor.burstLastMs = kLongAgo;
            actor.lastOnsetMs = kLongAgo;
            actor.lastOnsetDb = kSilentDb;
        }
    }

    // ── Stage 3 driver ───────────────────────────────────────────────────────

    /// The body half of the air-time rule: lift a contact that is not the head in
    /// proportion to how long the actor had been off the ground. A Shape-stage
    /// rule, so the carried onset gain, capped lift and accumulated trim all live
    /// in `Shape` and only the measurement is here. Returns the ramp it applied.
    float ApplyBodyAirTime(const StrategyContext& ctx, Contact& contact) {
        const AirTimeConfig& airCfg = ctx.cfg.strategies.airTime;
        if (!airCfg.bodyEnabled || contact.site == LimbSite::kHead) {
            return 0.0f;
        }
        const float clear = std::max(1.0f, airCfg.bodyClearMs);
        const float ramp =
            std::clamp(AirTimeMs(ctx, contact, false, 0.0f) / clear, 0.0f, 1.0f);
        if (ramp <= 0.0f) {
            return 0.0f;
        }

        Shape(contact, airCfg.bodyLift, ramp, ctx.cfg.intensity.dynamicRangeDb);
        return ramp;
    }

    void RunStrategies(TimeMs nowMs, float frameMs) {
        proposals.clear();

        for (Contact& contact : contacts) {
            ActorRuntime* actor = Find(contact.actorId);
            if (actor == nullptr || actor->state.tier == DistanceTier::kCulled) {
                continue;
            }
            // Before the tier check, so a landing this rule lifts can clear it.
            {
                StrategyContext airCtx{For(*actor), *actor, rng, nowMs, frameMs, &nextVoiceId};
                const float before = contact.intensity;
                if (const float ramp = ApplyBodyAirTime(airCtx, contact); ramp > 0.0f) {
                    TraceLine(contact.timeMs, contact.actorId, contact.limbIndex,
                              contact.sourceSeq, contact.impactSpeed, contact.intensity,
                              actor->state,
                              std::format("body air {:.2f} int {:+.2f}", ramp,
                                          contact.intensity - before));
                }
            }
            // Simplified tier: hero composites only. Nobody resolves grains, loops
            // or the bed at that range.
            if (actor->state.tier == DistanceTier::kSimplified && contact.intensity < 0.4f) {
                continue;
            }

            // One stream per contact rather than one per take: a shared stream makes
            // the scatter a function of how many cues came before, so an early
            // config change re-rolls the pitch of everything after it. Seeded from
            // the contact's own row, so `iRngSeed` still re-rolls the whole take.
            Rng contactRng;
            contactRng.Seed(StableSeed(cfg.slots.rngSeed, contact.sourceSeq));
            StrategyContext ctx{For(*actor), *actor, cfg.slots.stableVariants ? contactRng : rng,
                                nowMs,          frameMs, &nextVoiceId,
                                &stats,         bank};
            // Head contacts get a trace line whatever becomes of them: every head
            // rule judges geometry the cue list cannot show, and without this an
            // air time of zero and one that was never enabled look identical.
            if (tracing && contact.site == LimbSite::kHead) {
                const HeadStrike strike = ClassifyHead(ctx, contact);
                TraceLine(contact.timeMs, contact.actorId, contact.limbIndex, contact.sourceSeq,
                          contact.impactSpeed, contact.intensity, actor->state,
                          std::format("head air {:.0f}ms {:.2f} {:+.1f}dB gate {:.0f}{}{}",
                                      strike.airMs, strike.air, strike.airDb, strike.gate,
                                      strike.airFull ? " LED" : "", strike.crowded ? " CROWD" : ""));
            }
            for (IStrategy* strategy : strategies) {
                if (strategy->Propose(ctx, contact, proposals)) {
                    contact.claimed = true;
                    TraceLine(contact.timeMs, contact.actorId, contact.limbIndex,
                              contact.sourceSeq, contact.impactSpeed, contact.intensity,
                              actor->state, strategy->Name());
                    break;
                }
            }
        }

        // Every tier except culled. The full-detail-only strategies check the tier
        // themselves, and the closing cue is neither grain nor loop nor bed - so
        // gating the whole tick on kFull would leave a knockdown at fifteen metres
        // with no ending at all.
        for (auto& actor : actors) {
            if (!actor.inUse || actor.state.tier == DistanceTier::kCulled) {
                continue;
            }
            StrategyContext ctx{For(actor), actor,        rng,   nowMs,
                                frameMs,      &nextVoiceId, &stats, bank};
            for (IStrategy* strategy : strategies) {
                strategy->ProposeTick(ctx, proposals);
            }
        }
        stats.proposedCues += static_cast<std::uint32_t>(proposals.size());
    }

    // ── Stage 4: arbitration. Fixed rules, in order ──────────────────────────

    void Arbitrate(TimeMs nowMs, float frameMs) {
        order.clear();
        acceptedSeqs.clear();
        for (std::size_t i = 0; i < proposals.size(); ++i) {
            order.push_back(static_cast<std::uint16_t>(i));
            // Assembled here and nowhere else, which is what keeps the split
            // honest: a strategy able to write a priority is a strategy able to
            // make its own cue outrank the frame.
            //
            // A bypass keeps priority at its level - it is not an onset, so nothing
            // sorts it against anything.
            Proposal& proposal = proposals[i];
            proposal.priorityDb =
                proposal.bypass ? proposal.levelDb
                                : proposal.levelDb + SiteWeightDb(proposal.site);
        }
        // Most important first, ties broken by source row - loudest-first until a
        // weight is set, since priority is the level plus zero.
        std::ranges::stable_sort(order, [&](std::uint16_t a, std::uint16_t b) {
            if (proposals[a].priorityDb != proposals[b].priorityDb) {
                return proposals[a].priorityDb > proposals[b].priorityDb;
            }
            return proposals[a].sourceSeq < proposals[b].sourceSeq;
        });

        const float rateCap = Window(cfg.arb.rateCapMs, frameMs, cfg.arb.frameScaleK);
        const float chainWindow = Window(cfg.arb.chainMergeWindowMs, frameMs, cfg.arb.frameScaleK);
        const float burstWindow = Window(cfg.arb.burstWindowMs, frameMs, cfg.arb.frameScaleK);
        // The moment axis's own window, when it has one. Beside the ordinary one so
        // the two are visibly the same quantity; which a proposal is judged against
        // is decided per proposal below, since the latch opens and closes inside a
        // frame.
        const float heroBurstWindow =
            cfg.hero.burstWindowMs > 0.0f
                ? Window(cfg.hero.burstWindowMs, frameMs, cfg.arb.frameScaleK)
                : burstWindow;
        const float burstGap = Window(cfg.arb.burstMinGapMs, frameMs, cfg.arb.frameScaleK);

        // Pass one: the onsets and the bypasses. Ride-alongs wait, because an
        // accessory cannot be judged before the thing it is an accessory to.
        for (const std::uint16_t index : order) {
            Proposal& proposal = proposals[index];
            ActorRuntime* actor = Find(proposal.actorId);
            if (actor == nullptr || proposal.rideAlong) {
                continue;
            }
            if (proposal.bypass) {
                Emit(*actor, proposal, nowMs);
                continue;
            }

            // 1. Global rate cap. No two impact onsets closer than ~46 ms,
            //    regardless of limb: below that extra onsets add mud, not detail.
            // Absolute, because proposals are arbitrated loudest-first rather than
            // in time order, so a quieter contact can be judged against an onset
            // that happens *after* it.
            const auto sinceOnset =
                std::fabs(static_cast<float>(proposal.timeMs - actor->lastOnsetMs));
            const float cap = rateCap * proposal.rateCapScale;
            if (sinceOnset < cap) {
                // Unless it is properly louder: the cap only holds between
                // comparable levels. log_4's body slams down 42 ms after its foot
                // lands and 7 dB louder, and was lost.
                //
                // Both sides weighted, which is what carries the split *across*
                // ticks. A torso landing 20 ms after a hand used to need 6 dB of
                // real level to open its own onset; with 3 dB of torso weight it
                // needs 3, and a hand after a torso needs 9. 99.5% of the corpus's
                // clusters are one tick and the rest are this.
                const float overDb = proposal.priorityDb - actor->lastOnsetDb;
                if (overDb >= cfg.arb.rateCapOverrideDb) {
                    ++stats.rateCapOverrides;
                    ++actor->stats.rateCapOverrides;
                    spdlog::debug("seq {} at {:.0f} ms is {:.1f} dB over the onset holding the "
                                  "rate cap ({:.0f} ms gap); letting it open its own",
                                  proposal.sourceSeq, proposal.timeMs, overDb, sinceOnset);
                } else {
                    ++stats.droppedRateCap;
                    ++actor->stats.droppedRateCap;
                    char why[32]{};
                    std::snprintf(why, sizeof(why), "rate cap %.0f<%.0f", sinceOnset, cap);
                    Dropped(proposal, actor->state, why);
                    continue;
                }
            } else if (sinceOnset < rateCap) {
                // Inside the design's cap but outside the scaled one a moment asked
                // for, so it was let through on purpose. Counted as an override
                // because the verifier measures gaps against the nominal cap and
                // cannot re-derive which were granted.
                ++stats.rateCapOverrides;
                ++actor->stats.rateCapOverrides;
            }

            // 2. Chain merge. A strong hand impact silences the elbow and plays
            //    one arm flop.
            const auto chain = static_cast<std::size_t>(proposal.chain);
            if (proposal.chain != LimbChain::kNone && chain < kChainCount &&
                proposal.timeMs - actor->chainLastMs[chain] < chainWindow &&
                proposal.priorityDb <= actor->chainLastDb[chain]) {
                ++stats.droppedChainMerge;
                ++actor->stats.droppedChainMerge;
                Dropped(proposal, actor->state, "chain merge");
                continue;
            }

            // 3. Temporal masking. Anything more than ~12 dB under the decaying
            //    ceiling is dropped entirely, not played quietly - which is what
            //    turns a dozen simultaneous contacts into one event with texture.
            if (proposal.levelDb < actor->state.maskCeilingDb - cfg.arb.maskDropBelowDb) {
                ++stats.droppedMasking;
                ++actor->stats.droppedMasking;
                Dropped(proposal, actor->state, "masked");
                continue;
            }

            // 4. Burst shaping. The arbitrator picks bursts, not individual sounds:
            //    three to five grains inside 200-400 ms, then real silence. This is
            //    what turns a three-second tumble into four audible events.
            //
            // A waiver gives back part of the silence between bursts; a reset goes
            // further and closes the open burst outright, which is the only way
            // past one a scuff opened while the body was still in the air.
            const int grainCap = std::max(
                1, std::min(cfg.arb.burstMaxGrains, BudgetFor(actor->state).maxCuesPerBurst));
            // Which of the landing's limbs gets the reset is settled here rather
            // than in the strategy, because here the proposals are loudest-first.
            // A landing is five limbs carrying the same hero moment, and letting
            // each act on it turns one burst into five. One reset per moment; the
            // rest meet the ordinary budgets.
            const bool resetsBurst = proposal.resetsBurst && actor->heroResetsBurst;
            // Asked of the moment this proposal is arriving into, not the one the
            // burst opened in: a burst a moment opens is a landing and wants the
            // tight window, and the same burst once the latch closes is an ordinary
            // stretch wanting the wide one. So `Hero:fBurstWindowMs` means how long
            // a burst stays open *while a moment is running*.
            const float window =
                actor->state.moment == Moment::kHero ? heroBurstWindow : burstWindow;
            const bool burstOpen = proposal.timeMs - actor->burstStartMs < window && !resetsBurst;
            const bool opensBurst = !burstOpen;
            if (burstOpen) {
                if (actor->burstGrains >= grainCap) {
                    ++stats.droppedBurstCap;
                    ++actor->stats.droppedBurstCap;
                    Dropped(proposal, actor->state, "burst full");
                    continue;
                }
            } else if (const float gapScale = proposal.burstGapScale;
                       !Ancient(actor->burstStartMs) &&
                       proposal.timeMs - actor->burstLastMs <
                           burstGap * std::clamp(gapScale, 0.0f, 1.0f)) {
                ++stats.droppedBurstCap;
                ++actor->stats.droppedBurstCap;
                Dropped(proposal, actor->state, "burst gap");
                continue;
            }

            // Everything below is provisional until the proposal actually makes a
            // sound: a stack whose every layer fell under the voice floor is not an
            // audible moment and must not spend the burst budget, raise the masking
            // ceiling or count towards the reduction ratio.
            //
            // Only the fields the block below touches - Tick must not allocate, and
            // copying the whole runtime would copy the actor's name.
            const struct {
                TimeMs burstStartMs;
                TimeMs burstLastMs;
                TimeMs lastOnsetMs;
                float lastOnsetDb;
                int burstGrains;
                TimeMs chainMs;
                float chainDb;
                float maskCeilingDb;
                std::uint32_t admittedCount;
                bool heroResetsBurst;
            } before{actor->burstStartMs,
                     actor->burstLastMs,
                     actor->lastOnsetMs,
                     actor->lastOnsetDb,
                     actor->burstGrains,
                     chain < kChainCount ? actor->chainLastMs[chain] : 0.0,
                     chain < kChainCount ? actor->chainLastDb[chain] : 0.0f,
                     actor->state.maskCeilingDb,
                     actor->state.admittedCount,
                     actor->heroResetsBurst};

            if (opensBurst) {
                actor->burstStartMs = proposal.timeMs;
                actor->burstGrains = 0;
            }
            ++actor->burstGrains;
            actor->burstLastMs = proposal.timeMs;
            actor->lastOnsetMs = proposal.timeMs;
            // Both are read back by a *comparison*, so both store the weighted
            // figure: a ledger mixing the two scales would let a weight apply in one
            // direction only, and a torso would outrank a hand while the hand
            // outranked it back.
            actor->lastOnsetDb = proposal.priorityDb;
            if (chain < kChainCount) {
                actor->chainLastMs[chain] = proposal.timeMs;
                actor->chainLastDb[chain] = proposal.priorityDb;
            }
            // The mask ceiling stays on the level: masking asks whether this can be
            // heard under what is already sounding, which is a fact about air, so a
            // weight nobody can hear must not raise the next contact's bar.
            actor->state.maskCeilingDb = std::max(actor->state.maskCeilingDb, proposal.levelDb);
            ++actor->state.admittedCount;
            // Spent here rather than where it was granted: a reset that buys a
            // proposal past the budgets and then loses every layer to the voice
            // floor has bought nothing, and the next proposal must still get it.
            if (resetsBurst) {
                actor->heroResetsBurst = false;
            }
            // Nothing is charged here any more: the two per-knockdown counters
            // existed because a waiver was earned per contact, and the moment axis
            // decides once on the actor.

            if (Emit(*actor, proposal, nowMs) == 0) {
                actor->burstStartMs = before.burstStartMs;
                actor->burstLastMs = before.burstLastMs;
                actor->lastOnsetMs = before.lastOnsetMs;
                actor->lastOnsetDb = before.lastOnsetDb;
                actor->burstGrains = before.burstGrains;
                if (chain < kChainCount) {
                    actor->chainLastMs[chain] = before.chainMs;
                    actor->chainLastDb[chain] = before.chainDb;
                }
                actor->state.maskCeilingDb = before.maskCeilingDb;
                actor->state.admittedCount = before.admittedCount;
                actor->heroResetsBurst = before.heroResetsBurst;
                Dropped(proposal, actor->state, "silent");
                continue;
            }
            if (opensBurst) {
                ++stats.bursts;
                ++actor->stats.bursts;
            }
            acceptedSeqs.push_back(proposal.sourceSeq);
        }

        // Pass two: the accessories, which live or die with their parent - a crunch
        // with no impact under it is not a sound anybody can place.
        for (const std::uint16_t index : order) {
            Proposal& proposal = proposals[index];
            if (!proposal.rideAlong) {
                continue;
            }
            ActorRuntime* actor = Find(proposal.actorId);
            if (actor == nullptr ||
                std::ranges::find(acceptedSeqs, proposal.sourceSeq) == acceptedSeqs.end()) {
                continue;
            }
            Emit(*actor, proposal, nowMs);
        }
    }

    void Dropped(const Proposal& proposal, const CrashState& state, std::string_view why) {
        // The speed matters most on a drop line: "rate cap" against a 40 u/s brush
        // is the rule working, against a 440 u/s slam it is a bug.
        TraceLine(proposal.timeMs, proposal.actorId, proposal.limbIndex, proposal.sourceSeq,
                  proposal.impactSpeed, proposal.intensity, state, why);
        spdlog::debug("drop seq {} at {:.0f} ms: {} ({:.1f} dB)", proposal.sourceSeq,
                      proposal.timeMs, why, proposal.levelDb);
    }

    // ── Stage 5: hand the cue list over ──────────────────────────────────────

    /// Whether this slot is audible under the current layer mutes.
    ///
    /// Which switch governs which slot is `LayerMute` in Config.h, shared with the
    /// testbench's slot panel so the two cannot disagree. What is left here is the
    /// one thing that is not a per-slot switch: the surface section's master.
    static bool LayerAudible(const AlgorithmConfig& cfg, SlotId slot) {
        const bool* mute = LayerMute(cfg, slot);
        if (mute != nullptr && !*mute) {
            return false;
        }
        if (slot == SlotId::kSurfWood || slot == SlotId::kSurfStone ||
            slot == SlotId::kSurfSoft) {
            return cfg.surfaces.enabled;
        }
        return true;
    }

    std::uint32_t Emit(ActorRuntime& actor, const Proposal& proposal, TimeMs nowMs) {
        if (sink == nullptr) {
            return 0;
        }
        std::uint32_t emitted = 0;
        ExpireVoices(nowMs);

        // The motion budget for a bed, the moment axis's for everything else.
        //
        // A hero moment is a statement about one *contact*, and a grind is not a
        // contact: it was already running before the hit and goes on after, so
        // re-levelling it for the two hundred milliseconds a moment is open is a
        // swell in the bed with nothing in the world behind it. The slide is as
        // loud as the body is fast (Slots.md §2), and a hero hit does not make the
        // body faster.
        //
        // The same argument `postShapeDb` makes below. `maxCuesPerBurst` is
        // untouched - a loop never took a slot in the burst.
        const PhaseBudget& heroOrMotion = BudgetFor(actor.state);
        const PhaseBudget& bedBudget = MotionBudgetFor(actor.state);
        const PhaseBudget& budget = proposal.bypass ? bedBudget : heroOrMotion;
        // The trim only; `maxCuesPerBurst` comes off `budget` above and is never
        // smoothed. A bed is one voice held open across a state change, so it takes
        // the glided figure from `AdvanceBedTrim`; a contact-derived cue takes the
        // trim of the state it happened in. See `fBedTrimGlideMs`.
        const float budgetTrimDb = proposal.bypass ? actor.bedTrimDb : budget.gainTrimDb;
        const float masterDb =
            cfg.mix.masterGainDb + (actor.isPlayer ? cfg.player.masterGainDb : 0.0f);

        // No distance rolloff here, deliberately. Skyrim attenuates a positioned
        // voice itself through the BGSSoundOutput model, so doing it here too
        // attenuated everything twice: a contact 6.4 m away came out 8 dB down
        // before the engine had seen it, which is most of why the mix needed a
        // double-figure master gain.
        //
        // The game owns falloff and the renderer's job is to attach an output
        // model. The testbench owns none of it either, so what is tuned there is
        // the un-attenuated cue and both sides get the same curve from the same
        // engine.
        //
        // Distance still decides the *tier* - a budget decision, not a gain one.

        // One-shots only. The collapse exists because several points inside one
        // acoustic moment read as several events; a loop is a texture that lasts,
        // and pinning one where a hero was anchored is how a grind ends up
        // somewhere the body no longer is.
        const bool collapse = proposal.op == CueOp::kPlayOneShot &&
                              cfg.arb.spatialCollapseOnHero && proposal.boneIndex < 0 &&
                              nowMs < actor.collapseUntilMs;

        // Contact-derived cues only: a bypass proposal carries no intensity of its
        // own, so shaping it by one re-levels the bed against a zero it never had.
        const float postShapeDb =
            proposal.bypass ? 0.0f : PostShapeDb(proposal.intensity, actor.mode);

        // The band compressor is taken per layer, further down, against
        // `Layer::gainDb` - the same pre-trim scale as `levelDb`, where 0 dB is the
        // hardest contact the engine can hear. So every trim below applies on top
        // of the compressed value rather than being compressed with it, and the
        // setting survives a change to the master gain.
        //
        // Squeezed rather than clamped: a hard cap puts everything above it on one
        // level, so a threshold a few dB low turns a dozen distinct impacts into a
        // dozen identical ones.
        //
        // One cut per *layer*, not per proposal. The four impact layers are a split
        // by frequency, so one cut over all four could only hold the transient in
        // order to pay for the body's peak - reshaping the stack as level rises is
        // what a multiband is for. Every other proposal is single-layer, so this is
        // a no-op for them; it also fixes a Damage proposal handing its gore layer
        // the *crunch's* threshold because layer 0 decided for the stack.

        // One booking for the whole proposal, taken before any layer is built.
        //
        // A one-shot's layers all carry the same (actorId, sourceSeq), the key the
        // renderer mixes on, so the stack becomes one buffer and one BSSoundHandle.
        // Charging per layer charged four times for one voice and let the budget
        // run out *inside* a stack - not a quieter impact but a differently-built
        // one. `endsMs` is the longest layer's: its offset plus its own length.
        //
        // Loops book their own by id further down, and end when they are stopped
        // rather than on a clock.
        bool booked = false;
        if (proposal.op == CueOp::kPlayOneShot) {
            TimeMs endsMs = proposal.timeMs;
            for (int i = 0; i < proposal.layerCount; ++i) {
                endsMs = std::max(endsMs, proposal.timeMs + proposal.layers[i].offsetMs +
                                              Slot(proposal.layers[i].slot).maxLengthMs);
            }
            TakeVoice(actor, endsMs, 0);
            booked = true;
        }

        for (int slotIndex = 0; slotIndex < proposal.layerCount; ++slotIndex) {
            const Layer& layer = proposal.layers[slotIndex];

            // Off `layer.slot`, not `resolved.slot`, and before the bank is asked:
            // `imp_body_limb` plays `imp_body`'s recording until somebody records a
            // limb one, and reading the resolution would leave `fBodyLimbDb` inert
            // until then. What is held is the limb layer, whatever stands in.
            const rds::CompressBand band = rds::CompressBandFor(layer.slot, layer.reason);
            const float compressCutDb = rds::CompressCutDb(
                cfg.compress, rds::CompressThresholdDb(cfg.compress, band), layer.gainDb);

            ResolvedSound resolved{};
            if (proposal.op != CueOp::kStopLoop) {
                // A loop has no `sourceSeq`, and a token of 0 turns the stable
                // picker off, so every update cue of one grind drew a fresh variant.
                // Invisible while a variant only named a file - the renderer never
                // re-attaches the sound on an update - but the moment a variant
                // carries the library's pitch and trim, the *correction* flaps
                // between two files' answers on a voice playing neither. The voice
                // id is the stable identity a loop does have.
                const std::uint32_t token =
                    proposal.sourceSeq != 0 ? proposal.sourceSeq : proposal.voiceId;
                if (bank == nullptr ||
                    !bank->Resolve(layer.slot, proposal.surface, proposal.coverage, proposal.site,
                                   resolved, token)) {
                    // A declared-and-unfilled slot, or no bank. Skipped silently,
                    // which is what makes adding voice later a config change.
                    continue;
                }
            }

            Cue cue{};
            cue.timeMs = proposal.timeMs + layer.offsetMs;
            cue.op = proposal.op;
            // What the bank actually chose, which is not always what was asked: a
            // surface-coloured slot with no recording resolves to the default. The
            // renderer turns (slot, variant) back into samples by lookup, so it
            // needs the slot the variant indexes into. On a stop nothing resolved,
            // so the ask stands.
            cue.slot = proposal.op == CueOp::kStopLoop ? layer.slot : resolved.slot;
            cue.variant = resolved.variant;
            cue.gainDb = layer.gainDb + compressCutDb + budgetTrimDb + masterDb +
                         postShapeDb + proposal.postTrimDb +
                         LayerTrimDb(layer.slot, proposal.site, actor.isPlayer) +
                         // Over the *whole* stack, not the armour skin: heavy armour
                         // being louder is a fact about the body, not the rattle.
                         ArmorCompositeTrimDb(cfg, proposal.coverage) +
                         // The library's correction for the file actually picked -
                         // "this wav is hot". Here because this is the first moment
                         // anything knows which file resolved, and by then Stage 4
                         // has already sorted, rate-capped and burst-shaped, so it
                         // cannot change what was chosen.
                         resolved.trimDb;
            cue.compressCutDb = compressCutDb;
            cue.compressBand = band;
            // Multiplied into the pitch the engine chose rather than replacing it:
            // the scatter and biases are what this contact should sound like, and
            // the library's number corrects the recording. A file two semitones
            // flat is two semitones flat on every contact.
            cue.pitch = layer.pitch * resolved.pitch;
            cue.fadeMs = proposal.fadeMs;
            cue.position = collapse ? actor.collapsePoint : proposal.position;
            cue.boneIndex = proposal.boneIndex;
            cue.collapsed = collapse;
            cue.voiceId = proposal.voiceId;
            cue.actorId = proposal.actorId;
            cue.limbIndex = proposal.limbIndex;
            cue.site = proposal.site;
            cue.surface = proposal.surface;
            cue.coverage = proposal.coverage;
            cue.reason = layer.reason;
            cue.motion = actor.state.motion;
            cue.moment = actor.state.moment;
            cue.intensity = proposal.intensity;
            cue.sourceSeq = proposal.sourceSeq;

            if (proposal.op == CueOp::kStopLoop) {
                ReleaseVoice(proposal.voiceId);
            } else {
                if (cue.gainDb < cfg.mix.voiceFloorDb) {
                    // Not worth mixing in. The voice is already paid for, so this is
                    // purely "leave this layer out of the buffer".
                    continue;
                }
                if (proposal.op == CueOp::kStartLoop) {
                    TakeVoice(actor, kNever, proposal.voiceId);
                }
            }

            // Stage 5, and deliberately last. Every cost above has already been
            // paid, so a muted layer still spends what it would have and nothing
            // moves in to replace it - which is what makes muting an honest A/B.
            //
            // `emitted` counts it either way: the caller reads a zero as "this
            // proposal produced nothing" and rolls the burst state back, so
            // returning 0 for a fully muted proposal would undo the arbitration
            // this is supposed to leave untouched.
            const bool audible = LayerAudible(cfg, layer.slot);
            ++emitted;
            if (!audible) {
                ++stats.mutedCues;
                ++actor.stats.mutedCues;
                spdlog::debug("muted {} at {:.0f} ms", ToString(layer.slot), cue.timeMs);
                continue;
            }

            sink->Emit(cue);
            ++stats.emittedCues;
            ++actor.stats.emittedCues;
            if (compressCutDb < 0.0f) {
                ++stats.compressedCues;
                ++actor.stats.compressedCues;
            }
            if (stats.firstCueMs == 0.0 || cue.timeMs < stats.firstCueMs) {
                stats.firstCueMs = cue.timeMs;
            }
            stats.lastCueMs = std::max(stats.lastCueMs, cue.timeMs);
            spdlog::debug("cue {} {} at {:.0f} ms, {:.1f} dB, pitch {:.2f}", ToString(layer.slot),
                          ToString(layer.reason), cue.timeMs, cue.gainDb, cue.pitch);
        }

        // A stack whose every layer fell under the voice floor or resolved to
        // nothing is not an audible moment, and the caller is about to roll it
        // back. The voice it paid for goes with it, or the list leaks one slot per
        // silent proposal.
        if (booked && emitted == 0) {
            UntakeLastVoice();
        }
        return emitted;
    }
};

// ═════════════════════════════════════════════════════════════════════════════

Engine::Engine() : m_impl(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

void Engine::SetSoundBank(SoundBank* bank) { m_impl->bank = bank; }

void Engine::SetSink(ICueSink* sink) { m_impl->sink = sink; }

void Engine::SetConfig(const ConfigSet& config) {
    const bool seedChanged = m_impl->cfg.slots.rngSeed != config.Base().slots.rngSeed;
    m_impl->cfgs = config;
    m_impl->cfg = config.Base();

    // The bank is one object shared by every actor, so what it is told comes from
    // the ragdoll column - which is also the only column `[Slots]` has, since none
    // of it is `perMode`.
    //
    // Pushed on every SetConfig rather than only on a seed change. It used to
    // hang off `seedChanged`, which was true while the only thing that ever
    // replaced a config was the testbench pushing a whole new one with a new
    // seed; with a config set that can be swapped for any reason, four variant
    // switches would have gone quietly unapplied.
    if (m_impl->bank != nullptr) {
        m_impl->bank->SetStableVariants(m_impl->cfg.slots.stableVariants);
        m_impl->bank->SetConditions(m_impl->cfg.slots.conditionalVariants,
                                    m_impl->cfg.slots.surfaceConditions,
                                    m_impl->cfg.slots.armorConditions);
    }
    // The stream itself is only re-seeded when the seed actually moved: re-seeding
    // mid-scene throws away where the shuffle bag had got to, and a config swap
    // that keeps the seed should not re-roll every variant after it.
    if (seedChanged) {
        m_impl->rng.Seed(m_impl->cfg.slots.rngSeed == 0 ? 1u : m_impl->cfg.slots.rngSeed);
    }
}

void Engine::SetTracing(bool on) { m_impl->tracing = on; }

const std::vector<TraceRecord>& Engine::Trace() const { return m_impl->trace; }

void Engine::Tick(IFeed& feed, TimeMs nowMs) {
    Impl& impl = *m_impl;
    impl.drained.clear();
    feed.Drain(nowMs, impl.drained);

    // Zero means "unknown, use the configured floor"; 16.6 ms is 60 fps, which is
    // what a live feed that has not measured a frame yet should assume.
    const float frameMs = feed.FrameTimeSec() > 0.0f ? feed.FrameTimeSec() * 1000.0f : 16.6f;

    impl.Ingest(feed);

    const ListenerState& listener = feed.Listener();
    for (auto& actor : impl.actors) {
        if (actor.inUse) {
            impl.UpdateState(actor, nowMs, listener, frameMs);
        }
    }

    impl.RunStrategies(nowMs, frameMs);
    impl.Arbitrate(nowMs, frameMs);

    // Culling is what keeps a battlefield of ragdolls from becoming a performance
    // question, and it makes the mix cleaner at the same time.
    for (auto& actor : impl.actors) {
        if (actor.inUse && actor.state.tier == DistanceTier::kCulled) {
            impl.Release(actor, nowMs, "beyond the cull radius");
        }
    }

    // Animated mode's `ragdoll_end`: walking has no edge to close on, so an actor
    // acquired on their first footstep would be held - burst budget and hero count
    // with them - until they left the cull radius. Asked only of an actor animated
    // *now*, so a quiet stretch mid-fall cannot end a knockdown early, and only of
    // one that has seen a contact, so a freshly acquired actor is not released on
    // the tick it arrived.
    if (impl.cfg.game.animatedMode && impl.cfg.game.animatedIdleReleaseMs > 0.0f) {
        for (auto& actor : impl.actors) {
            if (!actor.inUse || !Animated(impl.cfg, actor) || actor.state.contactCount == 0) {
                continue;
            }
            const TimeMs last = std::max(actor.state.lastContactMs, actor.lastAdmittedMs);
            if (nowMs - last > impl.cfg.game.animatedIdleReleaseMs) {
                impl.Release(actor, nowMs, "animated and quiet");
            }
        }
    }
}

void Engine::Reset() {
    Impl& impl = *m_impl;
    impl.actors.clear();
    impl.liveVoices.clear();
    impl.contacts.clear();
    impl.proposals.clear();
    impl.drained.clear();
    impl.order.clear();
    impl.acceptedSeqs.clear();
    impl.trace.clear();
    impl.stats = EngineStats{};
    impl.nextVoiceId = 1;
    impl.nextSlideSeq = 0x8000'0000u;
    impl.rng.Seed(impl.cfg.slots.rngSeed == 0 ? 1u : impl.cfg.slots.rngSeed);
    if (impl.bank != nullptr) {
        impl.bank->SetStableVariants(impl.cfg.slots.stableVariants);
        impl.bank->SetConditions(impl.cfg.slots.conditionalVariants,
                                 impl.cfg.slots.surfaceConditions,
                                 impl.cfg.slots.armorConditions);
    }
}

const EngineStats& Engine::Stats() const { return m_impl->stats; }

const CrashState* Engine::State(ActorId actor) const {
    for (const auto& runtime : m_impl->actors) {
        if (runtime.inUse && runtime.state.actorId == actor) {
            return &runtime.state;
        }
    }
    return nullptr;
}

const CrashState* Engine::ActorAt(std::size_t index) const {
    // Counted over the in-use slots rather than indexed into the vector: the pool
    // reuses retired entries, so a caller walking 0..TrackedActors() would read a
    // dead slot the first time an actor let go mid-take.
    std::size_t seen = 0;
    for (const auto& runtime : m_impl->actors) {
        if (!runtime.inUse) {
            continue;
        }
        if (seen++ == index) {
            return &runtime.state;
        }
    }
    return nullptr;
}

std::size_t Engine::LiveVoices() const { return m_impl->liveVoices.size(); }

std::size_t Engine::TrackedActors() const {
    std::size_t count = 0;
    for (const auto& actor : m_impl->actors) {
        count += actor.inUse ? 1u : 0u;
    }
    return count;
}

}  // namespace rds
