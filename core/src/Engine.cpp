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

/// The limb chains that can carry a scrape loop of their own: two arms, two
/// legs and the head.
///
/// One loop per *limb* rather than per bone, which is the grain a listener
/// actually hears - a forearm and the hand on the end of it dragging together
/// are one arm scraping, not two sounds - and the bone the loop hangs on inside
/// the chain is whichever one is doing the most rubbing.
///
/// The torso is deliberately absent. A spine on the floor is not a limb
/// scraping, it is the body sliding, and that is the body loop's whole subject;
/// giving it a limb loop as well would play the same event twice.
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
// Not per actor and not globally. Both existed, both were guesses about the
// engine that the engine does not share, and both are gone.
//
// The global one went first: a voice-limit run started 288 sounds with 224 alive
// at once and the manager holding 257, and refused none of them - it never found
// a ceiling, it ran out of sound to play first. The per-actor one - eight, then
// twelve, from the design's own "six to eight per actor" (§14) - went the same
// way once it was tested against the same engine. Measured, Skyrim will take
// effectively as many voices as we can ask for.
//
// What that leaves behind is worth being explicit about, because "no cap" sounds
// like "no limit on how much you will hear". It is not. What decides how much is
// heard is the rate cap, the chain merge, temporal masking and burst shaping -
// rules that look at the contact in front of them and judge the mix. A ceiling
// on the count judges nothing: it takes sound away from whoever asked last
// rather than from whoever mattered least, which is why it kept deciding the mix
// behind every rule that was supposed to. A composite is one voice however many
// layers it has, so the count was never even measuring moments.
//
// The bookkeeping survives the cap, because `LiveVoices()` is a leak detector: a
// non-zero count with nothing tracked is a loop that was booked and never given
// back, and that is a real bug even when it silences nothing.

/// A loop's voice does not end on a clock; it ends when it is stopped.
constexpr TimeMs kNever = 1.0e18;

/// One voice in flight. The id is 0 for a one-shot and the loop's own id for a
/// loop, which is how a stop gives the voice back.
///
/// `actorId` is carried so the list can always be swept by owner. A loop books
/// `kNever` and is therefore only ever given back by name, so without an owner
/// an actor released mid-fall - which is every knockdown the NPC gets up from
/// before the bed has faded - stranded its entry for the rest of the session.
/// That used to silence the mod outright, because the count was capped and
/// twenty-four strandings filled it permanently. It no longer silences anything;
/// it is still a leak, and `LiveVoices()` is still how it is seen.
struct Voice {
    TimeMs endsMs{};
    std::uint32_t voiceId{};
    ActorId actorId{};
};
constexpr std::size_t kSiteCount = static_cast<std::size_t>(LimbSite::kCount);

[[nodiscard]] bool Ancient(TimeMs t) { return t <= kLongAgo / 2.0; }

[[nodiscard]] float Lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Two thresholds into a 0..1 position between them, clamped at both ends -
/// what config.md calls a ramp. `ScrapeLoopStrategy::Track` is the same
/// function; it stayed private to that class because nothing else needed one.
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
    /// slide-end impact is built without a feed row to look it up from, and
    /// re-deriving it there would be a second answer to a question ingest has
    /// already answered.
    float limbRadius{};

    float intensity{};    ///< 0..1, what the loudness curve was fed
    float onsetGainDb{};  ///< the level before any layer balance
    /// Intensity before the glancing-landing rule touched it. Stage 2 and the
    /// leading-limb tally read this rather than the reduced figure: how
    /// big the *fall* is must not move because one limb clipped the floor, or
    /// quietening a landing walks the whole event into a different phase and
    /// makes everything after it louder.
    float rawIntensity{};
    /// What the glancing-landing rule charged this contact, 1.0 for untouched.
    /// Kept on the contact so the crunch gate can optionally see it without
    /// recomputing the ratio.
    float glanceScale{1.0f};
    /// Every Shape-stage rule's post-arbitration half, summed: the glancing
    /// cut and the air-time lifts. Rides through to Emit so it
    /// changes loudness and nothing else.
    ///
    /// One field, not one per rule. They used to be four, kept apart "only so a
    /// trace can say which rule paid for what" - but every `Shape()` call
    /// already emits its own trace line carrying its own delta, so the
    /// attribution lives where it belongs and the contact does not have to grow
    /// a field every time a rule learns to trim.
    float modTrimDb{};
    std::uint64_t otherBody{};
    bool graze{};
    bool selfContact{};
    bool claimed{};  ///< the one cross-strategy mechanism there is
};

/// The order Stage 3 sees contacts in: strongest first, ties broken by the row
/// the contact came from. A named function rather than a lambda inside Ingest
/// because Stage 2 adds a contact of its own - the slide-end impact - and it has
/// to land where one the solver reported would have.
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

    /// How much this proposal *matters*, which is not the same question as how
    /// loud it is - `config.md`'s level/rank split, and the change 01 §8 listed
    /// as not started.
    ///
    /// `levelDb` plus the site's `[Arbitration]` weight, assembled once in
    /// `Arbitrate` so no strategy can reach it. Four rules read it and only
    /// four: the sort, the rate cap's override comparison, the chain merge, and
    /// what gets stored in `lastOnsetDb` / `chainLastDb` for the next proposal
    /// to be compared against. Masking and `maskCeilingDb` deliberately keep
    /// reading `levelDb`, because "can this be heard under that" is a question
    /// about air and not about importance.
    ///
    /// Every weight defaults to 0, so this is `levelDb` exactly until somebody
    /// moves one and the arbitrator sorts the way it always did.
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
    /// halo, the head accent's own voicing. Added at render, so it changes
    /// loudness without touching the sort, the class or the phase. One channel
    /// rather than one field per rule, because Emit must not grow a term every
    /// time a strategy learns to trim.
    float postTrimDb{};

    // ── the Budget stage's whole vocabulary ──────────────────────────────────
    //
    // Three fields, and every rule that buys a proposal past the arbitrator's
    // budgets writes them - the hero moment today. Carried here so the
    // arbitrator stays a list of fixed rules and the decision about which
    // contacts deserve a waiver stays with whoever understands them.
    //
    // The rule for a Budget-stage rule is the one thing to keep: it may move
    // the gaps and the burst - and it may never touch a level. A level before
    // arbitration is also a rank, so a rule that reached for one would not be
    // buying a contact past the budget, it would be making it outrank
    // everything in the frame.

    float burstGapScale{1.0f};
    float rateCapScale{1.0f};

    /// Close whatever burst was open and let this proposal start its own with
    /// the grain count back at zero.
    ///
    /// The strongest waiver there is, and the only one that can rescue a
    /// landing from a burst that a scuff opened and filled three hundred
    /// milliseconds earlier while the body was still in the air. It used to be
    /// reachable only through the air-time reset, with its own per-knockdown
    /// counter and its own five config keys; it is now what a hero moment does
    /// on the tick it is anchored.
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
    /// it. Stamped rather than inferred: every impact and every tick pose sample
    /// arrives with the phase the game thread read that frame.
    ///
    /// Only ever consulted through `Animated()`, and only in animated mode -
    /// with `GameIntegration:bAnimatedMode` off, nothing reads this field and a
    /// take replays exactly as it did before it existed. kUnknown is read as
    /// "not animated" so an old take with no phase on its rows is judged the way
    /// it always was.
    ActorPhase phase{ActorPhase::kUnknown};

    /// Whether the player has this body in their hands right now, off the last
    /// `held_start` / `held_stop` row. Stamped like `phase` is and never
    /// inferred: only the game thread can ask HIGGS, and only in VR is there
    /// anything to ask.
    ///
    /// Read by exactly one rule - `AccumDamage:bRequireHeld` - and false is the
    /// answer everywhere that rule is not in play, which is what lets a
    /// recording made before hold rows existed replay the way it always did.
    bool heldByPlayer{};

    TimeMs firstContactMs{kLongAgo};
    TimeMs lastAdmittedMs{kLongAgo};
    float energyRecent{};
    /// The same envelope as it stood before this tick's contacts were folded
    /// in. The hero rule's dominance clause needs it: `energyRecent` has
    /// already taken the maximum with every contact of this frame by the time
    /// Stage 2 runs, and a contact cannot be 1.3x itself. Snapshotted beside
    /// `worldContactBeforeTickMs`, which exists for the same reason.
    float energyRecentBeforeTick{};
    TimeMs energyStampMs{};
    float siteEnergy[kSiteCount]{};
    float groundZ{};
    bool haveGroundZ{};
    float headZ{};
    bool haveHeadZ{};

    /// The two geometric signals the head rules read, both refreshed in
    /// UpdateState. `lastWorldContactMs` is stamped by any limb *but* the head
    /// and neck touching something that is not a body;
    /// `worldContactBeforeTickMs` is that value from before this tick's contacts
    /// were folded in, which is what lets an arm coming down alongside the head
    /// read as part of the same dive rather than as proof the body was already
    /// down. The peer pair is this tick only.
    /// Hands are kept on their own stamp so the head half of the air-time rule
    /// can discount a hand thrown out in front of a dive without losing track of
    /// when one last touched down - see `headExcludeHands` and
    /// `headHandGraceMs`. The body half and the company rule count them either
    /// way, and so does the hero rule's arrival clause.
    TimeMs lastWorldContactMs{kLongAgo};
    TimeMs worldContactBeforeTickMs{kLongAgo};
    TimeMs lastHandContactMs{kLongAgo};
    TimeMs handContactBeforeTickMs{kLongAgo};
    std::int32_t worldPeers{};
    float worldPeerFastest{};

    /// A hero opened or re-anchored this tick and the burst is to start over.
    /// Set by `AnchorHero`, consumed by the arbitrator, cleared at the end of
    /// the tick - a moment resets the burst once, not on every later contact
    /// that happens to fall inside its window.
    bool heroResetsBurst{};

    // ── the pose, folded in from this tick's kLimbSample rows ────────────────
    //
    // Accumulated during Ingest and consumed in UpdateState, because a mass
    // weighted centre needs every limb of the tick before it means anything.
    Vec3 poseSum{};        ///< sum of position x nominal mass
    Vec3 poseVelSum{};     ///< sum of velocity x nominal mass
    float poseMass{};      ///< the weights, so the sums can be divided
    int poseLimbs{};       ///< how many limbs this tick actually reported
    bool sawPoseEver{};    ///< distinguishes "no pose in this take" from "none this tick"
    /// Whether any collision at all reached this actor this tick - self-hits and
    /// contacts nothing will voice included. Not a signal about sound: it is what
    /// tells the driven test that the tick's acceleration already has a
    /// collision to explain it. Cleared at the top of every Ingest.
    bool sawContactThisTick{};

    /// Last tick's centre, and when it was taken. The pair is what vertical
    /// acceleration is differenced from, and the stamp is why a dropped tick
    /// cannot silently turn into an enormous acceleration.
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
    /// same test the motion machine calls `touched`, written down so the next
    /// tick can still see it.
    ///
    /// The other half of the hysteresis, and it is here beside the free-fall
    /// pair because it is the same question asked of the other evidence.
    /// `airborne` is a latch on the *acceleration*, and it clears only after
    /// `fFreeFallHoldMs` of ticks that disagree - but a body bouncing along a
    /// floor is genuinely ballistic between bounces, so it tops the latch up
    /// and holds it on through the whole bounce train. Nothing in the machine
    /// remembered that the body had just been hit, so `Airborne` dropped to
    /// `Tumble` on the contact and `Tumble` put it straight back the next tick.
    /// See `fLandedHoldMs`.
    TimeMs lastTouchedMs{kLongAgo};
    /// When something last pushed this body in a way gravity cannot explain.
    TimeMs lastDrivenMs{kLongAgo};
    /// Whether anything has pushed this body during the *current* flight. The
    /// counter is per flight, not per frame and not per edge: a gate set high
    /// enough to make the flag flicker would otherwise report more driven
    /// flights than a gate that holds it steadily on, which is backwards.
    bool drivenThisFlight{};
    /// Where the body was when the current flight began, so the drop can be
    /// measured relative and survive a staircase.
    float airborneStartZ{};
    /// The flight that just ended, latched at the edge where the flag drops.
    ///
    /// A landing rule asks a question about a flight that is over. Reading the
    /// live clock instead asks whether the body is airborne *right now* and for
    /// how long, which is a different question and the wrong one at both ends -
    /// it pays out on mid-air clips, and it reads zero on the arrival, because
    /// by then the flag has cleared. Held for `MotionConfig::landingWindowMs`,
    /// and gone with the runtime when the knockdown ends, so a fall cannot be
    /// paid for twice.
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

    /// Where the body was last known to be, from the most recent admitted
    /// contact. This is what the continuous cues are placed at.
    ///
    /// `collapsePoint` cannot do this job, and must not be asked to. It is set
    /// only when a hero moment anchors, and its default is the origin - so a
    /// loop placed there before the first hero plays at the centre of the cell,
    /// and one placed there afterwards stays nailed to wherever the moment was
    /// anchored however far the body then tumbles. A voice at the origin is not
    /// silent, it is a quiet sound arriving from a fixed direction that has
    /// nothing to do with the body, which is exactly what that sounds like.
    Vec3 bodyPoint{};
    bool haveBodyPoint{};

    // strategy state, owned by the actor rather than by the strategy so the
    // strategies stay stateless and one instance can serve every actor

    /// What each damage tier has spent, one ledger per part per tier. Six
    /// counters where there used to be one, and the split is the point: a shared
    /// counter let whichever rule reached a contact first decide which layer you
    /// heard, and a crunch that was proposed and then dropped on arbitration
    /// spent the budget anyway.
    ///
    /// `lastMs` is the *proposal* time, not the emit time, for the same reason
    /// the count is charged at proposal: spacing is about how close together the
    /// engine is willing to break bone, and a tier that only charged for cues
    /// that survived would let a dropped crunch reopen the window immediately.
    struct DamageLedger {
        int count{};
        TimeMs lastMs{kLongAgo};
    };
    std::array<std::array<DamageLedger, 2>, static_cast<std::size_t>(DamageSite::kCount)> damage{};

    bool riseRunning{};
    std::uint32_t riseVoice{};
    float riseLastDb{kSilentDb};
    /// The anchor each running loop was last *told* about, which is not the same
    /// as the anchor it should be on. The renderer re-attaches a voice when a cue
    /// arrives and at no other time, so the engine has to know what it has already
    /// sent to know when a hop still needs sending. See `EmitLoopProposal`.
    std::uint16_t riseAnchor{};
    /// The motion trim as the bed is actually hearing it: `fBedTrimGlideMs`
    /// worth of glide behind `MotionBudgetFor`. Seeded on the first tick rather
    /// than started at zero, so the first loop of a take does not fade up out of
    /// a trim the body was never in.
    float bedTrimDb{};
    bool haveBedTrim{};

    /// The garment loop. One voice per actor, not one per limb: a shirt is one
    /// object, and splitting it would put four voices on every ragdoll to
    /// describe one shirt.
    bool rustleRunning{};
    std::uint32_t rustleVoice{};
    float rustleLastDb{kSilentDb};
    std::uint16_t rustleAnchor{};
    /// The armour class the loop started on, pinned for its life like
    /// `scrapeSlot` is. A coverage that resolves differently mid-fall must not
    /// swap the conditional variant under a running voice, which is a click.
    Coverage rustleCoverage{Coverage::kCloth};

    bool scrapeRunning{};
    std::uint32_t scrapeVoice{};
    float scrapeLastDb{kSilentDb};
    std::uint16_t scrapeAnchor{};
    /// The slot the body grind actually started on - the surface-coloured one
    /// where a file exists. Held for the life of the loop rather than being
    /// re-picked per tick: a body crossing from boards onto flagstone mid-skid
    /// should not swap files under a running voice, which is a click.
    SlotId scrapeSlot{SlotId::kScrapeLoop};

    /// The armour rattle riding the body grind, as its own voice.
    ///
    /// Not a second layer on the grind's proposal: a loop proposal carries one
    /// voice id through start, update and stop, so two layers sharing it would be
    /// two sounds with one lifecycle between them and stopping either would stop
    /// both. The limb loops are separate voices for the same reason.
    ///
    /// The slot is pinned at start like `scrapeSlot` is, and for the same
    /// reason: a body that loses its gauntlet mid-skid should not swap files
    /// under a running voice.
    bool armorSlideRunning{};
    std::uint32_t armorSlideVoice{};
    float armorSlideLastDb{kSilentDb};
    std::uint16_t armorSlideAnchor{};
    SlotId armorSlideSlot{SlotId::kArmorCloth};
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

        /// The bone the loop is on, and the bone that has been leading. The
        /// second only becomes the first once it has led for `fLimbHoldMs` -
        /// that is the stickiness that stops a scrape smearing as the contact
        /// hops between the bones of one arm.
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
        /// the solver's - those are asymmetric enough that a right arm would
        /// count for three times a left one (07 §6).
        float mass{};
        /// When it last grazed the world. Held for `fContactHoldMs`, because a
        /// contact stream is dense when a fall is busy and absent when it is not,
        /// and a fraction taken from one tick alone flickers at solver rate.
        TimeMs grazeMs{kLongAgo};
        /// Where the limb was, from the pose sidecar. Empty on a take with none,
        /// which is what makes the body anchor fall back on the heaviest limb
        /// that grazed rather than on a position nobody measured.
        Vec3 pos{};
        bool havePos{};

        // -- what the garment reads ------------------------------------------
        //
        // Kept per limb because the rustle drive is a sum over limbs of how each
        // one is moving *relative to the body*, and that cannot be recovered
        // from the mass-weighted centre the pose fold already builds - the whole
        // point of the measurement is what the centre averages away.

        /// How much cloth hangs here, from `FabricWeight(site, coverage)`.
        /// Seeded with the mass, from the profile, and 0 for a limb whose
        /// profile never arrived - which reads as "no garment", the quieter
        /// answer.
        float fabric{};
        /// This tick's velocity, and the previous tick's with the time it was
        /// taken. Two fields and not one, for the reason `tickTangent` sits
        /// beside `tangent`: the fold writes the new value while the difference
        /// still needs the old one, and one field would consume its own input.
        ///
        /// Its own timestamp rather than the centre's, because a limb can miss a
        /// tick the centre does not.
        Vec3 tickVel{};
        bool haveTickVel{};
        Vec3 vel{};
        bool haveVel{};
        TimeMs velMs{kLongAgo};
        /// This tick's rotation, rad/s, straight off the pose sample. Published
        /// on every limb row and thrown away until now.
        float angularSpeed{};
        /// The limb's radius, to turn radians into surface speed - the same
        /// product `Coupling` uses, and for the same reason: what drags a sleeve
        /// is how fast the surface is moving, not how many radians it turned.
        float radius{};

        /// This limb's own violence: a decaying peak-hold over its normalised
        /// thrash, 0 to 1. What `DamageViolenceConfig::limbShare` blends in.
        ///
        /// Not fabric-weighted, unlike everything else on this row that feeds the
        /// garment - a bare arm breaks exactly as well as a sleeved one, so the
        /// damage half reads the acceleration itself.
        float violence{};
    };
    LimbTrack limbs[kMaxLimbs]{};

    /// Sum of `fabric` over the profile, seeded beside it. The test that tells
    /// "wearing nothing we can name" apart from "this tick carried no pose",
    /// which the garment's own per-tick sum cannot do - see `ConsumeRustle`.
    float fabricTotal{};

    /// Accumulated damage, one pool per limb.
    ///
    /// Its own array rather than three more fields on `LimbTrack`, because
    /// everything on that row is derived from the *pose* stream and this is
    /// derived from the *contact* stream. They fill on different ticks, they
    /// survive a missing sidecar differently, and a limb with no pose still
    /// accumulates damage perfectly well.
    struct AccumTrack {
        /// How worked-over this limb is. Rises on every qualifying contact,
        /// heals exponentially, clamped at `fMaxPool`.
        float pool{};
        /// How far up the ladder this limb has already climbed. Comes back down
        /// as the pool heals, so a limb that is left alone and then attacked
        /// again climbs it a second time rather than being spent for ever.
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
    /// The limb a whole-body loop hangs on: the torso, resolved from the
    /// profile's bone names by `SiteFromBoneName` and never from an index.
    ///
    /// It used to be left at the default 0 and resolve to whatever Havok listed
    /// first. On a humanoid ragdoll `rigidBodies[0]` is `NPC COM [COM ]`, so the
    /// accident came out right, which is exactly why it survived - but nothing
    /// stated it and nothing enforced it, and on a creature or a modded skeleton
    /// the first body is a wing or a tail. A loop that belongs to the whole actor
    /// should say so.
    ///
    /// Falls back to 0 when no bone resolves to a torso, which is today's
    /// behaviour on a skeleton we cannot read.
    std::uint16_t bodyLimb{};

    /// What the *actor* is wearing, taken from the torso limb once at attach.
    ///
    /// The cues that have no contact - the airborne rise - have no limb to read
    /// a class off, and `Armor:bPerLimb = 0` wants this for the ones that do. A
    /// cuirass is the honest answer to "what is this body wearing" when only one
    /// answer is allowed.
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
    // Owned by the actor and read by the motion axis, because when a slide
    // starts and stops is one decision. It used to be two - the strategy kept
    // its own duration, distance and speed gates alongside `[Motion]`'s - and
    // the two could and did disagree, so a knockdown could sit in `Slide` with
    // no loop under it or grind away in `Tumble`.

    TimeMs grazeSinceMs{kLongAgo};  ///< when the current run of grazes opened
    TimeMs grazeLastMs{kLongAgo};   ///< and when the last one arrived
    /// Decaying peak-hold over contact tangent speed, with `fSlideGraceMs` for a
    /// constant. A running max - which is what this was - never comes down, so
    /// one fast skim held the slide's entry test open for the rest of the
    /// knockdown and pinned the loop's level and pitch at the loudest tangent
    /// the fall ever saw.
    float slideTangent{};
    /// The fastest tangent this tick, for the distance fallback on a take with
    /// no pose. Cleared once it has been integrated.
    float slideTickTangent{};
    float slideDistance{};  ///< units covered since the run of grazes opened

    /// How fast the *body* was going while it was still grazing: a peak-hold
    /// that decays while the grazing continues and then simply stops, so what it
    /// reports afterwards is the speed the body had when the slide was
    /// interrupted rather than a figure a hundred milliseconds of deceleration
    /// has already eaten. This is what the slide-end impact is built at and what
    /// the hero clause is measured on.
    float slideSpeed{};

    /// The hold as it stood before this tick folded its own grazes in.
    ///
    /// What a catch is measured against: "harder than the slide has been
    /// grinding lately" cannot be asked of a figure this very contact has
    /// already been maxed into, which would make every contact its own baseline
    /// and no contact a catch. The same shape as `energyRecentBeforeTick`, and
    /// for the same reason.
    float slideTangentBeforeTick{};

    /// The last graze's own tags. The slide-end impact is a contact the solver
    /// never reported, so it is coloured by the limb that was demonstrably
    /// grinding along the floor a frame ago rather than by a guess.
    std::uint16_t scrapeLimb{};
    LimbSite slideSite{};
    LimbChain slideChain{};
    Coverage slideCoverage{};
    SurfaceClass slideSurface{SurfaceClass::kSoft};
    float slideRadius{};


    EngineStats stats{};
};

