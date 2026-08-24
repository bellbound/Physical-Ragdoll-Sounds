#include "rds/SlotManifest.h"

#include "rds/Pcm.h"
#include "rds/Sfx.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace rds {
namespace {

// The manifest. Lengths and characters are the design's asset table verbatim,
// because they are also the brief the procedural stand-ins synthesise against -
// change a length here and the stand-in changes with it.
constexpr SlotDesc kSlots[] = {
    {SlotId::kImpTransient, "imp_transient", "impact", 3, 60.0f, 120.0f, false,
     "Bright, fast attack. The contact itself. The quietest layer of the stack"},
    {SlotId::kImpBody, "imp_body", "impact", 3, 150.0f, 250.0f, false,
     "Low-mid flesh and mass. The main body of the sound"},
    {SlotId::kImpSub, "imp_sub", "impact", 2, 250.0f, 400.0f, false,
     "Pitched boom sweeping ~150 Hz to 30 Hz. The loudest layer and the whole of the gnarl"},
    {SlotId::kSurfWood, "surf_wood", "surface", 2, 120.0f, 200.0f, false, "Hollow knock"},
    {SlotId::kSurfStone, "surf_stone", "surface", 2, 100.0f, 160.0f, false, "Hard, short"},
    {SlotId::kSurfSoft, "surf_soft", "surface", 2, 150.0f, 250.0f, false,
     "Dull. The default for anything unresolved"},
    {SlotId::kLimbTap, "limb_tap", "grain", 4, 40.0f, 100.0f, false,
     "Burst filler. Quiet, dry, heavily pitch-scattered"},
    {SlotId::kCrunchGran, "crunch_gran", "grain", 2, 250.0f, 400.0f, false,
     "Dense granular crackle in the low-mid. Density, not a snap"},
    {SlotId::kGoreWet, "gore_wet", "grain", 2, 200.0f, 400.0f, false,
     "Squelch. Obliterate tier only"},
    {SlotId::kScrapeLoop, "scrape_loop", "loop", 1, 1500.0f, 3000.0f, true,
     "Low-tilted grinding rumble with grain riding on it. NOT a hiss"},
    {SlotId::kFoleyCloth, "foley_cloth", "loop", 1, 1500.0f, 3000.0f, true,
     "Cloth rustle, no transients"},
    {SlotId::kAirWhoosh, "air_whoosh", "loop", 1, 1000.0f, 2000.0f, true, "Low airy movement"},
    {SlotId::kHeadImpact, "head_impact", "accent", 2, 300.0f, 500.0f, false,
     "Dull skull thud with a granular edge and a slight ring"},
    {SlotId::kSettleRest, "settle_rest", "accent", 2, 200.0f, 400.0f, false,
     "Soft final flop. Closes the event"},
    {SlotId::kGruntImpact, "grunt_impact", "voice", 0, 300.0f, 600.0f, false,
     "Declared and unfilled. Adding voice later is a config change, not a code change"},
    {SlotId::kScreamBig, "scream_big", "voice", 0, 800.0f, 1500.0f, false,
     "Declared and unfilled"},
};

static_assert(std::size(kSlots) == static_cast<std::size_t>(SlotId::kCount),
              "every SlotId needs a row, or Slot() indexes off the end of the table");

[[nodiscard]] constexpr std::size_t Index(SlotId id) { return static_cast<std::size_t>(id); }

/// The length of a procedural stand-in, as a pure function of the slot and the
/// variant.
///
/// Deliberately not a function of the surface, coverage or limb: `Get` has to
/// hand a renderer back exactly what `Resolve` chose from nothing but the
/// (slot, variant) a cue carries, and a length that varied with the axes could
/// not be recovered. The axes will pick between *files* once axis-suffixed
/// variants exist; until then they choose nothing.
[[nodiscard]] float StandInLengthMs(const SlotDesc& desc, std::uint8_t variant,
                                    std::size_t count) {
    const float t = count > 1 ? static_cast<float>(variant) / static_cast<float>(count - 1) : 0.5f;
    return desc.minLengthMs + (desc.maxLengthMs - desc.minLengthMs) * t;
}

/// xorshift64*, so the bag is reproducible across compilers and runs. std::mt19937
/// would do, but its state is 2.5 KB per bank and its sequence is not something
/// we want to be able to change by touching a standard library.
[[nodiscard]] std::uint64_t NextRandom(std::uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

}  // namespace

std::span<const SlotDesc> Slots() { return std::span<const SlotDesc>{kSlots}; }

const SlotDesc& Slot(SlotId id) {
    const std::size_t index = std::min(Index(id), std::size(kSlots) - 1);
    return kSlots[index];
}

std::string_view ToString(SlotId id) { return Slot(id).name; }

SlotId SurfaceSlot(SurfaceClass surface) {
    switch (surface) {
        case SurfaceClass::kWood:
            return SlotId::kSurfWood;
        case SurfaceClass::kStone:
            return SlotId::kSurfStone;
        case SurfaceClass::kMetal:
            // No metal skin authored. Stone is hard and short, which is the half
            // of metal that reads at impact; the ring is what is missing and the
            // pitch scatter covers for it better than a dull thud would.
            return SlotId::kSurfStone;
        case SurfaceClass::kWater:
        case SurfaceClass::kBody:
        case SurfaceClass::kSoft:
        case SurfaceClass::kCount:
            break;
    }
    return SlotId::kSurfSoft;
}

SurfaceClass SurfaceFromMaterial(std::uint32_t materialId) {
    // The IDs are 08 §3's, which are CRC32 of the MATT record's MNAM string and
    // match RE::MATERIAL_ID exactly. Only the ones a ragdoll can plausibly meet
    // are listed; everything else - including the ten engine IDs with no MATT
    // record at all, of which Trap turned up in a capture - lands on kSoft,
    // which is the design's stated default rather than a failure.
    switch (materialId) {
        // wood
        case 365420259u:   // WoodLight
        case 500811281u:   // Wood
        case 3070783559u:  // WoodHeavy
        case 1461712277u:  // WoodStairs
        case 1803571212u:  // WoodAsStairs
        case 732141076u:   // Barrel
        case 790784366u:   // Basket
        case 1264672850u:  // Book
            return SurfaceClass::kWood;

        // stone, and the hard brittle things that read the same at impact
        case 3741512247u:  // Stone
        case 1570821952u:  // StoneHeavy
        case 131151687u:   // StoneBroken
        case 899511101u:   // StoneStairs
        case 2892392795u:  // StoneStairsBroken
        case 1886078335u:  // StoneAsStairs
        case 3739830338u:  // Glass
        case 880200008u:   // GlassStairs
        case 873356572u:   // Ice
        case 2431524493u:  // IceForm
        case 1550912982u:  // BoulderSmall
        case 4283869410u:  // BoulderMedium
        case 1885326971u:  // BoulderLarge
        case 781661019u:   // CeramicMedium
            return SurfaceClass::kStone;

        // metal. ArmorHeavy/ArmorLight and the SkinMetal pair are bodies wearing
        // plate: acoustically that is metal, and the Coverage axis describes our
        // own limb rather than what it hit, so it cannot say this.
        case 2229413539u:  // MetalHeavy
        case 1288358971u:  // MetalSolid
        case 346811165u:   // MetalLight
        case 3074114406u:  // Chain
        case 438912228u:   // ChainMetal
        case 3708432437u:  // ArmorHeavy
        case 3424720541u:  // ArmorLight
        case 3387452107u:  // SkinMetalLarge
        case 3855001958u:  // SkinMetalSmall
        case 2742858142u:  // PotsPans
        case 3589100606u:  // Coin
            return SurfaceClass::kMetal;

        case 1024582599u:  // Water
        case 3764646153u:  // WaterPuddle
            return SurfaceClass::kWater;

        // bodies - another actor, or one of our own limbs
        case 591247106u:   // Skin
        case 2632367422u:  // SkinSmall
        case 2965929619u:  // SkinLarge
        case 2821299363u:  // SkinSkeleton
        case 2058949504u:  // BoneActor
        case 3049421844u:  // Bone
        case 2974920155u:  // Organic
        case 1322093133u:  // OrganicLarge
        case 668408902u:   // Insect
        case 220124585u:   // Meat
        case 2518321175u:  // Dragon
        case 1730220269u:  // Alduin
        case 1028101969u:  // DraugrSkeleton
        case 1574477864u:  // DragonSkeleton
        case 617099282u:   // DLC1DeerSkin
        case 2290050264u:  // DLC1SabreCatPelt
            return SurfaceClass::kBody;

        // named soft, so the mapping is deliberate rather than a fall-through
        case 1286705471u:  // Carpet
        case 3839073443u:  // Cloth
        case 3106094762u:  // Dirt
        case 428587608u:   // Gravel
        case 2168343821u:  // Sand
        case 534864873u:   // Ash
        case 1486385281u:  // Mud
        case 1848600814u:  // Grass
        case 398949039u:   // Snow
        case 1560365355u:  // SnowStairs
        case 3934839107u:  // Web
            return SurfaceClass::kSoft;

        default:
            break;
    }
    return SurfaceClass::kSoft;
}

SurfaceClass SurfaceFromLayer(ColLayer layer) {
    switch (layer) {
        case ColLayer::kBiped:
        case ColLayer::kDeadBip:
            return SurfaceClass::kBody;
        case ColLayer::kWater:
            return SurfaceClass::kWater;
        case ColLayer::kTrees:
        case ColLayer::kProps:
            return SurfaceClass::kWood;
        case ColLayer::kStatic:
            // World geometry with no material behind it. In the capture set this
            // never happens - the 4.5 % of contacts with no material are all
            // body-on-body - so this is the least-bad guess rather than a
            // measurement, and stone is the more common world material.
            return SurfaceClass::kStone;
        case ColLayer::kGround:
        case ColLayer::kOther:
            break;
    }
    return SurfaceClass::kSoft;
}

void SoundBank::ScanDirectory(const std::string& directory, bool onlyEmptySlots) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        spdlog::info("bank: no sound directory at '{}'", directory);
        return;
    }

    // Which slots this pass may fill, decided once and up front. Asking
    // "is this slot still empty" per file instead would let the first
    // `imp_body_01.wav` fill the slot and then skip 02 and 03 for being
    // second - three variants collapsing to one, silently, with the shuffle
    // bag then repeating the same file forever.
    bool mayFill[static_cast<std::size_t>(SlotId::kCount)];
    for (const auto& desc : kSlots) {
        // `scannedByName` and not `variants.empty()`: a slot whose named files
        // are all disabled or all missing ends up empty too, and filling that
        // one from the folder would answer "mute this" by playing the pack.
        mayFill[Index(desc.id)] = !onlyEmptySlots || m_slots[Index(desc.id)].scannedByName;
    }

    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            continue;
        }
        const auto& path = entry.path();
        auto extension = path.extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".wav") {
            continue;
        }

        // <slotname>_<NN>.wav. The number is only there to make the files
        // orderable; the variant index is the position in the sorted set, so
        // a gap in the numbering costs nothing.
        const auto stem = path.stem().string();
        const auto underscore = stem.find_last_of('_');
        if (underscore == std::string::npos) {
            continue;
        }
        const std::string_view name{stem.data(), underscore};
        int number = 0;
        const auto* numberBegin = stem.data() + underscore + 1;
        const auto* numberEnd = stem.data() + stem.size();
        if (std::from_chars(numberBegin, numberEnd, number).ec != std::errc{}) {
            continue;
        }

        for (const auto& desc : kSlots) {
            if (desc.name != name) {
                continue;
            }
            // Skipped whole rather than merged: a slot the ini named is a slot
            // somebody made a decision about, and quietly appending the files
            // that happen to be named after it would put back exactly what they
            // removed.
            if (!mayFill[Index(desc.id)]) {
                break;
            }
            ResolvedSound sound{};
            sound.slot = desc.id;
            sound.path = path.string();
            sound.procedural = false;
            sound.lengthMs = WavLengthMs(sound.path);
            if (sound.lengthMs <= 0.0f) {
                sound.lengthMs = desc.maxLengthMs;
                spdlog::warn("bank: {} is not a wav we can measure; assuming {:.0f} ms", sound.path,
                             sound.lengthMs);
            }
            m_slots[Index(desc.id)].variants.push_back(std::move(sound));
            break;
        }
    }

    // Filename order, which is what makes the trailing number mean something.
    // Only the slots this pass touched: LoadAssigned has already put the ini's
    // slots in the order the ini asked for and must not have them re-sorted.
    for (const auto& desc : kSlots) {
        auto& slot = m_slots[Index(desc.id)];
        if (!mayFill[Index(desc.id)]) {
            continue;
        }
        std::ranges::sort(slot.variants, [](const ResolvedSound& a, const ResolvedSound& b) {
            return a.path < b.path;
        });
    }
}

