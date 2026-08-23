#include "rds/Engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

#include "rds/Log.h"

namespace rds {
namespace {

constexpr TimeMs kLongAgo = -1.0e9;
constexpr float kSilentDb = -140.0f;
constexpr std::size_t kMaxLayers = 6;
constexpr std::size_t kChainCount = 7;
constexpr std::size_t kVoiceRing = 64;
/// A loop's voice does not end on a clock; it ends when it is stopped.
constexpr TimeMs kNever = 1.0e18;

/// One voice in flight. The id is 0 for a one-shot and the loop's own id for a
/// loop, which is how a stop gives the voice back.
struct Voice {
    TimeMs endsMs{};
    std::uint32_t voiceId{};
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

    float intensity{};    ///< 0..1, what the loudness curve was fed
    float onsetGainDb{};  ///< the level before any layer balance
    std::uint64_t otherBody{};
    bool graze{};
    bool selfContact{};
    bool claimed{};  ///< the one cross-strategy mechanism there is
};

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
    TimeMs restedAtMs{kLongAgo};
    float energyRecent{};
    TimeMs energyStampMs{};
    float siteEnergy[kSiteCount]{};
    float groundZ{};
    bool haveGroundZ{};
    float headZ{};
    bool haveHeadZ{};
    float grazeShare{};

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
    std::uint16_t scrapeLimb{};
    TimeMs grazeSinceMs{kLongAgo};
    float scrapeSpeed{};
    /// Tangent speed seen this tick only, integrated into scrapeDistance and then
    /// cleared. Separate from scrapeSpeed, which is a running max over the whole
    /// graze and would badly over-count if integrated.
    float scrapeTickSpeed{};
    float scrapeDistance{};  ///< units travelled along the surface since the graze began
    bool settleEmitted{};

    // voices in flight
    Voice voices[kVoiceRing]{};
    std::size_t voiceCount{};

    EngineStats stats{};
};

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
    proposal.position = ctx.actor.collapsePoint;
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

        // The quiet nine of every ten contacts are burst filler, not composites.
        // The job is not to decide which of thirty contacts to play; it is to
        // spend what survives on a few tight bursts with real silence between.
        if (contact.intensity < 0.15f) {
            ProposeTap(ctx, contact, out);
            return false;
        }

