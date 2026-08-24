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
constexpr std::size_t kVoiceRing = 64;

/// The voice budget, which used to be two config knobs and is not a thing worth
/// tuning by ear: below these the mix loses moments it needed and above them the
/// game's audio engine is the one that starts dropping sounds, neither of which
/// is a decision anybody makes from the panel.
///
/// One booking is **one voice the renderer will actually open**, not one layer.
/// That distinction is the whole of this budget. A composite is a timed stack of
/// four layers that the renderer mixes, to the sample, into a single buffer and
/// hands the engine as one BSSoundHandle - so charging per layer was counting a
/// thing that stopped existing when the mixing did. Twelve slots meant three
/// composites, the burst rule routinely asks for five, and the cap was quietly
/// deciding the mix behind every rule that was supposed to.
///
/// Per actor is the design's own number (§14, "six to eight per actor"), which
/// was always written in real voices and only now measures them. It is a mix
/// decision as much as a budget one: eight overlapping moments on one body is
/// already more than reads as one fall.
constexpr std::size_t kVoiceCapPerActor = 8;

/// There is deliberately no global cap.
///
/// There was one, and it was a guess about the engine that the engine does not
/// share: a voice-limit run started 288 sounds with 224 alive at once and the
/// manager holding 257, and refused none of them - it never found a ceiling, it
/// ran out of sound to play first. A global cap could therefore only ever take
/// sound away from the second and third body in a brawl to guard against a limit
/// that was not there, and it took it from whoever asked last rather than from
/// whoever mattered least.
///
/// What decides how much is heard is the rate cap, the chain merge, temporal
/// masking and burst shaping - rules that judge the mix. A ceiling on the count
/// judges nothing.
/// A loop's voice does not end on a clock; it ends when it is stopped.
constexpr TimeMs kNever = 1.0e18;

/// One voice in flight. The id is 0 for a one-shot and the loop's own id for a
/// loop, which is how a stop gives the voice back.
///
/// `actorId` is carried so the global list can always be swept by owner. A loop
/// books `kNever` and is therefore only ever given back by name, so without an
/// owner an actor released mid-fall - which is every knockdown the NPC gets up
/// from before the bed has faded - stranded its loop's slot for the rest of the
/// session. Twenty-four of those and the global cap is permanently full: every
/// later contact is dropped at `TakeVoice` and the mod goes silent with no error
/// anywhere, which is exactly what the 12:12 log shows.
struct Voice {
    TimeMs endsMs{};
    std::uint32_t voiceId{};
    ActorId actorId{};
};
constexpr std::size_t kSiteCount = static_cast<std::size_t>(LimbSite::kCount);

[[nodiscard]] bool Ancient(TimeMs t) { return t <= kLongAgo / 2.0; }

[[nodiscard]] float Lerp(float a, float b, float t) { return a + (b - a) * t; }

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
    bool ignoreRateCap{};

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
    int crunchCount{};
    bool crunchArmed{true};
    bool bedRunning{};
    std::uint32_t bedVoice{};
    float bedLastDb{kSilentDb};
    bool riseRunning{};
    std::uint32_t riseVoice{};
    float riseLastDb{kSilentDb};
    bool scrapeRunning{};
    std::uint32_t scrapeVoice{};
    float scrapeLastDb{kSilentDb};

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

    /// The last graze's own tags. The slide-end impact is a contact the solver
    /// never reported, so it is coloured by the limb that was demonstrably
    /// grinding along the floor a frame ago rather than by a guess.
    std::uint16_t scrapeLimb{};
    LimbSite slideSite{};
    LimbChain slideChain{};
    Coverage slideCoverage{};
    SurfaceClass slideSurface{SurfaceClass::kSoft};
    float slideRadius{};

    bool settleEmitted{};

    // voices in flight
    Voice voices[kVoiceRing]{};
    std::size_t voiceCount{};

    EngineStats stats{};
};

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
};

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
[[nodiscard]] float AirTimeMs(const StrategyContext& ctx, const Contact& contact,
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
    if (ctx.actor.state.haveBodySamples) {
        if (!ctx.actor.state.airborne) {
            return 0.0f;
        }
        // freeFlightSinceMs, not airborneSinceMs: a body that has been hauled
        // through the air for half a second has been unsupported that whole time
        // and falling for none of it, and the rules downstream pay out for
        // falling. See CrashState::driven.
        return std::max(0.0f,
                        static_cast<float>(contact.timeMs - ctx.actor.state.freeFlightSinceMs));
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
    return since;
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

/// What a head strike is worth in broken bone, and in what is left of the head.
///
/// Deterministic, unlike the body's probability gate - see HeadImpactConfig's
/// damage block for why the two are shaped differently. Measured on raw
/// `impactSpeed` as a fraction of the loud anchor, like every other gate.
///
/// Computed here rather than passed between strategies, exactly as `ClassifyHead`
/// is: it is a handful of floats, and it keeps HeadImpact and CrunchGore
/// independent of each other's running order while still letting the second one
/// yield to the first.
struct HeadDamage {
    bool crunch{};
    bool gore{};
    float crunchGainDb{};
    float goreGainDb{};
};

[[nodiscard]] HeadDamage ClassifyHeadDamage(const StrategyContext& ctx, const Contact& contact) {
    const HeadImpactConfig& head = ctx.cfg.strategies.head;
    HeadDamage damage{};
    if (!head.enabled || !head.damageEnabled || contact.site != LimbSite::kHead) {
        return damage;
    }

    const float anchor = std::max(1.0f, ctx.cfg.intensity.speedRefHigh);
    const float crunchAt = head.crunchAtFrac * anchor;
    const float goreAt = head.goreAtFrac * anchor;
    if (contact.impactSpeed < crunchAt) {
        return damage;
    }

    // The crunch ramp ends where the gore's begins, so the two read as one
    // statement about how bad it was rather than two overlapping ones.
    //
    // A gore threshold at or under the crunch's leaves no span to ramp over.
    // That collapses to "full at the threshold" rather than dividing by a span
    // of one unit - the same trap the body's crunch ramp documents, where a
    // degenerate span turned a ramp into a step in the wrong place.
    const float crunchSpan = goreAt - crunchAt;
    const float crunchRamp =
        crunchSpan > 1.0f ? std::clamp((contact.impactSpeed - crunchAt) / crunchSpan, 0.0f, 1.0f)
                          : 1.0f;
    damage.crunch = true;
    damage.crunchGainDb = Lerp(head.crunchQuietDb, head.crunchLoudDb, crunchRamp);

    if (contact.impactSpeed < goreAt) {
        return damage;
    }

    // Past here the crunch is capped at its loud end by construction - the ramp
    // above already clamped - and everything further is the gore's.
    const float goreSpan = head.goreFullFrac * anchor - goreAt;
    const float goreRamp =
        goreSpan > 1.0f ? std::clamp((contact.impactSpeed - goreAt) / goreSpan, 0.0f, 1.0f) : 1.0f;
    damage.gore = true;
    damage.goreGainDb = Lerp(head.goreQuietDb, head.goreLoudDb, goreRamp);
    return damage;
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
    proposal.ignoreRateCap = proposal.ignoreRateCap || waiver.ignoreRateCap;
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
                                 .ignoreRateCap = hero.ignoreRateCap,
                                 .resetsBurst = ctx.actor.heroResetsBurst});
}

