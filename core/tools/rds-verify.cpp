// rds-verify - run Verify() over a folder of recordings and print the report.
//
// The testbench's `--verify` mode without the testbench: same call, same checks,
// no window and no audio device. It exists so the backend can be checked before
// the front end is written, and so CI has something to run.
//
// Usage: rds-verify [recordings-dir] [sound-bank-dir]

#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "rds/ConfigManager.h"
#include "rds/Log.h"
#include "rds/Mix.h"
#include "rds/Offline.h"
#include "rds/Pcm.h"
#include "rds/Recording.h"
#include "rds/Sfx.h"
#include "rds/Synth.h"

namespace {

/// The bank, and the two properties a renderer depends on that nothing else
/// checks: that a cue's (slot, variant) round-trips back to the sound the engine
/// chose, and that a stand-in is the same audio every time it is rendered. The
/// second one matters because the testbench renders the cue list twice to A/B two
/// configs, and a stand-in that differed between renders would show up as a
/// config difference that was really just noise.
int PrintBankSummary(rds::SoundBank& bank) {
    int problems = 0;
    std::printf("sound bank\n");
    for (const auto& slot : rds::Slots()) {
        const std::size_t files = bank.FileCount(slot.id);
        if (slot.expectedVariants == 0) {
            std::printf("  %-14s declared and unfilled\n", std::string(slot.name).c_str());
            continue;
        }

        rds::ResolvedSound picked{};
        if (!bank.Resolve(slot.id, rds::SurfaceClass::kSoft, rds::Coverage::kBare,
                          rds::LimbSite::kTorso, picked)) {
            std::printf("  %-14s RESOLVE FAILED\n", std::string(slot.name).c_str());
            ++problems;
            continue;
        }
        rds::ResolvedSound recovered{};
        const bool roundTrips = bank.Get(slot.id, picked.variant, recovered) &&
                                recovered.path == picked.path &&
                                recovered.variant == picked.variant &&
                                recovered.procedural == picked.procedural &&
                                recovered.lengthMs == picked.lengthMs;

        if (files > 0) {
            std::printf("  %-14s %zu file(s), variant %u is %.0f ms%s\n",
                        std::string(slot.name).c_str(), files, picked.variant,
                        static_cast<double>(picked.lengthMs),
                        roundTrips ? "" : "  [GET DOES NOT ROUND-TRIP]");
        } else {
            const rds::SynthBuffer a = rds::Synthesise(recovered);
            const rds::SynthBuffer b = rds::Synthesise(recovered);
            const bool identical = a.samples == b.samples;
            std::printf("  %-14s 0/%u files, procedural (%.0f ms, %zu samples, %s)%s\n",
                        std::string(slot.name).c_str(), slot.expectedVariants,
                        static_cast<double>(a.LengthMs()), a.samples.size(),
                        identical ? "reproducible" : "NOT REPRODUCIBLE",
                        roundTrips ? "" : "  [GET DOES NOT ROUND-TRIP]");
            problems += identical ? 0 : 1;
        }
        problems += roundTrips ? 0 : 1;
    }
    std::printf("\n");
    return problems;
}

/// Write both ini files, edit one value into the file by hand, read it back, and
/// check that the value survived and that the comments did too.
///
/// Nothing else exercises ConfigManager - the engine takes a struct - and the
/// two properties that matter are exactly the ones a round trip tests: a fresh
/// install must come out complete and self-documenting, and saving must not eat
/// what somebody wrote in the file.
/// The path the game now renders through, checked without a game.
///
/// The plugin mixes each composite itself and hands the engine one PCM blob, so
/// three things have to hold that nothing else here touches: every pack file
/// decodes, a mixed composite actually contains the layers it was given at the
/// offsets it was given them, and the RIFF container we hand the engine reads
/// back as what we put in. A silent failure in any of them is a silent mod.
int CheckPcmAndMix(rds::SoundBank& bank) {
    int problems = 0;

    rds::PcmCache cache;
    cache.SetBank(&bank, 48000);

    std::size_t decoded = 0;
    std::size_t standIn = 0;
    for (const auto& slot : rds::Slots()) {
        for (std::uint8_t variant = 0; variant < bank.FileCount(slot.id); ++variant) {
            const rds::PcmBuffer& pcm = cache.Get(slot.id, variant);
            if (pcm.Empty()) {
                std::printf("pcm         FAIL - %s variant %u decoded to nothing\n",
                            std::string(slot.name).c_str(), variant);
                ++problems;
                continue;
            }
            // What the cache actually did, not what the bank says it should have.
            // A file that fails to decode falls back to its stand-in and the mod
            // still makes a noise, so counting the bank's view here would report
            // a healthy pack while every slot was quietly synthesised.
            if (pcm.procedural) {
                ++standIn;
                std::printf("pcm         FAIL - %s variant %u fell back to the stand-in\n",
                            std::string(slot.name).c_str(), variant);
                ++problems;
            } else {
                ++decoded;
            }
        }
    }

    // A composite with a known shape: two layers, the second 65 ms after the
    // first, which is the offset the whole design turns on.
    constexpr double kSubOffsetMs = 65.0;
    std::vector<rds::Cue> cues(2);
    cues[0].timeMs = 1000.0;
    cues[0].op = rds::CueOp::kPlayOneShot;
    cues[0].slot = rds::SlotId::kImpTransient;
    cues[0].gainDb = -6.0f;
    cues[1].timeMs = 1000.0 + kSubOffsetMs;
    cues[1].op = rds::CueOp::kPlayOneShot;
    cues[1].slot = rds::SlotId::kImpSub;
    cues[1].gainDb = 0.0f;

    rds::MixParams params;
    params.sampleRate = 48000;
    rds::MixBuffer mixed;
    if (!rds::MixComposite(cues, cache, params, mixed) || mixed.Empty()) {
        std::printf("mix         FAIL - a two-layer composite mixed to nothing\n\n");
        return problems + 1;
    }

    // The buffer starts at the earliest cue, not at the cue clock's origin.
    const bool startedAtEarliest = mixed.startMs == 1000.0;

    // The sub has to be audibly present at its own offset and absent before it.
    // Measured against the transient's own tail rather than against silence,
    // because the transient is still ringing there.
    const auto frameAt = [&](double ms) {
        return static_cast<std::size_t>(ms * 0.001 * 48000.0);
    };
    float beforeSub = 0.0f;
    for (std::size_t i = frameAt(kSubOffsetMs - 10.0); i < frameAt(kSubOffsetMs - 1.0) &&
                                                       i < mixed.samples.size(); ++i) {
        beforeSub = std::max(beforeSub, std::fabs(mixed.samples[i]));
    }
    float afterSub = 0.0f;
    for (std::size_t i = frameAt(kSubOffsetMs + 5.0); i < frameAt(kSubOffsetMs + 40.0) &&
                                                      i < mixed.samples.size(); ++i) {
        afterSub = std::max(afterSub, std::fabs(mixed.samples[i]));
    }
    const bool subArrived = afterSub > beforeSub * 1.5f;

    // Deterministic: the same cues and the same cache mix to the same samples.
    rds::MixBuffer again;
    rds::MixComposite(cues, cache, params, again);
    const bool deterministic = again.samples == mixed.samples;

    // And the container round-trips. Written to a real file because that is the
    // path a wav takes on the way in, and it exercises the chunk walk rather
    // than just the encoder.
    const auto wavPath = std::filesystem::temp_directory_path() / "rds-verify-mix.wav";
    const std::vector<std::uint8_t> encoded = rds::EncodeWavPcm16(mixed.samples, 48000);
    {
        std::ofstream out(wavPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
    }
    const rds::PcmBuffer readBack = rds::ReadWavMono(wavPath.string(), 48000);
    float worst = 0.0f;
    const std::size_t common = std::min(readBack.samples.size(), mixed.samples.size());
    for (std::size_t i = 0; i < common; ++i) {
        worst = std::max(worst, std::fabs(readBack.samples[i] - mixed.samples[i]));
    }
    // One 16-bit step is 1/32768; allow two for the round to nearest.
    const bool roundTripped = common == mixed.samples.size() && worst <= 2.0f / 32768.0f;

    problems += startedAtEarliest ? 0 : 1;
    problems += subArrived ? 0 : 1;
    problems += deterministic ? 0 : 1;
    problems += roundTripped ? 0 : 1;

    std::printf("pcm + mix   %-4s - %zu wav, %zu stand-in decoded; composite %.0f ms, peak %.2f; "
                "start %s, sub at +%.0f ms %s, deterministic %s, wav round-trip %s (worst %.1f LSB)\n\n",
                problems == 0 ? "ok" : "FAIL", decoded, standIn, mixed.LengthMs(), mixed.rawPeak,
                startedAtEarliest ? "yes" : "NO", kSubOffsetMs, subArrived ? "yes" : "NO",
                deterministic ? "yes" : "NO", roundTripped ? "yes" : "NO", worst * 32768.0f);
    return problems;
}


/// The mute: that it survives the ini, that it keeps the file's variant index,
/// and that nothing ever picks it.
///
/// The index is the part worth asserting. A mute could be implemented by leaving
/// the file out of the slot's variant list, and everything would still sound
/// right - but every variant after it would renumber, so a cue list recorded
/// before the mute would play different files after it, and unmuting would not
/// put the take back. The whole point of the design is that it does.
int CheckSfxMute(const std::string& bankDir) {
    rds::SfxLibrary library;
    library.Load(bankDir);

    rds::SfxAssignments assignments;
    assignments.SeedFromNames(library);

    // Any slot with two files will do; three is better, because it also proves
    // the survivors still take turns rather than the slot collapsing onto one.
    const rds::SlotDesc* subject = nullptr;
    for (const rds::SlotDesc& desc : rds::Slots()) {
        if (assignments.For(desc.id).files.size() >= 2) {
            subject = &desc;
            break;
        }
    }
    if (subject == nullptr) {
        std::printf("sfx mute   skipped - no slot in %s has two files to choose between\n\n",
                    bankDir.empty() ? "(no bank dir)" : bankDir.c_str());
        return 0;
    }

    const rds::SlotId slot = subject->id;
    const std::string victim = assignments.For(slot).files[1];
    assignments.For(slot).muted.push_back(victim);

    // ── the ini ──────────────────────────────────────────────────────────────
    const auto dir = std::filesystem::temp_directory_path() / "rds-verify-sfx";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "RagdollSounds_SFX.ini";

    rds::SfxAssignments reloaded;
    const bool saved = assignments.Save(file);
    if (saved) {
        (void)reloaded.Load(file);
    }
    const bool roundTrips = saved && reloaded == assignments;

    // ── the bank ─────────────────────────────────────────────────────────────
    rds::SoundBank bank;
    bank.LoadAssigned(library, assignments, bankDir);
    bank.Seed(1);

    const std::size_t expected = assignments.For(slot).files.size();
    const bool keptIndex = bank.FileCount(slot) == expected && bank.VariantMuted(slot, 1);

    // Both pickers, because they are two different paths through Resolve and
    // only one of them consults the shuffle bag.
    bool neverPicked = true;
    for (std::uint32_t i = 0; i < 400; ++i) {
        rds::ResolvedSound picked{};
        if (bank.Resolve(slot, rds::SurfaceClass::kSoft, rds::Coverage::kBare,
                         rds::LimbSite::kTorso, picked, i % 2 == 0 ? 0u : i)) {
            neverPicked = neverPicked && picked.variant != 1;
        }
    }

    // Every file muted is silence, not a synthesised replacement: muting
    // something is not a request to stand in for it.
    rds::SfxAssignments allMuted = assignments;
    allMuted.For(slot).muted = allMuted.For(slot).files;
    rds::SoundBank silent;
    silent.LoadAssigned(library, allMuted, bankDir);
    silent.Seed(1);
    rds::ResolvedSound nothing{};
    const bool goesSilent = !silent.Resolve(slot, rds::SurfaceClass::kSoft, rds::Coverage::kBare,
                                            rds::LimbSite::kTorso, nothing);

    const bool ok = roundTrips && keptIndex && neverPicked && goesSilent;
    std::printf("sfx mute    %s - %s: ini round-trip %s, kept its variant index %s, never "
                "picked %s, all-muted is silence %s\n\n",
                ok ? "ok  " : "FAIL", std::string(subject->name).c_str(),
                roundTrips ? "yes" : "NO", keptIndex ? "yes" : "NO", neverPicked ? "yes" : "NO",
                goesSilent ? "yes" : "NO");
    return ok ? 0 : 1;
}

int CheckConfigRoundTrip() {
    const auto dir = std::filesystem::temp_directory_path() / "rds-verify-config";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    auto& manager = rds::ConfigManager::Get();
    manager.Initialize(dir);
    manager.Load();

    const auto algorithmPath = dir / "RagdollSounds_Algorithm.ini";
    std::string text;
    {
        std::ifstream in(algorithmPath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t keys = rds::AlgorithmParams().size() + rds::GeneralParams().size();
    std::size_t comments = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        comments += text[i] == ';' ? 1u : 0u;
    }

    // A hand edit, plus a comment nothing in the schema knows about.
    const std::string before = "fRateCapMs = 46";
    const std::string after = "; a note somebody left in the file\nfRateCapMs = 61";
    const auto at = text.find(before);
    if (at == std::string::npos) {
        std::printf("config      FAIL - fRateCapMs was not written to the file\n\n");
        return 1;
    }
    text.replace(at, before.size(), after);
    {
        std::ofstream out(algorithmPath, std::ios::binary | std::ios::trunc);
        out << text;
    }

    manager.Load();
    const bool readBack = manager.Algorithm().arb.rateCapMs == 61.0f;
    manager.Save();

    std::string again;
    {
        std::ifstream in(algorithmPath, std::ios::binary);
        again.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const bool keptComment = again.find("a note somebody left in the file") != std::string::npos;
    const bool keptValue = again.find("fRateCapMs = 61") != std::string::npos;

    const bool ok = readBack && keptComment && keptValue;
    std::printf("config      %s - %zu keys, %zu comment lines; hand edit read back %s, "
                "comment survived save %s\n\n",
                ok ? "ok  " : "FAIL", keys, comments, readBack ? "yes" : "NO",
                keptComment && keptValue ? "yes" : "NO");
    return ok ? 0 : 1;
}

/// `--set Section:Key=value`, straight through the schema. One line here gives
/// every parameter a command-line override, which is the same payoff the ini
/// reader and the testbench's slider panel get from the same table.
bool ApplyOverride(rds::AlgorithmConfig& config, std::string_view assignment) {
    const auto equals = assignment.find('=');
    if (equals == std::string_view::npos) {
        return false;
    }
    const auto qualified = assignment.substr(0, equals);
    const auto value = assignment.substr(equals + 1);
    for (const auto& param : rds::AlgorithmParams()) {
        if (rds::QualifiedKey(param) == qualified) {
            rds::SetParam(&config, param, rds::ParseParam(param, value));
            std::printf("override %s = %s\n", std::string(qualified).c_str(),
                        rds::FormatParam(param, rds::GetParam(&config, param)).c_str());
            return true;
        }
    }
    std::printf("no such parameter: %s\n", std::string(qualified).c_str());
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path recordingsDir{"Research/NewRecordings"};
    std::string bankDir;
    std::vector<std::string> overrides;
    bool verbose = false;
    std::string only;

    std::string writeConfigDir;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--write-config" && i + 1 < argc) {
            writeConfigDir = argv[++i];
        } else if (arg == "--set" && i + 1 < argc) {
            overrides.emplace_back(argv[++i]);
        } else if (arg == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (arg == "--debug") {
            verbose = true;
        } else if (positional == 0) {
            recordingsDir = arg;
            ++positional;
        } else {
            bankDir = arg;
            ++positional;
        }
    }

    rds::log::Options logOptions;
    logOptions.directory = std::filesystem::temp_directory_path() / "rds-verify";
    logOptions.level = verbose ? rds::LogLevel::kDebug : rds::LogLevel::kWarn;
    logOptions.rotate = false;
    logOptions.alsoStdout = verbose;
    rds::log::Setup(logOptions);

    // Writing the pair a release ships. The plugin generates these on first run,
    // which means whatever the dev machine last ran with is what would be
    // packaged - the same trap Virtual HMD's deployment_files exists to avoid.
    // Generating them from the schema instead makes the shipped file a function
    // of the code rather than of somebody's last session.
    if (!writeConfigDir.empty()) {
        auto& manager = rds::ConfigManager::Get();
        manager.Initialize(std::filesystem::path{writeConfigDir});
        manager.Save();
        std::printf("wrote a complete default config pair into %s\n", writeConfigDir.c_str());
        return 0;
    }

    rds::SoundBank bank;
    bank.Load(bankDir);
    bank.Seed(1);
    int failures = PrintBankSummary(bank);
    failures += CheckConfigRoundTrip();
    // The round-trip check loads a general ini, and loading one applies its
    // `iLogLevel` - so `--debug` used to be switched back off by the check that
    // ran after it and the firehose never arrived. Re-asserted here rather than
    // moved, because the check has to write a real ini to be worth anything.
    rds::log::SetLevel(logOptions.level);
    failures += CheckSfxMute(bankDir);
    failures += CheckPcmAndMix(bank);

    rds::AlgorithmConfig config{};  // shipping defaults, which are the point
    for (const auto& assignment : overrides) {
        ApplyOverride(config, assignment);
    }
    rds::OfflineOptions options;
    options.seed = 1;
    options.trace = true;

    const auto takes = rds::Recording::Scan(recordingsDir);
    if (takes.empty()) {
        std::printf("no recordings under %s\n", recordingsDir.string().c_str());
        return 2;
    }

    int checksRun = 0;
    for (const auto& info : takes) {
        if (!only.empty() && info.stem.find(only) == std::string::npos) {
            continue;
        }
        rds::Recording recording;
        std::string error;
        const auto csv = recordingsDir / (info.stem + ".csv");
        if (!recording.Load(csv, error)) {
            std::printf("%-52s LOAD FAILED: %s\n", info.stem.c_str(), error.c_str());
            ++failures;
            continue;
        }

        const rds::VerifyReport report = rds::Verify(recording, config, bank, options);
        const rds::EngineStats& s = report.stats;
        std::printf("%s  (%s)\n", report.recordingStem.c_str(),
                    report.Passed() ? "all checks pass" : "FAILURES");
        // Where the events went. This is the first thing to look at when a check
        // fails, because a check can fail either because the algorithm is wrong
        // or because nothing ever reached it, and these two lines tell those
        // apart.
        std::printf("   events %u | contacts %u | rejected floor %u blowup %u | mirror %u "
                    "manifold %u foley %u\n",
                    s.eventsIn, s.contactsIn, s.rejectedBelowFloor, s.rejectedBlowup,
                    s.droppedMirror, s.collapsedManifold, s.routedToFoley);
        std::printf("   proposed %u | dropped rate %u chain %u mask %u burst %u voices %u | "
                    "emitted %u in %u bursts\n",
                    s.proposedCues, s.droppedRateCap, s.droppedChainMerge, s.droppedMasking,
                    s.droppedBurstCap, s.droppedVoiceCap, s.emittedCues, s.bursts);
        // The moment axis, on its own line. A take with four heroes has its
        // floor set too low; a take with none is either a gentle slump, which is
        // legitimate, or a floor set too high. `settle in flight` must be zero -
        // it counts closing cues emitted while the body was measurably still in
        // the air, which is the most obviously broken thing this engine can do.
        std::printf(
            "   heroes %u (+%u re-anchored, %u on head relief) | settle in flight %u | "
            "driven %u\n",
            s.heroes, s.heroReanchors, s.heroHeadRelief, s.settleInFlight,
            s.drivenFlights);
        // The slide, on its own line, because the pair is read together: how
        // many slides the entry test found at all, and how many of them ended
        // because something stopped the body rather than because it came to rest
        // or left the ground. The second number is the one the old machine had
        // no way to express and simply faded out over.
        std::printf("   slides %u (%u ended on an impact)\n", s.slides, s.slideImpacts);
        for (const auto& check : report.checks) {
            ++checksRun;
            failures += check.passed ? 0 : 1;
            std::printf("   %-4s %-18s %s\n", check.passed ? "ok" : "FAIL", check.name.c_str(),
                        check.detail.c_str());
        }
        std::printf("\n");
    }

    std::printf("%d checks over %zu recordings, %d failed\n", checksRun, takes.size(), failures);
    return failures == 0 ? 0 : 1;
}
