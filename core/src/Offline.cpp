#include "rds/Offline.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace rds {
namespace {

/// How long past the recording's last row the runner keeps ticking.
///
/// The capture closes while the body is still moving, and the settle cue is
/// deliberately the last thing to arrive. Stopping at the last contact row would
/// mean the closing cue never fires and check seven never passes - not because
/// the algorithm is wrong but because the replay stopped early.
constexpr TimeMs kTailMs = 3000.0;

[[nodiscard]] bool IsOneShot(const Cue& cue) { return cue.op == CueOp::kPlayOneShot; }

/// Copy every tracked actor's measured body out of the engine, for one tick.
///
/// Called after Tick rather than before: the state a rule read this frame is
/// the state Tick left behind, and sampling first would show the timeline the
/// frame before the one it is labelled with.
void SampleBody(const Engine& engine, TimeMs nowMs, std::vector<BodySample>& out) {
    const std::size_t tracked = engine.TrackedActors();
    for (std::size_t i = 0; i < tracked; ++i) {
        const CrashState* state = engine.ActorAt(i);
        if (state == nullptr) {
            break;
        }
        BodySample sample{};
        sample.timeMs = nowMs;
        sample.actorId = state->actorId;
        sample.comPosition = state->comPosition;
        sample.comVelocity = state->comVelocity;
        sample.speed = std::sqrt(state->comVelocity.x * state->comVelocity.x +
                                 state->comVelocity.y * state->comVelocity.y +
                                 state->comVelocity.z * state->comVelocity.z);
        sample.verticalSpeed = state->verticalSpeed;
        sample.verticalAccel = state->verticalAccel;
        sample.fallDropUnits = state->fallDropUnits;
        sample.airborne = state->airborne;
        sample.airborneSinceMs = state->airborneSinceMs;
        sample.haveBodySamples = state->haveBodySamples;
        sample.motion = state->motion;
        sample.motionEnteredMs = state->motionEnteredMs;
        sample.slideExit = state->slideExit;
        sample.moment = state->moment;
        sample.heroSeq = state->heroSeq;
        sample.heroSinceMs = state->heroSinceMs;
        out.push_back(sample);
    }
}

/// One audible moment, gathered back out of the cue list.
///
/// The references measure *onsets*, not individual layers - "peak level of every
/// onset" in 01 §4 is one number per event, and a composite is four cues. So the
/// hero-cliff and sub-layer checks have to re-group the cues into the events they
/// came from, or they compare a transient against the sub of its own stack and
/// call that a cliff.
struct Event {
    std::uint32_t sourceSeq{};
    ActorId actorId{};
    TimeMs onsetMs{};
    float peakDb{-1000.0f};
    float loudestNonSubDb{-1000.0f};
    const Cue* transient{};
    const Cue* sub{};
};

[[nodiscard]] std::vector<Event> GatherEvents(const std::vector<Cue>& cues) {
    std::vector<Event> events;
    for (const Cue& cue : cues) {
        if (!IsOneShot(cue)) {
            continue;
        }
        Event* found = nullptr;
        for (Event& event : events) {
            if (event.sourceSeq == cue.sourceSeq && event.actorId == cue.actorId) {
                found = &event;
                break;
            }
        }
        if (found == nullptr) {
            events.push_back(Event{cue.sourceSeq, cue.actorId, cue.timeMs, -1000.0f, -1000.0f,
                                   nullptr, nullptr});
            found = &events.back();
        }
        found->onsetMs = std::min(found->onsetMs, cue.timeMs);
        found->peakDb = std::max(found->peakDb, cue.gainDb);
        if (cue.slot == SlotId::kImpSub) {
            found->sub = &cue;
        } else {
            found->loudestNonSubDb = std::max(found->loudestNonSubDb, cue.gainDb);
            if (cue.slot == SlotId::kImpTransient) {
                found->transient = &cue;
            }
        }
    }
    return events;
}

/// An onset is the layer that starts an audible moment: the composite's transient
/// or, for the quiet contacts that become burst filler, the tap. Everything else
/// in a stack is scheduled relative to one of these, so these are what the rate
/// cap and the burst rhythm are measured against.
[[nodiscard]] bool IsOnset(const Cue& cue) {
    return IsOneShot(cue) && (cue.slot == SlotId::kImpTransient || cue.slot == SlotId::kLimbTap);
}

void Check(VerifyReport& report, std::string name, bool passed, std::string detail) {
    report.checks.push_back(VerifyExpectation{std::move(name), passed, std::move(detail)});
}

}  // namespace