void SoundBank::LogContents(const char* source) {
    for (const auto& desc : kSlots) {
        auto& slot = m_slots[Index(desc.id)];
        for (std::size_t i = 0; i < slot.variants.size(); ++i) {
            slot.variants[i].variant = static_cast<std::uint8_t>(i);
        }

        // "imp_sub: 0/2 files, procedural" is exactly the line that explains why
        // somebody's mod sounds thin, which is why it is at info and not debug.
        if (!slot.variants.empty()) {
            spdlog::info("bank: {}: {}/{} files{}", desc.name, slot.variants.size(),
                         desc.expectedVariants, slot.looping ? ", looping" : "");
        } else if (desc.expectedVariants == 0) {
            spdlog::info("bank: {}: declared and unfilled, skipped silently", desc.name);
        } else {
            spdlog::info("bank: {}: 0/{} files, procedural", desc.name, desc.expectedVariants);
        }
    }
    spdlog::info("bank: {}", source);

    Seed(m_rng == 0 ? 1u : static_cast<std::uint32_t>(m_rng));
}

void SoundBank::Load(const std::string& directory) {
    ClearOverrides();
    for (auto& slot : m_slots) {
        slot.variants.clear();
        slot.bag.clear();
        slot.bagCursor = 0;
        slot.scannedByName = true;
    }
    for (const auto& desc : kSlots) {
        m_slots[Index(desc.id)].looping = desc.isLoop;
    }

    ScanDirectory(directory, false);
    LogContents("filled from <slot>_<NN>.wav filenames");
}