        Proposal proposal = FromContact(contact);
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
        layer(SurfaceSlot(contact.surface), impact.surfaceOffsetMs, impact.surfaceGainAtMinDb,
              impact.surfaceGainAtMaxDb, CueReason::kSurfaceSkin);
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
    static void ProposeTap(const StrategyContext& ctx, const Contact& contact, ProposalList& out) {
        const ImpactCompositeConfig& impact = ctx.cfg.strategies.impact;
        Proposal proposal = FromContact(contact);
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
        // Head-down attitude at the moment of contact makes the gate more
        // willing: a faceplant rather than a knock.
        const float gate = head.gateFrac * ctx.cfg.intensity.speedRefHigh *
                           (ctx.actor.state.headDown ? 1.0f - head.headDownBonus : 1.0f);
        if (contact.impactSpeed < gate) {
            return false;
        }

        Proposal proposal = FromContact(contact);
        proposal.rideAlong = true;
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
        proposal.levelDb = contact.onsetGainDb + head.gainDb;
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

        // Every gate here is a fraction of the loud anchor, so the whole tier
        // structure moves with the range rather than having to be re-derived
        // whenever the anchor does.
        const float anchor = std::max(1.0f, ctx.cfg.intensity.speedRefHigh);
        const float goreGate = crunch.goreGateFrac * anchor;
        const float crunchGate = crunch.crunchGateFrac * anchor;
        const float certainSpeed = crunch.crunchCertainFrac * anchor;
        const float hysteresis = crunch.crunchHysteresisFrac * anchor;

        // Gore sits at the obliterate tier, above anything a fall can produce.
        if (crunch.goreEnabled && contact.impactSpeed >= goreGate &&
            contact.impactSpeed >= ctx.cfg.intensity.obliterateFrac * anchor) {
            out.push_back(Accessory(ctx, contact, SlotId::kGoreWet,
                                    contact.onsetGainDb + crunch.goreGainDb, CueReason::kGore));
            return false;
        }
        if (!crunch.crunchEnabled || ctx.actor.crunchCount >= crunch.maxCrunchesPerEvent) {
            return false;
        }
        if (contact.impactSpeed < crunchGate) {
            if (contact.impactSpeed < crunchGate - hysteresis) {
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
                               ? std::clamp((contact.impactSpeed - crunchGate) / span, 0.0f, 1.0f)
                               : 1.0f;
        const float floor = std::clamp(crunch.crunchGateProbability, 0.0f, 1.0f);
        const float probability = floor + (1.0f - floor) * ramp;
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
        Proposal proposal = FromContact(contact);
        proposal.rideAlong = true;
        proposal.boneIndex = BoneFor(ctx, contact.limbIndex);
        proposal.levelDb = gainDb;
        proposal.layerCount = 1;
        proposal.layers[0].slot = slot;
        proposal.layers[0].offsetMs = ctx.cfg.strategies.impact.bodyOffsetMs;
        proposal.layers[0].gainDb = gainDb;
        proposal.layers[0].reason = reason;
        return proposal;
    }
};

/// Sustained grazing contact drives a looping voice attached to the limb: a low
/// grinding rumble with grain riding on it, about 20 dB under the impacts, not a
/// hiss. It is also the one path here built entirely on an untested column -
/// tangent_speed has never seen a real scrape, because the take named `slide`
/// turned out to be an extreme push.
class ScrapeLoopStrategy final : public IStrategy {
public:
    [[nodiscard]] const char* Name() const override { return "ScrapeLoop"; }

    bool Propose(const StrategyContext& ctx, const Contact& contact, ProposalList&) override {
        if (!ctx.cfg.strategies.scrape.enabled || !contact.graze) {
            return false;
        }
        // Claimed: a graze feeds the scrape path and must not also land on the
        // impact path as a thud.
        if (Ancient(ctx.actor.grazeSinceMs)) {
            ctx.actor.grazeSinceMs = contact.timeMs;
        }
        ctx.actor.scrapeSpeed = std::max(ctx.actor.scrapeSpeed, contact.tangentSpeed);
        ctx.actor.scrapeTickSpeed = std::max(ctx.actor.scrapeTickSpeed, contact.tangentSpeed);
        ctx.actor.scrapeLimb = contact.limbIndex;
        return true;
    }

    void ProposeTick(const StrategyContext& ctx, ProposalList& out) override {
        const ScrapeLoopConfig& scrape = ctx.cfg.strategies.scrape;
        ActorRuntime& actor = ctx.actor;

        // Ground covered since the graze opened. Forward-integrated, one
        // accumulator, nothing read ahead - the same arithmetic works live.
        actor.scrapeDistance += actor.scrapeTickSpeed * ctx.frameMs * 0.001f;
        actor.scrapeTickSpeed = 0.0f;

        // Two ways in, because a slide has two shapes. A slow grind earns the
        // loop by lasting; a fast skid earns it by covering ground, and log_3
        // crosses two metres in less than the time gate wants - which is why the
        // one take that is mostly sliding had no slide in it.
        const bool longEnough = !Ancient(actor.grazeSinceMs) &&
                                (ctx.nowMs - actor.grazeSinceMs) >=
                                    Window(scrape.minDurationMs, ctx.frameMs,
                                           ctx.cfg.arb.frameScaleK);
        const bool farEnough = !Ancient(actor.grazeSinceMs) &&
                               actor.scrapeDistance >= scrape.minDistance;
        const bool sustained = (longEnough || farEnough) &&
                               (ctx.nowMs - actor.lastAdmittedMs) <
                                   Window(200.0f, ctx.frameMs, ctx.cfg.arb.frameScaleK);
        const bool wants = scrape.enabled && actor.state.tier == DistanceTier::kFull &&
                           actor.scrapeSpeed >= scrape.minTangentSpeed && sustained;

        if (wants) {
            const float span = std::max(1.0f, scrape.speedForMaxGain - scrape.speedForMinGain);
            const float track =
                std::clamp((actor.scrapeSpeed - scrape.speedForMinGain) / span, 0.0f, 1.0f);
            // Opening on distance means a glancing skid can open the loop, so the
            // level rides distance too: it comes in under the floor and only a
            // real slide walks it up. Without this, letting skids in would make
            // every tumble sound like a body being dragged.
            const float travelled =
                std::clamp(actor.scrapeDistance / std::max(1.0f, scrape.fadeInDistance), 0.0f,
                           1.0f);
            const float pitch =
                1.0f + scrape.pitchPerThousandUnits * actor.scrapeSpeed / 1000.0f;
            const std::size_t before = out.size();
            EmitLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice, actor.scrapeLastDb,
                             SlotId::kScrapeLoop,
                             scrape.gainDb + Lerp(-12.0f, 0.0f, track) +
                                 Lerp(scrape.fadeInFloorDb, 0.0f, travelled),
                             pitch, CueReason::kScrape, scrape.startFadeMs);
            if (out.size() > before) {
                out.back().limbIndex = actor.scrapeLimb;
            }
        } else if (actor.scrapeRunning) {
            StopLoopProposal(ctx, out, actor.scrapeRunning, actor.scrapeVoice,
                             SlotId::kScrapeLoop, CueReason::kScrape, scrape.stopFadeMs);
            actor.grazeSinceMs = kLongAgo;
            actor.scrapeSpeed = 0.0f;
            actor.scrapeDistance = 0.0f;
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

        const bool wantsBed = foley.enabled && actor.state.tier == DistanceTier::kFull &&
                              actor.state.phase != Phase::kRest;
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
        const bool wantsRise = foley.enabled && foley.airborneRise && actor.state.airborne &&
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
            actor.state.phase != Phase::kSettle) {
            return;
        }
        if ((ctx.nowMs - actor.state.phaseEnteredMs) < ctx.cfg.strategies.settle.delayMs) {
            return;
        }
        actor.settleEmitted = true;

        Proposal proposal{};
        proposal.timeMs = ctx.nowMs;
        proposal.actorId = actor.state.actorId;
        proposal.site = actor.state.leadingLimb;
        proposal.surface = actor.state.surfaceUnder;
        proposal.position = actor.collapsePoint;
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
        slot->state.phase = Phase::kRest;
        slot->name = profile != nullptr && !profile->name.empty() ? profile->name
                                                                  : std::format("{:08X}", id);
        slot->isPlayer = profile != nullptr && profile->isPlayer;
        return *slot;
    }

    void Release(ActorRuntime& actor, std::string_view why) {
        if (!actor.inUse) {
            return;
        }
        if (actor.stats.contactsIn > 0) {
            log::Summary(actor.name, actor.stats,
                         actor.lastAdmittedMs - actor.firstContactMs);
        }
        spdlog::debug("actor {} dropped: {}", actor.name, why);
        actor.inUse = false;
        actor.state = CrashState{};
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

    [[nodiscard]] const PhaseBudget& Budget(Phase phase) const {
        switch (phase) {
            case Phase::kLaunch: return cfg.phase.launch;
            case Phase::kAirborne: return cfg.phase.airborne;
            case Phase::kPrimaryImpact: return cfg.phase.primaryImpact;
            case Phase::kTumble: return cfg.phase.tumble;
            case Phase::kSlide: return cfg.phase.slide;
            case Phase::kSettle:
            case Phase::kRest:
            case Phase::kCount:
                break;
        }
        return cfg.phase.settle;
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
                return cfg.mix.surfaceTrimDb;
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
            case SlotId::kSurfWood:     return g.surfWood;
            case SlotId::kSurfStone:    return g.surfStone;
            case SlotId::kSurfSoft:     return g.surfSoft;
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

    void TraceLine(TimeMs timeMs, ActorId actorId, std::uint16_t limbIndex, std::uint32_t seq,
                   float impactSpeed, float intensity, Phase phase, std::string_view outcome) {
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
        record.phase = phase;
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
        if (actor.voiceCount >= static_cast<std::size_t>(std::max(1, cfg.arb.voiceCapPerActor)) ||
            actor.voiceCount >= kVoiceRing) {
            return false;
        }
        if (globalVoices.size() >= static_cast<std::size_t>(std::max(1, cfg.arb.voiceCapGlobal))) {
            return false;
        }
        actor.voices[actor.voiceCount++] = Voice{endsMs, voiceId};
        globalVoices.push_back(Voice{endsMs, voiceId});
        return true;
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
            // The collision layer is the reliable input and the material is an
            // enrichment that fails at the edges (07 §8), so the material is
            // preferred when there is one and the layer stands in when there is
            // not.
            contact.surface = event.otherMaterial != 0 ? SurfaceFromMaterial(event.otherMaterial)
                                                       : SurfaceFromLayer(event.otherLayer);
            contact.otherBody = event.otherBody;
            contact.selfContact = event.otherLimb >= 0;
            // A graze is sideways motion *instead of* a hit, not sideways motion
            // as well as one. Above the ceiling the ratio stops meaning anything:
            // log_4's head arrives at 241 u/s with 445 u/s of tangent, which the
            // ratio alone calls a scrape and which is in fact a skull hitting a
            // floor at speed while sliding. Ratio decides the quiet end; the
            // ceiling overrules it at the loud end.
            contact.graze = event.impactSpeed > 1.0f &&
                            event.impactSpeed < cfg.ingest.grazeMaxImpactSpeed &&
                            event.tangentSpeed / event.impactSpeed > cfg.ingest.grazeRatio;
            contact.intensity =
                Intensity(event.impactSpeed, contact.site,
                          limb != nullptr && limb->radius > 0.0f ? limb->radius : event.limbRadius,
                          event.bodySpeed, event.angularSpeed);
            contact.onsetGainDb = GainFromIntensity(contact.intensity);

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
        std::ranges::stable_sort(contacts, [](const Contact& a, const Contact& b) {
            if (a.impactSpeed != b.impactSpeed) {
                return a.impactSpeed > b.impactSpeed;
            }
            return a.sourceSeq < b.sourceSeq;
        });
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

    void HandleState(const FeedEvent& event, IFeed& feed) {
        const std::string_view state{event.text};
        if (state == "ragdoll_start") {
            ActorRuntime& actor = Acquire(event.actorId, feed.Profile(event.actorId));
            if (actor.state.phase == Phase::kRest) {
                actor.state.ragdollStartMs = event.timeMs;
                Enter(actor, Phase::kLaunch, event.timeMs);
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

    // ── Stage 1 and 2: crash state and the phase machine ─────────────────────

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
            // Body speed is a *measurement*, and the only one we have is the last
            // contact's - the capture carries no periodic limb sample, and in the
            // air a body speeds up rather than slowing down. So it is held rather
            // than decayed, and released slowly as a backstop against a fall
            // whose last contact was a hard one. Without the hold, a 533 ms
            // airborne gap mid-knockdown reads as the fall having ended and the
            // closing cue lands while the body is still in the air.
            state.bodySpeed *= std::exp(-elapsed / (5.0f * cfg.phase.settleQuietMs));
            actor.energyStampMs = nowMs;
        }
        const auto maskElapsed = static_cast<float>(nowMs - actor.maskStampMs);
        if (maskElapsed > 0.0f) {
            state.maskCeilingDb = std::max(
                kSilentDb, state.maskCeilingDb - cfg.arb.maskDecayDbPerSec * maskElapsed * 0.001f);
            actor.maskStampMs = nowMs;
        }

        float frameBodySpeed = 0.0f;
        for (const Contact& contact : contacts) {
            if (contact.actorId != state.actorId) {
                continue;
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
            state.energyAccum += contact.intensity;
            actor.energyRecent = std::max(actor.energyRecent, contact.impactSpeed);
            actor.siteEnergy[static_cast<std::size_t>(contact.site)] += contact.intensity;
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
            actor.grazeShare = Lerp(actor.grazeShare, contact.graze ? 1.0f : 0.0f, 0.3f);
            if (listener.timeMs > 0.0) {
                state.distanceToListener = Distance(contact.position, listener.position);
            }
        }

        if (frameBodySpeed > 0.0f) {
            state.bodySpeed = frameBodySpeed;
        }

        float best = 0.0f;
        for (std::size_t i = 0; i < kSiteCount; ++i) {
            if (actor.siteEnergy[i] > best) {
                best = actor.siteEnergy[i];
                state.leadingLimb = static_cast<LimbSite>(i);
            }
        }
        state.headDown = actor.haveHeadZ && actor.haveGroundZ && actor.headZ < actor.groundZ + 30.0f;
        state.slidingRatio = actor.grazeShare;
        state.height = actor.haveGroundZ ? 0.0f : cfg.phase.airborneMinHeight;

        // Distance, per actor per tick rather than per contact.
        state.tier = listener.timeMs <= 0.0                                        ? DistanceTier::kFull
                     : state.distanceToListener > cfg.distance.simplifiedRadius     ? DistanceTier::kCulled
                     : state.distanceToListener > cfg.distance.fullRadius           ? DistanceTier::kSimplified
                                                                                    : DistanceTier::kFull;

        state.airborne = !Ancient(actor.lastAdmittedMs) &&
                         (nowMs - actor.lastAdmittedMs) >
                             Window(cfg.phase.airborneMinTimeMs, frameMs, cfg.arb.frameScaleK) &&
                         state.phase != Phase::kRest && state.phase != Phase::kSettle;

        AdvancePhase(actor, nowMs, frameMs);
    }

    void AdvancePhase(ActorRuntime& actor, TimeMs nowMs, float frameMs) {
        CrashState& state = actor.state;
        const Phase previous = state.phase;
        const auto sincePhase = nowMs - state.phaseEnteredMs;
        const auto quietFor = nowMs - std::max(actor.lastAdmittedMs, actor.firstContactMs);

        // Fire on first contact. 96.2 % of episodes peak on their first row, so
        // the system can trigger immediately with the first contact's magnitude
        // and be right nineteen times in twenty - no look-ahead buffer, no
        // waiting a frame to see if it gets louder.
        float frameEnergy = 0.0f;
        for (const Contact& contact : contacts) {
            if (contact.actorId == state.actorId) {
                frameEnergy = std::max(frameEnergy, contact.intensity);
            }
        }
        const bool heroContact =
            frameEnergy > 0.0f &&
            frameEnergy >= cfg.phase.primaryImpactEnergyFrac * std::max(0.05f, state.energyAccum);

        switch (state.phase) {
            case Phase::kRest: {
                // A knockdown reopens only on a contact as strong as a real
                // moment of the one that just ended, and only after the get-up
                // blend. Anything weaker is the body still settling, and a death
                // ragdoll simply never leaves Rest - which is what removes the
                // death-versus-knockdown subsystem entirely.
                const bool blended =
                    Ancient(actor.restedAtMs) ||
                    (nowMs - actor.restedAtMs) >
                        Window(cfg.phase.getUpBlendMs, frameMs, cfg.arb.frameScaleK);
                const float reopenAt =
                    std::max(cfg.phase.settleEnergyFloor * 2.0f, state.peakSpeed * 0.4f);
                if (blended) {
                    for (const Contact& contact : contacts) {
                        if (contact.actorId == state.actorId && contact.impactSpeed >= reopenAt) {
                            Enter(actor, Phase::kLaunch, nowMs);
                            break;
                        }
                    }
                }
                break;
            }
            case Phase::kLaunch:
                if (heroContact) {
                    Enter(actor, Phase::kPrimaryImpact, nowMs);
                } else if (state.airborne) {
                    Enter(actor, Phase::kAirborne, nowMs);
                }
                break;
            case Phase::kAirborne:
                if (frameEnergy > 0.0f) {
                    Enter(actor, Phase::kPrimaryImpact, nowMs);
                }
                break;
            case Phase::kPrimaryImpact:
                // A small group of peers inside a couple of hundred ms, not one
                // hit - a faceplant genuinely has a knee, a chest and a head.
                if (sincePhase >
                    Window(cfg.phase.primaryImpactWindowMs, frameMs, cfg.arb.frameScaleK)) {
                    Enter(actor, Phase::kTumble, nowMs);
                }
                break;
            case Phase::kTumble:
            case Phase::kSlide: {
                const bool sliding =
                    !Ancient(actor.grazeSinceMs) &&
                    actor.scrapeSpeed >= cfg.phase.slideMinTangentSpeed &&
                    (nowMs - actor.grazeSinceMs) >=
                        Window(cfg.phase.slideMinDurationMs, frameMs, cfg.arb.frameScaleK);
                if (state.phase == Phase::kTumble && sliding) {
                    Enter(actor, Phase::kSlide, nowMs);
                } else if (state.phase == Phase::kSlide && !sliding) {
                    Enter(actor, Phase::kTumble, nowMs);
                }
                // Three conditions, not two: nothing has hit hard recently, the
                // body itself has stopped moving, and it has been quiet long
                // enough. Dropping the middle one closes the event during the
                // airborne gap between two bounces, and the closing cue lands
                // while the body is still in the air.
                if (actor.energyRecent < cfg.phase.settleEnergyFloor &&
                    state.bodySpeed < cfg.phase.settleEnergyFloor &&
                    quietFor > Window(cfg.phase.settleQuietMs, frameMs, cfg.arb.frameScaleK)) {
                    Enter(actor, Phase::kSettle, nowMs);
                }
                break;
            }
            case Phase::kSettle:
                if (actor.settleEmitted &&
                    sincePhase > Window(cfg.strategies.settle.delayMs + 100.0f, frameMs,
                                        cfg.arb.frameScaleK)) {
                    Enter(actor, Phase::kRest, nowMs);
                    actor.restedAtMs = nowMs;
                }
                break;
            case Phase::kCount:
                break;
        }

        if (state.phase != previous) {
            spdlog::debug("actor {} phase {} -> {} at {:.0f} ms", actor.name, ToString(previous),
                          ToString(state.phase), nowMs);
        }
    }

    void Enter(ActorRuntime& actor, Phase phase, TimeMs nowMs) {
        actor.state.phase = phase;
        actor.state.phaseEnteredMs = nowMs;
        if (phase == Phase::kPrimaryImpact) {
            // Spatial collapse: during a hero moment place every layer at one
            // point. Several points read as several events; one point reads as
            // one event with detail, which is what the references' 0.95-0.97
            // stereo correlation says they do.
            actor.collapseUntilMs = nowMs + cfg.arb.spatialCollapseWindowMs;
            for (const Contact& contact : contacts) {
                if (contact.actorId == actor.state.actorId) {
                    actor.collapsePoint = contact.position;
                    break;
                }
            }
        }
        if (phase == Phase::kLaunch) {
            actor.energyRecent = 0.0f;
            actor.crunchCount = 0;
            actor.crunchArmed = true;
            actor.settleEmitted = false;
            actor.state.energyAccum = 0.0f;
            actor.burstStartMs = kLongAgo;
            actor.burstLastMs = kLongAgo;
            actor.lastOnsetMs = kLongAgo;
            actor.lastOnsetDb = kSilentDb;
        }
    }

    // ── Stage 3 driver ───────────────────────────────────────────────────────

    void RunStrategies(TimeMs nowMs, float frameMs) {
        proposals.clear();

        for (Contact& contact : contacts) {
            ActorRuntime* actor = Find(contact.actorId);
            if (actor == nullptr || actor->state.tier == DistanceTier::kCulled) {
                continue;
            }
            // Simplified tier: hero composites only. No grains, no loops, no bed -
            // nobody resolves the detail at that range anyway.
            if (actor->state.tier == DistanceTier::kSimplified && contact.intensity < 0.4f) {
                continue;
            }

            StrategyContext ctx{cfg, *actor, rng, nowMs, frameMs, &nextVoiceId};
            for (IStrategy* strategy : strategies) {
                if (strategy->Propose(ctx, contact, proposals)) {
                    contact.claimed = true;
                    TraceLine(contact.timeMs, contact.actorId, contact.limbIndex,
                              contact.sourceSeq, contact.impactSpeed, contact.intensity,
                              actor->state.phase, strategy->Name());
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
                    std::snprintf(why, sizeof(why), "rate cap %.0f<%.0f", sinceOnset, rateCap);
                    Dropped(proposal, actor->state.phase, why);
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
                Dropped(proposal, actor->state.phase, "chain merge");
                continue;
            }

            // 3. Temporal masking. Anything more than ~12 dB under the decaying
            //    ceiling is dropped entirely, not played quietly - which is what
            //    turns a dozen simultaneous contacts into one event with texture.
            if (proposal.levelDb < actor->state.maskCeilingDb - cfg.arb.maskDropBelowDb) {
                ++stats.droppedMasking;
                ++actor->stats.droppedMasking;
                Dropped(proposal, actor->state.phase, "masked");
                continue;
            }

            // 4. Burst shaping. The arbitrator picks bursts, not individual
            //    sounds: three to five grains inside 200-400 ms, then real
            //    silence. This is the rule that turns a three-second tumble into
            //    four audible events.
            const int grainCap = std::max(
                1, std::min(cfg.arb.burstMaxGrains, Budget(actor->state.phase).maxCuesPerBurst));
            const bool burstOpen = proposal.timeMs - actor->burstStartMs < burstWindow;
            const bool opensBurst = !burstOpen;
            if (burstOpen) {
                if (actor->burstGrains >= grainCap) {
                    ++stats.droppedBurstCap;
                    ++actor->stats.droppedBurstCap;
                    Dropped(proposal, actor->state.phase, "burst full");
                    continue;
                }
            } else if (!Ancient(actor->burstStartMs) &&
                       proposal.timeMs - actor->burstLastMs < burstGap) {
                ++stats.droppedBurstCap;
                ++actor->stats.droppedBurstCap;
                Dropped(proposal, actor->state.phase, "burst gap");
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
            } before{actor->burstStartMs,
                     actor->burstLastMs,
                     actor->lastOnsetMs,
                     actor->lastOnsetDb,
                     actor->burstGrains,
                     chain < kChainCount ? actor->chainLastMs[chain] : 0.0,
                     chain < kChainCount ? actor->chainLastDb[chain] : 0.0f,
                     actor->state.maskCeilingDb,
                     actor->state.admittedCount};

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
                Dropped(proposal, actor->state.phase, "silent");
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

    void Dropped(const Proposal& proposal, Phase phase, std::string_view why) {
        // The speed matters most on a drop line: "rate cap" against a 40 u/s
        // brush is the rule working, and against a 440 u/s slam it is a bug.
        // Reporting 0 here made those two look identical in the export.
        TraceLine(proposal.timeMs, proposal.actorId, proposal.limbIndex, proposal.sourceSeq,
                  proposal.impactSpeed, proposal.intensity, phase, why);
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
    static bool LayerAudible(const LayerMuteConfig& m, SlotId slot) {
        switch (slot) {
            case SlotId::kImpTransient: return m.impTransient;
            case SlotId::kImpBody:      return m.impBody;
            case SlotId::kImpSub:       return m.impSub;
            case SlotId::kSurfWood:     return m.surfWood;
            case SlotId::kSurfStone:    return m.surfStone;
            case SlotId::kSurfSoft:     return m.surfSoft;
            case SlotId::kLimbTap:      return m.limbTap;
            case SlotId::kCrunchGran:   return m.crunchGran;
            case SlotId::kGoreWet:      return m.goreWet;
            case SlotId::kScrapeLoop:   return m.scrapeLoop;
            case SlotId::kFoleyCloth:   return m.foleyCloth;
            case SlotId::kAirWhoosh:    return m.airWhoosh;
            case SlotId::kHeadImpact:   return m.headImpact;
            case SlotId::kSettleRest:   return m.settleRest;
            default:                    return true;
        }
    }

    std::uint32_t Emit(ActorRuntime& actor, const Proposal& proposal, TimeMs nowMs) {
        if (sink == nullptr) {
            return 0;
        }
        std::uint32_t emitted = 0;
        ExpireVoices(actor, nowMs);

        const PhaseBudget& budget = Budget(actor.state.phase);
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

        // The voice cap is spent in this order, so the order decides which layer
        // is lost when the budget runs out. Onset first, then loudest: the onset
        // is the contact itself and an impact that arrives without its attack is
        // a different sound rather than a thinner one, while by pure loudness the
        // quietest layer goes first - and on a hero hit the quietest layer is the
        // transient. After the onset it is straight loudness, which keeps the sub
        // safe; the sub is the loudest layer and the whole of the gnarl, and it
        // is the one that must never be the one that goes.
        int byPriority[kMaxLayers]{};
        for (int i = 0; i < proposal.layerCount; ++i) {
            byPriority[i] = i;
        }
        std::stable_sort(byPriority + std::min(1, proposal.layerCount),
                         byPriority + proposal.layerCount, [&](int a, int b) {
                             return proposal.layers[a].gainDb > proposal.layers[b].gainDb;
                         });

        for (int slotIndex = 0; slotIndex < proposal.layerCount; ++slotIndex) {
            const Layer& layer = proposal.layers[byPriority[slotIndex]];

            ResolvedSound resolved{};
            if (proposal.op != CueOp::kStopLoop) {
                if (bank == nullptr ||
                    !bank->Resolve(layer.slot, proposal.surface, proposal.coverage, proposal.site,
                                   resolved)) {
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
            cue.gainDb = layer.gainDb + budget.gainTrimDb + masterDb + postShapeDb +
                         LayerTrimDb(layer.slot, actor.isPlayer);
            cue.pitch = layer.pitch;
            cue.fadeMs = proposal.fadeMs;
            cue.position = collapse ? actor.collapsePoint : proposal.position;
            cue.boneIndex = proposal.boneIndex;
            cue.voiceId = proposal.voiceId;
            cue.actorId = proposal.actorId;
            cue.limbIndex = proposal.limbIndex;
            cue.site = proposal.site;
            cue.surface = proposal.surface;
            cue.reason = layer.reason;
            cue.phase = actor.state.phase;
            cue.intensity = proposal.intensity;
            cue.sourceSeq = proposal.sourceSeq;

            if (proposal.op == CueOp::kStopLoop) {
                ReleaseVoice(actor, proposal.voiceId);
            } else {
                if (cue.gainDb < cfg.mix.voiceFloorDb) {
                    // Not worth a voice, and dropped before the cap ever sees it.
                    continue;
                }
                if (proposal.op != CueOp::kUpdateLoop) {
                    const TimeMs endsMs = proposal.op == CueOp::kStartLoop
                                              ? kNever
                                              : cue.timeMs + Slot(layer.slot).maxLengthMs;
                    if (!TakeVoice(actor, endsMs, proposal.voiceId)) {
                        ++stats.droppedVoiceCap;
                        ++actor.stats.droppedVoiceCap;
                        spdlog::debug("voice cap hit on {} for {}", actor.name,
                                      ToString(layer.slot));
                        continue;
                    }
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
            const bool audible = LayerAudible(cfg.layers, layer.slot);
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
            if (stats.firstCueMs == 0.0 || cue.timeMs < stats.firstCueMs) {
                stats.firstCueMs = cue.timeMs;
            }
            stats.lastCueMs = std::max(stats.lastCueMs, cue.timeMs);
            spdlog::debug("cue {} {} at {:.0f} ms, {:.1f} dB, pitch {:.2f}", ToString(layer.slot),
                          ToString(layer.reason), cue.timeMs, cue.gainDb, cue.pitch);
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
    impl.rng.Seed(impl.cfg.slots.rngSeed == 0 ? 1u : impl.cfg.slots.rngSeed);
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

std::size_t Engine::TrackedActors() const {
    std::size_t count = 0;
    for (const auto& actor : m_impl->actors) {
        count += actor.inUse ? 1u : 0u;
    }
    return count;
}

}  // namespace rds