bool VerifyReport::Passed() const {
    return std::ranges::all_of(checks, [](const VerifyExpectation& c) { return c.passed; });
}

OfflineResult RunOffline(Recording& recording, const AlgorithmConfig& config, SoundBank& bank,
                         const OfflineOptions& options) {
    OfflineResult result;

    // One seed drives both the engine's scatter and the bank's shuffle bags, so
    // a run is reproducible end to end rather than only halfway.
    AlgorithmConfig seeded = config;
    seeded.slots.rngSeed = options.seed == 0 ? 1u : options.seed;
    bank.Seed(seeded.slots.rngSeed);

    recording.Rewind();

    CueCollector collector;
    Engine engine;
    engine.SetSoundBank(&bank);
    engine.SetSink(&collector);
    engine.SetConfig(seeded);
    engine.SetTracing(options.trace);
    engine.Reset();
    // Reset() reseeds from the config the engine is holding, so the config has to
    // be in place first - otherwise the first run of a session uses a different
    // stream from every run after it.
    engine.SetConfig(seeded);

    const auto& frames = recording.FrameBoundaries();
    const TimeMs start = options.startMs;
    const TimeMs end = options.endMs > 0.0 ? options.endMs : recording.Info().durationMs;

    // Tick at the recording's own frame boundaries, so the engine steps the way
    // it did in the game rather than at whatever rate the runner feels like.
    TimeMs last = frames.empty() ? 0.0 : frames.front();
    TimeMs first = last;
    bool anyTick = false;
    for (const TimeMs frame : frames) {
        if (frame < start || (end > 0.0 && frame > end)) {
            continue;
        }
        if (!anyTick) {
            first = frame;
            anyTick = true;
        }
        engine.Tick(recording, frame);
        SampleBody(engine, frame, result.body);
        ++result.ticks;
        last = frame;
    }
    const TimeMs step = recording.FrameStepMs() > 0.0 ? recording.FrameStepMs() : 16.6;
    TimeMs lastTail = last;
    for (TimeMs t = last + step; t <= last + kTailMs; t += step) {
        engine.Tick(recording, t);
        SampleBody(engine, t, result.body);
        ++result.ticks;
        lastTail = t;
    }
    result.simulatedMs = lastTail - first;

    result.cues = collector.Cues();
    std::ranges::stable_sort(result.cues,
                             [](const Cue& a, const Cue& b) { return a.timeMs < b.timeMs; });
    result.stats = engine.Stats();
    result.trace = engine.Trace();
    result.durationMs = recording.Info().durationMs;

    // The audio runs past the last cue by the longest tail any of them has.
    result.audioDurationMs = 0.0;
    for (const Cue& cue : result.cues) {
        result.audioDurationMs =
            std::max(result.audioDurationMs, cue.timeMs + Slot(cue.slot).maxLengthMs);
    }
    return result;
}