void SoundBank::LoadAssigned(const SfxLibrary& library, const SfxAssignments& assignments,
                             const std::string& fallbackDirectory) {
    // Before the variants go: a force and a mute name a position in this list,
    // and the list is about to be rebuilt out from under them.
    ClearOverrides();
    for (auto& slot : m_slots) {
        slot.variants.clear();
        slot.bag.clear();
        slot.bagCursor = 0;
        slot.scannedByName = false;
    }

    std::size_t assigned = 0;
    std::size_t missing = 0;
    for (const auto& desc : kSlots) {
        auto& slot = m_slots[Index(desc.id)];
        const SlotAssignment& want = assignments.For(desc.id);
        slot.looping = want.looping;

        // Decided from the ini and not from what came out of it: this is what
        // says whether anybody has made a decision about the slot, and a slot
        // whose every named file is disabled has very much had one made.
        slot.scannedByName = want.files.empty();

        for (const std::string& file : want.files) {
            const SfxEntry* entry = library.Find(file);
            if (entry != nullptr && entry->disabled) {
                // Muted in the library. Still named here, so re-enabling it puts
                // it straight back on the slot in the same position - which is
                // the whole difference between disabling a sound and removing it.
                spdlog::info("bank: {} names '{}', which is disabled - skipped", desc.name, file);
                continue;
            }
            const std::filesystem::path path = library.PathOf(file);
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                // Named but absent: say so and carry on. The alternative is a
                // slot that silently plays one fewer variant than the ini says,
                // which is the failure nobody would ever find.
                spdlog::warn("bank: {} names '{}', which is not in the library", desc.name, file);
                ++missing;
                continue;
            }
            ResolvedSound sound{};
            sound.slot = desc.id;
            sound.path = path.string();
            sound.procedural = false;
            sound.lengthMs = 0.0f;
            if (entry != nullptr) {
                sound.lengthMs = entry->durationMs;
            }
            if (sound.lengthMs <= 0.0f) {
                sound.lengthMs = WavLengthMs(sound.path);
            }
            if (sound.lengthMs <= 0.0f) {
                sound.lengthMs = desc.maxLengthMs;
                spdlog::warn("bank: {} is not a wav we can measure; assuming {:.0f} ms", sound.path,
                             sound.lengthMs);
            }
            // Muted files are added and then suspended, not skipped. The skip is
            // what `disabled` in the library does, and it renumbers everything
            // after it; this one has to keep its variant index so unmuting puts
            // a recorded take back exactly as it was.
            const auto variant = static_cast<std::uint8_t>(slot.variants.size());
            slot.variants.push_back(std::move(sound));
            if (want.Muted(file)) {
                MuteVariant(desc.id, variant, true);
                spdlog::info("bank: {} names '{}', which is muted on this slot", desc.name, file);
            }
        }
        if (!slot.variants.empty()) {
            ++assigned;
        }
    }

    // Whatever the ini did not fill, the old way. A slot with no line here is a
    // slot nobody has made a decision about yet, and the shipped pack is a
    // better answer for it than a procedural stand-in.
    ScanDirectory(fallbackDirectory, true);

    // Said before the per-slot lines, because it is the line that explains them:
    // "0 slots assigned" and a full bank means the ini did nothing and the
    // filename convention did all of it.
    spdlog::info("bank: {} of {} slot(s) assigned by ini, {} named file(s) missing", assigned,
                 static_cast<std::size_t>(SlotId::kCount), missing);
    LogContents(assigned == 0 ? "no assignments - filled entirely from filenames"
                              : "filled from RagdollSounds_SFX.ini, falling back to filenames");
}