/// Whether this actor is being heard *because* of animated mode: on their feet,
/// and only reaching the pipeline at all because the ragdoll gate was bypassed.
///
/// Every one of the three per-layer switches in `GameIntegration` is a question
/// about this and nothing else - none of them may touch an actor who is actually
/// ragdolling, which is the case the whole mod was tuned for. kUnknown counts as
/// ragdolling for the same reason: a take recorded before the phase was stamped
/// has to be judged the way it was when it was tuned.
[[nodiscard]] bool Animated(const AlgorithmConfig& cfg, const ActorRuntime& actor) {
    return cfg.game.animatedMode && actor.phase != ActorPhase::kRagdoll &&
           actor.phase != ActorPhase::kUnknown;
}

/// How fast the body is travelling, for everything the slide decides.
///
/// The measured centre of mass wherever the take carries pose, which is what a
/// slide is actually about - one limb's tangent speed is the speed of a limb,
/// and a limb whipping on its constraints is not the body. Without pose there is
/// no such measurement to have, so it falls back on the held tangent rather than
/// on `bodySpeed`, which off a take with no sidecar is the last contact's limb
/// speed decaying towards nothing.
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

    /// The run's totals, for the handful of strategies that keep a counter of
    /// their own. Per-actor stats are reachable through `actor`; this is the
    /// engine-wide twin, which every other counter in the mod writes in the same
    /// breath as the per-actor one.
    EngineStats* stats{};

    /// Read-only, and only ever asked `HasSound`. A strategy proposes; it does
    /// not resolve - resolution happens once, at Emit, so the cue list and the
    /// audio cannot disagree about which file played. But a strategy that
    /// proposes a layer nothing can voice is not free: on the loop paths it
    /// costs a voice id and a stop cue for a sound that never started.
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

/// Which armour class a contact's layers should be coloured by.
///
/// Per limb by default, which needs no mechanism at all - `contact.coverage` is
/// already the class of the limb that hit, so heavy boots on an otherwise naked
/// body give feet the plate rattle and everything else the skin slap. Off, every
/// contact reads the actor's cuirass instead and the whole body clanks together.
[[nodiscard]] Coverage EffectiveCoverage(const StrategyContext& ctx, const Contact& contact) {
    return ctx.cfg.armor.perLimb ? contact.coverage : ctx.actor.bodyCoverage;
}

/// The class to hang on a cue that never came from a contact.
///
/// The airborne rise is the one such cue left, and it has no limb to read. A
/// cuirass is the honest default - it is what you hear moving on a body in
/// flight - with the last graze's limb and "no opinion" as the alternatives.
/// `kCloth` is what "no opinion" means here rather than a fudge: the class is
/// defined as clothing *and anything we cannot decide*, so it is already the
/// value that asserts nothing.
[[nodiscard]] Coverage ActorClassCoverage(const StrategyContext& ctx) {
    switch (ctx.cfg.armor.actorClassSource) {
        case 1:  return ctx.actor.slideCoverage;
        case 2:  return Coverage::kCloth;
        default: return ctx.actor.bodyCoverage;
    }
}

/// The per-class pitch bias, in semitones. Zero at the shipping defaults, so it
/// costs nothing until somebody turns it - see ArmorConfig's note on the voicing
/// worth trying first.
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

/// The per-class trim over the whole stack, applied at render. Also zero by
/// default. Heavy armour genuinely is louder; this says so without touching
/// which contact won.
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

/// A body that has touched nothing at all has the longest air time there is, not
/// the shortest. Every caller either caps this against its own ramp or compares
/// it against a threshold, so one sentinel serves both without a second field to
/// forget to check.
constexpr float kAirTimeUnbounded = 1e9f;

/// How long the actor had been clear of the world when this contact arrived.
///
/// Contacts inside one frame arrive microseconds apart - the measured median gap
/// inside a frame's batch of callbacks is 1 us against 20 ms between batches,
/// which is what `frameGapMs` exists to separate. Vayne log_2's faceplant lands
/// its hand 3 *microseconds* before its skull, and reading that as "the body was
/// already down" is what made the head rule fire on every trivial head roll and
/// never on the dive. Nothing inside the frame bucket can break the air time,
/// whatever limb it belongs to.
///
/// `excludeHands` is the head half's own forgiveness - the arm you throw out in
/// front of a dive - and is off for everything else.
///
/// The drop rides along because it is the same measurement asked of the same
/// flight, and the hero test's arrival clause needs both. Splitting them let the
/// two be read off different flights, which is how the clause came to compare a
/// live air time against a drop that had already been reset.
struct FlightMeasure {
    float airMs{};
    float dropUnits{};
};