void EmitLoopProposal(const StrategyContext& ctx, ProposalList& out, bool& running,
                      std::uint32_t& voice, float& lastGainDb, SlotId slot, float gainDb,
                      float pitch, CueReason reason, float fadeMs) {
    // A running loop only needs a cue when something about it actually changed.
    // Without this the bed emits an update every frame, which buries the cue list
    // under three hundred no-ops and makes "cues out" mean nothing.
    if (running && std::fabs(gainDb - lastGainDb) < 0.75f) {
        return;
    }
    lastGainDb = gainDb;

    Proposal proposal{};
    proposal.timeMs = ctx.nowMs;
    proposal.actorId = ctx.actor.state.actorId;
    proposal.position = ctx.actor.bodyPoint;
    proposal.surface = ctx.actor.state.surfaceUnder;
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
        const float bias = impact.pitchIntensityBiasSemis * contact.intensity;
        const float scatter = ctx.rng.Bipolar() * impact.pitchScatterSemis;
        const float semis = std::clamp(bias + scatter, -impact.pitchMaxSemis, impact.pitchMaxSemis);
        const float pitch = SemitonesToRatio(semis);

        const auto layer = [&](SlotId slot, float offsetMs, float minDb, float maxDb,
                               CueReason reason) {
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
            out2.pitch = pitch;
            out2.reason = reason;
        };

        // Loudness comes from layer balance, not from tiers: a light contact is
        // mostly transient with almost no sub, a heavy one is sub-dominant with
        // the transient riding on top. One continuum, no boundaries to hide.
        layer(SlotId::kImpTransient, impact.transientOffsetMs, impact.transientGainAtMinDb,
              impact.transientGainAtMaxDb, CueReason::kImpactComposite);
        const SurfaceConfig& surf = ctx.cfg.surfaces;
        if (surf.enabled) {
            layer(SurfaceSlot(contact.surface), surf.offsetMs, surf.gainAtMinDb,
                  surf.gainAtMaxDb, CueReason::kSurfaceSkin);
        }
        layer(SlotId::kImpBody, impact.bodyOffsetMs, impact.bodyGainAtMinDb,
              impact.bodyGainAtMaxDb, CueReason::kImpactComposite);
        layer(SlotId::kImpSub, impact.subOffsetMs, impact.subGainAtMinDb, impact.subGainAtMaxDb,
              CueReason::kImpactComposite);

        for (int i = 0; i < proposal.layerCount; ++i) {
            proposal.levelDb = std::max(proposal.levelDb, proposal.layers[i].gainDb);
        }
        out.push_back(proposal);
        return false;
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

        // The surface colour, on what used to be the one cue in the mod that could
        // not say what it hit. Same slot the composite's skin resolves to, so a
        // scuff and a landing on the same boards name the same floor - only
        // quieter, tighter, and held under the grain it is colouring.
        //
        // Deliberately left out of `levelDb`: the tap's rank is the tap's, so
        // colouring one can never move it up the arbitrator's sort. The headroom
        // clamp is what keeps that true of the mix as well as of the sort.
        const SurfaceConfig& surf = ctx.cfg.surfaces;
        if (surf.enabled && surf.onTaps && proposal.layerCount < static_cast<int>(kMaxLayers)) {
            Layer& skin = proposal.layers[proposal.layerCount++];
            skin.slot = SurfaceSlot(contact.surface);
            skin.offsetMs = surf.tapOffsetMs;
            skin.gainDb =
                std::min(contact.onsetGainDb + Lerp(surf.tapGainAtMinDb, surf.tapGainAtMaxDb,
                                                    contact.intensity),
                         proposal.levelDb + surf.tapHeadroomDb);
            // The ordinary scatter, not the tap's 1.6x: a repeat is audible as a
            // repeat in the grain, but the skin is colour and wants to stay put.
            skin.pitch = SemitonesToRatio(std::clamp(ctx.rng.Bipolar() * impact.pitchScatterSemis,
                                                     -impact.pitchMaxSemis, impact.pitchMaxSemis));
            skin.reason = CueReason::kSurfaceSkin;
        }
        out.push_back(proposal);
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
        // Head-down attitude, how long the body was in the air before the head
        // arrived, and how much company it has all move the same gate - see
        // ClassifyHead.
        const HeadStrike strike = ClassifyHead(ctx, contact);

        // Damage first, and deliberately above the accent's own gate: how hard
        // the skull landed and whether it earns an accent are different
        // questions, and a gate raised for voicing reasons should not quietly
        // take the consequences with it. In practice both thresholds sit well
        // above `gateFrac`, so the accent has fired anyway - but which of the
        // two is in charge should not depend on that staying true.
        //
        // Both are ride-alongs, so if the arbitrator drops this contact's onset
        // they die with it. A crunch with nothing under it is not a sound
        // anybody can place.
        const HeadDamage damage = ClassifyHeadDamage(ctx, contact);
        if (damage.crunch &&
            ctx.actor.crunchCount < ctx.cfg.strategies.crunch.maxCrunchesPerEvent) {
            // The same per-knockdown budget the body's rule spends from, so a
            // long tumble cannot become a bag of breaking sticks by coming in
            // through two doors.
            ++ctx.actor.crunchCount;
            const float accentOffsetMs = ctx.cfg.strategies.impact.bodyOffsetMs;
            out.push_back(RideAlongLayer(ctx, contact, SlotId::kCrunchGran,
                                         contact.onsetGainDb + damage.crunchGainDb,
                                         CueReason::kCrunch,
                                         accentOffsetMs + head.crunchDelayMs));
            if (damage.gore) {
                out.push_back(RideAlongLayer(ctx, contact, SlotId::kGoreWet,
                                             contact.onsetGainDb + damage.goreGainDb,
                                             CueReason::kGore,
                                             accentOffsetMs + head.goreDelayMs));
            }
            spdlog::debug("head damage on seq {} at {:.0f} u/s: crunch {:.1f} dB{}",
                          contact.sourceSeq, contact.impactSpeed, damage.crunchGainDb,
                          damage.gore ? std::format(", gore {:.1f} dB", damage.goreGainDb) : "");
        }

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

/// The gnarly gate. Discrete, because you cannot have thirty percent of a bone
/// break and one played quietly sounds like a bug - so it is softened with
/// probability, never with volume, plus hysteresis so it does not flicker
/// through a tumble.
class CrunchGoreStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "CrunchGore"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList& out) override {
        const CrunchGoreConfig& crunch = ctx.cfg.strategies.crunch;

        // A head strike the head's own damage rule fired on is already broken,
        // with a crunch placed and a level chosen for it. Re-deriving the answer
        // here rather than being told is the same pattern `ClassifyHead` uses in
        // two strategies: a handful of floats, and neither strategy has to know
        // the other's running order.
        if (ClassifyHeadDamage(ctx, contact).crunch) {
            return false;
        }

        // Every gate here is a fraction of the loud anchor, so the whole tier
        // structure moves with the range rather than having to be re-derived
        // whenever the anchor does.
        const float anchor = std::max(1.0f, ctx.cfg.intensity.speedRefHigh);
        const float goreGate = crunch.goreGateFrac * anchor;
        float crunchGate = crunch.crunchGateFrac * anchor;
        const float certainSpeed = crunch.crunchCertainFrac * anchor;
        const float hysteresis = crunch.crunchHysteresisFrac * anchor;

