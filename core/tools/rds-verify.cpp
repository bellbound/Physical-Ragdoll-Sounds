// rds-verify - run Verify() over a folder of recordings and print the report.
//
// The testbench's `--verify` mode without the testbench: same call, same checks,
// no window and no audio device. It exists so the backend can be checked before
// the front end is written, and so CI has something to run.
//
// Usage: rds-verify [recordings-dir] [sound-bank-dir]

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rds/ConfigManager.h"
#include "rds/Link.h"
#include "rds/Log.h"
#include "rds/Mix.h"
#include "rds/Offline.h"
#include "rds/Pcm.h"
#include "rds/Recording.h"
#include "rds/Sfx.h"

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
        // "Expects no variants" is not the same as "has none": the armour skins
        // expect nothing so that the feature is silent until somebody records
        // it, but they are meant to be filled. Report what is actually there.
        if (slot.expectedVariants == 0 && files == 0) {
            std::printf("  %-14s declared and unfilled\n", std::string(slot.name).c_str());
            continue;
        }

        rds::ResolvedSound picked{};
        if (!bank.Resolve(slot.id, rds::SurfaceClass::kSoft, rds::Coverage::kBare,
                          rds::LimbSite::kTorso, picked)) {
            // Nothing recorded for it and nothing along its fallback chain
            // either. Not a crash and no longer a stand-in: this layer does not
            // sound at all, which is a hole in the pack and is worth failing
            // over - a slot that is meant to be silent says so by expecting no
            // variants, and was reported above.
            std::printf("  %-14s 0/%u files, NOTHING TO PLAY\n", std::string(slot.name).c_str(),
                        slot.expectedVariants);
            ++problems;
            continue;
        }
        // Against the slot that actually resolved, not the one that was asked
        // for. A slot with a declared fallback - the surface-coloured scrapes -
        // answers with its fallback's file until somebody records one, and the
        // renderer turns a cue back into samples through `cue.slot`, which is
        // that same resolved slot. Asking `Get` for the slot we requested tests
        // a lookup nothing performs.
        rds::ResolvedSound recovered{};
        const bool roundTrips = bank.Get(picked.slot, picked.variant, recovered) &&
                                recovered.path == picked.path &&
                                recovered.variant == picked.variant &&
                                recovered.lengthMs == picked.lengthMs;

        // Where a slot is standing in for another, say so: "0 files" and "0
        // files, but you will hear scrape_loop" are different states of the
        // pack, and only one of them needs a recording before the mod is right.
        char fellBack[64] = "";
        if (picked.slot != slot.id) {
            std::snprintf(fellBack, sizeof(fellBack), ", plays %s",
                          std::string(rds::ToString(picked.slot)).c_str());
        }

        if (files > 0) {
            std::printf("  %-14s %zu file(s), variant %u is %.0f ms%s%s\n",
                        std::string(slot.name).c_str(), files, picked.variant,
                        static_cast<double>(picked.lengthMs), fellBack,
                        roundTrips ? "" : "  [GET DOES NOT ROUND-TRIP]");
        } else {
            // No file of its own, but it resolved - so it is playing somebody
            // else's recording, and `fellBack` says whose. That is the only way
            // a slot with no files sounds at all now.
            std::printf("  %-14s 0/%u files, no file of its own (%.0f ms)%s%s\n",
                        std::string(slot.name).c_str(), slot.expectedVariants,
                        static_cast<double>(picked.lengthMs), fellBack,
                        roundTrips ? "" : "  [GET DOES NOT ROUND-TRIP]");
        }
        problems += roundTrips ? 0 : 1;
    }
    std::printf("\n");
    return problems;
}

/// Write both ini files, edit one value by hand, read it back, and check that the
/// value and the comments survived. Nothing else exercises ConfigManager, and a
/// round trip tests the two properties that matter: a fresh install comes out
/// complete and self-documenting, and saving does not eat what somebody wrote.