[[nodiscard]] FlightMeasure FlightFor(const StrategyContext& ctx, const Contact& contact,
                                      bool excludeHands, float handGraceMs) {
    // Measured, where the take carries pose: how long the body has actually been
    // unsupported when this contact arrived. Not how long since anything last
    // touched, which is what everything below has to make do with.
    //
    // The difference is the whole point. On Vayne_impacts_log_2_cut_4 the old
    // path scored the opening 44.7 u/s scuff at the top of the ramp - nothing had
    // ever touched, so it read as maximally airborne - while the body was in fact
    // being shoved upward at +229 u/s^2. It scored the real fall at 0.26.
    //
    // `excludeHands` and `handGraceMs` are the inference's own forgiveness for an
    // arm thrown out in front of a dive, and mean nothing against a measurement:
    // a hand touching down ends the flight whatever the head rules would like.

    // Animated mode's own switch, and the only place air time is measured - so
    // one test here covers the head accent's lift, the hero test's arrival
    // clause and the airborne rise together. Off, a body on its feet reads as
    // having been on the ground the whole time, which is what it was before the
    // ragdoll gate was bypassed.
    if (Animated(ctx.cfg, ctx.actor) && !ctx.cfg.game.animatedAirTime) {
        return {};
    }
    if (ctx.actor.state.haveBodySamples) {
        const CrashState& state = ctx.actor.state;
        if (state.airborne) {
            // freeFlightSinceMs, not airborneSinceMs: a body that has been hauled
            // through the air for half a second has been unsupported that whole
            // time and falling for none of it, and the rules downstream pay out
            // for falling. See CrashState::driven.
            return {std::max(0.0f, static_cast<float>(contact.timeMs - state.freeFlightSinceMs)),
                    state.fallDropUnits};
        }
        // The flight is over, and this is the half the rules were missing. A
        // landing is judged on the frame it arrives or a frame or two later, and
        // by then the flag has cleared - so reading the live clock here returned
        // zero on the one contact the whole rule exists for. What it wants is the
        // flight that just ended, which is latched rather than recomputed.
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
    // A hand also stops counting while it is recent enough to belong to this
    // strike. Older than the grace window it is a peer like any other, which is
    // what keeps a break-fall, roll and clipped head from reading as a dive.
    float sinceHand = elapsedSince(ctx.actor.handContactBeforeTickMs);
    if (sinceHand <= frameGap || (excludeHands && sinceHand <= handGraceMs)) {
        sinceHand = -1.0f;
    }

    // Measured against the most recent peer that still counts. None at all is
    // unbounded, not zero: a limb that is the first thing to touch is as
    // airborne as anything gets.
    float since = kAirTimeUnbounded;
    if (sincePeer >= 0.0f) {
        since = std::min(since, sincePeer);
    }
    if (sinceHand >= 0.0f) {
        since = std::min(since, sinceHand);
    }
    // No drop without pose. The one caller that reads it is gated on
    // `haveBodySamples` and never gets here, and inventing a number for it would
    // be the exact guess that gate exists to refuse.
    return {since, 0.0f};
}

/// The air time alone, for the callers that ramp on it and never ask the drop.
[[nodiscard]] float AirTimeMs(const StrategyContext& ctx, const Contact& contact,
                              bool excludeHands, float handGraceMs) {
    return FlightFor(ctx, contact, excludeHands, handGraceMs).airMs;
}

/// What kind of head strike a contact is.
///
/// Closing speed cannot tell a dive from a sprawl. Vayne log_2 has a 402 u/s
/// head that is the tip of a spine whip - Spine1 298, Spine2 355, Head 402, all
/// in one frame - and a 294 u/s head that is a genuine faceplant. The two things
/// that do separate them are how long the body had been clear of the world when
/// the head arrived (16 and 35 ms for the sprawls against 597 for the dive) and
/// how much company the head has in its own frame (2 and 4 peers against 1).
///
/// Shared by the three strategies that care rather than passed between them, so
/// none of them has to run before another.
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

        // The boost lands before arbitration, where a level is also a rank, so
        // an unbounded one does not merely make the skull loud - it makes it
        // outrank every other proposal in the frame. `headMaxLevelDb` bounds
        // where the accent ends up rather than how much is added, because
        // `headGainDb` already bounds the addition on its own. A negative
        // `headGainDb` is a cut and is left alone.
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

/// A layer that is an accessory to somebody else's onset: a crunch on the impact
/// it belongs to, the wet layer on the crunch.
///
/// Always a ride-along, which is the whole point - it is judged in a second pass
/// and emitted only if the contact's own onset was accepted, because a crunch
/// with no impact under it is not a sound anybody can place.
///
/// `offsetMs` defaults to where the body layer sits, which is where a crunch has
/// always been put; the head's damage layers pass their own so they can land a
/// beat after the skull rather than with it.
/// The head keeps `crunch_gran`; the other two get a file of their own and fall
/// back to it when nobody has recorded one - see the slot manifest.
///
/// Free rather than a member because both damage systems need it and neither
/// owns it: which file a site's crunch plays is a fact about the sound bank, not
/// a tuning either of them gets an opinion about.
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

/// The head refund and the air-time budget reset used to live here: two
/// functions that wrote the *same four* proposal fields, one judged on head
/// speed and one on air time, each with its own per-knockdown counter to keep
/// in step with the other.
///
/// Both were a hero moment bought per contact, and both are gone. The evidence
/// they read is now the hero test's two clauses - dominance and arrival - and
/// what they bought is `HeroConfig`'s own budget, granted once to the moment
/// rather than once per limb that happened to propose during it.

// ── the modifier pipeline ────────────────────────────────────────────────────
//
// See the contract in Config.h. Two functions, because there were four copies of
// each and the comments admitted it: "built exactly like the two above and for
// the same reasons".

/// Shape one contact. The ONE place a rule may move `intensity` or
/// `onsetGainDb`.
///
/// Three things happen here that every hand-rolled copy had to remember, and
/// that one of them forgot:
///
///  - the onset gain is **carried with the intensity delta** rather than
///    recomputed from scratch, so a later rule cannot throw away what an earlier
///    one charged - recomputing would silently undo the glancing cut. Since
///    `GainFromIntensity(i)` is `-range * (1 - i)`, that delta is just
///    `range * (after - before)`, which is what the three copies were each
///    spelling out as a pair of calls;
///  - the level lift is **capped**, because a level before arbitration is also a
///    rank, and an unbounded one does not merely make a contact loud - it makes
///    it outrank everything in the frame. The head halo added its lift with no
///    cap at all, and this signature is what makes that unrepresentable;
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
/// arbitrator's budgets, and it may not touch a level.
///
/// The combining rules are not arbitrary. The burst gap takes the **smaller
/// scale**, which is the more generous of what two rules asked for said the
/// other way round, and the two flags are sticky.
void Grant(Proposal& proposal, const BudgetWaiver& waiver) {
    proposal.burstGapScale =
        std::min(proposal.burstGapScale, std::clamp(1.0f - waiver.burstGapFrac, 0.0f, 1.0f));
    proposal.rateCapScale =
        std::min(proposal.rateCapScale, std::clamp(1.0f - waiver.rateCapFrac, 0.0f, 1.0f));
    proposal.resetsBurst = proposal.resetsBurst || waiver.resetsBurst;
}

/// Whether a contact is a graze: sideways motion *instead of* a hit, rather than
/// sideways motion as well as one.
///
/// Above the ceiling the ratio stops meaning anything: log_4's head arrives at
/// 241 u/s with 445 u/s of tangent, which the ratio alone calls a scrape and
/// which is in fact a skull hitting a floor at speed while sliding. Ratio
/// decides the quiet end; the ceiling overrules it at the loud end.
[[nodiscard]] bool IsGraze(const IngestConfig& ingest, float impactSpeed, float tangentSpeed) {
    return impactSpeed > 1.0f && impactSpeed < ingest.grazeMaxImpactSpeed &&
           tangentSpeed / impactSpeed > ingest.grazeRatio;
}

/// Hand a proposal what a hero moment bought it.
///
/// Granted per proposal but *decided* once, on the actor, which is the thing
/// the old air-time reset had to work around by hand: a landing is five limbs
/// arriving in one frame with the same evidence behind every one of them, so
/// letting each of them earn its own reset turned one burst into five. Here
/// the moment axis has already picked the anchor, and every proposal of the
/// window simply carries what that decision is worth. Nothing is spent and
/// nothing is counted per contact.
void ApplyHeroBudget(Proposal& proposal, const StrategyContext& ctx) {
    const HeroConfig& hero = ctx.cfg.hero;
    if (!hero.enabled || ctx.actor.state.moment != Moment::kHero) {
        return;
    }
    // `resetsBurst` comes off the actor rather than the config because only the
    // tick the moment was anchored on restarts the burst. A later contact inside
    // the same window is a peer joining the burst, not a reason to throw it away
    // and start again.
    Grant(proposal, BudgetWaiver{.burstGapFrac = hero.burstGapFrac,
                                 .rateCapFrac = hero.rateCapFrac,
                                 .resetsBurst = ctx.actor.heroResetsBurst});
}

/// Where a loop hangs.
///
/// The limb is the answer that matters: the renderer attaches the voice to that
/// bone's node, and a node moves with the body on its own. `position` is only the
/// fallback for a node that does not resolve - an unrecognised skeleton, or 3D
/// that went away mid-fall.
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
    // A running loop only needs a cue when something about it actually changed.
    // Without this a loop emits an update every frame, which buries the cue list
    // under three hundred no-ops and makes "cues out" mean nothing.
    //
    // The deadband is per caller, because a bed and a grind want different answers
    // out of it. Three-quarters of a decibel is right for a loop nobody is meant
    // to notice moving, and it is most of what a breathing scrape actually does -
    // the loop was being held flat by the thing meant to keep the log readable.
    // See `ScrapeLoopConfig::levelDeadbandDb`.
    //
    // It is a deadband on the *level*, and only on the level. Where a loop hangs
    // is not a loudness question, and it used to be one by accident: the callers
    // patched the anchor onto the proposal *after* this function returned, so a
    // tick that produced no proposal produced no re-attachment either. The
    // renderer re-follows on a cue and at no other time, so a grind that hopped
    // onto the other hip while its level happened to be steady went on sounding
    // from the hip it left until something unrelated moved its gain. `fLimbHoldMs`
    // makes a hop deliberate and infrequent, which is precisely the update that
    // cannot be dropped.
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
/// biggest part of it - the pitched sub at +65 ms - arrives late. That late sub
/// is what makes a hit read as *mass* rather than as *contact*, and it is the
/// single biggest contributor to the gnarl.
class ImpactCompositeStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ImpactComposite"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const ImpactCompositeConfig& impact = ctx.cfg.strategies.impact;
        if (!impact.enabled) {
            return false;
        }

        // A hard head landing can buy back some of the arbitrator's budget - see
        // the refund block in HeadImpactConfig - and a long fall can reset it
        // outright, whichever limb it lands on. Classified here as well as in
        // HeadImpactStrategy rather than passed between the two: it is a handful
        // of floats, and it keeps the strategies independent of each other's
        // running order.
        const HeadStrike strike = ClassifyHead(ctx, contact);

        // The quiet nine of every ten contacts are burst filler for the ordinary
        // reason underneath. The job is not to decide which of thirty contacts
        // to play; it is to spend what survives on a few tight bursts with real
        // silence between.
        if (contact.intensity < impact.tapBelowIntensity) {
            ProposeTap(ctx, contact, out, strike);
            return false;
        }

        Proposal proposal = FromContact(contact);
        ApplyHeroBudget(proposal, ctx);
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);

        // Pitch is free and continuous in this engine and it beats doubling the
        // bank: scatter per voice plus a systematic downward bias with intensity,
        // clamped so it never sounds like a pitch trick.
        const Coverage coverage = EffectiveCoverage(ctx, contact);
        proposal.coverage = coverage;
        // The armour bias joins the intensity bias *before* the same clamp, so
        // plate can never push the composite past the pitch ceiling the scatter
        // is already held to. Free dynamics: an armoured body reads heavier and a
        // naked one lighter with no assets installed at all.
        const float bias = impact.pitchIntensityBiasSemis * contact.intensity +
                           ArmorPitchSemis(ctx.cfg, coverage);
        const float scatter = ctx.rng.Bipolar() * impact.pitchScatterSemis;
        const float semis = std::clamp(bias + scatter, -impact.pitchMaxSemis, impact.pitchMaxSemis);
        const float pitch = SemitonesToRatio(semis);
        // The bias is about *mass*, and the contact instant is not a mass. Hertzian
        // contact time goes as v^(-1/5): double the closing speed and the contact
        // gets 13% shorter, so a harder hit is if anything fractionally *brighter*.
        // Nothing about hitting the floor faster softens the two surfaces. Biasing
        // the transient down with intensity therefore darkens exactly the hits that
        // should read hardest, and it darkens them worst where the clamp catches -
        // measured over two takes, half the composites landed on -3 semis exactly,
        // so the scatter had stopped varying anything and become a fixed transpose.
        //
        // The body and the sub keep it: a bigger effective mass really does ring
        // lower, and the armour bias is the same claim about the body underneath.
        //
        // One `scatter` draw feeds both, deliberately - the layers of one composite
        // should move together, and a second Bipolar() here would shift every
        // downstream variant and scatter draw and make a pinned seed stop comparing
        // against yesterday's takes.
        const float contactPitch =
            SemitonesToRatio(std::clamp(scatter, -impact.pitchMaxSemis, impact.pitchMaxSemis));

        // `ranks` is what keeps a layer out of `levelDb`, and the surface skin is
        // the one layer that asks for it. `ProposeTap` has always excluded its own
        // skin - colouring a tap must never move it up the arbitrator's sort - and
        // the composite folded its skin in, so the same layer answered the same
        // question two ways. A floor is what a contact *hit*, not how big it was,
        // and a rank assembled from it is `config.md`'s confusion in a new place.
        //
        // No-op at the shipping defaults, and deliberately so: body is -8..-2 and
        // surface -12..-6 over the same 6 dB span, so the skin sits exactly 4 dB
        // under the body at every intensity and can never be the max. That is
        // arithmetic nobody wrote down, and it stops being true the moment either
        // pair is re-voiced. The invariant should hold by construction.
        //
        // `massPitch` is the same shape of switch for the intensity pitch bias:
        // on for the layers that carry the body's weight, off for the ones that
        // are about the contact instead. See `contactPitch` above.
        const auto layer = [&](SlotId slot, float offsetMs, float minDb, float maxDb,
                               CueReason reason, bool ranks = true, bool massPitch = true) {
            if (proposal.layerCount >= static_cast<int>(kMaxLayers)) {
                return;
            }
            Layer& out2 = proposal.layers[proposal.layerCount++];
            out2.slot = slot;
            // The onset itself does not move: the 46 ms rate cap was measured
            // against onsets, and scattering the first layer would let two of
            // them land inside it. Everything after scatters a few ms so the
            // composite envelope is never identical - structured, not random,
            // which builds the shape instead of smearing it.
            out2.offsetMs =
                offsetMs + (offsetMs == 0.0f ? 0.0f : ctx.rng.Bipolar() * impact.offsetScatterMs);
            out2.gainDb = contact.onsetGainDb + Lerp(minDb, maxDb, contact.intensity);
            out2.pitch = massPitch ? pitch : contactPitch;
            out2.reason = reason;
            if (ranks) {
                proposal.levelDb = std::max(proposal.levelDb, out2.gainDb);
            }
        };

        // Loudness comes from layer balance, not from tiers: a light contact is
        // mostly transient with almost no sub, a heavy one is sub-dominant with
        // the transient riding on top. One continuum, no boundaries to hide.
        layer(SlotId::kImpTransient, impact.transientOffsetMs, impact.transientGainAtMinDb,
              impact.transientGainAtMaxDb, CueReason::kImpactComposite, true, false);
        const SurfaceConfig& surf = ctx.cfg.surfaces;
        if (surf.enabled) {
            // The floor's own offset and ramp, not the section's: glass ramps
            // steeply and carpet barely ramps at all, and one global pair could
            // only ever be right for one of them. A class with no block of its
            // own is holding its parent's numbers here, so this is a plain read
            // rather than a chain walk on the hot path.
            const SurfaceSkinConfig& skin = surf.Skin(contact.surface);
            layer(SurfaceSlot(contact.surface), skin.offsetMs, skin.gainAtMinDb,
                  skin.gainAtMaxDb, CueReason::kSurfaceSkin, false);
        }
        // What it was wearing, on the same terms as what it hit: after the
        // strike, before the mass, and out of the rank. Metal moving because
        // something stopped is a consequence of the contact rather than the
        // contact itself, so at offset 0 it would fuse with the transient into
        // one brighter click instead of reading as armour.
        //
        // The slot resolves to nothing at all until somebody records a file for
        // this class, and a layer that resolves to nothing is skipped in Emit -
        // which is what makes the whole section additive.
        const ArmorConfig& armor = ctx.cfg.armor;
        const SlotId armorSlot = ArmorSlot(coverage);
        if (armor.enabled && CanSound(ctx, armorSlot)) {
            layer(armorSlot, armor.offsetMs, armor.gainAtMinDb, armor.gainAtMaxDb,
                  CueReason::kArmorSkin, false);
        }
        // The torso's layer or the limb's - the ramp, the offset and the rank are
        // the same either way, because which wav carries the mass is a question
        // about timbre and not about how big this contact was. A bank with no
        // `imp_body_limb` recorded resolves both back to `imp_body`, so the split
        // costs nothing until somebody records one.
        layer(BodySlot(contact.site), impact.bodyOffsetMs, impact.bodyGainAtMinDb,
              impact.bodyGainAtMaxDb, CueReason::kImpactComposite);
        layer(SlotId::kImpSub, impact.subOffsetMs, impact.subGainAtMinDb, impact.subGainAtMaxDb,
              CueReason::kImpactComposite);

        // Held under the stack whatever the ramp says, and necessarily after the
        // stack exists - `levelDb` is not final until the sub has been added. A
        // plate rattle is the layer most likely to be long and loud relative to
        // what it is colouring, because armour keeps moving after the body has
        // stopped.
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
        // exactly the proposal a hero moment's budget is worth spending on.
        ApplyHeroBudget(proposal, ctx);
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
        proposal.levelDb = contact.onsetGainDb + impact.transientGainAtMinDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = SlotId::kLimbTap;
        proposal.layers[0].gainDb = proposal.levelDb;
        // Heavily pitch-scattered, because a tap is the one layer where a repeat
        // is audible as a repeat rather than as texture.
        proposal.layers[0].pitch = SemitonesToRatio(
            std::clamp(ctx.rng.Bipolar() * impact.pitchScatterSemis * 1.6f, -impact.pitchMaxSemis,
                       impact.pitchMaxSemis));
        proposal.layers[0].reason = CueReason::kLimbTap;

        // The colour, on what used to be the one cue in the mod that could not say
        // what it hit or what it was wearing. Same slots the composite's skins
        // resolve to, so a scuff and a landing on the same boards in the same
        // plate name the same floor and the same armour - only quieter, tighter,
        // and held under the grain they are colouring.
        //
        // Both are deliberately left out of `levelDb`, which `AddSkin` enforces
        // by never touching it: the tap's rank is the tap's, so colouring one can
        // never move it up the arbitrator's sort. The headroom clamp is what
        // keeps that true of the mix as well as of the sort.
        const SurfaceConfig& surf = ctx.cfg.surfaces;
        // `onTaps` is the floor's, because water is why it had to be: a splash
        // on nine of every ten contacts is absurd where a knock on boards is
        // exactly right. The offset and the headroom stay the section's - they
        // describe the grain being coloured, not the floor colouring it.
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
    /// It never touches `levelDb`, and that is the point rather than an omission.
    /// A colour layer allowed into the max that computes the level would be a
    /// colour layer allowed to change the *rank* the arbitrator sorts by, which
    /// is how a scuff on the floorboards gets ahead of a real impact. Enforcing
    /// it here means the next skin cannot get it wrong by being written out
    /// longhand a fourth time.
    ///
    /// `kCount` for the slot is "this class has no skin", which is how the
    /// armour section stays silent until somebody records one.
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
        // The ordinary scatter, not the tap's 1.6x: a repeat is audible as a
        // repeat in the grain, but a skin is colour and wants to stay put.
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
        // The only armour rule anywhere near the head, and deliberately the
        // whole of it: one read of the contact's class, before any
        // classification runs, deciding whether this strategy runs at all.
        //
        // No armour skin is layered on `head_impact` and no armour condition is
        // threaded through ClassifyHead. This strategy is the most heavily
        // conditioned code in the mod and a fourth axis inside it would buy a
        // helmet at the price of making it harder to reason about.
        //
        // What goes away is the *accent* - the dull skull thud with the ring on
        // it, which is what reads as bare skull and is exactly wrong under a
        // helm. The composite still fires, both skins still fire, and damage
        // still fires through DamageStrategy's own door, which since the merge
        // is a different strategy entirely: this gate cannot reach a crunch even
        // by accident. A head in plate landing hard is still loud; it just stops
        // sounding like a melon.
        //
        // A strategy-level gate, not a phase-level one, so it does not break
        // "no state suppresses a contact": that rule is about the motion axis,
        // which cannot judge a contact. This reads the contact in front of it.
        const ArmorConfig& armor = ctx.cfg.armor;
        if (armor.enabled &&
            ((armor.noHeadOnLight && contact.coverage == Coverage::kLight) ||
             (armor.noHeadOnHeavy && contact.coverage == Coverage::kHeavy))) {
            return false;
        }
        // Head-down attitude, how long the body was in the air before the head
        // arrived, and how much company it has all move the same gate - see
        // ClassifyHead.
        const HeadStrike strike = ClassifyHead(ctx, contact);

        // Damage - the crunch and the gore - used to be decided here, and it is
        // `DamageStrategy`'s now. It was never really the accent's business: how
        // hard the skull landed and whether it earns an accent are different
        // questions, and a gate raised for voicing reasons should not quietly
        // take the consequences with it. Keeping the two here also meant the head
        // and the body had two rules for one sound, sharing one budget.

        if (contact.impactSpeed < strike.gate) {
            return false;
        }

        Proposal proposal = FromContact(contact);
        // A head impact that *is* the moment is the event, not an accent on
        // one, so it stops being an accessory that dies with whatever composite
        // the arbitrator happened to drop. Anything else still rides along,
        // because a head landing is still a body landing.
        //
        // The trigger used to be the top of the head's own air-time ramp, which
        // is a private measurement that says nothing about whether the mix
        // considered this a moment - a head could be fully clear of the world
        // during an entirely unremarkable roll. Now it is the moment axis, and
        // specifically *this contact anchored it*: being merely inside somebody
        // else's hero window makes the accent a peer, not the event.
        proposal.rideAlong = !(ctx.cfg.strategies.head.claimsOnsetOnHero &&
                               ctx.actor.state.moment == Moment::kHero &&
                               ctx.actor.state.heroSeq == contact.sourceSeq);
        // An onset of its own has to be able to pay its own way past the
        // budgets, so it carries what the moment is worth. Nothing is spent:
        // the moment was decided once, on the actor, and every proposal it
        // covers reads the same decision.
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
/// One rule where there were two. The head's used to live inside
/// `HeadImpactStrategy` and was deterministic; everything else was judged here by
/// a probability gate. Both shapes survive as tunings - a tier whose two
/// probabilities are 1 is the deterministic rule exactly - so what the merge
/// removed is the second gate, the second level ramp, and the single budget the
/// two of them spent from in whatever order contacts happened to arrive.
///
/// Discrete, still: you cannot have thirty percent of a bone break, and one
/// played quietly sounds like a bug. A tier that should not be certain is
/// softened with probability, never with volume.
///
/// Everything it proposes is a ride-along, so if the arbitrator drops the onset
/// under a contact its damage dies with it. A crunch with nothing beneath it is
/// not a sound anybody can place.
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

        // Every gate is a fraction of the loud anchor, so the whole tier
        // structure moves with the range rather than having to be re-derived
        // whenever the anchor does.
        const float anchor = std::max(1.0f, ctx.cfg.intensity.speedRefHigh);

        // Optionally a glancing landing has to arrive proportionally faster to
        // break something - see GlancingImpactConfig::scaleCrunchGate. Off, a
        // bone breaks at whatever angle it likes and the glance rule has already
        // made the layer quieter through the intensity behind its level.
        const float own = ctx.cfg.glancing.scaleCrunchGate
                              ? contact.impactSpeed * contact.glanceScale
                              : contact.impactSpeed;

        // What this part is judged on: its own arrival, how hard the whole body
        // is being dealt with, or a blend - see DamagePartConfig::bodyForceShare.
        // The envelope has already taken the maximum with every contact of this
        // frame by now, so the blend can only raise the speed, never lower it.
        const float speed =
            Lerp(own, std::max(own, ctx.actor.energyRecent),
                 std::clamp(part.bodyForceShare, 0.0f, 1.0f));

        // Past the obliterate point the limits loosen for this contact: the
        // budget gains slots and the spacing shrinks. It used to be a second gate
        // *under* the gore, which made the most extreme contacts the mod can see
        // the hardest ones to hear.
        const bool obliterate = speed >= ctx.cfg.intensity.obliterateFrac * anchor;

        // A head that led the body in is held to its own crunch gate, which is
        // what puts a crunch on a slow dive without putting one on every fast
        // sprawl: Vayne log_2's 294 u/s faceplant crunches where its 402 u/s
        // spine whip, faster by a third, does not.
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

        // The two tiers are independent, and deliberately: gore is not nested
        // inside crunch. It has its own threshold, its own budget and its own
        // spacing, so a contact bad enough to be wet still sounds wet on a frame
        // where the crunch budget is gone. Nesting it made the rarest and most
        // expensive layer in the mod the easiest one to suppress by accident.
        // What kind of fall this contact is happening inside, 0 to 1 - which is
        // a different question from how hard it hit, and the only one the tiers
        // could not previously ask. See DamageViolenceConfig for why it is a
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

    /// How violent the fall this contact arrived in has been, 0 to 1.
    ///
    /// A blend of the body's own window and the contact limb's, because the two
    /// come apart and both are real: a body sliding to a stop with one leg still
    /// cartwheeling is quiet on the first and loud on the second.
    ///
    /// Both halves are peak-holds that rise only on contact-free ticks, so this
    /// cannot contain the collision it is about to judge - which is the whole
    /// reason it is a window rather than a reading.
    [[nodiscard]] static float Violence(const StrategyContext& ctx, const Contact& contact) {
        const DamageViolenceConfig& viol = ctx.cfg.strategies.damage.violence;
        if (!viol.enabled) {
            return 0.0f;
        }
        const ActorRuntime& actor = ctx.actor;
        // No pose, no measurement, and the fallback is *off* rather than a guess
        // - the same rule every other reader of the pose stream follows. On a
        // take with no sidecar the damage rule behaves exactly as it did before
        // this existed.
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
    ///
    /// `gate` is passed separately from `tier.atFrac` because the air-time rule
    /// can lower it for a led head. The *ramps* stay measured from the tier's own
    /// threshold either way: the override moves whether a crunch happens, never
    /// how loud it is, so a dive that only crunches because of that rule arrives
    /// at the quiet end of the ramp instead of jumping into the middle of it.
    static void Fire(const StrategyContext& ctx, const Contact& contact, ProposalList& out,
                     DamageSite site, std::size_t tierIndex, const DamageTierConfig& tier,
                     float anchor, float speed, bool obliterate, float gate,
                     float flatProbability, SlotId slot, CueReason reason, float violence) {
        if (!tier.enabled) {
            return;
        }
        const DamageViolenceConfig& viol = ctx.cfg.strategies.damage.violence;

        // A violent fall lowers the bar. It moves the **gate** and never the
        // ramp, exactly as the air-time head rule does: a contact admitted only
        // because the body was thrashing arrives at the quiet end of the level
        // ramp rather than jumping into the middle of it, so lowering the bar is
        // not also a way of making what comes through it loud.
        //
        // Measured as a fraction of the tier's own span, so one number suits all
        // six tiers however far apart their thresholds sit, and so it can never
        // invert the gate or push it below zero.
        if (violence > 0.0f && viol.gateDropFrac > 0.0f) {
            const float span = std::max(0.0f, tier.capFrac - tier.atFrac) * anchor;
            gate = std::max(0.0f,
                            gate - violence * std::clamp(viol.gateDropFrac, 0.0f, 1.0f) * span);
        }
        if (speed < gate) {
            return;
        }
        const DamageConfig& dmg = ctx.cfg.strategies.damage;
        auto& ledger = ctx.actor.damage[static_cast<std::size_t>(site)][tierIndex];

        // Budget and spacing, both relaxed past the obliterate point rather than
        // waived: a raised budget is still a budget, so a ridiculous impulse from
        // another mod cannot machine-gun the layer.
        // Violence loosens the same two limits the obliterate point does, and for
        // the same reason - the difference is only what counts as extreme. It is
        // also the only lever that moves occurrence by an amount anybody can
        // hear: measured on the corpus these tiers are budget-limited rather
        // than threshold-limited by about eight to one, so the gate drop above
        // admits candidates into a ledger that is usually already spent. See
        // DamageViolenceConfig for the numbers.
        //
        // Rounded, not truncated: violence peaks near 0.5 on real falls, and a
        // truncating bonus of 2 would grant nothing on any of them.
        const int violenceBudget =
            violence > 0.0f
                ? static_cast<int>(std::lround(violence * static_cast<float>(viol.budgetBonus)))
                : 0;
        const int budget =
            tier.budget + (obliterate ? dmg.obliterateBudgetBonus : 0) + violenceBudget;
        if (ledger.count >= budget) {
            return;
        }
        float spacingMs =
            tier.spacingMs * (obliterate ? std::max(0.0f, dmg.obliterateSpacingScale) : 1.0f);
        if (violence > 0.0f) {
            spacingMs *= Lerp(1.0f, std::max(0.0f, viol.spacingScale), violence);
        }
        if (spacingMs > 0.0f && ctx.nowMs - ledger.lastMs < spacingMs) {
            return;
        }

        // A cap at or under the threshold is not an error - it is how you ask for
        // a step instead of a ramp - and collapses to "full from the threshold
        // up" rather than dividing by a span of one unit.
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
        // The other half of occurrence, and the conservative one: the gate drop
        // above changes *which* contacts are eligible, this changes how often the
        // already-eligible ones fire. Applied after the flat override as well,
        // because a led head landing inside a violent tumble is no less likely to
        // break than one landing inside a calm one.
        if (violence > 0.0f && viol.chanceBonus > 0.0f) {
            probability = std::clamp(probability + violence * viol.chanceBonus, 0.0f, 1.0f);
        }
        if (probability < 1.0f && (probability <= 0.0f || ctx.rng.Unit() > probability)) {
            return;
        }

        // Charged at proposal, and the ledger is stamped here rather than at
        // emit, because spacing is a statement about how often the engine is
        // willing to break bone. A tier that only charged for cues that survived
        // arbitration would reopen its window on every drop.
        ++ledger.count;
        ledger.lastMs = ctx.nowMs;

        // The "intensity" half. A tier is discrete - a bone either broke or it
        // did not - so violence is never allowed to soften one into existence;
        // it only pushes a break that was already happening further forward.
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
///
/// It decides nothing about *when* a slide happens. That is the motion axis',
/// and this strategy's only questions are which loops, how loud, how fast, where
/// they hang, and - when it stops - how quickly to let go.
///
/// Two kinds of loop and they run together rather than one being chosen:
///
///  - **The body grind**, whose level is the body's measured speed *and* how
///    much of the body is actually on the surface. The second half is new and is
///    the whole of the fix: the level used to be speed alone, so a corpse dragged
///    by one ankle read exactly as loud as the same corpse lying flat, because
///    nothing anywhere in the mod measured how much of it was touching.
///  - **The limb grinds**, one per chain, each on the bone inside that chain
///    doing the most rubbing. Light, dry and well under the body grind. They are
///    what carries the bottom of the range, and they are why the body grind is
///    allowed to be silent there.
///
/// Blended rather than switched, on one continuous measurement. A body that
/// starts flat and rolls onto a shoulder trades weight between them smoothly;
/// picking one would put a boundary in the middle of a slide and a boundary is
/// the thing a listener hears.
class ScrapeLoopStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ScrapeLoop"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        if (!scrape.enabled || !contact.graze || !Allowed(ctx)) {
            return false;
        }

        // Claiming a graze keeps it off the impact path, which is right while a
        // slide is running - you do not want a thud on every frame of a skid -
        // and was being done all the time, sliding or not. Roughly half of all
        // worthwhile contacts in an ordinary tumble classify as grazes, so half
        // of them were taken off the impact path, no slide opened because a
        // single glancing knock is not one, and nothing was played in their
        // place. Half the contacts of a knockdown deleted from the mix.
        //
        // Unconditional now rather than a switch. There is no reading of the mix
        // in which a graze with no grind under it should be silent, so the old
        // `bClaimWhileSlidingOnly` is gone instead of inverted - see
        // `ScrapeLoopConfig::slidesDontClaim`.
        //
        // The bookkeeping behind the claim lives in Stage 1 with the rest of the
        // per-contact tallies, because the motion axis reads it and Stage 2 runs
        // before Stage 3.
        if (ctx.actor.state.motion != Motion::kSlide) {
            return false;
        }

        // ...and inside a slide, a rub hard enough to also be a hit is not ours
        // either. The test above is a phase judging a contact, which is the
        // thing 01 §3.1 says a phase does not get to do; this is the contact
        // judged in front of us, the way the tap branch, masking and the chain
        // merge all judge theirs. No grain for one that falls through: the
        // impact path is voicing that collision, and a grain on top would be a
        // second onset spending the same burst budget for one hit.
        //
        // The boolean is the gate and the intensity is where it bites, so the
        // pair reads as one sentence: a slide does not claim what is over this.
        // Off at the default, where a grind keeps everything under it.
        if (scrape.slidesDontClaim && contact.intensity >= scrape.claimBelowIntensity) {
            return false;
        }

        // A catch. Fired on a contact the solver actually reported and never on
        // an inference - the settle system and the synthesised slide impact were
        // both deleted for inventing sound where there was no collision, and
        // this is the opposite case: a real contact the mod was throwing away.
        ProposeGrain(ctx, contact, out);
        return true;
    }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        // Everything about a slide is voiced at full detail or not at all.
        // Nobody resolves which limb a grind is on at fifteen metres, and the
        // mod already strips loops out past `fFullRadius`.
        const bool alive = scrape.enabled && Allowed(ctx) &&
                           actor.state.motion == Motion::kSlide && actor.haveBodyPoint &&
                           actor.state.tier == DistanceTier::kFull;

        // How much of the body is on the surface, and therefore how much of the
        // body grind there is. Off, the ramp is 1 and the level is speed alone,
        // which is what the mod did before the fraction existed.
        const float fracSpan = std::max(0.001f, scrape.bodyFracFull - scrape.bodyFracStart);
        const float weight =
            !scrape.fractionEnabled
                ? 1.0f
                : std::clamp((actor.contactFraction - scrape.bodyFracStart) / fracSpan, 0.0f, 1.0f);

        BodyLoop(ctx, out, alive, weight);
        LimbLoops(ctx, out, alive, weight);
    }