        // A head that led the body in is held to its own gate and its own
        // chance, which is what puts a crunch on a slow dive without putting one
        // on every fast sprawl: Vayne log_2's 294 u/s faceplant crunches where
        // its 402 u/s spine whip, faster by a third, does not.
        const AirTimeConfig& air = ctx.cfg.strategies.airTime;
        bool ledHead = false;
        if (air.headEnabled && air.headCrunchGateFrac > 0.0f) {
            const HeadStrike strike = ClassifyHead(ctx, contact);
            const float headGate = air.headCrunchGateFrac * anchor;
            if (strike.airFull && headGate < crunchGate) {
                crunchGate = headGate;
                ledHead = true;
            }
        }

        // Optionally a glancing landing has to arrive proportionally faster to
        // break something - see GlancingImpactConfig::scaleCrunchGate. Off, a
        // bone breaks at whatever angle it likes and the glance rule has already
        // made the crunch quieter through the intensity behind its level.
        const float crunchSpeed = ctx.cfg.glancing.scaleCrunchGate
                                      ? contact.impactSpeed * contact.glanceScale
                                      : contact.impactSpeed;

        // Gore sits at the obliterate tier, above anything a fall can produce.
        if (crunch.goreEnabled && crunchSpeed >= goreGate &&
            crunchSpeed >= ctx.cfg.intensity.obliterateFrac * anchor) {
            out.push_back(Accessory(ctx, contact, SlotId::kGoreWet,
                                    contact.onsetGainDb + crunch.goreGainDb, CueReason::kGore));
            return false;
        }
        if (!crunch.crunchEnabled || ctx.actor.crunchCount >= crunch.maxCrunchesPerEvent) {
            return false;
        }
        if (crunchSpeed < crunchGate) {
            if (crunchSpeed < crunchGate - hysteresis) {
                ctx.actor.crunchArmed = true;
            }
            return false;
        }
        if (!ctx.actor.crunchArmed) {
            return false;
        }
        // An ordinary shove peaks at 355-543 u/s and a three-metre fall at
        // 600-855, so a gate at 500 reaching certainty at 700 has an ordinary
        // knockdown crack occasionally - which feels alive - and a real fall
        // crack every time.
        //
        // The ramp starts at `crunchGateProbability` rather than at zero. A gate
        // whose chance is nil at the speed it opens at has not opened there at
        // all, and the bottom of the span was dead: with 500/700 the first
        // crunches only appeared around 550, which is not what either the slider
        // or the design says. The floor makes the threshold a maybe, which is
        // what softening a gate with probability was always supposed to mean.
        //
        // A certain speed at or below the gate is not an error and must not
        // become one: it is how you ask for a hard gate. Collapse it to
        // certainty above the threshold rather than dividing by a span of one
        // unit, which turned the ramp into a step in the wrong place.
        const float span = certainSpeed - crunchGate;
        const float ramp = span > 1.0f
                               ? std::clamp((crunchSpeed - crunchGate) / span, 0.0f, 1.0f)
                               : 1.0f;
        const float floor = std::clamp(crunch.crunchGateProbability, 0.0f, 1.0f);
        // A led head does not use that ramp - see `headCrunchProbability`. Its
        // gate sits far below `crunchCertainFrac`, so the ramp would put a
        // faceplant at the very bottom of the span and it would almost never
        // fire, which is the failure this rule exists to remove.
        const float probability = ledHead ? std::clamp(air.headCrunchProbability, 0.0f, 1.0f)
                                          : floor + (1.0f - floor) * ramp;
        if (ctx.rng.Unit() > probability) {
            return false;
        }
        ctx.actor.crunchArmed = false;
        ++ctx.actor.crunchCount;
        out.push_back(Accessory(ctx, contact, SlotId::kCrunchGran,
                                contact.onsetGainDb + crunch.crunchGainDb, CueReason::kCrunch));
        return false;
    }

private:
    [[nodiscard]] static Proposal Accessory(const StrategyContext& ctx, const Contact& contact,
                                            SlotId slot, float gainDb, CueReason reason) {
        return RideAlongLayer(ctx, contact, slot, gainDb, reason,
                              ctx.cfg.strategies.impact.bodyOffsetMs);
    }
};

/// The voicing of `Motion::kSlide`: a low grinding rumble with grain riding on
/// it, about 20 dB under the impacts, not a hiss.
///
/// It decides nothing about *when* a slide happens. That is the motion axis',
/// and this strategy's only questions are how loud, how fast, and - when it
/// stops - how quickly to let go. Loudness and pitch are the body's measured
/// speed and nothing else: a slide is exactly as loud as the body is fast, which
/// is the one thing about it a listener can check against what they see. The
/// fades stay because their job is to hide the transition, not to shape the
/// level.
///
/// Every one of those used to be something else. The level rode a distance ramp
/// and a running maximum of contact tangent speed, and the "pitch with speed"
/// slider was named for a speed nothing had measured - the tangent of whichever
/// graze had been fastest since the slide opened, held for as long as it lasted.
/// All three were standing in for a body speed the engine did not have when they
/// were written and does now.
class ScrapeLoopStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ScrapeLoop"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList&) override {
        // Claimed: a graze feeds the scrape path and must not also land on the
        // impact path as a thud. The bookkeeping behind the claim lives in
        // Stage 1 with the rest of the per-contact tallies, because the motion
        // axis reads it and Stage 2 runs before Stage 3.
        return ctx.cfg.strategies.scrape.enabled && contact.graze;
    }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        const bool wants = scrape.enabled && actor.state.motion == Motion::kSlide &&
                           actor.haveBodyPoint && actor.state.tier == DistanceTier::kFull;

        if (wants) {
            const float speed = SlideSpeed(actor);
            const float span = std::max(1.0f, scrape.speedForMaxGain - scrape.speedForMinGain);
            const float track =
                std::clamp((speed - scrape.speedForMinGain) / span, 0.0f, 1.0f);
            const float pitch = 1.0f + scrape.pitchPerThousandUnits * speed / 1000.0f;
            const std::size_t before = out.size();
            EmitLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice, actor.scrapeLastDb,
                             SlotId::kScrapeLoop,
                             scrape.gainDb + Lerp(scrape.speedRangeDb, 0.0f, track), pitch,
                             CueReason::kScrape, scrape.startFadeMs);
            if (out.size() > before) {
                out.back().limbIndex = actor.scrapeLimb;
            }
        } else if (actor.scrapeRunning) {
            // Which fade, from how the slide ended. A slide that ends in
            // friction ends slowly; one that ends because the body launched ends
            // the instant the surface does, and the ordinary fade drags a
            // grinding rumble out behind a body that is already in the air.
            const float fadeMs = actor.state.slideExit == SlideExit::kLaunched
                                     ? scrape.launchFadeMs
                                     : scrape.stopFadeMs;
            StopLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice,
                             SlotId::kScrapeLoop, CueReason::kScrape, fadeMs);
        }
    }
};