/// The path the game renders through, checked without a game.
///
/// The plugin mixes each composite itself and hands the engine one PCM blob, so
/// three things have to hold: every pack file decodes, a mixed composite contains
/// the layers it was given at the offsets it was given, and the RIFF container
/// reads back as what we put in. A silent failure in any is a silent mod.
int CheckPcmAndMix(rds::SoundBank& bank) {
    int problems = 0;

    rds::PcmCache cache;
    cache.SetBank(&bank, 48000);

    std::size_t decoded = 0;
    for (const auto& slot : rds::Slots()) {
        for (std::uint8_t variant = 0; variant < bank.FileCount(slot.id); ++variant) {
            const rds::PcmBuffer& pcm = cache.Get(slot.id, variant);
            if (pcm.Empty()) {
                std::printf("pcm         FAIL - %s variant %u decoded to nothing\n",
                            std::string(slot.name).c_str(), variant);
                ++problems;
                continue;
            }
            // What the cache actually did, not what the bank says it should
            // have. A file the bank measured at load can still fail to decode
            // here, and with nothing synthesised behind it that is a layer the
            // mod has silently lost - which the empty check above catches.
            ++decoded;
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

    std::printf("pcm + mix   %-4s - %zu wav decoded; composite %.0f ms, peak %.2f; "
                "start %s, sub at +%.0f ms %s, deterministic %s, wav round-trip %s (worst %.1f LSB)\n\n",
                problems == 0 ? "ok" : "FAIL", decoded, mixed.LengthMs(), mixed.rawPeak,
                startedAtEarliest ? "yes" : "NO", kSubOffsetMs, subArrived ? "yes" : "NO",
                deterministic ? "yes" : "NO", roundTripped ? "yes" : "NO", worst * 32768.0f);
    return problems;
}


/// The mute: that it survives the ini, keeps the file's variant index, and is
/// never picked. The index is the part worth asserting - leaving the file out of
/// the variant list would sound right but renumber everything after it, so a cue
/// list recorded before the mute would play different files and unmuting would not
/// put the take back.

/// The specificity ladder: a tagged file wins where it matches, is invisible where
/// it does not, and never silences its slot.
///
/// Worth its own check because all three are easy to get individually right and
/// collectively wrong. The third is the one that would ship: tag the only file on
/// a slot, and a resolver treating a condition as a filter rather than a
/// preference deletes that layer from most of the game without a word.
int CheckSfxConditions(const std::string& bankDir) {
    rds::SfxLibrary library;
    library.Load(bankDir);

    rds::SfxAssignments assignments;
    assignments.SeedFromNames(library);

    const rds::SlotDesc* subject = nullptr;
    for (const rds::SlotDesc& desc : rds::Slots()) {
        if (assignments.For(desc.id).files.size() >= 2) {
            subject = &desc;
            break;
        }
    }
    if (subject == nullptr) {
        std::printf("sfx cond   skipped - no slot in %s has two files to choose between\n\n",
                    bankDir.empty() ? "(no bank dir)" : bankDir.c_str());
        return 0;
    }

    const rds::SlotId slot = subject->id;
    const std::string tagged = assignments.For(slot).files[1];
    assignments.For(slot).SetConditionAt(1, {rds::SurfaceMatch::kStone, rds::CoverageMatch::kAny});

    // The ini has to carry it, or none of the rest of this reaches the game.
    const auto dir = std::filesystem::temp_directory_path() / "rds-verify-cond";
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

    // And the wire, which is the other way a tag reaches the engine: the panel
    // pushes an assignment table to a running game on the frame it is edited,
    // and a condition dropped there would look exactly like one that had never
    // been set - working in the testbench, silently absent in the game.
    rds::SfxAssignments pushed;
    rds::link::DecodeSfx(rds::link::EncodeSfx(assignments), pushed);
    const bool survivesLink = pushed.For(slot).conditions == assignments.For(slot).conditions;

    rds::SoundBank bank;
    bank.LoadAssigned(library, assignments, bankDir);
    bank.Seed(1);
    bank.SetConditions(true, true, true);

    // On stone the tagged file is the only candidate: it is the most specific
    // tier that has anything in it, so the untagged ones drop out.
    bool onlyOnStone = true;
    for (std::uint32_t i = 0; i < 200; ++i) {
        rds::ResolvedSound picked{};
        if (bank.Resolve(slot, rds::SurfaceClass::kStone, rds::Coverage::kBare,
                         rds::LimbSite::kTorso, picked, i % 2 == 0 ? 0u : i)) {
            onlyOnStone = onlyOnStone && picked.variant == 1;
        }
    }

    // Off stone it is invisible, and the others take turns as they always did.
    bool neverOffStone = true;
    for (std::uint32_t i = 0; i < 200; ++i) {
        rds::ResolvedSound picked{};
        if (bank.Resolve(slot, rds::SurfaceClass::kWood, rds::Coverage::kBare,
                         rds::LimbSite::kTorso, picked, i % 2 == 0 ? 0u : i)) {
            neverOffStone = neverOffStone && picked.variant != 1;
        }
    }

    // A slot whose *every* file is tagged for a surface it is not on plays its
    // full set rather than going silent. A condition is a preference.
    rds::SfxAssignments allTagged = assignments;
    for (std::size_t i = 0; i < allTagged.For(slot).files.size(); ++i) {
        allTagged.For(slot).SetConditionAt(i, {rds::SurfaceMatch::kStone, rds::CoverageMatch::kAny});
    }
    rds::SoundBank stubborn;
    stubborn.LoadAssigned(library, allTagged, bankDir);
    stubborn.Seed(1);
    stubborn.SetConditions(true, true, true);
    rds::ResolvedSound anything{};
    const bool fallsBack = stubborn.Resolve(slot, rds::SurfaceClass::kWood, rds::Coverage::kBare,
                                            rds::LimbSite::kTorso, anything, 0);

    // And the master switch really does turn the whole thing off.
    bank.SetConditions(false, true, true);
    bool ignoresWhenOff = false;
    for (std::uint32_t i = 0; i < 200 && !ignoresWhenOff; ++i) {
        rds::ResolvedSound picked{};
        if (bank.Resolve(slot, rds::SurfaceClass::kWood, rds::Coverage::kBare,
                         rds::LimbSite::kTorso, picked, i % 2 == 0 ? 0u : i)) {
            ignoresWhenOff = picked.variant == 1;
        }
    }

    const bool ok = roundTrips && survivesLink && onlyOnStone && neverOffStone && fallsBack &&
                    ignoresWhenOff;
    std::printf("sfx cond   %s - %s on '%s': ini round-trip %s, link round-trip %s, wins on "
                "stone %s, absent off stone %s, unsatisfiable falls back %s, master switch %s\n\n",
                ok ? "ok  " : "FAIL", std::string(subject->name).c_str(), tagged.c_str(),
                roundTrips ? "yes" : "NO", survivesLink ? "yes" : "NO",
                onlyOnStone ? "yes" : "NO", neverOffStone ? "yes" : "NO", fallsBack ? "yes" : "NO",
                ignoresWhenOff ? "yes" : "NO");
    return ok ? 0 : 1;
}

/// The same file on one slot twice: once plain, once tagged - "keep it in the set,
/// and make it *the* one on stone". A condition used to be stored against the
/// filename, so the tag landed on both copies. Everything about a condition is per
/// placement now, and this checks it through the ini and the wire, because a
/// placement that only exists in memory is one the game never hears.
int CheckSfxDuplicatePlacement(const std::string& bankDir) {
    rds::SfxLibrary library;
    library.Load(bankDir);

    rds::SfxAssignments assignments;
    assignments.SeedFromNames(library);

    const rds::SlotDesc* subject = nullptr;
    for (const rds::SlotDesc& desc : rds::Slots()) {
        if (assignments.For(desc.id).files.size() >= 2) {
            subject = &desc;
            break;
        }
    }
    if (subject == nullptr) {
        std::printf("sfx twice  skipped - no slot in %s has two files to choose between\n\n",
                    bankDir.empty() ? "(no bank dir)" : bankDir.c_str());
        return 0;
    }

    const rds::SlotId slot = subject->id;
    // The file that is already plain at 0, added again and tagged. The two
    // placements are the same sound and differ only in what they are for.
    const std::string file = assignments.For(slot).files[0];
    assignments.For(slot).Add(file, {rds::SurfaceMatch::kStone, rds::CoverageMatch::kAny});
    const auto twice = assignments.For(slot).files.size() - 1;
    const bool tagsOnlyTheNewOne = assignments.For(slot).ConditionAt(0).Unconditional() &&
                                   !assignments.For(slot).ConditionAt(twice).Unconditional();

    const auto dir = std::filesystem::temp_directory_path() / "rds-verify-twice";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto iniFile = dir / "RagdollSounds_SFX.ini";
    rds::SfxAssignments reloaded;
    const bool saved = assignments.Save(iniFile);
    if (saved) {
        (void)reloaded.Load(iniFile);
    }
    const bool roundTrips = saved && reloaded == assignments;

    rds::SfxAssignments pushed;
    rds::link::DecodeSfx(rds::link::EncodeSfx(assignments), pushed);
    const bool survivesLink = pushed == assignments;

    rds::SoundBank bank;
    bank.LoadAssigned(library, assignments, bankDir);
    bank.Seed(1);
    bank.SetConditions(true, true, true);

    // On stone the tagged placement is the only candidate - the plain copy of
    // the same file included, because plain is a lower tier and not a synonym.
    bool onlyTheTagged = true;
    for (std::uint32_t i = 0; i < 200; ++i) {
        rds::ResolvedSound picked{};
        if (bank.Resolve(slot, rds::SurfaceClass::kStone, rds::Coverage::kBare,
                         rds::LimbSite::kTorso, picked, i % 2 == 0 ? 0u : i)) {
            onlyTheTagged = onlyTheTagged && picked.variant == twice;
        }
    }

    // Off stone the plain copy is still in the set: the tag took nothing away
    // from the sound, only added a place it wins.
    bool plainStillPlays = false;
    bool taggedStaysAway = true;
    for (std::uint32_t i = 0; i < 200; ++i) {
        rds::ResolvedSound picked{};
        if (bank.Resolve(slot, rds::SurfaceClass::kWood, rds::Coverage::kBare,
                         rds::LimbSite::kTorso, picked, i % 2 == 0 ? 0u : i)) {
            plainStillPlays = plainStillPlays || picked.variant == 0;
            taggedStaysAway = taggedStaysAway && picked.variant != twice;
        }
    }

    const bool ok = tagsOnlyTheNewOne && roundTrips && survivesLink && onlyTheTagged &&
                    plainStillPlays && taggedStaysAway;
    std::printf("sfx twice  %s - %s plays '%s' plain and tagged: one tag not two %s, ini "
                "round-trip %s, link round-trip %s, tagged copy wins on stone %s, plain copy "
                "still plays off it %s, tagged copy absent off it %s\n\n",
                ok ? "ok  " : "FAIL", std::string(subject->name).c_str(), file.c_str(),
                tagsOnlyTheNewOne ? "yes" : "NO", roundTrips ? "yes" : "NO",
                survivesLink ? "yes" : "NO", onlyTheTagged ? "yes" : "NO",
                plainStillPlays ? "yes" : "NO", taggedStaysAway ? "yes" : "NO");
    return ok ? 0 : 1;
}

/// The per-file corrections: a pitch and a trim that belong to the recording rather
/// than the assignment, reaching Stage 5 through `ResolvedSound`.
///
/// Worth its own check because the corpus replay cannot see it - `main` fills its
/// bank with the by-name scan, which has no library behind it and no metadata to
/// read a correction out of.
///
/// Four things, and the length is the one that would ship broken: pitch here is
/// resampling, so a corrected file plays shorter and a renderer sizing its buffer
/// off the container length cuts the tail off every corrected sound.
int CheckSfxCorrections(const std::string& bankDir) {
    rds::SfxLibrary library;
    library.Load(bankDir);

    rds::SfxAssignments assignments;
    assignments.SeedFromNames(library);

    const rds::SlotDesc* subject = nullptr;
    for (const rds::SlotDesc& desc : rds::Slots()) {
        if (!assignments.For(desc.id).files.empty()) {
            subject = &desc;
            break;
        }
    }
    if (subject == nullptr) {
        std::printf("sfx adjust skipped - no slot in %s has a file to correct\n\n",
                    bankDir.empty() ? "(no bank dir)" : bankDir.c_str());
        return 0;
    }

    const rds::SlotId slot = subject->id;
    const std::string target = assignments.For(slot).files[0];

    constexpr float kPitch = 1.25f;
    constexpr float kTrimDb = -3.5f;
    float declaredMs = 0.0f;
    for (rds::SfxEntry& entry : library.MutableEntries()) {
        if (entry.file == target) {
            entry.pitch = kPitch;
            entry.trimDb = kTrimDb;
            declaredMs = entry.durationMs;
        }
    }

    rds::SoundBank bank;
    bank.LoadAssigned(library, assignments, bankDir);
    bank.Seed(1);

    // Pinned rather than picked: which variant a token lands on is not what this
    // is about, and a slot with three files would otherwise fail at random.
    bank.ForceVariant(slot, 0);
    rds::ResolvedSound picked{};
    const bool resolved = bank.Resolve(slot, rds::SurfaceClass::kSoft, rds::Coverage::kBare,
                                       rds::LimbSite::kTorso, picked, 1u);
    bank.ForceVariant(slot, rds::SoundBank::kNoVariant);

    const bool carriesPitch = resolved && std::fabs(picked.pitch - kPitch) < 0.001f;
    const bool carriesTrim = resolved && std::fabs(picked.trimDb - kTrimDb) < 0.001f;
    // The length has the pitch in it. Skipped rather than failed where nothing
    // knows the duration, which is what a library with no metadata looks like.
    const bool lengthShrinks =
        declaredMs <= 0.0f || std::fabs(picked.lengthMs - declaredMs / kPitch) < 1.0f;

    // ── the sidecar round-trip ───────────────────────────────────────────────
    //
    // Hand-written rather than saved, because SaveMeta writes beside the real
    // file and a verifier must not edit the library it is checking. The clamp is
    // the point: these are the two keys somebody will reach into the ini and
    // type a 0 or a 50 into, and a 0 divides by zero in EffectiveDurationMs.
    const auto dir = std::filesystem::temp_directory_path() / "rds-verify-adjust";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto wav = dir / "probe.wav";
    bool readBack = false;
    bool clamped = false;
    if (std::ofstream out{rds::SfxLibrary::MetaPathFor(wav)}) {
        out << "[Sfx]\nName = probe\nPitch = 1.2500\nTrimDb = -3.50\n";
        out.close();
        rds::SfxEntry entry{};
        readBack = rds::SfxLibrary::LoadMeta(wav, entry) &&
                   std::fabs(entry.pitch - kPitch) < 0.001f &&
                   std::fabs(entry.trimDb - kTrimDb) < 0.001f;
    }
    if (std::ofstream out{rds::SfxLibrary::MetaPathFor(wav)}) {
        out << "[Sfx]\nName = probe\nPitch = 0\nTrimDb = -400\n";
        out.close();
        rds::SfxEntry entry{};
        clamped = rds::SfxLibrary::LoadMeta(wav, entry) && entry.pitch >= 0.5f &&
                  entry.trimDb >= -24.0f;
    }
    std::filesystem::remove_all(dir, ec);

    const bool ok = carriesPitch && carriesTrim && lengthShrinks && readBack && clamped;
    std::printf("sfx adjust %s - %s '%s' at %.2fx %+.1f dB: pitch through %s, trim through %s, "
                "length %.0f ms against %.0f declared %s, sidecar read back %s, hand edit "
                "clamped %s\n\n",
                ok ? "ok  " : "FAIL", std::string(subject->name).c_str(), target.c_str(), kPitch,
                kTrimDb, carriesPitch ? "yes" : "NO", carriesTrim ? "yes" : "NO", picked.lengthMs,
                declaredMs, lengthShrinks ? "yes" : "NO", readBack ? "yes" : "NO",
                clamped ? "yes" : "NO");
    return ok ? 0 : 1;
}

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
    const bool readBack = manager.Algorithm(rds::ActorMode::kRagdoll).arb.rateCapMs == 61.0f;
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
bool ApplyOverride(rds::ConfigSet& config, std::string_view assignment) {
    const auto equals = assignment.find('=');
    if (equals == std::string_view::npos) {
        return false;
    }
    const auto qualified = assignment.substr(0, equals);
    const auto value = assignment.substr(equals + 1);
    for (const auto& param : rds::AlgorithmParams()) {
        if (rds::QualifiedKey(param) != qualified) {
            continue;
        }
        // Every column, not only the ragdoll one: `--set` is how a check says
        // "run the corpus with this number changed", and a take that happens to
        // hold an upright actor must be run with the same number.
        // `Section.Combat:Key` is how to move one column on its own.
        for (rds::AlgorithmConfig& column : config.modes) {
            rds::SetParam(&column, param, rds::ParseParam(param, value));
        }
        std::printf("override %s = %s\n", std::string(qualified).c_str(),
                    rds::FormatParam(param, rds::GetParam(&config.Base(), param)).c_str());
        return true;
    }
    // Then the mode-qualified rows, which address one column of the set.
    for (const rds::ActorMode mode : {rds::ActorMode::kGameplay, rds::ActorMode::kCombat}) {
        for (const auto& param : rds::ModeParams(mode)) {
            if (rds::QualifiedKey(param) != qualified) {
                continue;
            }
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

/// How much of a make-up gain the composites would actually deliver.
///
/// `MixComposite` soft-clips its buffer with a tanh at `clipCeiling`, so "ship the
/// slider at 0.5 and put 6 dB back in the mix" is only free if the composites are
/// quiet enough that 6 dB more is still under the knee. Above it the tanh eats the
/// difference and the player hears "more compressed" rather than "louder".
///
/// Measured by mixing every group twice, once as it ships and once with the gain
/// added to every cue - which is what raising `fMasterGainDb` does. The loss is
/// the gap between the louder mix's peak and the peak a straight-line clip would
/// have given. Zero means the gain arrived intact.
void CollectHeadroom(const std::vector<rds::Cue>& cues, rds::PcmCache& cache,
                     const rds::MixParams& params, const std::vector<float>& candidatesDb,
                     std::vector<std::vector<float>>& lossDbOut, std::vector<float>& peakOut) {
    // Grouped the way GameRenderer groups: one acoustic moment is (actor,
    // sourceSeq), and the damage layers are a second voice inside it. Both halves
    // take the moment's earliest time as their base, so this measures the buffers
    // the game actually builds rather than a merged one it no longer makes.
    struct Key {
        rds::ActorId actorId;
        std::uint32_t sourceSeq;
        bool gore;
        bool operator==(const Key&) const = default;
    };
    std::vector<std::pair<Key, std::vector<rds::Cue>>> groups;
    std::vector<std::pair<Key, rds::TimeMs>> bases;

    const auto isGore = [](rds::CueReason reason) {
        return reason == rds::CueReason::kCrunch || reason == rds::CueReason::kGore;
    };

    for (const rds::Cue& cue : cues) {
        if (cue.op != rds::CueOp::kPlayOneShot) {
            continue;
        }
        const Key key{cue.actorId, cue.sourceSeq, isGore(cue.reason)};
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const auto& g) { return g.first == key; });
        if (it == groups.end()) {
            groups.emplace_back(key, std::vector<rds::Cue>{cue});
        } else {
            it->second.push_back(cue);
        }
        // The base is the moment's, so it spans both halves of the key.
        const Key moment{cue.actorId, cue.sourceSeq, false};
        auto bit = std::find_if(bases.begin(), bases.end(),
                                [&](const auto& b) { return b.first == moment; });
        if (bit == bases.end()) {
            bases.emplace_back(moment, cue.timeMs);
        } else {
            bit->second = std::min(bit->second, cue.timeMs);
        }
    }

    rds::MixBuffer mixed;
    std::vector<rds::Cue> raised;
    for (const auto& [key, groupCues] : groups) {
        const rds::TimeMs base =
            std::find_if(bases.begin(), bases.end(), [&](const auto& b) {
                return b.first.actorId == key.actorId && b.first.sourceSeq == key.sourceSeq;
            })->second;

        if (!rds::MixComposite(groupCues, cache, params, mixed, base) || mixed.Empty()) {
            continue;
        }
        float basePeak = 0.0f;
        for (float sample : mixed.samples) {
            basePeak = std::max(basePeak, std::fabs(sample));
        }
        if (basePeak <= 0.0f) {
            continue;
        }
        peakOut.push_back(basePeak);

        for (std::size_t c = 0; c < candidatesDb.size(); ++c) {
            raised = groupCues;
            for (rds::Cue& cue : raised) {
                cue.gainDb += candidatesDb[c];
            }
            if (!rds::MixComposite(raised, cache, params, mixed, base) || mixed.Empty()) {
                continue;
            }
            float loudPeak = 0.0f;
            for (float sample : mixed.samples) {
                loudPeak = std::max(loudPeak, std::fabs(sample));
            }
            const float wanted = basePeak * rds::DbToLinear(candidatesDb[c]);
            lossDbOut[c].push_back(20.0f *
                                   std::log10(std::max(loudPeak, 1.0e-9f) / wanted));
        }
    }
}

/// The percentile of an already-sorted run of samples.
[[nodiscard]] float Percentile(const std::vector<float>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0f;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[std::min(index, sorted.size() - 1)];
}

void PrintHeadroom(const std::vector<float>& candidatesDb,
                   std::vector<std::vector<float>>& lossDb, std::vector<float>& peaks,
                   float clipCeiling) {
    if (peaks.empty()) {
        std::printf("headroom    no composites mixed - nothing to measure\n\n");
        return;
    }
    std::ranges::sort(peaks);
    std::printf("headroom\n");
    std::printf("   %zu composites at the shipping defaults; post-clip peak median %.3f, "
                "p90 %.3f, p99 %.3f, max %.3f against a %.2f ceiling\n",
                peaks.size(), Percentile(peaks, 0.50), Percentile(peaks, 0.90),
                Percentile(peaks, 0.99), peaks.back(), clipCeiling);
    std::printf("   %-9s %-9s %-9s %-9s %-9s %s\n", "make-up", "slider", "worst", "p99", "p90",
                "over 0.5 dB");
    for (std::size_t c = 0; c < candidatesDb.size(); ++c) {
        std::vector<float>& loss = lossDb[c];
        if (loss.empty()) {
            continue;
        }
        std::ranges::sort(loss);
        const std::size_t bad =
            static_cast<std::size_t>(std::ranges::count_if(loss, [](float v) { return v < -0.5f; }));
        std::printf("   %+-8.2f %-9.3f %-9.2f %-9.2f %-9.2f %zu (%.1f%%)\n", candidatesDb[c],
                    1.0f / rds::DbToLinear(candidatesDb[c]), loss.front(),
                    Percentile(loss, 0.01), Percentile(loss, 0.10), bad,
                    100.0 * static_cast<double>(bad) / static_cast<double>(loss.size()));
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    std::filesystem::path recordingsDir{"Research/NewRecordings"};
    std::string bankDir;
    std::vector<std::string> overrides;
    bool verbose = false;
    bool headroom = false;
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
        } else if (arg == "--headroom") {
            headroom = true;
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
    failures += CheckSfxConditions(bankDir);
    failures += CheckSfxDuplicatePlacement(bankDir);
    failures += CheckSfxCorrections(bankDir);
    failures += CheckPcmAndMix(bank);

    rds::ConfigSet config{};  // shipping defaults in all three columns, which are the point
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

    // The make-up gains a shipped slider default would need. 0.6 and 0.5 are the
    // two positions worth asking about; the rest bracket them.
    const std::vector<float> candidatesDb{2.0f, 3.0f, 4.44f, 5.0f, 6.02f, 8.0f};
    std::vector<std::vector<float>> headroomLoss(candidatesDb.size());
    std::vector<float> headroomPeaks;
    rds::PcmCache headroomCache;
    rds::MixParams headroomParams;
    headroomParams.sampleRate = 48000;
    if (headroom) {
        headroomCache.SetBank(&bank, headroomParams.sampleRate);
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

        if (headroom) {
            // Its own run rather than Verify's, because Verify does not hand the
            // cue list back and the measurement is about the samples.
            const rds::OfflineResult result = rds::RunOffline(recording, config, bank, options);
            CollectHeadroom(result.cues, headroomCache, headroomParams, candidatesDb,
                            headroomLoss, headroomPeaks);
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
                    "manifold %u self %u\n",
                    s.eventsIn, s.contactsIn, s.rejectedBelowFloor, s.rejectedBlowup,
                    s.droppedMirror, s.collapsedManifold, s.droppedSelfContact);
        std::printf("   proposed %u | dropped rate %u chain %u mask %u burst %u | "
                    "emitted %u in %u bursts\n",
                    s.proposedCues, s.droppedRateCap, s.droppedChainMerge, s.droppedMasking,
                    s.droppedBurstCap, s.emittedCues, s.bursts);
        // The moment axis, on its own line. A take with four heroes has its
        // floor set too low; a take with none is either a gentle slump, which is
        // legitimate, or a floor set too high.
        std::printf("   heroes %u (+%u re-anchored, %u on head relief) | driven %u\n", s.heroes,
                    s.heroReanchors, s.heroHeadRelief, s.drivenFlights);
        // The slide, on its own line: how many the entry test found at all.
        std::printf("   slides %u\n", s.slides);
        // The garment's raw peaks, and only when there are any - the feature is
        // off by default and a row of zeroes on every take would be noise
        // asserting a measurement nobody took.
        if (s.rustleThrashPeak > 0.0f || s.rustleTumblePeak > 0.0f) {
            std::printf("   rustle thrash mean %.0f peak %.0f u/s2 | tumble mean %.0f peak %.0f "
                        "u/s | %u ticks | violence peak %.2f\n",
                        s.rustleThrashMean, s.rustleThrashPeak, s.rustleTumbleMean,
                        s.rustleTumblePeak, s.rustleTicks, s.violencePeak);
        }
        if (s.violenceTicks > 0) {
            std::printf("   violence thrash mean %.0f peak %.0f u/s2 | %u ticks | violence %.2f\n",
                        s.violenceThrashMean, s.violenceThrashPeak, s.violenceTicks,
                        s.violencePeak);
        }
        for (const auto& check : report.checks) {
            ++checksRun;
            failures += check.passed ? 0 : 1;
            std::printf("   %-4s %-18s %s\n", check.passed ? "ok" : "FAIL", check.name.c_str(),
                        check.detail.c_str());
        }
        std::printf("\n");
    }

    if (headroom) {
        PrintHeadroom(candidatesDb, headroomLoss, headroomPeaks, headroomParams.clipCeiling);
    }

    std::printf("%d checks over %zu recordings, %d failed\n", checksRun, takes.size(), failures);
    return failures == 0 ? 0 : 1;
}