private:
    /// Whether a slide may be voiced on this actor at all.
    ///
    /// Asked on the tick path as well as the contact one, and asked *before* the
    /// loop is judged alive rather than after: `ProposeTick` turning it false is
    /// how a grind that was already running gets its stop cue, so switching
    /// `bAnimatedSlide` off mid-slide ends the loop instead of stranding it.
    [[nodiscard]] static bool Allowed(const StrategyContext& ctx) {
        return !Animated(ctx.cfg, ctx.actor) || ctx.cfg.game.animatedSlide;
    }

    /// The speed the loops are levelled on: the body's, with a little of the
    /// contact's spikiness blended in.
    ///
    /// Body speed is smooth by nature and a level that follows it alone reads as
    /// a constant however correct it is. Contact tangent speed is genuinely
    /// spiky, because limbs load and unload as a body tumbles. A wobble *around*
    /// the body speed and never a replacement for it: the level used to be driven
    /// purely by contact speed, which is the speed of a limb rather than of the
    /// body, and that was wrong.
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
    /// for it resolves back to the default grind inside the bank, so this can
    /// name a slot that does not exist yet without going silent.
    [[nodiscard]] static SlotId SurfaceOf(const ScrapeLoopConfig& scrape, SlotId base,
                                          SurfaceClass surface) {
        return scrape.surfaceVariants ? ScrapeSurfaceSlot(base, surface) : base;
    }

    void BodyLoop(const StrategyContext& ctx, ProposalList& out, bool alive, float weight) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        const float speed = LevelSpeed(ctx);
        const float track = Track(speed, scrape.speedForMinGain, scrape.speedForMaxGain);
        const float gainDb = scrape.gainDb + Lerp(scrape.speedRangeDb, 0.0f, track) +
                             Lerp(scrape.bodyFracRangeDb, 0.0f, weight);
        const float pitch = 1.0f + scrape.pitchPerThousandUnits * speed / 1000.0f;

        // Silent under `fBodyFracStart` rather than quiet: below that the limb
        // loops are the whole of the slide, and a body grind running at the
        // bottom of its own ramp is a voice held open to play nothing.
        //
        // The floor is part of the same test and not an afterthought. Both ramps
        // bottom out around -47 dB against a -48 dB voice floor, and a loop
        // *started* under the floor is worse than one not started at all: Stage 5
        // drops the layer, so no `kStartLoop` reaches the renderer, but the
        // strategy has already marked the loop running - and every later cue is
        // an update against a voice id nothing holds. The grind would then never
        // arrive for the whole of that slide, however loud it got.
        // Stopped at the bottom of its own ramp, on both axes, rather than at a
        // level.
        //
        // The fraction axis has always done this - `weight > 0` above - and the
        // speed axis never did, so the job of ending a grind that had run out of
        // speed fell to `Mix:fVoiceFloorDb`. That worked only by arithmetic
        // coincidence: at the shipped gains both ramps bottom out near -47 dB
        // and the floor sits at -48, so the loop was cut a decibel under its own
        // quietest. Move `fGainDb` up four, or the floor down, and the two stop
        // corresponding - the grind then runs on at the bottom of its ramp,
        // emitting nothing (its level is not moving, so the deadband holds every
        // update back) until the slide state itself ends. A constant, unchanging
        // grind is exactly what that sounds like.
        //
        // `fSpeedForMinGain` is the honest test because it is already the answer
        // to "how slow is not a grind any more": it is where the ramp bottoms
        // out, it is in units per second where a listener can check it against
        // what they see, and it survives any change to the gains above it. The
        // limb loops have had their own version of this all along in
        // `fLimbMinTangentSpeed`; this is the body's.
        //
        // The floor test stays as a backstop, because it also guards the other
        // failure it was written for: a loop *started* under the floor is marked
        // running by the strategy while Stage 5 drops the cue, and every update
        // after it addresses a voice the renderer never opened.
        const bool wants = alive && scrape.bodyEnabled && weight > 0.0f &&
                           speed > scrape.speedForMinGain &&
                           gainDb >= ctx.cfg.mix.voiceFloorDb;

        if (!wants) {
            if (actor.scrapeRunning) {
                StopLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice,
                                 actor.scrapeSlot, CueReason::kScrape, StopFadeMs(ctx));
            }
            // The armour rides the grind and cannot outlive it. Every other exit
            // below stops the pair together; this one returned early and left the
            // skin running with nothing above it to update or stop it, so it hung
            // there until the actor was released - stranded by the back door, in
            // exactly the way the body grind's own floor test exists to prevent.
            // Silent today because no armour class has a recording, which is the
            // only reason it has not been heard.
            if (actor.armorSlideRunning) {
                StopLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                                 actor.armorSlideSlot, CueReason::kArmorSkin, StopFadeMs(ctx));
            }
            return;
        }

        if (!actor.scrapeRunning) {
            actor.scrapeSlot = SurfaceOf(scrape, SlotId::kScrapeLoop, actor.slideSurface);
        }
        // Where the grind is, decided before the cue rather than patched onto it
        // afterwards - which is what let the level deadband swallow a hop.
        //
        // The root is roughly the pelvis and is not where the sound is even for a
        // genuine full-body slide; nearest-to-contact is, and unlike "lowest" it
        // survives a staircase, a wall and a ceiling.
        LoopAnchor anchor = BodyAnchor(actor);
        if (scrape.bodyFollowsContact && actor.haveBodyAnchor) {
            anchor.limbIndex = actor.bodyAnchor;
            if (actor.bodyAnchor < actor.limbCount && actor.limbs[actor.bodyAnchor].havePos) {
                anchor.position = actor.limbs[actor.bodyAnchor].pos;
            } else if (actor.haveGrazeCentre) {
                anchor.position = actor.grazeCentre;
            }
        }
        EmitLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice, actor.scrapeLastDb,
                         actor.scrapeAnchor, actor.scrapeSlot, gainDb, pitch, CueReason::kScrape,
                         scrape.startFadeMs, anchor, actor.slideCoverage,
                         scrape.levelDeadbandDb);

        // The armour riding it: same anchor, same fades, its own voice. Flat
        // rather than ramped because a slide has no single intensity - it has a
        // duration, and the grind's own level already tracks how hard the body
        // is pressing. Silent until somebody records the class's file.
        const ArmorConfig& armor = ctx.cfg.armor;
        if (!armor.enabled || !armor.onSlide ||
            !CanSound(ctx, ArmorSlot(actor.slideCoverage))) {
            if (actor.armorSlideRunning) {
                StopLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                                 actor.armorSlideSlot, CueReason::kArmorSkin, StopFadeMs(ctx));
            }
            return;
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
            return;
        }
        EmitLoopProposal(ctx, out, actor.armorSlideRunning, actor.armorSlideVoice,
                         actor.armorSlideLastDb, actor.armorSlideAnchor, actor.armorSlideSlot,
                         armorDb, pitch, CueReason::kArmorSkin, scrape.startFadeMs, anchor,
                         actor.slideCoverage, scrape.levelDeadbandDb);
    }

    void LimbLoops(const StrategyContext& ctx, ProposalList& out, bool alive, float weight) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        const auto hold = static_cast<float>(std::max(0.0f, scrape.contactHoldMs));
        const int budget = std::clamp(scrape.maxLimbLoops, 0, static_cast<int>(kScrapeChainCount));

        struct Candidate {
            std::size_t index{};
            float score{};
        };

        // The budget is handed out on the entry test and taken away on a cheaper
        // hold test - hysteresis, the same shape as the slide's own entry and
        // exit speeds. A chain that already has a loop keeps it while it still
        // clears `fLimbHoldTangentSpeed`, whatever the other chains are doing;
        // only what is left of the budget is offered to newcomers, ranked by how
        // hard they are rubbing. Without the split, two legs whose tangents
        // cross swap one voice back and forth every other tick.
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

        // The duck, scaled by the body grind's own weight so it arrives with the
        // body rather than switching on. Deep enough is suppression rather than
        // damping, which is what makes one slider cover both.
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

            // Ducked under the mix's own floor is not a quiet loop, it is a voice
            // held open to play nothing - and it is what somebody asking the duck
            // to *suppress* rather than damp has asked for. Stopped, so the voice
            // goes back to the budget and the impacts can have it, and so the
            // loop is never left marked running with no `kStartLoop` behind it -
            // see the same test on the body grind.
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
            const std::size_t before = out.size();
            EmitLoopProposal(ctx, out, loop.running, loop.voice, loop.lastDb, loop.sentAnchor,
                             loop.slot, gainDb, pitch, CueReason::kScrape, scrape.startFadeMs,
                             anchor, actor.slideCoverage, scrape.levelDeadbandDb);
            if (out.size() == before) {
                continue;
            }
            // Identity rather than placement, so it stays here: the slot is held
            // for the life of the loop and the renderer ignores a variant on an
            // update, which is what stops a file swapping under a running voice.
            Proposal& proposal = out.back();
            proposal.site = loop.site;
            proposal.surface = loop.surface;
        }
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

    /// One catch: the moment a limb snags mid-slide and lets go again.
    ///
    /// A slide's character is its irregularity, and the loop has none - the
    /// reference recordings put sixty-five grit peaks a second on top of the
    /// rumble and our file has the rumble alone. These are the coarse half of
    /// that, the individual snags, and they are the single biggest thing between
    /// "a slide" and "a noise file".
    ///
    /// Measured against how hard the slide has been grinding *before this tick*,
    /// which is the only baseline that makes the test mean anything: fold the
    /// contact in first and every contact is its own baseline and none of them is
    /// a catch.
    void ProposeGrain(const StrategyContext& ctx, const Contact& contact,
                      ProposalList& out) const {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;
        if (!scrape.grainEnabled || contact.selfContact) {
            return;
        }
        const float baseline = std::max(1.0f, actor.slideTangentBeforeTick);
        if (contact.tangentSpeed < scrape.grainCatchRatio * baseline) {
            return;
        }
        if (!Ancient(actor.lastGrainMs) &&
            static_cast<float>(contact.timeMs - actor.lastGrainMs) < scrape.grainMinGapMs) {
            return;
        }
        if (ctx.rng.Unit() > std::clamp(scrape.grainProbability, 0.0f, 1.0f)) {
            return;
        }
        actor.lastGrainMs = contact.timeMs;

        // An onset of its own rather than a ride-along: there is no composite
        // under a catch to be an accessory to - the loop is not an onset - and a
        // ride-along with no parent is dropped. It goes through the arbitrator
        // like everything else and is paid for out of `Slide`'s own grain budget,
        // which is the line `iSlideMaxCues` was named for.
        Proposal proposal = FromContact(contact);
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
        proposal.levelDb = contact.onsetGainDb + scrape.grainGainDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = SlotId::kScrapeGrain;
        proposal.layers[0].gainDb = proposal.levelDb;
        proposal.layers[0].reason = CueReason::kScrape;
        proposal.layers[0].pitch =
            1.0f + scrape.grainPitchScatter * (ctx.rng.Unit() * 2.0f - 1.0f);
        out.push_back(proposal);
    }
};

/// The airborne anticipation rise, and nothing else.
///
/// The cloth bed that used to sit under every ragdoll went with its slot:
/// `bFoleyCloth = 0` in all thirty-eight saved configs, so it was never once
/// heard on purpose. The name still fits what is left - an air whoosh driven
/// by how the body is moving is motion foley.
class MotionFoleyStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "MotionFoley"; }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const MotionFoleyConfig& foley = ctx.cfg.strategies.foley;
        ActorRuntime& actor = ctx.actor;

        // Skipped for your own ragdoll: you are the one moving and the view
        // already tells you.
        const bool wantsRise = foley.enabled && foley.airborneRise && actor.haveBodyPoint &&
                               actor.state.airborne &&
                               actor.state.tier == DistanceTier::kFull &&
                               !(actor.isPlayer && ctx.cfg.player.skipAirborneWhoosh);
        if (wantsRise) {
            EmitLoopProposal(ctx, out, actor.riseRunning, actor.riseVoice, actor.riseLastDb,
                             actor.riseAnchor, SlotId::kAirWhoosh, foley.airborneRiseGainDb, 1.0f,
                             CueReason::kAirborneRise, 150.0f, BodyAnchor(actor),
                             ActorClassCoverage(ctx));
        } else if (actor.riseRunning) {
            StopLoopProposal(ctx, out, actor.riseRunning, actor.riseVoice, SlotId::kAirWhoosh,
                             CueReason::kAirborneRise, 200.0f);
        }
    }
};

/// Damage from being worked on, rather than from one bad landing.
///
/// A separate strategy and not a fifth tier, because it reads a different thing:
/// the tiers judge *this contact* and this judges *this limb's recent history*.
/// The measured case is a head bashed against a wall twenty-four times whose
/// hardest contact is 371 u/s against a 432 gate - every tier is right to refuse
/// every one of them, and the skull should still come apart.
///
/// Nothing here reads the tier config, the violence window or the garment. It
/// keeps its own pool per limb, its own ladder and its own budget, and the only
/// thing it borrows is which slot a site's crunch plays.
class AccumDamageStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "AccumDamage"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const AccumDamageConfig& acc = ctx.cfg.strategies.accum;
        if (!acc.enabled || contact.limbIndex >= kMaxLimbs) {
            return false;
        }
        ActorRuntime& actor = ctx.actor;

        // The two scope switches, and both of them are read *here* - before the
        // pool is touched - rather than down beside the break.
        //
        // A pool that fills on a contact the ladder can never fire on is worse
        // than useless: it is damage banked against the moment the switch stops
        // applying, so a leg worked on for ten seconds with `bHeadOnly` set
        // would still be sitting one contact from a break the instant somebody
        // turned it off, and a body dropped out of the player's hands would
        // carry a full pool into the fall that follows. Scoping the *measurement*
        // is what makes these mean what they say.
        //
        // Nothing else has to be undone for that to hold. Healing is a function
        // of elapsed time and the stamp it reads is only written on a contact
        // that got past here, so a limb that spends ten seconds out of scope
        // heals across all ten of them on the first contact that comes back in.
        if (acc.headOnly && DamageSiteFor(contact.site) != DamageSite::kHead) {
            return false;
        }
        if (acc.requireHeld && !actor.heldByPlayer) {
            return false;
        }

        ActorRuntime::AccumTrack& track = actor.accum[contact.limbIndex];

        // Heal first, against this limb's own clock. Per limb rather than per
        // actor because the limbs are hit at different times and a shared stamp
        // would heal an arm on the tick a leg was struck.
        if (!Ancient(track.lastMs)) {
            const auto dt = static_cast<float>(ctx.nowMs - track.lastMs) * 0.001f;
            if (dt > 0.0f) {
                track.pool *= std::exp(-dt / std::max(0.001f, acc.healMs * 0.001f));
            }
        }
        track.lastMs = ctx.nowMs;

        // What this contact was worth. Measured on intensity rather than closing
        // speed: the pool is counting how much of a beating a limb has taken,
        // and intensity is the figure that already accounts for mass, radius and
        // coupling. The floor is what keeps a settling scrabble from breaking a
        // bone on its own after long enough.
        const float over = contact.intensity - acc.ignoreBelowIntensity;
        if (over > 0.0f) {
            track.pool = std::min(acc.maxPool, track.pool + over * acc.perHitScale);
        }
        actor.stats.AccumPeak(track.pool);
        if (ctx.stats != nullptr) {
            ctx.stats->AccumPeak(track.pool);
        }

        // Down the ladder as it heals, so a limb left alone and then attacked
        // again climbs it a second time rather than being spent for ever - but
        // only once it has fallen *well* below the rung, never merely below it.
        // Without the margin a pool resting near a threshold steps down and back
        // up on alternate contacts and fires the same rung on a loop, which read
        // as 51 breaks on one take before this was here.
        while (track.stage > 0 &&
               track.pool < StageAt(acc, track.stage - 1) * std::clamp(acc.rearmFrac, 0.0f, 1.0f)) {
            --track.stage;
        }

        // ...and up it, one rung at a time. One rung per contact even when a
        // single hit clears two: each rung is a separate break and two arriving
        // on one frame is a stack, not a sequence.
        const AccumDamageStageConfig* rung = Rung(acc, track.stage);
        if (rung == nullptr || rung->atDamage <= 0.0f || track.pool < rung->atDamage) {
            return false;
        }

        // **Reaching a rung arms the limb; it does not break it.** The pool says
        // this limb is ready to go, and then it waits - held at the rung, taking
        // more damage, doing nothing - until a blow arrives with enough in it to
        // actually finish the job.
        //
        // Without this the break lands on whichever contact happened to tip the
        // arithmetic over, and that is very often a nothing: the twenty-fourth
        // gentle scuff of a beating, arriving at the same intensity as the
        // twenty-third, which produced no sound at all. A bone going is the
        // loudest thing in the mod and it has to land on a hit somebody can see.
        //
        // Being armed is a *property of the pool* rather than a flag, which is
        // what makes it heal correctly for free: a limb left alone drops back
        // under the rung and is simply no longer ready, with no second piece of
        // state to keep in step with the first.
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
/// A bypass, like the other two loops - not an onset, not a ride-along. It never
/// claims a contact, it cannot suppress one, and no arbitration rule sees it. So
/// it can fill the gaps the arbitrator makes without ever competing for them,
/// which is the whole reason a bed is allowed to be continuous.
///
/// Everything about *when* it plays is read off the two axes rather than decided
/// here: the level comes from `CrashState::rustleDrive`, which Stage 1 measured,
/// and the only thing this strategy adds on top is the slide duck. A strategy
/// deciding for itself when a body is tumbling is exactly the mistake the scrape
/// loop made with its own duration and speed gates.
class ClothRustleStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ClothRustle"; }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const RustleConfig& rustle = ctx.cfg.strategies.rustle;
        ActorRuntime& actor = ctx.actor;

        const float drive = actor.state.rustleDrive;

        // Every reason there might be nothing to play, in one test.
        //
        // `haveBodySamples` is not optional: without a pose sidecar every field
        // the drive is built from is zero, and zero is a lie rather than a
        // measurement. The layer switches off rather than guessing, exactly as
        // the hero test's arrival clause does.
        //
        // `CanSound` matters more here than for most layers. A loop proposed for
        // a slot nobody has recorded still books a voice id and still sends a
        // stop cue for a sound that never started, and with `cloth_rustle`
        // shipping empty that is the *default* path.
        // Animated mode's own switch sits with the rest of them rather than
        // above: like every other clause here it has to be re-asked every tick,
        // because a loop that was running when it went false is stopped by
        // `alive` going false and by nothing else.
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

        // Stopped at the bottom of its own ramp *and* under the voice floor,
        // which is the lesson the body grind paid for: a loop started under the
        // floor has its cue dropped by Stage 5 while the strategy has already
        // marked it running, and every update after that addresses a voice the
        // renderer never opened.
        if (!alive || gainDb < ctx.cfg.mix.voiceFloorDb) {
            if (actor.rustleRunning) {
                StopLoopProposal(ctx, out, actor.rustleRunning, actor.rustleVoice,
                                 SlotId::kClothRustle, CueReason::kRustle, rustle.stopFadeMs);
            }
            return;
        }

        // Pinned at start, like the grind's slot is. The class picks the
        // conditional variant, so letting it move mid-fall would swap the file
        // under a running voice - a click, and for a change nobody asked for.
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
    /// The class the whole actor answers to. `bodyCoverage` - the torso's - is
    /// the right source here whatever `[Armor] iActorClassSource` says about the
    /// other actor-level cues: a garment is what the body is wearing, and the
    /// slide's coverage is meaningless for a body that is not sliding.
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

    /// How far the running grind pulls the garment down.
    ///
    /// Scaled by the grind's own weight rather than switched by the slide state,
    /// so it arrives with the body loop instead of stepping in when the motion
    /// axis changes its mind. Zero when the body loop is not carrying anything,
    /// which is the case a limb-only skid is.
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
    ///
    /// Phased on the actor's own ragdoll start rather than on the engine clock,
    /// so two bodies falling side by side do not breathe in unison - and, more
    /// importantly, so the wobble is a function of the take rather than of when
    /// the take happened to be replayed. A random walk would be the obvious
    /// implementation and is the wrong one: anything drawing per tick re-rolls
    /// every variant and scatter downstream of it, and two exports would then
    /// differ everywhere rather than where the edit bit.
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
    // Contacts in against audible moments out. The design's 10:1 - the number
    // that says whether suppression is doing its job.
    return bursts == 0 ? 0.0f : static_cast<float>(contactsIn) / static_cast<float>(bursts);
}

// ═════════════════════════════════════════════════════════════════════════════

struct Engine::Impl {
    AlgorithmConfig cfg{};
    SoundBank* bank{};
    ICueSink* sink{};
    bool tracing{};
    Rng rng;
    std::uint32_t nextVoiceId{1};
    /// Sequence numbers for the contacts the engine makes up rather than
    /// receives - today just the slide-end impact. Its own range, well above
    /// anything a feed will hand out, so a synthetic contact can never collide
    /// with a real row in the variant shuffle, the accepted set or the trace.
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
    /// budget - nothing is refused against it - a leak detector.
    std::vector<Voice> liveVoices;

    // Stage 3 in the order it runs. ScrapeLoop first because it is the only one
    // that claims; Damage after the composite because it only ever rides along.
    //
    // Order no longer decides anything about damage, and that is the point of the
    // merge: the head and the body used to be two rules spending one budget, so
    // whichever reached a contact first won, and a body crunch that was proposed
    // and then dropped could silence the skull behind it. Six budgets, one rule.
    ScrapeLoopStrategy scrape;
    HeadImpactStrategy head;
    ImpactCompositeStrategy composite;
    DamageStrategy damage;
    MotionFoleyStrategy foley;
    /// Last, and it does not matter that it is: it claims nothing, reads nothing
    /// another strategy writes, and bypasses arbitration. Placed beside the
    /// other continuous bed for the same reason `foley` sits where it does.
    ClothRustleStrategy rustle;
    /// Its own strategy, not a fifth tier: the tiers judge one contact and this
    /// judges a limb's history. After `damage` so a contact that breaks a bone
    /// on its own merits has already been judged on them.
    AccumDamageStrategy accum;
    std::array<IStrategy*, 7> strategies{};