/// The continuous bed: cloth, air, and the airborne anticipation rise as a
/// parameter on it. The references put it 30-36 dB under the hero hit, and it is
/// what papers over the one-shots underneath.
class MotionFoleyStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "MotionFoley"; }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const MotionFoleyConfig& foley = ctx.cfg.strategies.foley;
        ActorRuntime& actor = ctx.actor;

        // haveBodyPoint, and not just the phase: a loop is placed at a world
        // point, and before the first contact of a knockdown there is no world
        // point to place it at. Starting one anyway put the bed at the origin.
        // The wait is a frame or two of a fall that has not made a sound yet.
        const bool wantsBed = foley.enabled && actor.haveBodyPoint &&
                              actor.state.tier == DistanceTier::kFull &&
                              actor.state.motion != Motion::kResting;
        if (wantsBed) {
            const float span = std::max(1.0f, foley.speedForMaxGain - foley.speedForMinGain);
            const float track =
                std::clamp((actor.state.bodySpeed - foley.speedForMinGain) / span, 0.0f, 1.0f);
            float gainDb = foley.bedGainDb + Lerp(-14.0f, 0.0f, track);
            // The bed falls 8-15 dB in the 50-100 ms before a hero impact in
            // three of the four reference clips. Three out of four is not enough
            // to build on, so this is a toggle to be tuned by ear.
            if (foley.preImpactDuck && actor.state.airborne) {
                gainDb += foley.preImpactDuckDb;
            }
            EmitLoopProposal(ctx, out, actor.bedRunning, actor.bedVoice, actor.bedLastDb,
                             SlotId::kFoleyCloth, gainDb, 1.0f, CueReason::kFoleyBed, 120.0f);
        } else if (actor.bedRunning) {
            StopLoopProposal(ctx, out, actor.bedRunning, actor.bedVoice, SlotId::kFoleyCloth,
                             CueReason::kFoleyBed, 250.0f);
        }

        // Skipped for your own ragdoll: you are the one moving and the view
        // already tells you.
        const bool wantsRise = foley.enabled && foley.airborneRise && actor.haveBodyPoint &&
                               actor.state.airborne &&
                               actor.state.tier == DistanceTier::kFull &&
                               !(actor.isPlayer && ctx.cfg.player.skipAirborneWhoosh);
        if (wantsRise) {
            EmitLoopProposal(ctx, out, actor.riseRunning, actor.riseVoice, actor.riseLastDb,
                             SlotId::kAirWhoosh, foley.airborneRiseGainDb, 1.0f,
                             CueReason::kAirborneRise, 150.0f);
        } else if (actor.riseRunning) {
            StopLoopProposal(ctx, out, actor.riseRunning, actor.riseVoice, SlotId::kAirWhoosh,
                             CueReason::kAirborneRise, 200.0f);
        }
    }
};