bool SoundBank::IsLooping(SlotId slot) const { return m_slots[Index(slot)].looping; }


void SoundBank::Seed(std::uint32_t seed) {
    // 0 would leave xorshift stuck at 0 forever, and a seed of 0 is the game's
    // way of saying "from the clock" - which the caller has already resolved by
    // the time it reaches here.
    m_rng = seed == 0 ? 0x9E3779B97F4A7C15ULL : (0x9E3779B97F4A7C15ULL ^ seed);
    m_seed = seed == 0 ? 1u : seed;
    for (auto& slot : m_slots) {
        slot.bag.clear();
        slot.bagCursor = 0;
        slot.lastVariant = 0xFF;
    }
}

std::size_t SoundBank::FileCount(SlotId slot) const {
    return m_slots[Index(slot)].variants.size();
}

void SoundBank::ForceVariant(SlotId slot, std::uint8_t variant) {
    SlotFiles& files = m_slots[Index(slot)];
    files.forced =
        (variant != kNoVariant && variant < files.variants.size()) ? variant : kNoVariant;
}

std::uint8_t SoundBank::ForcedVariant(SlotId slot) const { return m_slots[Index(slot)].forced; }

void SoundBank::MuteVariant(SlotId slot, std::uint8_t variant, bool muted) {
    SlotFiles& files = m_slots[Index(slot)];
    if (variant >= files.variants.size()) {
        return;
    }
    if (files.muted.size() < files.variants.size()) {
        files.muted.resize(files.variants.size(), 0);
    }
    const std::uint8_t want = muted ? 1u : 0u;
    if (files.muted[variant] == want) {
        return;
    }
    files.muted[variant] = want;
    if (muted) {
        ++files.mutedCount;
    } else {
        --files.mutedCount;
    }
    // The bag was dealt out of the old set, so a cursor into it would keep
    // handing out a variant that is no longer in play.
    files.bag.clear();
    files.bagCursor = 0;
}