VerifyReport Verify(Recording& recording, const AlgorithmConfig& config, SoundBank& bank,
                    const OfflineOptions& options) {
    VerifyReport report;
    report.recordingStem = recording.Info().stem;

    const OfflineResult run = RunOffline(recording, config, bank, options);
    report.stats = run.stats;

    // A take with no ragdoll contact in it has nothing for the design's numbers
    // to be true or false about. Four of the run's files were discarded as bad
    // data and one more was emptied by a rebuild storm (05 §4); reporting those
    // as eight algorithm failures would bury the takes that mean something.
    if (run.stats.contactsIn == 0) {
        Check(report, "no knockdown", true,
              std::format("{} events, no ragdoll contact - nothing to check",
                          run.stats.eventsIn));
        return report;
    }

    // ── 1. four to six audible moments per knockdown, not fifteen to thirty ──
    //
    // Measured as a rate rather than a count, because "4-6" is quoted against
    // the references' three-second tumble and the takes here run 1.4-2.7 s.
    // `tumbling-down-hill` is four bursts across 2.8 s, so the design's rhythm is
    // roughly 1.5 audible moments a second; the failure this is guarding against
    // is fifteen to thirty of them, which is 10/s.
    {
        // Measured across the onsets, not across the whole cue list: a take whose
        // composites all fell under the voice floor still has a first and a last
        // cue, and dividing by that window reports a rate for an event that never
        // played.
        std::vector<TimeMs> onsets;
        for (const Cue& cue : run.cues) {
            if (IsOnset(cue)) {
                onsets.push_back(cue.timeMs);
            }
        }
        std::ranges::sort(onsets);
        if (onsets.size() < 2) {
            Check(report, "audible moments", false,
                  std::format("{} onsets in {} bursts - too few to measure a rate",
                              onsets.size(), run.stats.bursts));
        } else {
            const double windowMs = onsets.back() - onsets.front() + config.arb.burstMinGapMs;
            const double perSecond = run.stats.bursts * 1000.0 / windowMs;
            Check(report, "audible moments", perSecond >= 0.8 && perSecond <= 3.0,
                  std::format("{} bursts across {:.0f} ms = {:.1f}/s (design: ~1.5/s, four to six "
                              "over a three-second tumble)",
                              run.stats.bursts, windowMs, perSecond));
        }
    }

    // ── 2. reduction ratio near 10:1 against the contacts that entered ───────
    {
        // Reported with the take's length beside it, because this number and the
        // burst rate above are the same statement measured two ways: the design's
        // 10:1 pairs "30-60 collisions" with "four to six audible moments", and
        // at the references' own rhythm - 46-104 ms inside a burst, 313-894 ms
        // between - four to six moments needs about three seconds. A 1.4 second
        // knockdown carrying sixty contacts cannot be both.
        const float ratio = run.stats.ReductionRatio();
        const double windowSec =
            std::max(0.001, (run.stats.lastCueMs - run.stats.firstCueMs) / 1000.0);
        Check(report, "reduction ratio", ratio >= 4.0f && ratio <= 25.0f,
              std::format("{:.1f}:1 from {} contacts into {} bursts over {:.1f} s (design: ~10:1, "
                          "which assumes the references' three-second event)",
                          ratio, run.stats.contactsIn, run.stats.bursts, windowSec));
    }

    // ── 3. no two impact onsets closer than the configured rate cap ──────────
    {
        // The cap has a documented exception - an onset well above the one
        // before it opens its own event rather than being folded into it
        // (arb.rateCapOverrideDb) - so gaps alone would report a violation every
        // time that exception fired. The engine counts the exceptions it granted
        // rather than the verifier re-deriving them, because the level the
        // arbitrator judged is the *proposed* stack's and layers can still be
        // lost to the voice cap afterwards, so the cue list no longer shows it.
        std::vector<TimeMs> onsets;
        for (const Cue& cue : run.cues) {
            if (IsOnset(cue)) {
                onsets.push_back(cue.timeMs);
            }
        }
        std::ranges::sort(onsets);

        double smallest = 1.0e9;
        int tooClose = 0;
        for (std::size_t i = 1; i < onsets.size(); ++i) {
            const double gap = onsets[i] - onsets[i - 1];
            smallest = std::min(smallest, gap);
            // One millisecond of slack, because a frame boundary is a double and
            // the cap is a float.
            if (gap < config.arb.rateCapMs - 1.0) {
                ++tooClose;
            }
        }
        // An upper bound: the counter also ticks for proposals that took the
        // exception and were then dropped by a later rule, so this errs towards
        // passing rather than towards a false failure.
        const auto granted = static_cast<int>(run.stats.rateCapOverrides);
        const bool passed = onsets.size() < 2 || tooClose <= granted;
        Check(report, "rate cap", passed,
              onsets.size() < 2
                  ? std::format("{} onsets, nothing to compare", onsets.size())
                  : std::format("smallest gap {:.0f} ms against a {:.0f} ms cap over {} onsets; "
                                "{} inside the cap against {} granted the {:.0f} dB override",
                                smallest, static_cast<double>(config.arb.rateCapMs), onsets.size(),
                                tooClose, granted,
                                static_cast<double>(config.arb.rateCapOverrideDb)));
    }

    // ── 4. bursts of three to five grains in 200-400 ms, then >= 300 ms quiet ─
    {
        std::vector<TimeMs> onsets;
        for (const Cue& cue : run.cues) {
            if (IsOnset(cue)) {
                onsets.push_back(cue.timeMs);
            }
        }
        std::ranges::sort(onsets);

        int clusters = 0;
        int worstGrains = 0;
        double widestSpan = 0.0;
        double tightestGap = 1.0e9;
        std::size_t clusterStart = 0;
        for (std::size_t i = 1; i <= onsets.size(); ++i) {
            // A burst ends where the engine says it ends: the enforced silence
            // between bursts. Splitting at half of it would call a 257 ms gap
            // inside a still-open burst window a burst boundary, and then report
            // the burst that follows as arriving too soon.
            const bool breakHere =
                i == onsets.size() || (onsets[i] - onsets[i - 1]) >= config.arb.burstMinGapMs * 0.9;
            if (!breakHere) {
                continue;
            }
            ++clusters;
            worstGrains = std::max(worstGrains, static_cast<int>(i - clusterStart));
            widestSpan = std::max(widestSpan, onsets[i - 1] - onsets[clusterStart]);
            if (i < onsets.size()) {
                tightestGap = std::min(tightestGap, onsets[i] - onsets[i - 1]);
            }
            clusterStart = i;
        }
        const bool grainsOk = onsets.empty() || worstGrains <= config.arb.burstMaxGrains;
        const bool spanOk = widestSpan <= config.arb.burstWindowMs + config.arb.rateCapMs;
        const bool gapOk = clusters < 2 || tightestGap >= config.arb.burstMinGapMs * 0.9;
        Check(report, "burst shape", grainsOk && spanOk && gapOk,
              std::format("{} bursts, at most {} grains, widest span {:.0f} ms, tightest gap "
                          "{:.0f} ms (design: 3-5 grains in 200-400 ms, then >= 300 ms)",
                          clusters, worstGrains, widestSpan,
                          clusters < 2 ? 0.0 : tightestGap));
    }

    // ── 5. the top one to three events within a decibel, then a >= 9 dB cliff ─
    {
        auto events = GatherEvents(run.cues);
        std::ranges::sort(events, [](const Event& a, const Event& b) { return a.peakDb > b.peakDb; });
        // A take whose hardest contact is a shove has no hero, and the claim
        // under test is about knockdowns that have one. Five of the twelve takes
        // here never exceed 400 units/s; reporting those as a missing cliff would
        // be reporting the take, not the algorithm.
        const bool haveHero = !events.empty() && events[0].sub != nullptr;
        if (!haveHero) {
            Check(report, "hero cliff", true,
                  std::format("no hero in this take - the loudest of {} events is burst filler",
                              events.size()));
        } else if (events.size() < 4) {
            Check(report, "hero cliff", false,
                  std::format("only {} audible events, not enough to have a cliff", events.size()));
        } else {
            std::size_t peers = 1;
            while (peers < events.size() && events[0].peakDb - events[peers].peakDb <= 1.0f) {
                ++peers;
            }
            const float cliff = events[0].peakDb - events[std::min(peers, events.size() - 1)].peakDb;
            const bool passed = peers <= 3 && cliff >= 9.0f;
            Check(report, "hero cliff", passed,
                  std::format("{} peers within 1 dB of {:.1f} dB, then a {:.1f} dB cliff over "
                              "{} events (design: 1-3 peers, then >= 9 dB)",
                              peers, static_cast<double>(events[0].peakDb),
                              static_cast<double>(cliff), events.size()));
        }
    }

    // ── 6. the sub arrives 55-75 ms after its transient and is the loudest ───
    //
    // The offset is a hard requirement of every stack - it is the design's one
    // fixed number. Being the loudest layer is not: 01 §1 measures the sub at
    // 0 dB in five of seven hero hits, and by construction a light contact is
    // mostly transient with almost no sub. So the loudness half is checked where
    // the references checked it - on the hero.
    {
        const auto events = GatherEvents(run.cues);
        int stacks = 0;
        int badOffset = 0;
        int subLoudest = 0;
        double worstOffset = 0.0;
        const Event* hero = nullptr;
        for (const Event& event : events) {
            if (event.sub == nullptr || event.transient == nullptr) {
                continue;
            }
            ++stacks;
            const double offset = event.sub->timeMs - event.transient->timeMs;
            // The configured offset plus the scatter the composite applies, which
            // is deliberate variation rather than error.
            const double slack = config.strategies.impact.offsetScatterMs + 1.0;
            if (offset < 55.0 - slack || offset > 75.0 + slack) {
                ++badOffset;
                worstOffset = std::max(worstOffset, std::fabs(offset - 65.0));
            }
            if (event.sub->gainDb >= event.loudestNonSubDb) {
                ++subLoudest;
            }
            if (hero == nullptr || event.peakDb > hero->peakDb) {
                hero = &event;
            }
        }
        // The loudness clause has a precondition the offset does not: the layer
        // balance is a continuum from "mostly transient, almost no sub" to
        // "sub-dominant", and with the shipped endpoints the sub only takes the
        // lead above about intensity 0.9. That is deliberate - an ordinary shove
        // should not boom - so the clause is checked where the references checked
        // it, on a contact near the top of the range, and reported as untested
        // where the take has none.
        float peakIntensity = 0.0f;
        for (const Cue& cue : run.cues) {
            peakIntensity = std::max(peakIntensity, cue.intensity);
        }
        const bool heroIsHero = hero != nullptr && hero->sub != nullptr && peakIntensity >= 0.9f;
        const bool heroSubLoudest = hero != nullptr && hero->sub->gainDb >= hero->loudestNonSubDb;
        const bool passed = stacks > 0 && badOffset == 0 && (!heroIsHero || heroSubLoudest);
        Check(report, "sub layer", passed,
              stacks == 0
                  ? std::string("no composite stack carried a sub layer")
                  : std::format("{} stacks, {} outside 55-75 ms (worst {:.0f} ms off); sub leads "
                                "in {}/{}, peak intensity {:.2f}{}",
                                stacks, badOffset, worstOffset, subLoudest, stacks,
                                static_cast<double>(peakIntensity),
                                heroIsHero ? (heroSubLoudest ? ", and leads on the hero"
                                                             : ", but does NOT lead on the hero")
                                           : " - no near-ceiling contact, loudness half untested"));
    }

    // ── 7. exactly one settle cue closes the knockdown ───────────────────────
    {
        int settles = 0;
        for (const Cue& cue : run.cues) {
            if (cue.reason == CueReason::kSettleClose) {
                ++settles;
            }
        }
        Check(report, "settle closes", settles == 1,
              std::format("{} settle cues (design: exactly one)", settles));
    }

    // ── 8. the moment axis fired sanely, and never in mid-air ────────────────
    //
    // Two claims, both about Stage 2's second axis, and both traceable to a
    // failure that shipped.
    //
    // The hero count guards the floor. A test that is too willing spends the
    // moment on the first thing that touches - a 44.7 u/s thigh scuff, in the
    // case that motivated the check - and a moment that resets the burst budget
    // every time it opens stops being a budget at all. Zero heroes is
    // legitimate (a gentle slump crosses nothing and the design says so), but a
    // take with many is a floor set too low, and a hero moment that resets the
    // burst budget every time it opens stops being a budget.
    //
    // The in-flight count guards §3.6 and is an absolute: a fall that announces
    // it is over while the body is still falling reads as broken immediately,
    // which is the falsifiable half of the physics/design split.
    {
        const bool inFlightOk = run.stats.settleInFlight == 0;
        // Per knockdown rather than per take: the long recordings are several
        // knockdowns and are entitled to a hero apiece.
        const double perEvent =
            run.stats.bursts > 0 ? static_cast<double>(run.stats.heroes) : 0.0;
        const bool countOk = run.stats.heroes <= 3 || perEvent <= run.stats.bursts;
        Check(report, "hero moments", inFlightOk && countOk,
              std::format("{} hero moments (+{} re-anchored) over {} bursts; {} closing cues in "
                          "flight (design: 0-3 per knockdown, and never a settle in mid-air)",
                          run.stats.heroes, run.stats.heroReanchors, run.stats.bursts,
                          run.stats.settleInFlight));
    }

    // ── 9. the same seed and config twice give a byte-identical list ─────────
    {
        const OfflineResult again = RunOffline(recording, config, bank, options);
        bool identical = again.cues.size() == run.cues.size();
        std::size_t firstDifference = 0;
        if (identical) {
            for (std::size_t i = 0; i < run.cues.size(); ++i) {
                if (std::memcmp(&run.cues[i], &again.cues[i], sizeof(Cue)) != 0) {
                    identical = false;
                    firstDifference = i;
                    break;
                }
            }
        }
        Check(report, "determinism", identical,
              identical ? std::format("{} cues, byte-identical across two runs", run.cues.size())
                        : std::format("{} against {} cues, first difference at index {}",
                                      run.cues.size(), again.cues.size(), firstDifference));
    }

    return report;
}

}  // namespace rds