    Impl() {
        drained.reserve(256);
        contacts.reserve(128);
        proposals.reserve(128);
        order.reserve(128);
        acceptedSeqs.reserve(128);
        actors.reserve(8);
        liveVoices.reserve(kVoiceReserve);
        strategies = {&scrape, &head, &composite, &damage, &accum, &foley, &rustle};
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

    /// Our own anatomical mass per limb, and the body's total.
    ///
    /// Read from the profile because that is the half that resolved every bone
    /// name to a site, and never from the solver's masses - those are asymmetric
    /// enough that the same movement on a right arm would count for three times
    /// what it does on a left one (07 §6).
    ///
    /// A body whose profile never arrived keeps a total of zero, and the contact
    /// fraction then reads zero for ever. That is the quieter answer on purpose:
    /// no measurement means limb-only, never a guess at loud.
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
            // How much garment, which is a different question from how much
            // body - see FabricWeight. Seeded from the profile because that is
            // the half that resolved the bone name to a site *and* carries what
            // is worn on it; a limb whose profile never arrived keeps 0, so it
            // contributes no garment rather than a guessed one.
            actor.limbs[i].fabric =
                FabricWeight(profile->limbs[i].site, profile->limbs[i].coverage);
            actor.fabricTotal += actor.limbs[i].fabric;
            // The first torso body - COM, pelvis or a spine - and the first only,
            // because a humanoid has four of them and they are all equally "the
            // body". Nothing here indexes: `site` came off the bone name, which is
            // the half that survives a skeleton we have never seen.
            if (!haveBodyLimb && profile->limbs[i].site == LimbSite::kTorso) {
                actor.bodyLimb = static_cast<std::uint16_t>(i);
                actor.bodyCoverage = profile->limbs[i].coverage;
                haveBodyLimb = true;
            }
        }
    }

    ActorRuntime& Acquire(ActorId id, const ActorProfile* profile) {
        if (ActorRuntime* existing = Find(id); existing != nullptr) {
            // A profile that arrived after the actor did, or a ragdoll rebuilt
            // onto a different skeleton. Guarded on not already knowing, so the
            // ordinary path is one compare.
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
        // Before the summary, so a loop that was still running when the actor let
        // go is stopped rather than stranded. Only the motion axis's own route
        // to Resting stops a loop, and an NPC who gets up mid-tumble never takes it:
        // `ragdoll_end` arrives in Ingest, which runs before the strategies, so
        // the actor is gone by the time MotionFoley would have asked for the stop.
        // What is left behind is a voice that never expires, a scrape or a bed
        // still playing in the renderer, and no way to reach either again.
        StopActorLoops(actor, nowMs);

        if (actor.stats.contactsIn > 0) {
            log::Summary(actor.name, actor.stats,
                         actor.lastAdmittedMs - actor.firstContactMs);
        }
        spdlog::debug("actor {} dropped: {}", actor.name, why);

        // Belt and braces on the sweep above: whatever this actor still owns goes
        // back to the budget, whether or not it came from one of the three loops.
        // Read before the state is cleared - CrashState{} takes the actor id with
        // it, and sweeping on a zeroed id would drop somebody else's voices.
        const ActorId id = actor.state.actorId;
        std::erase_if(liveVoices, [id](const Voice& v) { return v.actorId == id; });

        actor.inUse = false;
        actor.state = CrashState{};
    }

    /// Every loop this actor still holds, stopped for real: a cue to the sink so
    /// the renderer lets its voice go, and the budget entry given back.
    void StopActorLoops(ActorRuntime& actor, TimeMs nowMs) {
        StopOneLoop(actor, nowMs, actor.riseRunning, actor.riseVoice, SlotId::kAirWhoosh,
                    CueReason::kAirborneRise, 200.0f);
        StopOneLoop(actor, nowMs, actor.scrapeRunning, actor.scrapeVoice, actor.scrapeSlot,
                    CueReason::kScrape, cfg.strategies.scrape.stopFadeMs);
        StopOneLoop(actor, nowMs, actor.armorSlideRunning, actor.armorSlideVoice,
                    actor.armorSlideSlot, CueReason::kArmorSkin,
                    cfg.strategies.scrape.stopFadeMs);
        StopOneLoop(actor, nowMs, actor.rustleRunning, actor.rustleVoice, SlotId::kClothRustle,
                    CueReason::kRustle, cfg.strategies.rustle.stopFadeMs);
        for (auto& loop : actor.limbLoops) {
            StopOneLoop(actor, nowMs, loop.running, loop.voice, loop.slot, CueReason::kScrape,
                        cfg.strategies.scrape.stopFadeMs);
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
        // already run this tick, and a stop is not a thing the arbitrator may
        // decline anyway.
        Cue cue{};
        // The release, not the last contact it admitted. A loop is stopped
        // *now* - that is the whole point of this path - and the two are not
        // the same instant: an actor who gets up mid-slide was last hit some
        // frames before `knock_get_up` arrived, while its grind went on
        // emitting updates at every tick in between.
        //
        // Stamping the stop with `lastAdmittedMs` put it in the past, behind
        // those updates. That survives a renderer which reads cues in arrival
        // order - the game's does, and never saw this - but the offline runner
        // sorts the cue list by time before it renders, so the stop landed in
        // front of its own updates and the mixer, which ends a loop on the last
        // point it holds, found no stop there to end it on. One grind and two
        // limb scrapes left running for the remaining two minutes of the take.
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

    /// How much of the body is arriving through this contact, 0 to 1.
    ///
    /// A limb travelling with the body barely rotates. A limb whipping round a
    /// joint rotates hard, and carries only its own mass into the surface. So
    /// compare the speed the limb's *surface* is turning at (angular x radius)
    /// against the speed its centre is moving at: mostly turning is a flail,
    /// mostly moving is the body arriving.
    ///
    /// Backward-looking only - every term is from this one contact - so it
    /// behaves identically live and in replay.
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
                                  float angularSpeed) const {
        const IntensityConfig& in = cfg.intensity;
        const float span = std::max(1.0f, in.speedRefHigh - in.speedRefLow);
        float normalised = std::max(0.0f, (impactSpeed - in.speedRefLow) / span);

        // Soft-clip rather than reject: other mods' impulses arrive far outside
        // anything physical, and a silent obliterate is the worst outcome there
        // is.
        if (normalised > in.softClipKnee) {
            const float room = std::max(0.05f, 1.0f - in.softClipKnee);
            normalised = in.softClipKnee + room * std::tanh((normalised - in.softClipKnee) / room);
        }
        float value = std::pow(std::clamp(normalised, 0.0f, 1.0f), in.curveExponent);

        // Size, from our own nominal mass table and the limb's bounding radius,
        // never the solver's masses - those are asymmetric enough to make the
        // right arm three times louder than the left for identical movement.
        constexpr float kReferenceMass = 2.5f;
        constexpr float kReferenceRadius = 14.5f;

        // Which mass is behind this contact - see EffectiveMassConfig. Off, the
        // limb's own; on, blended towards the body's by how much of the limb's
        // motion is translation rather than rotation. The radius term below is
        // left alone either way: limb *size* is a legitimate timbre difference
        // and it is worth about a decibel, not twenty.
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
    /// GlancingImpactConfig. Neutral for anything the rule does not judge, so it
    /// costs nothing when off.
    struct GlanceCut {
        float ramp{};                 ///< 0 a square landing, 1 fully glancing
        float intensityScale{1.0f};   ///< class and rank
        float gainDb{};               ///< level and rank, before arbitration
        float trimDb{};               ///< level only, after arbitration
    };

    [[nodiscard]] GlanceCut Glance(LimbSite site, float impactSpeed, float tangentSpeed,
                                   float bodySpeed) const {
        const GlancingImpactConfig& g = cfg.glancing;
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

        // ...but back off as the contact stops looking like a landing and starts
        // looking like a slide. Both are mostly-tangential; only a slide keeps
        // running sideways at many times its closing speed.
        const float ratio = tangentSpeed / std::max(1.0f, impactSpeed);
        const float slideSpan = std::max(0.01f, g.slideRatioFull - g.slideRatioStart);
        ramp *= 1.0f - std::clamp((ratio - g.slideRatioStart) / slideSpan, 0.0f, 1.0f);

        cut.ramp = ramp;
        cut.intensityScale = Lerp(1.0f, std::clamp(g.maxIntensityScale, 0.0f, 1.0f), ramp);
        cut.gainDb = g.maxGainCutDb * ramp;
        cut.trimDb = g.maxTrimCutDb * ramp;
        return cut;
    }

    /// The whole range onto about 35 dB. The references span 13-17 dB across
    /// their onsets with the bed 30-36 dB under the hero hit; a naive log curve
    /// would give 60 and sound wrong at both ends.
    [[nodiscard]] float GainFromIntensity(float intensity) const {
        return -cfg.intensity.dynamicRangeDb * (1.0f - intensity);
    }

    /// The Stage 5 half of the same curve - see PostIntensityConfig.
    ///
    /// Returns a *difference*: what the level would have been under the post
    /// numbers, minus what GainFromIntensity already charged for it. That is
    /// what makes it addable at the end, and what makes neutral defaults cost
    /// exactly nothing rather than something within a rounding error of it.
    [[nodiscard]] float PostShapeDb(float intensity) const {
        const PostIntensityConfig& post = cfg.intensity.post;
        const float raw = std::clamp(intensity, 0.0f, 1.0f);

        float shaped = raw;
        if (post.softClipKnee < 1.0f && shaped > post.softClipKnee) {
            const float room = std::max(0.05f, 1.0f - post.softClipKnee);
            shaped = post.softClipKnee + room * std::tanh((shaped - post.softClipKnee) / room);
        }
        shaped = std::pow(std::clamp(shaped, 0.0f, 1.0f), post.curveExponent);

        const float range = std::max(0.0f, cfg.intensity.dynamicRangeDb + post.extraRangeDb);
        return -range * (1.0f - shaped) - GainFromIntensity(raw);
    }

    /// What this actor may spend right now, from both Stage 2 axes.
    ///
    /// Motion owns the trim and the grain count; the hero latch overrides both
    /// while it is open. This one function is the entire coupling between the
    /// two axes, and keeping it to one function is why they could be split at
    /// all - "physics owns what the body is doing, design owns how loud the mix
    /// is" is a sentence about precedence, and this is the precedence.
    [[nodiscard]] const PhaseBudget& BudgetFor(const CrashState& state) const {
        if (state.moment == Moment::kHero) {
            return cfg.hero.budget;
        }
        return MotionBudgetFor(state);
    }

    /// The motion half on its own, with the hero latch not consulted.
    ///
    /// What a bed is levelled by. The split exists because the two axes answer
    /// different questions and only one of them is about a bed: physics owns
    /// what the body is doing, and a body that is grinding along the floor is
    /// grinding along the floor whether or not the hit that just landed was the
    /// event of the fall.
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

    /// The role trim - every surface skin together, every grain together. The
    /// balance between kinds of layer, which is what the composite is about.
    ///
    /// Which kind a slot is now comes from `SlotDesc::family` rather than from a
    /// list of slot names kept in step by hand. The three composite layers are
    /// the exception and keep a trim each, because the balance *between* them is
    /// the thing the composite is: they are not three of a kind, they are the
    /// kind.
    [[nodiscard]] float RoleTrimDb(SlotId slot, bool isPlayer) const {
        switch (slot) {
            case SlotId::kImpTransient:
                return cfg.mix.transientTrimDb;
            // Both body layers, because the role trim is what balances the body
            // against the transient and the sub - a decision about the shape of
            // the composite, which does not change with which wav carries the
            // mass. Missing the second case here is silent: the family switch
            // below returns 0 for kImpact, so a limb composite would quietly lose
            // the whole body trim rather than fail to build.
            case SlotId::kImpBody:
            case SlotId::kImpBodyLimb:
                return cfg.mix.bodyTrimDb;
            case SlotId::kImpSub:
                // A 30 Hz boom at zero distance through headphones is
                // overwhelming, and in VR low frequency is felt as much as heard.
                return cfg.mix.subTrimDb + (isPlayer ? cfg.player.subTrimDb : 0.0f);
            default:
                break;
        }
        switch (Slot(slot).family) {
            case SlotFamily::kSurface:
                return cfg.surfaces.trimDb;
            case SlotFamily::kArmor:
                return cfg.armor.trimDb;
            // An accent is a grain that arrives on its own. It has always taken
            // the grain trim; saying so by family rather than by name is the only
            // change here.
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

    /// How much rank a contact on this site is given over its own level - the
    /// only writer of `Proposal::priorityDb`'s weight half. See the priority
    /// block in `ArbitrationConfig`.
    ///
    /// Binned through `DamageSiteFor`, which is the engine's one site->part
    /// mapping: the damage tiers read it, `BodySlot` reads it, and this reads
    /// it. Three questions about which part of a body a contact is on, answered
    /// the same way, so the neck counts as the column everywhere and an
    /// unreadable skeleton counts as a limb everywhere.
    [[nodiscard]] float SiteWeightDb(LimbSite site) const {
        switch (DamageSiteFor(site)) {
            case DamageSite::kHead:  return cfg.arb.headWeightDb;
            case DamageSite::kSpine: return cfg.arb.torsoWeightDb;
            case DamageSite::kLimb:  break;
        }
        return cfg.arb.limbWeightDb;
    }

    /// The per-file trim - see SlotGainConfig. The two declared-and-unfilled
    /// slots have none, for the same reason they have no mute: nothing ever
    /// resolves to them, so a slider for either would be a control over silence.
    [[nodiscard]] float SlotTrimDb(SlotId slot) const {
        const SlotGainConfig& g = cfg.slotGains;
        // A variant of another slot takes that slot's trim, and the manifest is
        // where that is recorded. See `SlotDesc::trimsWith`.
        //
        // The surface skins come out of the surfaces list rather than out of
        // SlotGainConfig, and there are thirteen of them, so they are a table
        // lookup ahead of the switch - exactly as their mutes are in
        // `LayerMute`.
        if (const SurfaceClass surface = SurfaceOfSlot(TrimOwner(slot));
            surface != SurfaceClass::kCount) {
            return cfg.surfaces.Skin(surface).trimDb;
        }
        switch (TrimOwner(slot)) {
            case SlotId::kImpTransient: return g.impTransient;
            case SlotId::kImpBody:      return g.impBody;
            // A trim of its own, for the reason the three crunches have one: two
            // separate recordings arrive at two different levels. It shares
            // `imp_body`'s *mute*, because silencing the body layer is one
            // decision - which is the same split `mutesWith` and `trimsWith` are
            // two columns for.
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
            // The surface-coloured grinds arrive here as the loop they colour.
            // Balancing one against the other is what the surface panel's own
            // trims are for.
            case SlotId::kScrapeLoop:   return g.scrapeLoop;
            case SlotId::kScrapeLimb:   return g.scrapeLimb;
            case SlotId::kAirWhoosh:    return g.airWhoosh;
            case SlotId::kHeadImpact:   return g.headImpact;
            case SlotId::kSettleRest:   return g.settleRest;
            default:                    return 0.0f;
        }
    }

    /// The per-part balance - see CompositeBalanceConfig. Which body part made
    /// this cue, crossed with which of the composite's four layers this is.
    ///
    /// Binned through `DamageSiteFor`, which is the same bin `BodySlot` uses to
    /// choose between `imp_body` and `imp_body_limb`. That is the point of
    /// reusing it rather than writing a third switch: the layer a limb *plays*
    /// and the trim a limb *gets* can never disagree about what counts as a limb,
    /// and the neck stays with the column in both.
    ///
    /// Silent for everything that is not one of the four: the taps, the crunches,
    /// the gore, the head accent and the loops are per-part somewhere better, and
    /// the armour skin is a different axis entirely.
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
        // Both body slots, for the reason `RoleTrimDb` takes both: which wav
        // carries the mass is a question about timbre, and this one is about how
        // much mass the part gets. Missing the second case would be silent.
        switch (slot) {
            case SlotId::kImpTransient: return part.transientTrimDb;
            case SlotId::kImpBody:
            case SlotId::kImpBodyLimb:  return part.bodyTrimDb;
            case SlotId::kImpSub:       return part.subTrimDb;
            default:                    break;
        }
        // The skins by family rather than by name, so a fourth surface is a
        // manifest row and not an edit here.
        return Slot(slot).family == SlotFamily::kSurface ? part.surfaceTrimDb : 0.0f;
    }

    /// Every trim that depends on the *layer* rather than on the proposal: what
    /// kind of layer it is, which file it resolved to, and which body part made
    /// it. One term with three arguments rather than three terms, because `Emit`
    /// must not grow a summand each time a rule learns to trim (01 §5).
    [[nodiscard]] float LayerTrimDb(SlotId slot, LimbSite site, bool isPlayer) const {
        return RoleTrimDb(slot, isPlayer) + SlotTrimDb(slot) + SiteBalanceDb(slot, site);
    }


    /// Takes the whole crash state rather than one enum, because Stage 2 has two
    /// axes now and a trace that recorded only one of them could not explain
    /// why a quiet-looking contact came out loud.
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

    /// Record a voice as being in flight. It cannot fail: nothing is capped
    /// against this list any more - see "There is no voice budget" above.
    ///
    /// A one-shot books for as long as its file runs. A loop books `kNever` and
    /// is given back by name in ReleaseVoice, because a loop that has been
    /// playing for longer than its own file length has not finished, it has
    /// wrapped - expiring it by time would drop it out of the list while it was
    /// still audible and hide the leak the list exists to show.
    void TakeVoice(ActorRuntime& actor, TimeMs endsMs, std::uint32_t voiceId) {
        liveVoices.push_back(Voice{endsMs, voiceId, actor.state.actorId});
    }

    /// Undo the booking made most recently.
    ///
    /// A one-shot's booking has no id to release it by - only loops carry one -
    /// and it does not need one: nothing else books between taking a proposal's
    /// voice and finding out whether the proposal made a sound, so the newest
    /// entry is always the one to undo.
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
            // Pose, not signal. It never becomes a Contact and never makes a
            // sound; it is the only measurement of where the body actually is,
            // and everything that wants to know whether the body is falling
            // reads what it accumulates here.
            //
            // Tick samples only. The older captures carry a limb_sample per limb
            // at ragdoll_start and ragdoll_end - two snapshots, a launch pose
            // rather than a signal - and taking those for pose would be worse
            // than having none: two frames 1.5 seconds apart set the "we have
            // measurements" flag, which turns off every fallback the take
            // actually needs, and then differences a velocity across a gap that
            // is not a frame.
            if (event.kind == EventKind::kLimbSample) {
                if (pose::IsTickSample(event)) {
                    HandlePose(event, feed);
                }
                continue;
            }
            if (event.kind != EventKind::kImpact) {
                // Touch and separate carry no contact point and both re-fire on
                // most frames of a persisting manifold; limb samples and listener
                // rows are state, not signal.
                continue;
            }
            // The gate. The ragdoll bodies exist and collide the whole time -
            // keyframed while the actor is animated - so without this the mod
            // plays impacts while NPCs walk around (07 §1).
            //
            // `GameIntegration:bAnimatedMode` is the switch that opens it on
            // purpose, to hear exactly that. The feed has its own copy of this
            // gate in the contact callback, and both have to be open for a
            // walking actor's contacts to arrive here at all.
            if (event.phase != ActorPhase::kRagdoll && !cfg.game.animatedMode) {
                continue;
            }

            const ActorProfile* profile = feed.Profile(event.actorId);
            ActorRuntime& actor = Acquire(event.actorId, profile);
            actor.phase = event.phase;

            // The floor, with the slide relief 01 §5's Admit stage is for.
            // Asked of the contact's own tangent speed and never of the motion
            // state: ingest runs before Stage 2, so the state here is last
            // tick's (01 §7.4). Off at the default - see `slideFloorFrac`.
            float floorSpeed = cfg.ingest.minImpactSpeed;
            if (cfg.ingest.slideFloorFrac < 1.0f) {
                const float from = cfg.ingest.minImpactSpeed;
                const float span =
                    std::max(1.0f, cfg.ingest.slideFloorAtTangent - from);
                const float ramp =
                    std::clamp((event.tangentSpeed - from) / span, 0.0f, 1.0f);
                floorSpeed *= Lerp(1.0f, std::clamp(cfg.ingest.slideFloorFrac, 0.0f, 1.0f), ramp);
            }
            if (event.impactSpeed < floorSpeed) {
                ++stats.rejectedBelowFloor;
                ++actor.stats.rejectedBelowFloor;
                continue;
            }
            if (IsBlowup(event)) {
                ++stats.rejectedBlowup;
                ++actor.stats.rejectedBlowup;
                spdlog::debug("seq {} rejected: blow-up, {:.0f} against {:.0f} u/s",
                              event.sourceSeq, event.impactSpeed, std::fabs(event.normalSpeed));
                continue;
            }

            // Stamped here, above every rule that decides whether this collision
            // is worth *hearing*, because the driven test is not asking that. It
            // asks whether the tick's acceleration has a collision in it to
            // explain it, and a self-hit explains one just as well as a world
            // hit: the centre is mass-weighted off our nominal table rather than
            // the solver's, so a limb swinging into the body's own torso does not
            // cancel out of it the way a true internal impulse would. On
            // devbench_5 that leak reads 554 u/s^2 at 6226 ms - over the driven
            // gate, with nothing outside the body touching it at all.
            actor.sawContactThisTick = true;

            const LimbInfo* limb = profile != nullptr ? profile->Limb(event.limbIndex) : nullptr;
            if (limb == nullptr) {
                // An unrecognised skeleton still sounds - sized off limbRadius -
                // rather than going silent, which is the whole point of not
                // indexing the limb set blindly (07 §7).
                spdlog::debug("seq {} has no limb {} in the profile; sizing off radius",
                              event.sourceSeq, event.limbIndex);
            }

            // Every self-collision fires twice - 624 of 624 ordered pairs had
            // their mirror in the same frame. Keeping the copy whose own body
            // sorts first is a decision each end can make alone.
            if (cfg.ingest.dropMirroredSelfContacts && event.otherLimb >= 0 && limb != nullptr &&
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
            // The collision layer is the reliable input and the material is an
            // enrichment that fails at the edges (07 §8), so the material is
            // preferred when there is one and the layer stands in when there is
            // not.
            contact.surface = event.otherMaterial != 0 ? SurfaceFromMaterial(event.otherMaterial)
                                                       : SurfaceFromLayer(event.otherLayer);
            contact.otherBody = event.otherBody;
            contact.selfContact = event.otherLimb >= 0;
            contact.graze = IsGraze(cfg.ingest, event.impactSpeed, event.tangentSpeed);
            contact.intensity = Intensity(event.impactSpeed, contact.site, contact.limbRadius,
                                          event.bodySpeed, event.angularSpeed);
            // A landing limb that only clipped the surface can be charged on
            // intensity - which also demotes it through the composite/tap branch
            // - or purely on level, or both. What it must never do is change how
            // big the fall itself looks, so the untouched figure is kept.
            contact.rawIntensity = contact.intensity;
            const GlanceCut glance =
                Glance(contact.site, event.impactSpeed, event.tangentSpeed, event.bodySpeed);
            contact.glanceScale = glance.intensityScale;
            contact.modTrimDb += glance.trimDb;
            contact.intensity = std::clamp(contact.intensity * glance.intensityScale, 0.0f, 1.0f);
            contact.onsetGainDb = GainFromIntensity(contact.intensity) + glance.gainDb;

            // All contact points of one manifold collapse to their max. Grouped
            // by (limb, other body) within the frame rather than bracketed by
            // manifold_first/manifold_last: 244 rows carry `last` with no
            // `first`, and both flags re-fire across the frames of a persisting
            // manifold.
            if (cfg.ingest.collapseManifolds) {
                bool merged = false;
                for (Contact& existing : contacts) {
                    // (this body, other body), which is what 07 §3 measured as a
                    // x1.07 over-count. Grouping by the limb alone instead merges
                    // a hand hitting the floor with the same hand hitting a
                    // thigh, which is two collisions and should be two decisions -
                    // and it collapses the very frames a landing is made of.
                    if (existing.actorId != contact.actorId ||
                        existing.limbIndex != contact.limbIndex ||
                        existing.otherBody != contact.otherBody) {
                        continue;
                    }
                    merged = true;
                    ++stats.collapsedManifold;
                    ++actor.stats.collapsedManifold;
                    // The fastest member wins outright, graze flag included.
                    // Carrying the flag over used to make it sticky, and a
                    // manifold that opened with a 217 u/s skim then took a 444 u/s
                    // slam on the same limb stayed classified a graze - so the
                    // scrape path claimed it and the slam made no sound at all.
                    // Whichever contact is loudest is the one being described.
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

        // Half of all contacts are one limb touching another limb of the same
        // body, and an arm brushing your own thigh makes no impact sound. Below
        // the threshold a self-contact is dropped; above it a genuine self-hit
        // gets through.
        std::size_t write = 0;
        for (std::size_t i = 0; i < contacts.size(); ++i) {
            const Contact& contact = contacts[i];
            if (contact.selfContact && contact.impactSpeed < cfg.ingest.selfContactThreshold) {
                ++stats.droppedSelfContact;
                if (ActorRuntime* actor = Find(contact.actorId); actor != nullptr) {
                    ++actor->stats.droppedSelfContact;
                }
                continue;
            }
            contacts[write++] = contacts[i];
        }
        contacts.resize(write);

        // Strongest first, ties broken by the row the contact came from: a
        // deterministic order that is a function of the input and not of the
        // container.
        std::ranges::stable_sort(contacts, StrongestFirst);
    }

    /// Reject blow-ups on the arithmetic, not on a threshold. impactSpeed is the
    /// solver's closing speed; normalSpeed is the same quantity recomputed from
    /// both bodies' motion state. A row where they disagree is a row the
    /// rigid-body arithmetic cannot reproduce, which is what a blow-up is (07 §2).
    [[nodiscard]] bool IsBlowup(const FeedEvent& event) const {
        if (event.normalSpeed != 0.0f) {
            // Magnitudes. normalSpeed comes out exactly negated on 23 % of good
            // rows, because which body of the pair the normal points away from is
            // not fixed; compared signed, a quarter of the dataset looks broken.
            const float ours = std::fabs(event.normalSpeed);
            const float theirs = std::fabs(event.impactSpeed);
            return std::fabs(ours - theirs) / std::max(1.0f, theirs) >
                   cfg.ingest.blowupDisagreeFrac;
        }
        // Backstops, and only for when there is no reconstruction to check
        // against. Both of the old guards were far too tight: 700 u/s rejects two
        // clean takes and 25 rad/s rejects 8.3 % of good rows.
        return event.impactSpeed > cfg.ingest.blowupSpeedCeiling ||
               event.angularSpeed > cfg.ingest.blowupAngularCeiling;
    }

    /// Fold one limb's pose into this tick's running centre.
    ///
    /// Mass-weighted rather than "whatever the body called COM reports": the
    /// ragdoll's own COM body is one rigid body among eighteen and it whips
    /// about on its constraints like any other, while the weighted centre of the
    /// whole set is what actually follows a ballistic arc. Our nominal table is
    /// the weight, never the solver's masses - those are asymmetric enough that
    /// the right arm would drag the centre sideways (07 §6).
    ///
    /// An unrecognised skeleton lands on kUnknown for every limb, which weights
    /// them all equally. That is a plain average of the body, which is a
    /// perfectly good centre and much better than going blind.
    void HandlePose(const FeedEvent& event, IFeed& feed) {
        const ActorProfile* profile = feed.Profile(event.actorId);
        ActorRuntime& actor = Acquire(event.actorId, profile);
        // The pose is the only row an actor who never collides with anything
        // carries, so in animated mode it is what tells the rustle which of the
        // two worlds this actor is in.
        actor.phase = event.phase;
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

        // Kept per limb as well as summed, because the body grind has to hang on
        // *a* bone and the only way to say which one is nearest the contact is to
        // know where each of them is. Costs one copy per limb per tick and it is
        // the same data the centre is already being built out of.
        if (event.limbIndex < kMaxLimbs) {
            ActorRuntime::LimbTrack& track = actor.limbs[event.limbIndex];
            track.pos = event.position;
            track.havePos = true;
            if (track.mass <= 0.0f) {
                track.mass = mass;
            }
            // Rotation, kept rather than dropped. It has been published on every
            // limb row all along and read only for contacts; the garment wants
            // it because a limb spinning at a *steady* rate drags its sleeve
            // continuously with no acceleration in it at all.
            //
            // The velocity lands in `tickVel`; ConsumePose differences it
            // against `vel` and then promotes it, so the fold cannot overwrite
            // its own input.
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
            // Only for an actor that is not already falling. A second
            // `ragdoll_start` mid-tumble is the game re-arming a knockdown that
            // never finished, and restarting the run on it would reset the burst
            // budget and the hero count in the middle of a fall. A fresh
            // `Acquire` is already in `Launch`, so this only has to leave a
            // running one alone.
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
        if (state == "held_start" || state == "held_stop") {
            // Find rather than Acquire: a hold is not a reason to open a
            // knockdown. The feed only publishes these for an actor it is already
            // tracking, and it re-publishes a live hold on the far side of a
            // `ragdoll_start` - which is the one edge that gives the actor a
            // fresh runtime with the flag cleared.
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
    /// Two independent signals summed over the limbs and weighted by how much
    /// *garment* each carries rather than how much body it is:
    ///
    ///   thrash  how hard the limb is accelerating relative to the body, u/s^2
    ///   tumble  how fast its surface is moving from rotation, u/s
    ///
    /// Each is normalised through its own ramp before they are blended, because
    /// the two are not commensurable - one is an acceleration and the other a
    /// speed - and adding them raw would bury a unit conversion inside a weight.
    ///
    /// Called from ConsumePose with the body's own acceleration, which is the
    /// reference the relative term subtracts and is not stored anywhere else.
    void ConsumeRustle(ActorRuntime& actor, TimeMs nowMs, float dt, const Vec3& bodyAccel) {
        const RustleConfig& rust = cfg.strategies.rustle;
        const DamageViolenceConfig& viol = cfg.strategies.damage.violence;
        // **Two measurements, not one shared one.** They read the same pose
        // stream and they weight it differently, because they are asking
        // different questions: the garment asks how much *cloth* is moving and
        // weights each limb by how much of it hangs there; violence asks how
        // much *body* is being thrown about and weights by anatomical mass.
        //
        // The first build of this had damage reading the garment's figure, which
        // was wrong twice over - a naked body breaks as well as a clothed one,
        // and tuning the rustle silently moved how often bones broke.
        //
        // The per-limb work below is shared because the *acceleration* is one
        // number however it is later weighted; everything downstream of it is
        // computed twice, through two sets of ramps.
        if (!rust.enabled && !viol.enabled) {
            // Off means nothing is measured, not merely nothing is played. Every
            // field stays 0 and the corpus replays byte-identically.
            return;
        }

        // A moving average and **not** a peak-hold, which is a correction rather
        // than a preference: measured on the corpus, a peak-hold saturated at
        // 1.00 on twelve of the thirteen takes. That is what a maximum does - a
        // knockdown always contains one very violent instant, so "the worst
        // moment of the last half-second" is nearly always the top of the range,
        // and a term that reads 1.0 through every fall is not a term, it is a
        // constant offset on the thresholds.
        //
        // The question is "has this been violent", not "did something violent
        // happen", so the answer is an average. It falls as well as rises, so a
        // fall that was bad and has calmed reads as calm.
        const float holdTau = std::max(1.0f, viol.holdMs) * 0.001f;
        const float decay = dt > 0.0f ? std::exp(-dt / holdTau) : 1.0f;
        const float rise = 1.0f - decay;
        // **The asymmetry that makes this measurement mean anything.** A limb
        // hitting stone has its velocity reversed inside one solver step, so on a
        // contact tick the thrash *is* the collision restated - and a violence
        // figure allowed to rise there would hand every contact its own impact
        // speed back as evidence that the fall was violent. See 01 §7.4 on the
        // `driven` gate, which failed in exactly this way and for exactly this
        // reason.
        const bool mayRise = !actor.sawContactThisTick;

        // The garment's sums, weighted by cloth.
        float thrashSum = 0.0f;
        float tumbleSum = 0.0f;
        float fabricSum = 0.0f;
        // Violence's sums, weighted by anatomical mass. Separate accumulators
        // rather than one scaled afterwards: the weights differ per limb, so
        // there is no factor that turns one of these into the other.
        float violThrashSum = 0.0f;
        float violTumbleSum = 0.0f;
        float massSum = 0.0f;

        for (std::size_t i = 0; i < actor.limbCount && i < kMaxLimbs; ++i) {
            ActorRuntime::LimbTrack& track = actor.limbs[i];
            const bool sampled = track.haveTickVel;
            // Consumed either way: a limb that misses a tick must not have last
            // tick's velocity read as this tick's.
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
            // The same guard the centre uses, for the same reason: a gap far
            // longer than a frame is a rebuilt ragdoll or a resumed replay, not
            // an enormous acceleration. Re-seed across it rather than
            // differencing.
            const bool usable = track.haveVel && limbDt > 0.0f && limbDt <= 0.5f;

            if (usable) {
                Vec3 a{(track.tickVel.x - track.vel.x) / limbDt,
                       (track.tickVel.y - track.vel.y) / limbDt,
                       (track.tickVel.z - track.vel.z) / limbDt};
                // Violence is always relative - free fall is not violence - and
                // the garment has a switch only because it is worth an A/B
                // there. Where they agree, one subtraction serves both.
                Vec3 rel{a.x - bodyAccel.x, a.y - bodyAccel.y, a.z - bodyAccel.z};
                const float absLen = Length(a);
                const float relLen = Length(rel);

                // Clamped per limb and before the sum. Pose has no equivalent of
                // ingest's blow-up rejection, so a teleported limb would
                // otherwise dominate the whole body; and in the ordinary case it
                // is what keeps one limb striking stone from saturating a
                // measurement the other seventeen are still contributing to.
                // Two ceilings, because the two consumers may want different
                // tolerances for the same absurd number.
                thrashSum +=
                    track.fabric * std::min(rust.relativeToBody ? relLen : absLen,
                                            rust.thrashCeiling);

                const float violThrash = std::min(relLen, viol.thrashCeiling);
                violThrashSum += track.mass * violThrash;

                // This limb's own violence, off the raw acceleration rather than
                // any weighted share of it: a bare arm breaks exactly as well as
                // a sleeved one, and a light one no less than a heavy one. It is
                // the only figure here that is unweighted, and deliberately -
                // `limbShare` asks about *this limb*, not about its contribution
                // to the body.
                if (mayRise) {
                    const float sample = RampAt(violThrash, viol.thrashFloor, viol.thrashFull);
                    track.violence += (sample - track.violence) * rise;
                }
            }

            // Surface speed, not radians. The same product `Coupling` takes, and
            // for the same reason: what drags a sleeve across an arm is how fast
            // the surface is travelling - and, for the other consumer, how much
            // the limb is being wrung about its own axis.
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
            // Nothing sampled this tick is two different questions. An actor
            // wearing nothing we can name has no measurement and stays at zero -
            // the quieter answer, never a guess at loud. An actor whose next pose
            // frame has simply not landed yet must not have the last one thrown
            // away: zeroing it drew a sawtooth at the pose rate into the raw
            // drive, and through the near-instant attack into the level itself.
            if (actor.fabricTotal <= 0.0f) {
                state.rustleDriveRaw = 0.0f;
            }
        } else {
            const float inv = 1.0f / fabricSum;
            const float thrashMean = thrashSum * inv;
            const float tumbleMean = tumbleSum * inv;
            // The two raw means at their peak, before either ramp. This is the
            // measurement the ramps are guesses for - see EngineStats.
            const auto tally = [&](EngineStats& into) {
                into.rustleThrashPeak = std::max(into.rustleThrashPeak, thrashMean);
                into.rustleTumblePeak = std::max(into.rustleTumblePeak, tumbleMean);
                // A running mean rather than a sum, so the field means the same
                // thing whether it is read mid-take or at the end and cannot
                // overflow on a long one.
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
            // Speed multiplies and air adds, and the asymmetry is the argument.
            // A body drifting fast and limply must stay silent, so speed can only
            // ever scale something the limbs are already doing; a free fall makes
            // the relative term read nothing by construction, so if it is to be
            // heard at all it has to come from outside that product.
            raw *= 1.0f + rust.speedWeight * RampAt(state.bodySpeed, 0.0f, rust.speedForFull);
            if (rust.airWeight > 0.0f && state.airborne) {
                raw += rust.airWeight *
                       RampAt(std::fabs(state.verticalSpeed), 0.0f, rust.airSpeedForFull);
            }
            state.rustleDriveRaw = std::clamp(raw, 0.0f, 1.0f);
        }

        // The body-wide violence, off its own mass-weighted sums and its own
        // ramps. It shares the pose stream with the garment and nothing else -
        // no fabric weighting, no speed multiplier, no airborne term, because
        // none of those describe how badly a body is being dealt with.
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

        // The envelope, asymmetric on purpose. Fast up because cloth responds
        // immediately; slow down because a garment carries on moving, which is
        // what fills the gaps between bounces and what leaves a fabric tail
        // behind every impact.
        const float target = state.rustleDriveRaw;
        const float tauMs = target > state.rustleDrive ? rust.attackMs : rust.releaseMs;
        const float tau = std::max(1.0f, tauMs) * 0.001f;
        // A time constant and not a per-tick fraction, so the envelope is the
        // same shape at 24 fps and at 144 - the rule every window in this engine
        // is held to.
        const float alpha = dt > 0.0f ? 1.0f - std::exp(-dt / tau) : 1.0f;
        state.rustleDrive += (target - state.rustleDrive) * std::clamp(alpha, 0.0f, 1.0f);
    }

    /// Turn this tick's accumulated limb poses into a centre, a velocity, a
    /// vertical acceleration, and the one question all of it exists to answer:
    /// is anything holding this body up.
    ///
    /// Why acceleration and not height. Height needs a ground to be above, and
    /// the only ground we have is the last floor contact - which is stale the
    /// moment the body starts travelling, and on a staircase is a step it left
    /// three bounces ago. Acceleration needs no reference at all: a body nothing
    /// is supporting falls at gravity, on the flat, on stairs and off a cliff
    /// alike.
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
        // overwritten below. The honest answer to "how far has the body slid",
        // and the one thing a contact point cannot say: the contact point hops
        // between manifold points and between limbs, and its displacement is
        // mostly jitter that has nothing to do with sliding.
        actor.comStepUnits =
            actor.haveLastCom ? Distance(com, actor.lastCom) : 0.0f;

        state.comPosition = com;
        state.comVelocity = vel;
        state.verticalSpeed = vel.z;
        // Measured, not held. bodySpeed used to be the last contact's, decayed
        // by an exponential, because there was no periodic measurement to have -
        // see the comment this replaces at the top of UpdateState.
        state.bodySpeed = Length(vel);

        const auto dt = static_cast<float>(nowMs - actor.lastComMs) * 0.001f;
        // A gap far longer than a frame is a rebuilt ragdoll or a resumed
        // replay, not an enormous acceleration. Re-seed rather than differencing
        // across it.
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

        // The garment, measured here because this is where the per-limb pose
        // already is and because the relative term needs the body's own
        // acceleration, which is `accel` and is not kept anywhere else. `dt` is
        // passed rather than recovered, since `lastComMs` has just been moved
        // on and the difference it would give is zero.
        ConsumeRustle(actor, nowMs, dt, accel);

        // What gravity cannot account for. Free fall is straight down at exactly
        // g and nothing sideways, so the length of (a - g) is zero for a body
        // nothing is touching and grows with whatever is pushing it. No direction
        // is privileged: a leash hauls sideways, a shout hurls backwards, a blast
        // throws upward, and all three are the same measurement.
        const Vec3 residual{accel.x, accel.y,
                            accel.z + std::max(0.0f, cfg.motion.gravityUnitsPerSec2)};
        state.drivenResidual = Length(residual);

        // Steeper than free fall still counts: a body driven into the ground has
        // nothing holding it up either. What is excluded is the other sign - a
        // shove pushes the body *up*, and reading that as flight is exactly the
        // bug this replaces.
        const float gate = -cfg.motion.freeFallFrac * std::max(1.0f, cfg.motion.gravityUnitsPerSec2);
        const bool ballistic = state.verticalAccel <= gate;
        if (ballistic) {
            actor.freeFallForMs += dt * 1000.0f;
            actor.lastFreeFallMs = nowMs;
        } else if (Ancient(actor.lastFreeFallMs) ||
                   (nowMs - actor.lastFreeFallMs) > cfg.motion.freeFallHoldMs) {
            // Only once the hold has run out, so a body clipping something on the
            // way down does not land and take off a dozen times.
            actor.freeFallForMs = 0.0f;
        }

        const bool wasAirborne = state.airborne;
        state.airborne = actor.freeFallForMs >= cfg.motion.freeFallMinMs;

        // Only ever asked while the body is otherwise unsupported. A body lying
        // on the floor is held up by the floor, which is a force like any other
        // and reads as a residual of a full g - true, and useless. What the rules
        // want to know is narrower: *this flight*, is it a fall or a throw.
        //
        // And never on a frame the body is hitting the world, which is the whole
        // failure this gate had. The residual is "acceleration gravity does not
        // explain", and a collision is the largest such acceleration in the fall
        // by an order of magnitude: the leash yank this gate was measured on
        // reads 900-1600 u/s^2, and Proventus_Avenicci_devbench_5's arrival at
        // 2919 ms reads 5110. So every landing declared itself driven, the clock
        // below was pushed forward onto the frame of impact, and every flight in
        // that take reported 20-23 ms and 0 units against a real 61-331 ms and
        // 4-43. A frame with a contact in it has its residual already explained,
        // so it is not evidence of anything pushing and must not be read as any.
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
            // Latched, because this is the edge every landing rule was written
            // for and the only place the whole flight is still known. A frame
            // later `airborne` is false and the live clock reads zero, which is
            // what made the air-time rules pay out on mid-air clips and refuse
            // the arrival. Kept for `landingWindowMs` and no longer.
            actor.lastFlightMs =
                std::max(0.0f, static_cast<float>(nowMs - state.freeFlightSinceMs));
            actor.lastFlightDropUnits = state.fallDropUnits;
            actor.lastFlightEndedMs = nowMs;
            spdlog::debug("actor {} landed at {:.0f} ms after {:.0f} ms and {:.0f} units",
                          actor.name, nowMs, actor.lastFlightMs, actor.lastFlightDropUnits);
        } else if (state.airborne) {
            // Relative, and therefore stair-proof: how far this body has come
            // down since it left support, never how high it is above anything.
            state.fallDropUnits = std::max(0.0f, actor.airborneStartZ - com.z);
        }

        if (state.driven) {
            // The clock that air time is read off. Pushed forward for as long as
            // something is pushing, so what it reports is how long the body has
            // been falling *on its own* - which is the question every rule that
            // reads it was actually asking. The drop goes with it: units covered
            // under power were not fallen either.
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

        // An envelope follower over contact speed, not a running total: "how
        // hard has this fall been hitting lately" is a question about the last
        // few hundred milliseconds, and keeping it in units/s is what makes it
        // comparable with minImpactSpeed and with the 355-543 of an ordinary
        // shove. A sum would have made the number depend on how many limbs
        // happened to be touching. The hero test's dominance clause is what
        // reads it.
        const auto elapsed = static_cast<float>(nowMs - actor.energyStampMs);
        if (elapsed > 0.0f) {
            actor.energyRecent *= std::exp(-elapsed / 300.0f);
            // Body speed used to be a guess, and this is the guess. The only
            // measurement available was the last contact's, the capture carried
            // no periodic limb sample, and in the air a body speeds up rather
            // than slowing down - so it is released slowly rather than dropped.
            //
            // A take with pose has the measurement, and ConsumePose overwrites
            // this a few lines below. The decay is kept for the takes that do
            // not, which is every take recorded before the pose sidecar existed.
            if (!actor.sawPoseEver) {
                state.bodySpeed *= std::exp(-elapsed / 1500.0f);
            }
            // A running maximum never comes down, and this one used to be one:
            // a single fast skim held the slide's entry test open for the rest
            // of the knockdown. Decayed on the grace window, so what it reports
            // is how hard the body has been grinding *recently*.
            actor.slideTangent *=
                std::exp(-elapsed / std::max(20.0f, cfg.motion.slideGraceMs));
            // Each chain's own hold, on the same constant and for the same
            // reason: a running maximum never comes down, so one hard scuff of a
            // forearm would have held that arm's loop at its loudest for the
            // rest of the knockdown.
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

        // Snapshot the world-contact stamp before this tick folds its own
        // contacts in. What the head rules want to know is whether the body was
        // already down when the head arrived, and an arm landing in the *same*
        // frame as the head is part of that landing, not evidence against it.
        actor.worldContactBeforeTickMs = actor.lastWorldContactMs;
        actor.handContactBeforeTickMs = actor.lastHandContactMs;
        actor.worldPeers = 0;
        actor.worldPeerFastest = 0.0f;
        // Same idea, same place, for the hero rule's dominance clause: it asks
        // whether this contact stands out from the recent peak, and the loop
        // below is about to fold this contact into that very peak.
        actor.energyRecentBeforeTick = actor.energyRecent;
        // And the same snapshot for the grain layer's "harder than the slide has
        // been grinding lately" test, which the loop below is about to fold this
        // tick's grazes into.
        actor.slideTangentBeforeTick = actor.slideTangent;
        // A moment resets the burst on the tick it is anchored and not after.
        actor.heroResetsBurst = false;

        float frameBodySpeed = 0.0f;
        bool grazedThisTick = false;
        // The grazing contacts' own mass-weighted centre, built as they arrive.
        // What the body grind's bone is chosen as the nearest to - and the reason
        // it is nearest-to-contact and not lowest is that down is not a reliable
        // direction: a body can grind along a wall, down a staircase, or across a
        // ceiling, and a rule that assumes the floor is below gets all three
        // wrong.
        Vec3 grazeSum{};
        float grazeMass = 0.0f;
        float heaviestGraze = 0.0f;
        for (const Contact& contact : contacts) {
            if (contact.actorId != state.actorId) {
                continue;
            }
            // A peer is any limb but the head and the neck touching something
            // that is not a body - so a self-hit, or brushing another ragdoll,
            // never counts as the body having reached the floor.
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
            // A fresh measurement replaces the held one outright rather than
            // taking a maximum with it: a frame where every limb reports 30 u/s
            // is the body having stopped, and that is exactly the moment the
            // event should be allowed to close.
            frameBodySpeed = std::max(frameBodySpeed, contact.bodySpeed);

            // A near-vertical normal is the ground, which is what both the height
            // and the surface underneath are measured against.
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
            // The slide's tallies. Here rather than in ScrapeLoopStrategy
            // because the motion axis is what reads them and Stage 2 runs before
            // Stage 3 - a slide whose evidence was gathered by a strategy was a
            // slide the motion axis could only see one tick late.
            //
            // World grazes only: an arm brushing your own thigh is not the body
            // travelling along a surface, and letting self-contacts open a slide
            // is how a corpse folding up on itself starts grinding.
            if (contact.graze && !contact.selfContact) {
                grazedThisTick = true;
                // A run of grazes with a hole in it is two runs. Without this a
                // body that bounced, flew for a fifth of a second and landed
                // again was credited with the grazing from before the bounce and
                // re-opened its slide on the frame it touched down - so a
                // skipping body was in Slide continuously and the state said
                // nothing about what it was doing.
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
                actor.slideCoverage = contact.coverage;
                actor.slideSurface = contact.surface;
                actor.slideRadius = contact.limbRadius;

                // Which limb, so the contact fraction can be a sum over the ones
                // that are actually rubbing rather than "is anything rubbing".
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
                    loop.tangent = std::max(loop.tangent, contact.tangentSpeed);
                    loop.surface = contact.surface;
                    // The bone inside the chain that is doing the most rubbing
                    // this tick. It only becomes the one the loop hangs on once
                    // it has led for `fLimbHoldMs` - see the hold below.
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

        // Only when there is nothing better. A contact reports the speed of one
        // limb, and a limb whipping on its constraints is not the body - which
        // is the whole reason the measured centre is preferred where a take
        // carries one.
        if (frameBodySpeed > 0.0f && !actor.sawPoseEver) {
            state.bodySpeed = frameBodySpeed;
        }

        // Ground covered since the run of grazes opened, and how fast the body was
        // going while it was covering it.
        //
        // Both are held rather than decayed once the grazing stops, which is the
        // point: what the slide-end impact needs is the speed the body had when
        // it was interrupted, not that figure with however long it took to
        // notice already subtracted from it.
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

        // Measured where the take carries pose - ConsumePose has already set the
        // flag from the body's own acceleration, and nothing here may overrule
        // it. The fallback below is the old inference, kept for takes recorded
        // before the sidecar existed and wrong in exactly the ways that motivated
        // replacing it: a body that has merely stopped touching anything reads as
        // flying, which is maximal at ragdoll_start and on a body lying still.
        if (!actor.sawPoseEver) {
            state.airborne =
                !Ancient(actor.lastAdmittedMs) &&
                (nowMs - actor.lastAdmittedMs) >
                    Window(cfg.motion.airborneMinTimeMs, frameMs, cfg.arb.frameScaleK);
        }

        // Motion first, then moment. The order matters: the moment axis is
        // allowed to read what the body is doing, and the budget the arbitrator
        // finally asks for is a function of both.
        AdvanceMotion(actor, nowMs, frameMs);
        AdvanceMoment(actor, nowMs, frameMs);
        AdvanceBedTrim(actor, frameMs);
    }

    /// The motion trim the bed is levelled by, glided rather than stepped.
    ///
    /// Advanced here - once per actor per tick, after the motion axis has
    /// settled and before any strategy has proposed anything - so every cue of
    /// a tick is levelled by one number, and that number is this tick's. Doing
    /// it per proposal would make a loop and the impact beside it disagree
    /// about what the body was doing.
    ///
    /// A time constant and not a per-tick fraction, so the glide is the same
    /// shape at 24 fps and at 144 - the rule every window in this engine is
    /// held to.
    void AdvanceBedTrim(ActorRuntime& actor, float frameMs) {
        const float target = MotionBudgetFor(actor.state).gainTrimDb;
        // Seeded, never faded into. A take opens with the body already in some
        // motion, and gliding up from 0 dB would be an eight-decibel swell at
        // the top of every fall that nothing in the world is behind.
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

    /// How much of the body is on the surface, 0 to 1.
    ///
    /// A sum of our own anatomical masses over the limbs that have grazed inside
    /// `fContactHoldMs`, over the mass of the whole body. One foot is about
    /// 1.5 %; both feet and both shins about 13 %; a body flat on its back and
    /// skidding is 60 % and up. That is the quantity the body grind's level
    /// should be a function of, and it is the one the mod never had - the level
    /// stood on speed alone, so a corpse dragged by one ankle read exactly as
    /// loud as the same corpse lying flat.
    ///
    /// The hold is what turns a stream of collisions into a state. Contacts are
    /// dense when a fall is busy and absent when it is not, so a fraction taken
    /// from one tick's contacts alone flickers between 45 % and nothing at solver
    /// rate, and a level following that is a tremolo rather than a grind.
    ///
    /// **This shapes the loop and nothing else.** No inferred state may silence a
    /// contact, and this one is nowhere near the impact path on purpose.
    void UpdateContactFraction(ActorRuntime& actor, TimeMs nowMs) const {
        if (actor.bodyMass <= 0.0f) {
            // No profile, so no sites, so no honest sum. Zero is the quieter
            // answer: the limb loops still play and the body grind does not.
            actor.contactFraction = 0.0f;
            return;
        }
        const auto hold = static_cast<float>(std::max(0.0f, cfg.strategies.scrape.contactHoldMs));
        float touching = 0.0f;
        for (std::size_t i = 0; i < actor.limbCount && i < kMaxLimbs; ++i) {
            const ActorRuntime::LimbTrack& limb = actor.limbs[i];
            if (!Ancient(limb.grazeMs) && static_cast<float>(nowMs - limb.grazeMs) <= hold) {
                touching += limb.mass;
            }
        }
        actor.contactFraction = std::clamp(touching / actor.bodyMass, 0.0f, 1.0f);
    }

    /// Which bone each loop hangs on.
    ///
    /// The body grind takes the limb nearest the grazing contacts' own centre -
    /// measured where the take carries pose, and the heaviest limb that grazed
    /// where it does not. A limb loop takes whichever bone inside its chain has
    /// been doing the most rubbing, but only once it has been doing it for
    /// `fLimbHoldMs`: the original worry that a scrape hopping between bones
    /// would smear rather than track was legitimate, and this is the cure applied
    /// to the hop rather than to the following.
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
            // grinding is still a great deal closer to the truth than the pelvis.
            actor.bodyAnchor = actor.heaviestGrazeLimb;
            actor.haveBodyAnchor = true;
        }

        const auto hold = static_cast<float>(std::max(0.0f, cfg.strategies.scrape.limbHoldMs));
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
    /// Every edge is available from every state that can reach it. That is the
    /// difference from the machine this replaces, which could only reach
    /// `Airborne` from `Launch` - so the flag was measured correctly every tick
    /// and then consulted in exactly one place, and a body that left the ground
    /// again halfway down a staircase was never airborne twice.
    void AdvanceMotion(ActorRuntime& actor, TimeMs nowMs, float frameMs) {
        CrashState& state = actor.state;
        const Motion previous = state.motion;

        // The hardest contact this actor took this tick, on the untouched
        // figure. How big the fall is must not move because one limb clipped
        // the floor or because a rule lifted a neighbour - reading the adjusted
        // intensity here let a modifier walk the actor into a different state
        // and quieten everything that followed.
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

        // Has the floor stopped arguing about it.
        //
        // The Airborne edge below drops to Tumble on `touched`, and the Tumble
        // edge returns on `airborne` - which is a latch on the acceleration and
        // survives the contact, so without this the two take turns one per tick
        // for as long as a body bounces along a floor. This is the memory the
        // machine did not have: a contact does not merely move the state once,
        // it holds the body out of flight until the contacts stop.
        //
        // Asked of the same `touched` the other edge fires on, so the two cannot
        // disagree about what a contact is, and stamped above rather than read
        // off `lastAdmittedMs` for the same reason - that one counts self-hits,
        // and a corpse elbowing itself is not the floor.
        const bool clearOfContacts =
            cfg.motion.landedHoldMs <= 0.0f || Ancient(actor.lastTouchedMs) ||
            (nowMs - actor.lastTouchedMs) >
                Window(cfg.motion.landedHoldMs, frameMs, cfg.arb.frameScaleK);

        // Sliding is a question about sustained tangential motion, asked the same
        // way from Tumble and from Slide. Two ways in, because a slide has two
        // shapes: a slow grind earns it by lasting, a fast skid by covering
        // ground - and devbench_3 crosses two metres of floor in less time than
        // the duration gate wants, which is why the one take that is mostly
        // sliding used to have no slide in it.
        //
        // The grace window is what keeps it alive between contacts. Collisions
        // are dense when a fall is busy and absent when it is not, so a couple of
        // quiet frames in the middle of a long grind is a solver artefact rather
        // than the body having stopped - but a stretch with no grazing at all in
        // it is not a slide any more whatever else is true.
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

        // What *keeps* one open is a different question, and asking it with the
        // entry test was a gate with no hysteresis in it. The tangent hold decays
        // on the grace window, so re-applying the entry speed every tick ended
        // slides on the decay curve rather than on anything the body did - a
        // slide would open at 200 u/s of tangent and close seventy milliseconds
        // later because the hold had fallen to 119, with the body still moving
        // and still grinding.
        //
        // The two ways out are going airborne and the graze stream drying up.
        // "The tangent hold decayed" is neither of them, so what is asked here
        // is whether the body is still grazing - or, where the take carries
        // pose, whether it is still *travelling*.
        //
        // That second clause is the exit half of the entry speed, and it is a
        // different measurement rather than the same one asked twice. The entry
        // gate reads the contacts, which is right for opening a slide and wrong
        // for holding one: collisions are dense when a fall is busy and absent
        // when it is not, so a long low grind loses its graze stream while the
        // body is still moving and the grind stops with the corpse still going.
        // `bodySpeed` comes off the pose sidecar and is the body itself, so it
        // cannot decay out from under a slide the way a contact hold does.
        //
        // Gated on pose for exactly that reason, and bounded by `slideHoldMaxMs`
        // so a drifting pose stream cannot hold a grind open indefinitely. Off
        // by default, where `slideHolds` is `grazing` and nothing has changed.
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
                // A landing is a contact. A body that merely stopped falling
                // without touching anything - caught on a slope, or dropped
                // into water - goes back to tumbling rather than staying in a
                // flight it is no longer in.
                if (touched || !state.airborne) {
                    EnterMotion(actor, Motion::kTumble, nowMs);
                }
                break;
            case Motion::kTumble:
            case Motion::kSlide:
                // Two ways out of a slide. Going airborne is *measured*; the
                // other is the graze stream drying up, which is a statement
                // about the contact data and nothing more.
                if (state.airborne && state.haveBodySamples && clearOfContacts) {
                    // The edge the old machine did not have: `airborne` was
                    // measured every tick and then consulted only in Launch, so
                    // a body that left the ground again halfway down a
                    // staircase was never airborne twice.
                    //
                    // `clearOfContacts` is the hysteresis that edge needed and
                    // did not get. `airborne` alone is not enough to *re-enter*
                    // flight, because it is a latch that a bouncing body keeps
                    // topped up while the floor is still hitting it: see
                    // `fLandedHoldMs`, and the 300 ms of tick-rate Tumble /
                    // Airborne alternation in Eldawyn_devbench_1 that it is
                    // named for. Nothing about the measurement changed - only
                    // how long this edge waits before believing it a second
                    // time.
                    //
                    // Gated on pose, and that is not caution - it is the whole
                    // reason the edge is safe now. Without pose `airborne` is
                    // inferred from the gap since the last contact, so a body
                    // that has merely stopped touching anything reads as
                    // flying, and this edge would fly a corpse lying still on
                    // the floor for the rest of the take.
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

    /// Close the slide, on whichever edge is leaving it.
    ///
    /// One function for both because the bookkeeping is the same and only the
    /// consequence differs: the exit chooses the loop's fade, and `kEnded` also
    /// offers the lift to whatever collision is in this tick. The exit is left
    /// on the crash state afterwards rather than being a transient - the
    /// strategy reads it on the tick it stops the loop, and the timeline reads
    /// it to mark the end of a span it has already drawn.
    void LeaveSlide(ActorRuntime& actor, TimeMs nowMs, SlideExit why) {
        if (actor.state.motion != Motion::kSlide) {
            return;
        }
        actor.state.slideExit = why;
        // `slideSpeed` is a peak over the whole slide and is logged as one. It
        // is a diagnostic and nothing reads it as a signal.
        spdlog::debug(
            "actor {} slide ended {} at {:.0f} ms after {:.0f} units, peak {:.0f} u/s",
            actor.name, ToString(why), nowMs, actor.slideDistance, actor.slideSpeed);
        actor.grazeSinceMs = kLongAgo;
        actor.grazeLastMs = kLongAgo;
        actor.slideDistance = 0.0f;
        actor.slideTangent = 0.0f;
        actor.slideSpeed = 0.0f;
    }

    /// A hero floor from a fraction of the loud anchor.
    ///
    /// One function so the ordinary floor and the head's relieved one cannot
    /// come to disagree about what a fraction means, and so the clamp at zero is
    /// written once rather than at each call.
    [[nodiscard]] float HeroFloor(float frac) const {
        return std::max(0.0f, frac * cfg.intensity.speedRefHigh);
    }

    /// How much of the loud anchor `[HeadImpact]`'s relief takes off that floor,
    /// or nothing at all when it is switched off.
    ///
    /// Negative is refused rather than passed through: a "relief" that raised
    /// the gate would be the one thing the name promises it cannot do, and the
    /// schema's range says the same thing from the other side.
    [[nodiscard]] float HeadRelief() const {
        return std::max(0.0f, cfg.strategies.head.heroFloorReliefFrac);
    }

    /// Whether a contact earns that relief: a head, hard enough in its own
    /// right, with both the accent and the relief switched on.
    ///
    /// A plain threshold, and deliberately *not* `ClassifyHead`'s gate. That one
    /// already folds in head-down attitude, air time and company, so reusing it
    /// would move this rule whenever any of those three were tuned - and what
    /// this asks is a simpler question than any of them: was the skull the thing
    /// that hit.
    [[nodiscard]] bool HeadFloorRelieved(const Contact& contact) const {
        const HeadImpactConfig& headCfg = cfg.strategies.head;
        return headCfg.enabled && headCfg.heroFloorRelief && contact.site == LimbSite::kHead &&
               HeadRelief() > 0.0f &&
               contact.impactSpeed >= headCfg.heroFloorReliefAtFrac * cfg.intensity.speedRefHigh;
    }

    /// Stage 2, axis two. What the mix is doing.
    ///
    /// Latched and windowed rather than a running description, and measured on
    /// raw `impactSpeed` throughout. The test this replaces read intensity
    /// against a running energy total, and both halves of that were wrong: the
    /// total only ever grew, so past 1/0.35 the test could never fire again;
    /// and intensity clamps at 1.0, so above `speedRefHigh` every contact read
    /// the same and the test meant to find the biggest moment of a fall was
    /// blind at the top of its own range.
    void AdvanceMoment(ActorRuntime& actor, TimeMs nowMs, float frameMs) {
        CrashState& state = actor.state;
        const HeroConfig& hero = cfg.hero;
        if (!hero.enabled) {
            state.moment = Moment::kOrdinary;
            return;
        }

        // Expire first, so a re-anchor is a decision about a window that is
        // still open rather than about one that had already closed.
        if (state.moment == Moment::kHero &&
            (nowMs - state.heroSinceMs) > Window(hero.windowMs, frameMs, cfg.arb.frameScaleK)) {
            state.moment = Moment::kOrdinary;
            state.heroPeakSpeed = 0.0f;
            state.heroSeq = 0;
            spdlog::debug("actor {} hero window closed at {:.0f} ms", actor.name, nowMs);
        }

        const float baseFloor = HeroFloor(hero.floorFrac);

        for (const Contact& contact : contacts) {
            if (contact.actorId != state.actorId || contact.selfContact) {
                continue;
            }

            // Per contact rather than per tick, because `[HeadImpact]`'s relief
            // makes the floor a function of which limb arrived.
            const bool relieved = HeadFloorRelieved(contact);
            const float floor = relieved ? HeroFloor(hero.floorFrac - HeadRelief()) : baseFloor;
            if (contact.impactSpeed < floor) {
                continue;
            }

            // The envelope as it stood *before* this tick's contacts were folded
            // in. `energyRecent` has already taken the maximum with every contact
            // of this frame by the time we get here, and a contact cannot be 1.3x
            // itself - reading the live value would make the dominance clause dead.
            //
            // Floored at the hero floor, and that floor is doing real work. The
            // envelope decays with a 300 ms constant, so a few hundred
            // milliseconds of quiet take it to nothing - and against nothing,
            // *everything* is dominant. Left unfloored this clause fired on every
            // contact over the floor that happened to follow a gap, which on the
            // long takes meant twenty-one hero moments in one recording and a
            // burst budget that reset so often it stopped being a budget.
            //
            // What the floor says is that when nothing has happened recently
            // there is no peak to stand out from, so the contact has to be hard
            // in its own right - `dominanceRatio` above the floor rather than
            // above silence.
            //
            // And this is the second half of what the head's relief buys, which
            // is easy to miss: a relieved contact is measured against a lower
            // clamp as well as a lower gate, so it can be dominant over a quiet
            // envelope that would otherwise have held it to the ordinary floor.
            // That is deliberate. Letting the head past the absolute gate and
            // then judging it against the unrelieved clamp would move the number
            // without moving the outcome, which is the failure mode a gate that
            // reads as free but never fires has.
            const float recent = std::max(floor, actor.energyRecentBeforeTick);

            // Dominance: it stands out from the last few hundred milliseconds.
            const bool dominant = contact.impactSpeed >= hero.dominanceRatio * recent;

            // Arrival: it landed out of a real, measured flight. Deliberately
            // unavailable without pose - the old inference scores the first
            // contact of a take at the top of its ramp, because nothing had
            // ever touched, and that is precisely the bug that spent the hero
            // moment on a 44.7 u/s scuff at 696 ms.
            //
            // Both halves off one `FlightFor`, so they are two questions about
            // the same flight. Asking the air time of the contact and the drop of
            // the live state let them come from different ones - and after a
            // landing the live drop is whatever the next flight has reset it to.
            bool arrived = false;
            if (state.haveBodySamples) {
                StrategyContext ctx{cfg, actor, rng, nowMs, frameMs, &nextVoiceId, &stats, bank};
                const FlightMeasure flight = FlightFor(ctx, contact, false, 0.0f);
                arrived = flight.airMs >= hero.arrivalMinAirMs &&
                          flight.dropUnits >= hero.arrivalMinDropUnits;
            }

            if (!dominant && !arrived) {
                continue;
            }

            // Did the relief actually change the answer? Both halves of it can,
            // so both are re-asked at the unrelieved floor: the absolute gate,
            // and the clamp the dominance ratio is measured against.
            //
            // Counted rather than inferred, because from the hero count alone a
            // relief firing on contacts that would have anchored anyway looks
            // exactly like one doing the work it was turned on for.
            bool creditRelief = false;
            if (relieved) {
                const bool baseDominant =
                    contact.impactSpeed >=
                    hero.dominanceRatio * std::max(baseFloor, actor.energyRecentBeforeTick);
                creditRelief =
                    !(contact.impactSpeed >= baseFloor && (baseDominant || arrived));
            }

            if (state.moment == Moment::kHero) {
                // Re-anchor rather than open a second moment. A landing is five
                // limbs arriving over a couple of hundred milliseconds and it
                // should read as one event with peers; what moves is which of
                // them the moment is *about*.
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

    /// Open a hero moment on this contact, or move an open one onto it.
    ///
    /// Both do the same three things - restart the window, mark the burst for a
    /// reset, and move the point every layer collapses to - which is why they
    /// are one function. The spatial collapse point is the interesting one: it
    /// belongs on the moment's own peak rather than on its first grain, so a
    /// landing whose 608 u/s slam arrives 132 ms after the 294 u/s head that
    /// opened the window collapses onto the slam.
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
        // The burst reset is consumed by the arbitrator this tick. It is what
        // absorbs the old air-time budget reset: the thing a long fall most
        // often lost to was not a gate but a burst that a scuff opened and
        // filled while the body was still in the air.
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
            // Cleared as the span opens, never as it closes: the strategy reads
            // it on the tick after the one that set it, and the timeline reads
            // it for as long as the span it marks is still on screen.
            actor.state.slideExit = SlideExit::kNone;
            ++stats.slides;
            ++actor.stats.slides;
        }
        if (motion == Motion::kLaunch) {
            actor.energyRecent = 0.0f;
            actor.energyRecentBeforeTick = 0.0f;
            // All six damage ledgers at once: counts back to zero and the spacing
            // windows back to never. A body being launched again is the only
            // event boundary the engine has, and it is the one a listener would
            // also call the start of something new.
            actor.damage = {};
            // The accumulated ladder's per-knockdown budget goes back with them,
            // and only the budget: the pools themselves keep healing on their own
            // clock rather than being zeroed here. A body relaunched mid-beating
            // has not stopped being beaten, and wiping what it had taken would
            // let the ladder be climbed from the bottom again and again by
            // anything that re-arms a knockdown.
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

    /// The body half of the air-time rule: lift a contact that is not the head
    /// in proportion to how long the actor had been off the ground when it
    /// arrived.
    ///
    /// A Shape-stage rule, so the whole of the dance it used to spell out - the
    /// carried onset gain, the capped lift, the accumulated trim - lives in
    /// `Shape`, and what is left here is only the measurement and its ramp.
    ///
    /// Returns the ramp position it applied, 0 when the rule did nothing.
    float ApplyBodyAirTime(const StrategyContext& ctx, Contact& contact) {
        const AirTimeConfig& airCfg = cfg.strategies.airTime;
        if (!airCfg.bodyEnabled || contact.site == LimbSite::kHead) {
            return 0.0f;
        }
        const float clear = std::max(1.0f, airCfg.bodyClearMs);
        const float ramp =
            std::clamp(AirTimeMs(ctx, contact, false, 0.0f) / clear, 0.0f, 1.0f);
        if (ramp <= 0.0f) {
            return 0.0f;
        }

        Shape(contact, airCfg.bodyLift, ramp, cfg.intensity.dynamicRangeDb);
        return ramp;
    }

    void RunStrategies(TimeMs nowMs, float frameMs) {
        proposals.clear();

        for (Contact& contact : contacts) {
            ActorRuntime* actor = Find(contact.actorId);
            if (actor == nullptr || actor->state.tier == DistanceTier::kCulled) {
                continue;
            }
            // The body half of the air-time rule, on the same terms and in the
            // same place: before the tier check, so a landing this rule lifts
            // can clear it.
            {
                StrategyContext airCtx{cfg, *actor, rng, nowMs, frameMs, &nextVoiceId};
                const float before = contact.intensity;
                if (const float ramp = ApplyBodyAirTime(airCtx, contact); ramp > 0.0f) {
                    TraceLine(contact.timeMs, contact.actorId, contact.limbIndex,
                              contact.sourceSeq, contact.impactSpeed, contact.intensity,
                              actor->state,
                              std::format("body air {:.2f} int {:+.2f}", ramp,
                                          contact.intensity - before));
                }
            }
            // Simplified tier: hero composites only. No grains, no loops, no bed -
            // nobody resolves the detail at that range anyway.
            if (actor->state.tier == DistanceTier::kSimplified && contact.intensity < 0.4f) {
                continue;
            }

            // One stream per contact rather than one per take. A shared stream
            // makes the scatter a function of how many cues came before, so a
            // config change early in a take re-rolls the pitch of everything
            // after it and two exports differ everywhere instead of where the
            // change bit. Seeded from the contact's own row, so changing
            // `iRngSeed` still re-rolls the whole take.
            Rng contactRng;
            contactRng.Seed(StableSeed(cfg.slots.rngSeed, contact.sourceSeq));
            StrategyContext ctx{cfg, *actor, cfg.slots.stableVariants ? contactRng : rng,
                                nowMs, frameMs, &nextVoiceId, &stats, bank};
            // Head contacts get a line of their own in the trace whatever
            // becomes of them. Every head rule is a judgement about geometry the
            // cue list cannot show, and without this an air time that came out
            // at zero and one that was never enabled look identical in the
            // export.
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

        // Every tier except culled: the strategies that are full-detail only -
        // the bed and the grinding loop - check the tier themselves, and the
        // closing cue is neither a grain nor a loop nor the bed, so gating the
        // whole tick on kFull would leave a knockdown at fifteen metres with no
        // ending at all.
        for (auto& actor : actors) {
            if (!actor.inUse || actor.state.tier == DistanceTier::kCulled) {
                continue;
            }
            StrategyContext ctx{cfg, actor, rng, nowMs, frameMs, &nextVoiceId, &stats, bank};
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
            // Assembled here and nowhere else, which is the whole of what keeps
            // the split honest: a strategy that could write a priority would be
            // a strategy able to make its own cue outrank the frame, and that is
            // the trap `config.md` was written about. The arbitrator is the one
            // place that gets to say what matters.
            //
            // A bypass keeps priority at its level. It is not an onset, so
            // nothing sorts it against anything, and giving a loop a weight
            // would be a number with no reader.
            Proposal& proposal = proposals[i];
            proposal.priorityDb =
                proposal.bypass ? proposal.levelDb
                                : proposal.levelDb + SiteWeightDb(proposal.site);
        }
        // Most important first, ties broken by the row the contact came from.
        // That is loudest-first until a weight is set, because priority is the
        // level plus zero.
        std::ranges::stable_sort(order, [&](std::uint16_t a, std::uint16_t b) {
            if (proposals[a].priorityDb != proposals[b].priorityDb) {
                return proposals[a].priorityDb > proposals[b].priorityDb;
            }
            return proposals[a].sourceSeq < proposals[b].sourceSeq;
        });

        const float rateCap = Window(cfg.arb.rateCapMs, frameMs, cfg.arb.frameScaleK);
        const float chainWindow = Window(cfg.arb.chainMergeWindowMs, frameMs, cfg.arb.frameScaleK);
        const float burstWindow = Window(cfg.arb.burstWindowMs, frameMs, cfg.arb.frameScaleK);
        // The moment axis's own window, when it has one. Frame-scaled the same
        // way and computed here beside the ordinary one so the two are visibly
        // the same quantity; which of them a proposal is judged against is
        // decided per proposal below, because the latch opens and closes inside
        // a frame and a burst is a per-actor thing.
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
            //    regardless of limb: below that extra onsets do not add detail,
            //    they add mud.
            // Signed on purpose, then taken absolute: proposals are arbitrated
            // loudest-first rather than in time order, so a quieter contact can
            // be judged against an onset that happens *after* it. The rule is
            // "one onset per 46 ms", and which side of the accepted one this
            // proposal falls on does not change that.
            const auto sinceOnset =
                std::fabs(static_cast<float>(proposal.timeMs - actor->lastOnsetMs));
            const float cap = rateCap * proposal.rateCapScale;
            if (sinceOnset < cap) {
                // Unless it is properly louder. The cap exists because onsets
                // closer than this add mud rather than detail - but that is only
                // true between comparable levels. A contact well above the one
                // holding the cap is a new event, and silencing it is how a
                // multi-limb crash came out as three sounds: log_4's body slams
                // down 42 ms after its foot lands and 7 dB louder, and was lost.
                // Both sides weighted, which is what carries the split *across*
                // ticks. A torso landing 20 ms after a hand used to need 6 dB of
                // real level to open its own onset; with 3 dB of torso weight it
                // needs 3, and a hand arriving after a torso needs 9. The same
                // one number that reorders a frame also decides who may
                // interrupt whom between frames - and it has to, because 99.5 %
                // of the corpus's clusters are one tick and the rest are this.
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
                // Inside the design's cap, but outside the scaled one a moment
                // asked for - so the engine let it through on purpose. Counted
                // as an override for the same reason the dB exception is: the
                // verifier measures gaps against the nominal cap and cannot
                // re-derive which of them were granted, because the scale lived
                // on a proposal that no longer exists by then.
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

            // 4. Burst shaping. The arbitrator picks bursts, not individual
            //    sounds: three to five grains inside 200-400 ms, then real
            //    silence. This is the rule that turns a three-second tumble into
            //    four audible events.
            // A refund waives part of the silence the arbitrator insists on
            // between bursts. It defaults to nothing, so an unrefunded proposal
            // meets exactly the budgets it always did.
            //
            // An air-time reset goes further and closes the open burst outright,
            // so the landing opens its own with the grain count back at zero -
            // which is the only way past a burst that a scuff opened and filled
            // while the body was still in the air.
            const int grainCap = std::max(
                1, std::min(cfg.arb.burstMaxGrains, BudgetFor(actor->state).maxCuesPerBurst));
            // Which of the landing's limbs actually gets the reset is settled
            // here rather than in the strategy, because here the proposals are
            // in order - loudest first - and the count is authoritative.
            //
            // A landing is five limbs arriving in one frame carrying the same
            // hero moment behind every one of them. Letting each of them act on
            // it turns the one burst it was meant to be into five, which is the
            // trap the old air-time reset documented and which this rule walked
            // straight back into the first time it was written. One reset per
            // moment; the rest of the limbs meet the ordinary budgets, which is
            // what keeps a landing one burst instead of one per limb.
            const bool resetsBurst = proposal.resetsBurst && actor->heroResetsBurst;
            // Which window, asked of the moment this proposal is arriving into
            // rather than of the one the burst opened in. A burst that a moment
            // opens is a landing and wants the tight window; the same burst once
            // the latch has closed is an ordinary stretch again and wants the
            // wide one. Reading it off the live state is also what makes the two
            // numbers mean what their names say - `Hero:fBurstWindowMs` is how
            // long a burst stays open *while a moment is running*, and nothing
            // has to remember which state a burst was born in.
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

            // Everything below is provisional until the proposal actually makes
            // a sound. A stack whose every layer fell under the voice floor or
            // the cap is not an audible moment, and it must not spend the burst
            // budget, raise the masking ceiling or count towards the reduction
            // ratio - all three would be counting silence as an event.
            // Only the fields the block below touches, because Tick must not
            // allocate and copying the whole runtime would copy the actor's name.
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
            // Both of these are read back by a *comparison*, so both store the
            // weighted figure - a ledger that mixed the two scales would let a
            // weight apply in one direction and not the other, and a torso would
            // outrank a hand while the hand outranked it back.
            actor->lastOnsetDb = proposal.priorityDb;
            if (chain < kChainCount) {
                actor->chainLastMs[chain] = proposal.timeMs;
                actor->chainLastDb[chain] = proposal.priorityDb;
            }
            // The mask ceiling stays on the level, and that is the line. Masking
            // asks whether this can be heard under what is already sounding,
            // which is a fact about air: a weight nobody can hear must not raise
            // the ceiling the next contact has to clear.
            actor->state.maskCeilingDb = std::max(actor->state.maskCeilingDb, proposal.levelDb);
            ++actor->state.admittedCount;
            // Spent here rather than where it was granted, for the same reason
            // the old refund was: a reset that buys a proposal past the budgets
            // and then loses every layer to the voice floor has bought nothing,
            // and the next proposal down must still be able to use it.
            if (resetsBurst) {
                actor->heroResetsBurst = false;
            }
            // Nothing is charged here any more. The two per-knockdown counters
            // that used to be - one for the head refund, one for the air-time
            // reset - existed because a waiver was earned per contact and five
            // limbs of one landing would each earn their own. The moment axis
            // decides once, on the actor, so there is nothing left to
            // double-count and nothing to roll back below.

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

        // Pass two: the accessories, which live or die with their parent. A
        // crunch with no impact under it is not a sound anybody can place.
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
        // The speed matters most on a drop line: "rate cap" against a 40 u/s
        // brush is the rule working, and against a 440 u/s slam it is a bug.
        // Reporting 0 here made those two look identical in the export.
        TraceLine(proposal.timeMs, proposal.actorId, proposal.limbIndex, proposal.sourceSeq,
                  proposal.impactSpeed, proposal.intensity, state, why);
        spdlog::debug("drop seq {} at {:.0f} ms: {} ({:.1f} dB)", proposal.sourceSeq,
                      proposal.timeMs, why, proposal.levelDb);
    }

    // ── Stage 5: hand the cue list over ──────────────────────────────────────

    /// Returns how many cues actually reached the sink, which is what tells an
    /// accepted proposal from an audible one.
    /// Whether this slot is audible under the current layer mutes.
    ///
    /// The two declared-and-unfilled voice slots have no mute of their own -
    /// they never reach here, because resolution skips them silently.
    /// Which switch governs which slot is `LayerMute` in Config.h, shared with
    /// the testbench's slot panel so the panel and the engine cannot disagree
    /// about what is muted. What is left here is the one thing that is not a
    /// per-slot switch: the surface section's master, which takes all three
    /// skins down at once.
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
        // A hero moment is a statement about one *contact* - this hit is the
        // event, spend the frame on it - and the trim that comes with it is how
        // much louder that contact may be. A grind is not a contact and cannot
        // be part of the moment: it was already running before the hit and it
        // goes on running after, so re-levelling it for the two hundred
        // milliseconds a moment is open is a swell in the bed with nothing in
        // the world behind it. The slide is exactly as loud as the body is fast
        // (Slots.md §2), and a hero hit does not make the body faster.
        //
        // This is the same argument `postShapeDb` already makes a few lines
        // down and for the same reason: a bypass proposal carries no intensity
        // of its own, so anything that levels by one is levelling it against a
        // zero it never had. `maxCuesPerBurst` is untouched - a loop is not a
        // grain and never took a slot in the burst - so the moment still gets
        // its whole budget for the contacts it is actually about.
        const PhaseBudget& heroOrMotion = BudgetFor(actor.state);
        const PhaseBudget& bedBudget = MotionBudgetFor(actor.state);
        const PhaseBudget& budget = proposal.bypass ? bedBudget : heroOrMotion;
        // The trim only. `maxCuesPerBurst` still comes off `budget` above and is
        // never smoothed - a budget is a count of voices, and half a voice is not
        // a thing.
        //
        // A bed is one voice held open across a state change, so it takes the
        // glided figure `AdvanceBedTrim` carries; a contact-derived cue is an
        // event and takes the trim of the state it happened in. See
        // `fBedTrimGlideMs`.
        const float budgetTrimDb = proposal.bypass ? actor.bedTrimDb : budget.gainTrimDb;
        const float masterDb =
            cfg.mix.masterGainDb + (actor.isPlayer ? cfg.player.masterGainDb : 0.0f);

        // No distance rolloff here, deliberately.
        //
        // Skyrim attenuates a positioned voice itself, through the BGSSoundOutput
        // model attached to the sound - minDistance, maxDistance and a five-point
        // curve, with DoGetAudibility() culling it past the end. Doing it here as
        // well attenuated everything twice: a contact 6.4 m from the listener came
        // out 8 dB down before the engine had even seen it, which is most of why
        // the mix needed a double-figure master gain to be audible at all.
        //
        // So the game owns falloff and the renderer's job is to attach an output
        // model (SkyrimNet uses the dialogue one, 0x000B5184, for exactly this).
        // The testbench owns none of it either, which keeps the two honest: what
        // is tuned there is the un-attenuated cue, and both sides then get the
        // same curve applied by the same engine.
        //
        // Distance still decides the *tier* - see DistanceTier above. That is a
        // budget decision, not a gain one: what to spend a voice on, not how loud
        // to make it.

        // One-shots only. The collapse exists because several points inside one
        // acoustic moment read as several events; a loop is not a moment, it is a
        // texture that lasts, and pinning one to wherever a hero was anchored is
        // how a grind ends up somewhere the body no longer is. It is also what
        // was quietly undoing the limb attachment for the whole of a hero window.
        const bool collapse = proposal.op == CueOp::kPlayOneShot &&
                              cfg.arb.spatialCollapseOnHero && proposal.boneIndex < 0 &&
                              nowMs < actor.collapseUntilMs;

        // Contact-derived cues only. A bypass proposal - the two loops and the
        // settle cue - carries no intensity of its own, so shaping it by one
        // would be re-levelling the bed against a zero it never had.
        const float postShapeDb = proposal.bypass ? 0.0f : PostShapeDb(proposal.intensity);

        // The band compressor is taken per layer, further down. Measured against
        // `Layer::gainDb`, which is on the same pre-trim scale as `levelDb` - the
        // mod's own, where 0 dB is the hardest contact the engine can hear - so
        // every trim below applies on top of the compressed value rather than
        // being compressed with it. That is what makes the setting survive a
        // change to the master gain: turn the mod up and its compression comes up
        // too. See CompressConfig for why it is not measured at the speaker.
        //
        // Squeezed rather than clamped: a hard cap puts everything above it on
        // exactly one level, so a threshold a few dB low turns a dozen distinct
        // impacts into a dozen identical ones. A ratio keeps them ordered.
        //
        // **It used to be one cut for the whole proposal, off layer 0's reason.**
        // That was written to protect the composite's layer balance, and for a
        // compressor whose classes are kinds of moment it is the right rule. It
        // stopped being the right rule when the four impact layers got lines of
        // their own: they are a split by *frequency*, not by kind, and one cut
        // over all four could only hold the transient in order to pay for the
        // body's peak. What replaced it is a multiband, and reshaping the stack
        // as level rises is what a multiband is for. See CompressConfig for the
        // measurement the four thresholds were separated on.
        //
        // Every proposal but the impact composite is single-layer, so
        // `Layer::gainDb == levelDb` and the change is a no-op for all of them -
        // the taps, the head accent, the crunch, the gore, both loops and the
        // bed come out bit-identical. It also fixes one thing quietly: a Damage
        // proposal used to hand its gore layer the *crunch's* threshold, because
        // layer 0 decided for the stack, so `fGoreDb` only ever applied when
        // gore happened to be first.
        //
        // Stage 5, like every other term on that line, so it cannot decide which
        // contacts win the rate cap - a held hero hit is still the hero hit of
        // its frame.

        // One booking for the whole proposal, taken before any layer is built.
        //
        // A one-shot proposal's layers all carry the same (actorId, sourceSeq),
        // which is exactly the key the renderer mixes on, so the stack below
        // becomes one buffer and one BSSoundHandle however many layers it has.
        // Charging each layer therefore charged four times for one voice, and -
        // worse - let the budget run out *inside* a stack, which is not a
        // quieter impact but a differently-built one: the loser was chosen by a
        // priority list rather than by any of the rules that are supposed to
        // decide what the mix contains.
        //
        // So it is all or nothing, which is the only thing a mixed buffer can
        // be. `endsMs` is the longest layer's, since the voice lasts as long as
        // the buffer does: the offset the layer sits at plus its own length.
        //
        // Loops book their own, by id, further down - a loop is genuinely one
        // voice per loop, and it ends when it is stopped rather than on a clock.
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

            // Off `layer.slot` and not `resolved.slot`, and necessarily before
            // the bank is asked: `imp_body_limb` plays `imp_body`'s recording
            // until somebody records a limb one, and a threshold read off the
            // resolution would leave `fBodyLimbDb` inert until that day. What is
            // being held is the limb layer, whichever file is standing in for it.
            const rds::CompressBand band = rds::CompressBandFor(layer.slot, layer.reason);
            const float compressCutDb = rds::CompressCutDb(
                cfg.compress, rds::CompressThresholdDb(cfg.compress, band), layer.gainDb);

            ResolvedSound resolved{};
            if (proposal.op != CueOp::kStopLoop) {
                // A loop has no `sourceSeq` - it traces back to no single feed
                // event - and a token of 0 turns the stable picker off, so every
                // update cue of one grind drew a fresh variant out of the bag.
                // That was invisible while a variant only named a file: the
                // renderer re-follows position, gain and pitch on an update and
                // never re-attaches the sound, so whatever the bag said was
                // thrown away.
                //
                // It stops being invisible the moment a variant carries the
                // library's own pitch and trim, because then the *correction*
                // would flap between two files' answers every time the deadband
                // let an update through, on a voice playing neither. The voice
                // id is the stable identity a loop does have: one per grind, for
                // as long as it grinds.
                const std::uint32_t token =
                    proposal.sourceSeq != 0 ? proposal.sourceSeq : proposal.voiceId;
                if (bank == nullptr ||
                    !bank->Resolve(layer.slot, proposal.surface, proposal.coverage, proposal.site,
                                   resolved, token)) {
                    // A declared-and-unfilled slot, or no bank at all. Skipped
                    // silently, which is what makes adding voice later a config
                    // change and not a code change.
                    continue;
                }
            }

            Cue cue{};
            cue.timeMs = proposal.timeMs + layer.offsetMs;
            cue.op = proposal.op;
            // What the bank actually chose, which is not always what was asked
            // for: a surface-coloured slot with no recording behind it resolves
            // to the default one. The renderer turns (slot, variant) back into
            // samples through a plain lookup, so it has to be told the slot the
            // variant is an index into. On a stop nothing resolved and the slot
            // is provenance only, which is why the ask stands there.
            cue.slot = proposal.op == CueOp::kStopLoop ? layer.slot : resolved.slot;
            cue.variant = resolved.variant;
            cue.gainDb = layer.gainDb + compressCutDb + budgetTrimDb + masterDb +
                         postShapeDb + proposal.postTrimDb +
                         LayerTrimDb(layer.slot, proposal.site, actor.isPlayer) +
                         // The per-class trim is over the *whole* stack, not over
                         // the armour skin - heavy armour being louder is a fact
                         // about the body, not about the rattle. Zero by default.
                         ArmorCompositeTrimDb(cfg, proposal.coverage) +
                         // The library's own correction for the file that was
                         // actually picked - "this wav is hot", `config.md`'s
                         // example of a Trim. It arrives here and nowhere
                         // earlier because *here* is the first moment anything
                         // knows which file this layer resolved to: Stage 4 has
                         // already sorted, rate-capped and burst-shaped by the
                         // time Resolve is called, so this number is incapable
                         // of changing what was chosen. That is the ordering,
                         // not a promise.
                         resolved.trimDb;
            cue.compressCutDb = compressCutDb;
            cue.compressBand = band;
            // Multiplied into the pitch the engine chose rather than replacing
            // it: the scatter, the intensity bias and the armour bias are what
            // this contact should sound like, and the library's number is a
            // correction to the recording. A file two semitones flat is two
            // semitones flat on every contact, quiet or loud.
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
                    // Not worth mixing in. Under the per-proposal booking this
                    // costs nothing on its own - the voice is already paid for -
                    // so it is purely "leave this layer out of the buffer".
                    continue;
                }
                if (proposal.op == CueOp::kStartLoop) {
                    TakeVoice(actor, kNever, proposal.voiceId);
                }
            }

            // Stage 5, and deliberately the last thing that happens. Every cost
            // above - the rate cap, the chain merge, the mask ceiling, the voice
            // budget - has already been paid, so a muted layer still spends what
            // it would have spent and nothing moves in to replace it. That is
            // what makes muting a layer an honest A/B rather than a new mix.
            //
            // `emitted` counts it either way: the caller reads a zero as "this
            // proposal produced nothing" and rolls the whole burst state back,
            // so returning 0 here because everything was muted would undo the
            // arbitration this is supposed to leave untouched. Only the sink and
            // the stats see the difference.
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
        // nothing is not an audible moment, and the caller is about to roll the
        // whole proposal back. The voice it paid for goes back with it, or the
        // budget leaks one slot per silent proposal - the same failure as the
        // stranded loops, arriving from the other direction.
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

void Engine::SetConfig(const AlgorithmConfig& config) {
    const bool seedChanged = m_impl->cfg.slots.rngSeed != config.slots.rngSeed;
    m_impl->cfg = config;
    if (seedChanged) {
        m_impl->rng.Seed(config.slots.rngSeed == 0 ? 1u : config.slots.rngSeed);
        if (m_impl->bank != nullptr) {
            m_impl->bank->SetStableVariants(config.slots.stableVariants);
            m_impl->bank->SetConditions(config.slots.conditionalVariants,
                                        config.slots.surfaceConditions,
                                        config.slots.armorConditions);
        }
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

    // Culling is not an optimisation bolted on afterwards - it is what keeps a
    // battlefield of ragdolls from ever becoming a performance question, and it
    // makes the mix cleaner at the same time.
    for (auto& actor : impl.actors) {
        if (actor.inUse && actor.state.tier == DistanceTier::kCulled) {
            impl.Release(actor, nowMs, "beyond the cull radius");
        }
    }

    // Animated mode's `ragdoll_end`. A knockdown has an edge to close on and
    // walking has none, so an actor acquired on their first footstep would be
    // held - burst budget and hero count with them - until they walked out of
    // the cull radius. Asked only of an actor who is animated *now*, so a quiet
    // stretch in the middle of a fall cannot end a knockdown early, and only of
    // one that has actually seen a contact, so a freshly acquired actor whose
    // stamps are still `kLongAgo` is not released on the tick it arrived.
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
    // Counted over the in-use slots rather than indexed into the vector: the
    // pool reuses retired entries, so the raw index is a slot number and not
    // an actor number, and a caller walking 0..TrackedActors() would read a
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