bool SoundBank::VariantMuted(SlotId slot, std::uint8_t variant) const {
    const SlotFiles& files = m_slots[Index(slot)];
    return variant < files.muted.size() && files.muted[variant] != 0;
}

void SoundBank::ClearOverrides() {
    for (SlotFiles& slot : m_slots) {
        slot.forced = kNoVariant;
        slot.muted.clear();
        slot.mutedCount = 0;
        slot.bag.clear();
        slot.bagCursor = 0;
    }
}

/// Stable per-cue randomness: one avalanche of (seed, token, slot) rather than a
/// position in a running stream. Same contact and same slot means the same
/// variant no matter what happened earlier in the take, which is what makes two
/// exports of the same recording comparable.
[[nodiscard]] std::uint32_t StableHash(std::uint32_t seed, std::uint32_t token,
                                       std::uint32_t salt) {
    std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(seed) << 32) ^
                      (static_cast<std::uint64_t>(token) << 8) ^ salt;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return static_cast<std::uint32_t>(h);
}

bool SoundBank::Resolve(SlotId slot, SurfaceClass surface, Coverage coverage, LimbSite site,
                        ResolvedSound& out, std::uint32_t token) {
    const SlotDesc& desc = Slot(slot);
    SlotFiles& files = m_slots[Index(slot)];

    // A pin is a decision, so it comes before the count, before the mutes and
    // before either picker: while it is set this slot has exactly one answer,
    // which is the whole of what auditioning one file against a whole take is.
    if (files.forced != kNoVariant && files.forced < files.variants.size()) {
        files.lastVariant = files.forced;
        return Get(slot, files.forced, out);
    }

    // How many things there are to choose between: the real files if any exist,
    // otherwise the variants the brief says the slot will eventually have, so
    // the stand-ins vary the same way the files will.
    const std::size_t count =
        files.variants.empty() ? desc.expectedVariants : files.variants.size();
    if (count == 0) {
        // The declared-and-unfilled voice slots. Callers skip them silently -
        // that is what "adding voice later is a config change" means.
        return false;
    }

    // Which of them are in play. Nothing is suspended in the shipping mod and
    // usually nothing is here either, so that case builds no list and indexes
    // straight into the variant numbers.
    std::uint8_t allowed[256];
    std::size_t allowedCount = count;
    if (files.mutedCount != 0) {
        allowedCount = 0;
        for (std::size_t i = 0; i < count && allowedCount < std::size(allowed); ++i) {
            if (i >= files.muted.size() || files.muted[i] == 0) {
                allowed[allowedCount++] = static_cast<std::uint8_t>(i);
            }
        }
        if (allowedCount == 0) {
            // Every variant suspended. Silence is what the gesture asked for -
            // falling through to a stand-in would answer "mute this" by
            // synthesising it.
            return false;
        }
    }
    const auto variantAt = [&](std::size_t index) {
        return files.mutedCount == 0 ? static_cast<std::uint8_t>(index) : allowed[index];
    };

    // Derived from the contact rather than from the bag's position, when the
    // caller offers an identity to derive from. The bag is stable *in sequence*,
    // which is exactly what breaks under an A/B: insert one cue early in a take
    // and every later cue of that slot shifts a place.
    if (m_stableVariants && token != 0) {
        const std::uint32_t h = StableHash(m_seed, token, static_cast<std::uint32_t>(slot));
        const std::size_t index = h % allowedCount;
        auto pick = variantAt(index);
        // Keep the one property the bag was for. Rotating by a second slice of
        // the same hash stays a function of this contact alone, so it does not
        // reintroduce the coupling the bag had.
        if (allowedCount > 1 && pick == files.lastVariant) {
            const auto step = static_cast<std::size_t>(1 + ((h >> 16) % (allowedCount - 1)));
            pick = variantAt((index + step) % allowedCount);
        }
        files.lastVariant = pick;
        (void)surface;
        (void)coverage;
        (void)site;
        return Get(slot, pick, out);
    }

    // Shuffle bag rather than random: random repeats immediately, and immediate
    // repeats are what people notice. Refilled and reshuffled when it empties.
    if (files.bagCursor >= files.bag.size()) {
        files.bag.resize(allowedCount);
        for (std::size_t i = 0; i < allowedCount; ++i) {
            files.bag[i] = variantAt(i);
        }
        for (std::size_t i = allowedCount; i > 1; --i) {
            const auto j = static_cast<std::size_t>(NextRandom(m_rng) % i);
            std::swap(files.bag[i - 1], files.bag[j]);
        }
        files.bagCursor = 0;
    }
    const std::uint8_t pick = files.bag[files.bagCursor++];
    files.lastVariant = pick;

    // The axes are timbre only, never physics (07 §11), and until axis-suffixed
    // files exist there is nothing for them to pick between: the surface already
    // picked the *slot* through SurfaceSlot, and coverage and size will select
    // among variants of a slot once `<name>_<axis>_<NN>.wav` files are authored.
    // They are deliberately not folded into the stand-in's length, because Get
    // has to reproduce a resolution from the (slot, variant) a cue carries and
    // nothing else.
    (void)surface;
    (void)coverage;
    (void)site;

    return Get(slot, pick, out);
}

bool SoundBank::Get(SlotId slot, std::uint8_t variant, ResolvedSound& out) const {
    const SlotDesc& desc = Slot(slot);
    const SlotFiles& files = m_slots[Index(slot)];

    if (!files.variants.empty()) {
        if (variant >= files.variants.size()) {
            return false;
        }
        out = files.variants[variant];
        return true;
    }
    if (desc.expectedVariants == 0 || variant >= desc.expectedVariants) {
        // The declared-and-unfilled voice slots. Callers skip them silently -
        // that is what "adding voice later is a config change" means.
        return false;
    }

    out = ResolvedSound{};
    out.slot = slot;
    out.variant = variant;
    out.procedural = true;
    out.lengthMs = StandInLengthMs(desc, variant, desc.expectedVariants);
    return true;
}

}  // namespace rds