/// One quiet cue when the energy drops below the floor. Falls that trail off
/// feel unfinished - and exactly one, not one per limb that is still twitching.
class SettleCloseStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "SettleClose"; }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        ActorRuntime& actor = ctx.actor;
        if (!ctx.cfg.strategies.settle.enabled || actor.settleEmitted ||
            actor.state.motion != Motion::kResting) {
            return;
        }
        if ((ctx.nowMs - actor.state.motionEnteredMs) < ctx.cfg.strategies.settle.delayMs) {
            return;
        }
        // Never while the body is measurably still in the air. The motion rule
        // already refuses to enter Resting with the body moving, so this is a
        // backstop rather than the guard - but the failure it guards against is
        // the one that shipped: the closing cue fired 184 ms into a flight and
        // 121 units above the ground, and a fall that announces it is over
        // while the body is still falling is the single most obviously broken
        // thing this engine can do.
        if (actor.state.airborne) {
            return;
        }
        actor.settleEmitted = true;

        Proposal proposal{};
        proposal.timeMs = ctx.nowMs;
        proposal.actorId = actor.state.actorId;
        proposal.site = actor.state.leadingLimb;
        proposal.surface = actor.state.surfaceUnder;
        proposal.position = actor.bodyPoint;
        proposal.bypass = true;
        proposal.levelDb = ctx.cfg.strategies.settle.gainDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = SlotId::kSettleRest;
        proposal.layers[0].gainDb = ctx.cfg.strategies.settle.gainDb;
        proposal.layers[0].reason = CueReason::kSettleClose;
        out.push_back(proposal);
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
    std::vector<Voice> globalVoices;

    // Stage 3 in the order it runs. ScrapeLoop first because it is the only one
    // that claims; CrunchGore last because it only ever rides along.
    ScrapeLoopStrategy scrape;
    HeadImpactStrategy head;
    ImpactCompositeStrategy composite;
    CrunchGoreStrategy crunch;
    MotionFoleyStrategy foley;
    SettleCloseStrategy settle;
    std::array<IStrategy*, 6> strategies{};

    Impl() {
        drained.reserve(256);
        contacts.reserve(128);
        proposals.reserve(128);
        order.reserve(128);
        acceptedSeqs.reserve(128);
        actors.reserve(8);
        globalVoices.reserve(kVoiceRing);
        strategies = {&scrape, &head, &composite, &crunch, &foley, &settle};
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

    ActorRuntime& Acquire(ActorId id, const ActorProfile* profile) {
        if (ActorRuntime* existing = Find(id); existing != nullptr) {
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
        slot->state.motion = Motion::kResting;
        slot->name = profile != nullptr && !profile->name.empty() ? profile->name
                                                                  : std::format("{:08X}", id);
        slot->isPlayer = profile != nullptr && profile->isPlayer;
        return *slot;
    }

    void Release(ActorRuntime& actor, std::string_view why) {
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
        StopActorLoops(actor);

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
        actor.voiceCount = 0;
        std::erase_if(globalVoices, [id](const Voice& v) { return v.actorId == id; });

        actor.inUse = false;
        actor.state = CrashState{};
    }

    /// Every loop this actor still holds, stopped for real: a cue to the sink so
    /// the renderer lets its voice go, and the budget entry given back.
    void StopActorLoops(ActorRuntime& actor) {
        StopOneLoop(actor, actor.bedRunning, actor.bedVoice, SlotId::kFoleyCloth,
                    CueReason::kFoleyBed, 250.0f);
        StopOneLoop(actor, actor.riseRunning, actor.riseVoice, SlotId::kAirWhoosh,
                    CueReason::kAirborneRise, 200.0f);
        StopOneLoop(actor, actor.scrapeRunning, actor.scrapeVoice, SlotId::kScrapeLoop,
                    CueReason::kScrape, cfg.strategies.scrape.stopFadeMs);
    }

    void StopOneLoop(ActorRuntime& actor, bool& running, std::uint32_t voiceId, SlotId slot,
                     CueReason reason, float fadeMs) {
        if (!running) {
            return;
        }
        running = false;
        ReleaseVoice(actor, voiceId);
        if (sink == nullptr) {
            return;
        }
        // Straight to the sink rather than through a proposal: arbitration has
        // already run this tick, and a stop is not a thing the arbitrator may
        // decline anyway.
        Cue cue{};
        cue.timeMs = actor.lastAdmittedMs;
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
        switch (state.motion) {
            case Motion::kLaunch: return cfg.motion.launch;
            case Motion::kAirborne: return cfg.motion.airborne;
            case Motion::kTumble: return cfg.motion.tumble;
            case Motion::kSlide: return cfg.motion.slide;
            case Motion::kResting:
            case Motion::kCount:
                break;
        }
        return cfg.motion.resting;
    }

    /// The role trim - every surface skin together, every grain together. The
    /// balance between kinds of layer, which is what the composite is about.
    [[nodiscard]] float RoleTrimDb(SlotId slot, bool isPlayer) const {
        switch (slot) {
            case SlotId::kImpTransient:
                return cfg.mix.transientTrimDb;
            case SlotId::kImpBody:
                return cfg.mix.bodyTrimDb;
            case SlotId::kImpSub:
                // A 30 Hz boom at zero distance through headphones is
                // overwhelming, and in VR low frequency is felt as much as heard.
                return cfg.mix.subTrimDb + (isPlayer ? cfg.player.subTrimDb : 0.0f);
            case SlotId::kSurfWood:
            case SlotId::kSurfStone:
            case SlotId::kSurfSoft:
                return cfg.surfaces.trimDb;
            case SlotId::kLimbTap:
            case SlotId::kCrunchGran:
            case SlotId::kGoreWet:
            case SlotId::kHeadImpact:
            case SlotId::kSettleRest:
                return cfg.mix.grainTrimDb;
            case SlotId::kScrapeLoop:
            case SlotId::kFoleyCloth:
            case SlotId::kAirWhoosh:
                return cfg.mix.loopTrimDb;
            case SlotId::kGruntImpact:
            case SlotId::kScreamBig:
            case SlotId::kCount:
                break;
        }
        return 0.0f;
    }

    /// The per-file trim - see SlotGainConfig. The two declared-and-unfilled
    /// slots have none, for the same reason they have no mute: nothing ever
    /// resolves to them, so a slider for either would be a control over silence.
    [[nodiscard]] float SlotTrimDb(SlotId slot) const {
        const SlotGainConfig& g = cfg.slotGains;
        switch (slot) {
            case SlotId::kImpTransient: return g.impTransient;
            case SlotId::kImpBody:      return g.impBody;
            case SlotId::kImpSub:       return g.impSub;
            case SlotId::kSurfWood:     return cfg.surfaces.woodTrimDb;
            case SlotId::kSurfStone:    return cfg.surfaces.stoneTrimDb;
            case SlotId::kSurfSoft:     return cfg.surfaces.softTrimDb;
            case SlotId::kLimbTap:      return g.limbTap;
            case SlotId::kCrunchGran:   return g.crunchGran;
            case SlotId::kGoreWet:      return g.goreWet;
            case SlotId::kScrapeLoop:   return g.scrapeLoop;
            case SlotId::kFoleyCloth:   return g.foleyCloth;
            case SlotId::kAirWhoosh:    return g.airWhoosh;
            case SlotId::kHeadImpact:   return g.headImpact;
            case SlotId::kSettleRest:   return g.settleRest;
            default:                    return 0.0f;
        }
    }

    [[nodiscard]] float LayerTrimDb(SlotId slot, bool isPlayer) const {
        return RoleTrimDb(slot, isPlayer) + SlotTrimDb(slot);
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

    void ExpireVoices(ActorRuntime& actor, TimeMs nowMs) {
        std::size_t write = 0;
        for (std::size_t i = 0; i < actor.voiceCount; ++i) {
            if (actor.voices[i].endsMs > nowMs) {
                actor.voices[write++] = actor.voices[i];
            }
        }
        actor.voiceCount = write;
        std::erase_if(globalVoices, [nowMs](const Voice& v) { return v.endsMs <= nowMs; });
    }

    /// A one-shot books a voice for as long as its file runs. A loop books one
    /// until it is stopped - `kNever`, released by ReleaseVoice - because a loop
    /// that has been playing for longer than its own file length has not
    /// finished, it has wrapped, and expiring it by time would hand the same
    /// voice out twice and quietly raise the cap.
    [[nodiscard]] bool TakeVoice(ActorRuntime& actor, TimeMs endsMs, std::uint32_t voiceId) {
        if (actor.voiceCount >= kVoiceCapPerActor || actor.voiceCount >= kVoiceRing) {
            spdlog::debug("voice cap: {} is holding {} of {} of its own", actor.name,
                          actor.voiceCount, kVoiceCapPerActor);
            return false;
        }
        actor.voices[actor.voiceCount++] = Voice{endsMs, voiceId, actor.state.actorId};
        globalVoices.push_back(Voice{endsMs, voiceId, actor.state.actorId});
        return true;
    }

    /// Give back the booking made most recently by this actor.
    ///
    /// A one-shot's booking has no id to release it by - only loops carry one -
    /// and it does not need one: nothing else books between taking a proposal's
    /// voice and finding out whether the proposal made a sound, so the newest
    /// entry is always the one to undo.
    void UntakeLastVoice(ActorRuntime& actor) {
        if (actor.voiceCount > 0) {
            --actor.voiceCount;
        }
        if (!globalVoices.empty()) {
            globalVoices.pop_back();
        }
    }

    void ReleaseVoice(ActorRuntime& actor, std::uint32_t voiceId) {
        if (voiceId == 0) {
            return;
        }
        std::size_t write = 0;
        for (std::size_t i = 0; i < actor.voiceCount; ++i) {
            if (actor.voices[i].voiceId != voiceId) {
                actor.voices[write++] = actor.voices[i];
            }
        }
        actor.voiceCount = write;
        std::erase_if(globalVoices, [voiceId](const Voice& v) { return v.voiceId == voiceId; });
    }

    // ── Stage 0: ingest ──────────────────────────────────────────────────────

    void Ingest(IFeed& feed) {
        contacts.clear();
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
            if (event.phase != ActorPhase::kRagdoll) {
                continue;
            }

            const ActorProfile* profile = feed.Profile(event.actorId);
            ActorRuntime& actor = Acquire(event.actorId, profile);

            if (event.impactSpeed < cfg.ingest.minImpactSpeed) {
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
        // the threshold a self-contact only thickens the bed; above it a genuine
        // self-hit gets through.
        std::size_t write = 0;
        for (std::size_t i = 0; i < contacts.size(); ++i) {
            const Contact& contact = contacts[i];
            if (contact.selfContact && contact.impactSpeed < cfg.ingest.selfContactThreshold) {
                ++stats.routedToFoley;
                if (ActorRuntime* actor = Find(contact.actorId); actor != nullptr) {
                    ++actor->stats.routedToFoley;
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
    }

    void HandleState(const FeedEvent& event, IFeed& feed) {
        const std::string_view state{event.text};
        if (state == "ragdoll_start") {
            ActorRuntime& actor = Acquire(event.actorId, feed.Profile(event.actorId));
            if (actor.state.motion == Motion::kResting) {
                actor.state.ragdollStartMs = event.timeMs;
                EnterMotion(actor, Motion::kLaunch, event.timeMs);
                spdlog::debug("actor {} ragdoll_start -> Launch", actor.name);
            }
            return;
        }
        if (state == "ragdoll_end" || state == "knock_get_up" || state == "actor_gone" ||
            state == "session_stop") {
            if (ActorRuntime* actor = Find(event.actorId); actor != nullptr) {
                Release(*actor, state);
            }
        }
    }

    // ── Stage 1 and 2: crash state, then the two axes ────────────────────────

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
        const bool detect = cfg.motion.drivenEnabled && cfg.motion.drivenResidual > 0.0f;
        const bool wasDriven = state.driven;
        if (detect && state.airborne && state.drivenResidual > cfg.motion.drivenResidual) {
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
            spdlog::debug("actor {} landed at {:.0f} ms after {:.0f} ms and {:.0f} units",
                          actor.name, nowMs, nowMs - state.freeFlightSinceMs, state.fallDropUnits);
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

        // What the settle rule reads is an envelope follower over contact speed,
        // not a running total: "is the fall still going" is a question about the
        // last few hundred milliseconds, and keeping it in units/s is what makes
        // settleEnergyFloor = 45 mean "nothing harder than 45 units/s recently"
        // and comparable with minImpactSpeed and the 355-543 of an ordinary
        // shove. A sum would have made the number depend on how many limbs
        // happened to be touching, and a long gentle scrape would close the event
        // in the middle of the fall.
        const auto elapsed = static_cast<float>(nowMs - actor.energyStampMs);
        if (elapsed > 0.0f) {
            actor.energyRecent *= std::exp(-elapsed / 300.0f);
            // Body speed used to be a guess, and this is the guess. The only
            // measurement available was the last contact's, the capture carried
            // no periodic limb sample, and in the air a body speeds up rather
            // than slowing down - so it was held rather than decayed, and
            // released slowly as a backstop against a fall whose last contact
            // was a hard one. Without the hold, a 533 ms airborne gap
            // mid-knockdown read as the fall having ended and the closing cue
            // landed while the body was still in the air.
            //
            // A take with pose has the measurement, and ConsumePose overwrites
            // this a few lines below. The decay is kept for the takes that do
            // not, which is every take recorded before the pose sidecar existed.
            if (!actor.sawPoseEver) {
                state.bodySpeed *= std::exp(-elapsed / (5.0f * cfg.motion.settleQuietMs));
            }
            // A running maximum never comes down, and this one used to be one:
            // a single fast skim held the slide's entry test open for the rest
            // of the knockdown. Decayed on the grace window, so what it reports
            // is how hard the body has been grinding *recently*.
            actor.slideTangent *=
                std::exp(-elapsed / std::max(20.0f, cfg.motion.slideGraceMs));
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
        // A moment resets the burst on the tick it is anchored and not after.
        actor.heroResetsBurst = false;

        float frameBodySpeed = 0.0f;
        bool grazedThisTick = false;
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
            state.airborne = !Ancient(actor.lastAdmittedMs) &&
                             (nowMs - actor.lastAdmittedMs) >
                                 Window(cfg.motion.airborneMinTimeMs, frameMs, cfg.arb.frameScaleK) &&
                             state.motion != Motion::kResting;
        }

        // Motion first, then moment. The order matters: the moment axis is
        // allowed to read what the body is doing, and the budget the arbitrator
        // finally asks for is a function of both.
        AdvanceMotion(actor, nowMs, frameMs);
        AdvanceMoment(actor, nowMs, frameMs);
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
        const auto quietFor = nowMs - std::max(actor.lastAdmittedMs, actor.firstContactMs);

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
        // The three ways out are coming to rest, going airborne and hitting
        // something. "The tangent hold decayed" is none of them, so all that is
        // asked here is whether the body is still grazing at all.
        const bool slideHolds = grazing;

        // Three conditions, not two: nothing has hit hard recently, the body
        // itself has stopped moving, and it has been quiet long enough.
        // Dropping the middle one closes the event during the airborne gap
        // between two bounces, and the closing cue lands while the body is
        // still in the air.
        const bool spent = actor.energyRecent < cfg.motion.settleEnergyFloor &&
                           state.bodySpeed < cfg.motion.settleEnergyFloor &&
                           quietFor > Window(cfg.motion.settleQuietMs, frameMs, cfg.arb.frameScaleK);

        switch (state.motion) {
            case Motion::kResting: {
                // What wakes a fall that had gone quiet. Not "any contact":
                // the last twenty contacts of a knockdown are limbs flopping,
                // and keeping them at the resting budget is the main lever the
                // design has for staying unobtrusive. But not "nothing"
                // either, which is what the old Settle was - a one-way door
                // that swallowed the six loudest contacts of a fall.
                //
                // The bar scales with the fall's own peak, so it means the same
                // thing in a shove and in a ten-metre drop.
                // The get-up blend, as a minimum dwell. This is what the old
                // machine did too - it stamped the moment it reached Rest and
                // refused to reopen for `getUpBlendMs` after it - and it is
                // measured from entering Resting rather than from the
                // `ragdoll_end` row because that row releases the actor
                // outright, runtime and all, so there is nothing left by then
                // to measure against.
                const bool blended =
                    (nowMs - state.motionEnteredMs) >
                    Window(cfg.motion.getUpBlendMs, frameMs, cfg.arb.frameScaleK);
                const float wakeAt = std::max(cfg.motion.settleEnergyFloor * 2.0f,
                                              state.peakSpeed * cfg.motion.restingExitPeakFrac);
                if (blended && frameSpeed >= wakeAt) {
                    EnterMotion(actor, slideOpens ? Motion::kSlide : Motion::kTumble, nowMs);
                }
                break;
            }
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
                // flight it is no longer in. And a body that ran out of energy
                // while nominally airborne is a pose-less take whose airborne
                // flag never falls, so `spent` has to be able to end it.
                if (touched) {
                    EnterMotion(actor, Motion::kTumble, nowMs);
                } else if (spent) {
                    EnterMotion(actor, Motion::kResting, nowMs);
                } else if (!state.airborne) {
                    EnterMotion(actor, Motion::kTumble, nowMs);
                }
                break;
            case Motion::kTumble:
            case Motion::kSlide:
                // Three ways out of a slide, and only three. The order is the
                // order of certainty: coming to rest and going airborne are
                // *measured*, and hitting something is what is left when neither
                // of those happened - a slide that stops for no reason the body
                // can account for was stopped by something.
                //
                // Spent first. A body that has stopped is resting whatever else
                // the flags say, and on a take with no pose the airborne flag
                // below is exactly what a stopped body looks like.
                if (spent) {
                    LeaveSlide(actor, nowMs, SlideExit::kRested);
                    EnterMotion(actor, Motion::kResting, nowMs);
                } else if (state.airborne && state.haveBodySamples) {
                    // The edge the old machine did not have: `airborne` was
                    // measured every tick and then consulted only in Launch, so
                    // a body that left the ground again halfway down a
                    // staircase was never airborne twice.
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
                    LeaveSlide(actor, nowMs, SlideExit::kStruck);
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

    /// Close the slide, on whichever of the three edges is leaving it.
    ///
    /// One function for all three because the bookkeeping is the same and only
    /// the consequence differs: `kRested` and `kLaunched` just choose the fade,
    /// and `kStruck` also places the impact that stopped it. The exit is left on
    /// the crash state afterwards rather than being a transient - the strategy
    /// reads it on the tick it stops the loop, and the timeline reads it to mark
    /// the end of a span it has already drawn.
    void LeaveSlide(ActorRuntime& actor, TimeMs nowMs, SlideExit why) {
        if (actor.state.motion != Motion::kSlide) {
            return;
        }
        actor.state.slideExit = why;
        spdlog::debug("actor {} slide ended {} at {:.0f} ms after {:.0f} units at {:.0f} u/s",
                      actor.name, ToString(why), nowMs, actor.slideDistance, actor.slideSpeed);
        if (why == SlideExit::kStruck) {
            PlaceSlideImpact(actor, nowMs);
        }
        actor.grazeSinceMs = kLongAgo;
        actor.grazeLastMs = kLongAgo;
        actor.slideDistance = 0.0f;
        actor.slideTangent = 0.0f;
        actor.slideSpeed = 0.0f;
    }

    /// The collision that stopped a slide, which the contact stream regularly
    /// does not report.
    ///
    /// A limb catching on a doorframe fires one glancing row and then the body
    /// is simply stopped: there is no thud in the data to build a cue on, and
    /// what the mod did with that was fade a grinding loop out over silence at
    /// the loudest moment of the slide. So the impact is synthesised - at the
    /// speed the body was travelling when the grazing stopped, on the limb that
    /// was demonstrably grinding along the floor, against the surface it was
    /// grinding on.
    ///
    /// It is a real `Contact` pushed into this tick's stream rather than a cue
    /// of its own, which is the whole reason this is a dozen lines: the ordinary
    /// composite, the crunch gate, arbitration and the trace all pick it up
    /// without knowing it is unusual, and it is built bigger or smaller by the
    /// same intensity curve as everything else.
    ///
    /// Stage 1 has already run for this actor, so it is deliberately not folded
    /// into `energyRecent` or `peakSpeed`: it is our own inference, and letting
    /// it raise the envelope that the rest of the fall is judged against would
    /// be the engine arguing with itself.
    void PlaceSlideImpact(ActorRuntime& actor, TimeMs nowMs) {
        if (!cfg.motion.slideEndImpact || actor.slideSpeed < cfg.ingest.minImpactSpeed) {
            return;
        }
        CrashState& state = actor.state;

        Contact contact{};
        contact.timeMs = nowMs;
        contact.actorId = state.actorId;
        contact.limbIndex = actor.scrapeLimb;
        // Its own range, so it can never collide with a feed row's seq - which
        // the variant shuffle, the accepted set and the trace all key on.
        contact.sourceSeq = nextSlideSeq++;
        contact.impactSpeed = actor.slideSpeed;
        contact.bodySpeed = actor.slideSpeed;
        contact.position = actor.haveBodyPoint ? actor.bodyPoint : state.comPosition;
        contact.normal = Vec3{0.0f, 0.0f, 1.0f};
        contact.site = actor.slideSite;
        contact.chain = actor.slideChain;
        contact.coverage = actor.slideCoverage;
        contact.surface = actor.slideSurface;
        contact.limbRadius = actor.slideRadius;
        // No angular term and no glancing cut: what stopped the slide was the
        // body arriving square against something, which is the opposite of the
        // clipped landing the glancing rule exists to quieten.
        contact.intensity = Intensity(contact.impactSpeed, contact.site, contact.limbRadius,
                                      contact.bodySpeed, 0.0f);
        contact.rawIntensity = contact.intensity;
        contact.onsetGainDb = GainFromIntensity(contact.intensity);
        // In its sorted place rather than on the end: Ingest leaves this list
        // strongest-first and the strategies walk it in that order, so a contact
        // appended after the sort would be the one row in the frame whose
        // position depended on when it was made rather than on how hard it was.
        contacts.insert(std::ranges::lower_bound(contacts, contact, StrongestFirst), contact);

        ++stats.slideImpacts;
        ++actor.stats.slideImpacts;
        spdlog::debug("actor {} slide-end impact seq {} on {} at {:.0f} u/s, intensity {:.2f}",
                      actor.name, contact.sourceSeq, ToString(contact.site), contact.impactSpeed,
                      contact.intensity);

        // The hero clause the dominance test cannot supply. A slide is a long
        // stretch of grazes, so `energyRecent` is *low* when one ends - which
        // would make a gentle stop dominant - and this contact is measured on the
        // body rather than on a limb, so it is not on the same scale as the peak
        // it would be compared against. How fast the body was actually going when
        // it was stopped is the honest test, and it is the one thing a listener
        // can check against what they saw.
        const float gate = cfg.hero.slideEndFrac * cfg.intensity.speedRefHigh;
        if (cfg.hero.enabled && gate > 0.0f && actor.slideSpeed >= gate) {
            AnchorHero(actor, contact, nowMs, state.moment == Moment::kHero, false);
        }
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
            bool arrived = false;
            if (state.haveBodySamples) {
                StrategyContext ctx{cfg, actor, rng, nowMs, frameMs, &nextVoiceId};
                arrived = AirTimeMs(ctx, contact, false, 0.0f) >= hero.arrivalMinAirMs &&
                          state.fallDropUnits >= hero.arrivalMinDropUnits;
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
            actor.crunchCount = 0;
            actor.crunchArmed = true;
            actor.settleEmitted = false;
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
                                nowMs, frameMs, &nextVoiceId};
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
            StrategyContext ctx{cfg, actor, rng, nowMs, frameMs, &nextVoiceId};
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
        }
        // Loudest first, ties broken by the row the contact came from.
        std::ranges::stable_sort(order, [&](std::uint16_t a, std::uint16_t b) {
            if (proposals[a].levelDb != proposals[b].levelDb) {
                return proposals[a].levelDb > proposals[b].levelDb;
            }
            return proposals[a].sourceSeq < proposals[b].sourceSeq;
        });

        const float rateCap = Window(cfg.arb.rateCapMs, frameMs, cfg.arb.frameScaleK);
        const float chainWindow = Window(cfg.arb.chainMergeWindowMs, frameMs, cfg.arb.frameScaleK);
        const float burstWindow = Window(cfg.arb.burstWindowMs, frameMs, cfg.arb.frameScaleK);
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
                // A closing cue while the body is measurably still in the air
                // is the most obviously broken thing this engine can do, and it
                // shipped: a settle fired at 1520 ms against a flight measured
                // from 1115 to 1699, 184 ms into the air and 121 units above
                // the ground, with the real landing still to come at 1765.
                //
                // SettleClose already refuses on the same condition, and
                // refuses without marking itself done so it simply tries again
                // once the body is down. This is the invariant restated where
                // it cannot be missed - it covers any strategy that ever emits
                // a closing cue, not just that one - and the counter is a
                // tripwire: it must read zero, and a build where it does not is
                // a build where something upstream stopped checking.
                if (proposal.layerCount > 0 &&
                    proposal.layers[0].reason == CueReason::kSettleClose &&
                    actor->state.airborne) {
                    ++stats.settleInFlight;
                    ++actor->stats.settleInFlight;
                    Dropped(proposal, actor->state, "settle in flight");
                    continue;
                }
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
            if (sinceOnset < rateCap) {
                // Unless it is properly louder. The cap exists because onsets
                // closer than this add mud rather than detail - but that is only
                // true between comparable levels. A contact well above the one
                // holding the cap is a new event, and silencing it is how a
                // multi-limb crash came out as three sounds: log_4's body slams
                // down 42 ms after its foot lands and 7 dB louder, and was lost.
                const float overDb = proposal.levelDb - actor->lastOnsetDb;
                if (overDb >= cfg.arb.rateCapOverrideDb || proposal.ignoreRateCap) {
                    ++stats.rateCapOverrides;
                    ++actor->stats.rateCapOverrides;
                    spdlog::debug("seq {} at {:.0f} ms is {:.1f} dB over the onset holding the "
                                  "rate cap ({:.0f} ms gap); letting it open its own",
                                  proposal.sourceSeq, proposal.timeMs, overDb, sinceOnset);
                } else {
                    ++stats.droppedRateCap;
                    ++actor->stats.droppedRateCap;
                    char why[32]{};
                    std::snprintf(why, sizeof(why), "rate cap %.0f<%.0f", sinceOnset, rateCap);
                    Dropped(proposal, actor->state, why);
                    continue;
                }
            }

            // 2. Chain merge. A strong hand impact silences the elbow and plays
            //    one arm flop.
            const auto chain = static_cast<std::size_t>(proposal.chain);
            if (proposal.chain != LimbChain::kNone && chain < kChainCount &&
                proposal.timeMs - actor->chainLastMs[chain] < chainWindow &&
                proposal.levelDb <= actor->chainLastDb[chain]) {
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
            const bool burstOpen =
                proposal.timeMs - actor->burstStartMs < burstWindow && !resetsBurst;
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
            actor->lastOnsetDb = proposal.levelDb;
            if (chain < kChainCount) {
                actor->chainLastMs[chain] = proposal.timeMs;
                actor->chainLastDb[chain] = proposal.levelDb;
            }
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
        ExpireVoices(actor, nowMs);

        const PhaseBudget& budget = BudgetFor(actor.state);
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

        const bool collapse = cfg.arb.spatialCollapseOnHero && proposal.boneIndex < 0 &&
                              nowMs < actor.collapseUntilMs;

        // Contact-derived cues only. A bypass proposal - the two loops and the
        // settle cue - carries no intensity of its own, so shaping it by one
        // would be re-levelling the bed against a zero it never had.
        const float postShapeDb = proposal.bypass ? 0.0f : PostShapeDb(proposal.intensity);

        // The class compressor, taken once for the whole proposal.
        //
        // Measured against `levelDb`, which is the max across the layers and is
        // the number Stage 4 sorted on - so the scale is the mod's own, where 0
        // is the hardest contact the engine can hear, and every trim below
        // applies on top of the compressed value rather than being compressed
        // with it. That is what makes the setting survive a change to the master
        // gain: turn the mod up and its compression comes up too. See
        // CompressConfig for why it is not measured at the speaker.
        //
        // Squeezed rather than clamped, which is the other half of the same
        // argument: a hard cap puts everything above it on exactly one level, so
        // a threshold a few dB low turns a dozen distinct impacts into a dozen
        // identical ones. A ratio keeps them ordered.
        //
        // Not per layer, either. The layers of a composite arrive at four
        // different levels on purpose, and holding each of them where it stood
        // would collapse that into a flat stack - sub, body and transient all
        // together - which is a different impact rather than a quieter one. One
        // offset off all of them keeps the shape and moves the level.
        //
        // Which threshold applies comes off layer 0, the layer the proposal was
        // built around: the transient of a composite, the tap, the head accent,
        // the crunch. It is the layer that says what the moment *is*.
        //
        // Stage 5, like every other term on this line, so it cannot decide which
        // contacts win the rate cap - a held hero hit is still the hero hit of
        // its frame.
        float compressCutDb = 0.0f;
        if (proposal.layerCount > 0) {
            compressCutDb = rds::CompressCutDb(
                cfg.compress, rds::CompressThresholdDb(cfg.compress, proposal.layers[0].reason),
                proposal.levelDb);
        }

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
            if (!TakeVoice(actor, endsMs, 0)) {
                ++stats.droppedVoiceCap;
                ++actor.stats.droppedVoiceCap;
                spdlog::debug("voice cap hit on {} for a {}-layer stack", actor.name,
                              proposal.layerCount);
                return 0;
            }
            booked = true;
        }

        for (int slotIndex = 0; slotIndex < proposal.layerCount; ++slotIndex) {
            const Layer& layer = proposal.layers[slotIndex];

            ResolvedSound resolved{};
            if (proposal.op != CueOp::kStopLoop) {
                if (bank == nullptr ||
                    !bank->Resolve(layer.slot, proposal.surface, proposal.coverage, proposal.site,
                                   resolved, proposal.sourceSeq)) {
                    // A declared-and-unfilled slot, or no bank at all. Skipped
                    // silently, which is what makes adding voice later a config
                    // change and not a code change.
                    continue;
                }
            }

            Cue cue{};
            cue.timeMs = proposal.timeMs + layer.offsetMs;
            cue.op = proposal.op;
            cue.slot = layer.slot;
            cue.variant = resolved.variant;
            cue.gainDb = layer.gainDb + compressCutDb + budget.gainTrimDb + masterDb +
                         postShapeDb + proposal.postTrimDb +
                         LayerTrimDb(layer.slot, actor.isPlayer);
            cue.compressCutDb = compressCutDb;
            cue.pitch = layer.pitch;
            cue.fadeMs = proposal.fadeMs;
            cue.position = collapse ? actor.collapsePoint : proposal.position;
            cue.boneIndex = proposal.boneIndex;
            cue.collapsed = collapse;
            cue.voiceId = proposal.voiceId;
            cue.actorId = proposal.actorId;
            cue.limbIndex = proposal.limbIndex;
            cue.site = proposal.site;
            cue.surface = proposal.surface;
            cue.reason = layer.reason;
            cue.motion = actor.state.motion;
            cue.moment = actor.state.moment;
            cue.intensity = proposal.intensity;
            cue.sourceSeq = proposal.sourceSeq;

            if (proposal.op == CueOp::kStopLoop) {
                ReleaseVoice(actor, proposal.voiceId);
            } else {
                if (cue.gainDb < cfg.mix.voiceFloorDb) {
                    // Not worth mixing in. Under the per-proposal booking this
                    // costs nothing on its own - the voice is already paid for -
                    // so it is purely "leave this layer out of the buffer".
                    continue;
                }
                if (proposal.op == CueOp::kStartLoop &&
                    !TakeVoice(actor, kNever, proposal.voiceId)) {
                    ++stats.droppedVoiceCap;
                    ++actor.stats.droppedVoiceCap;
                    spdlog::debug("voice cap hit on {} for the {} loop", actor.name,
                                  ToString(layer.slot));
                    continue;
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
            UntakeLastVoice(actor);
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
            impl.Release(actor, "beyond the cull radius");
        }
    }
}

void Engine::Reset() {
    Impl& impl = *m_impl;
    impl.actors.clear();
    impl.globalVoices.clear();
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

std::size_t Engine::LiveVoices() const { return m_impl->globalVoices.size(); }

std::size_t Engine::TrackedActors() const {
    std::size_t count = 0;
    for (const auto& actor : m_impl->actors) {
        count += actor.inUse ? 1u : 0u;
    }
    return count;
}

}  // namespace rds
