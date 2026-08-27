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
// because they are the brief a recording is made and checked against -
// change a length here and the stand-in changes with it.
constexpr SlotDesc kSlots[] = {
    {SlotId::kImpTransient, "imp_transient", SlotFamily::kImpact, 3, 60.0f, 120.0f, false,
     "Bright, fast attack. The contact itself. The quietest layer of the stack"},
    {SlotId::kImpBody, "imp_body", SlotFamily::kImpact, 3, 150.0f, 250.0f, false,
     "Low-mid flesh and mass. The main body of the sound, and the torso's own"},
    {SlotId::kImpSub, "imp_sub", SlotFamily::kImpact, 2, 250.0f, 400.0f, false,
     "Pitched boom sweeping ~150 Hz to 30 Hz. The loudest layer and the whole of the gnarl"},
    {SlotId::kSurfWood, "surf_wood", SlotFamily::kSurface, 2, 120.0f, 200.0f, false,
     "Hollow knock"},
    {SlotId::kSurfStone, "surf_stone", SlotFamily::kSurface, 2, 100.0f, 160.0f, false,
     "Hard, short"},
    {SlotId::kSurfSoft, "surf_soft", SlotFamily::kSurface, 2, 150.0f, 250.0f, false,
     "Dull. The default for anything unresolved"},

    // The ten classes with nothing recorded yet. `expectedVariants = 0` is what
    // keeps them silent-by-inheritance rather than silent-by-accident: Resolve
    // returns false, PlaysAs walks the `fallback` column, and the composite gets
    // the parent's skin. Deliberately no mute or trim shared with the parent -
    // each class owns its own, because owning them is the point of the class.
    {SlotId::kSurfMetal, "surf_metal", SlotFamily::kSurface, 0, 100.0f, 200.0f, false,
     "A short clang with no pitched ring behind it. Falls back to stone, which is the hard "
     "half of metal without the ring", SlotId::kSurfStone},
    {SlotId::kSurfWater, "surf_water", SlotFamily::kSurface, 0, 150.0f, 350.0f, false,
     "A body arriving in water: the displacement, not the splash grain", SlotId::kSurfSoft},
    {SlotId::kSurfBody, "surf_body", SlotFamily::kSurface, 0, 120.0f, 250.0f, false,
     "Flesh on flesh. The most common contact in the whole capture set and the one with "
     "no colour of its own until now", SlotId::kSurfSoft},
    {SlotId::kSurfDirt, "surf_dirt", SlotFamily::kSurface, 3, 150.0f, 250.0f, false,
     "Packed earth: grain, not weight. Brighter and far deader than soft - a reference dirt "
     "contact measures -9 dB tilt at a 5 kHz centroid and is 20 dB down in 26 ms, which is "
     "nearer stone than the cushion it falls back to. The dull-and-no-grain brief this line "
     "used to carry had it backwards", SlotId::kSurfSoft},
    {SlotId::kSurfGravel, "surf_gravel", SlotFamily::kSurface, 0, 150.0f, 300.0f, false,
     "Loose stones scattering. Soft underneath with a rattle riding on it", SlotId::kSurfSoft},
    {SlotId::kSurfSnow, "surf_snow", SlotFamily::kSurface, 0, 150.0f, 300.0f, false,
     "A compressing squeak with the top end rolled off. Absorbent", SlotId::kSurfSoft},
    {SlotId::kSurfIce, "surf_ice", SlotFamily::kSurface, 0, 100.0f, 200.0f, false,
     "Hard and bright with a hairline crack in it. Stone, but colder", SlotId::kSurfStone},
    {SlotId::kSurfGlass, "surf_glass", SlotFamily::kSurface, 0, 80.0f, 200.0f, false,
     "Brittle. Barely there at a brush and a shatter at speed - the widest intensity ramp "
     "of any surface", SlotId::kSurfStone},
    {SlotId::kSurfWaterPuddle, "surf_water_puddle", SlotFamily::kSurface, 0, 100.0f, 250.0f, false,
     "A wet slap with something solid under it. Shorter and brighter than open water",
     SlotId::kSurfWater},
    {SlotId::kSurfBone, "surf_bone", SlotFamily::kSurface, 0, 120.0f, 250.0f, false,
     "A dry rattle over the flesh underneath. Draugr and skeletons", SlotId::kSurfBody},

    // The armour skins. Every one ships with `expectedVariants = 0`, which is what
    // makes the feature additive: Resolve returns false for a slot with no files
    // and none expected, and the layer is skipped silently.
    //
    // Deliberately no `fallback` between them: a missing plate rattle is not
    // improved by playing the leather one.
    {SlotId::kArmorBare, "armor_bare", SlotFamily::kArmor, 0, 80.0f, 200.0f, false,
     "A flat skin slap. Wet-ish, no snap. Nothing equipped"},
    {SlotId::kArmorCloth, "armor_cloth", SlotFamily::kArmor, 0, 100.0f, 250.0f, false,
     "A soft cloth thump. Deliberately close to nothing - this is the default case"},
    {SlotId::kArmorLight, "armor_light", SlotFamily::kArmor, 0, 100.0f, 250.0f, false,
     "Leather creak with a small buckle jingle riding on it"},
    {SlotId::kArmorHeavy, "armor_heavy", SlotFamily::kArmor, 0, 120.0f, 300.0f, false,
     "Plate rattle. Metallic, short, no pitched ring - the clank around the impact, not a bell"},

    {SlotId::kLimbTap, "limb_tap", SlotFamily::kGrain, 4, 40.0f, 100.0f, false,
     "Burst filler. Quiet, dry, heavily pitch-scattered"},
    {SlotId::kCrunchGran, "crunch_gran", SlotFamily::kGrain, 2, 250.0f, 400.0f, false,
     "Dense granular crackle in the low-mid. Density, not a snap. The skull's, and "
     "what the other two fall back to"},
    // One mute between the three crunches - silencing damage is one decision -
    // but a trim each, because three separate recordings arrive at three
    // different levels. That is why `mutesWith` and `trimsWith` are two columns.
    {SlotId::kSpineCrunch, "spine_crunch", SlotFamily::kGrain, 2, 250.0f, 400.0f, false,
     "The column going: lower, wetter and longer than the skull's, with a wrench in it "
     "rather than a shatter", SlotId::kCrunchGran, SlotId::kCrunchGran},
    {SlotId::kLimbCrunch, "limb_crunch", SlotFamily::kGrain, 2, 200.0f, 350.0f, false,
     "One bone out on a limb: drier, tighter and higher than the skull's. A snap with "
     "grain behind it, not a crush", SlotId::kCrunchGran, SlotId::kCrunchGran},
    {SlotId::kGoreWet, "gore_wet", SlotFamily::kGrain, 2, 200.0f, 400.0f, false,
     "Squelch. The top tier, and the one layer all three parts share"},
    {SlotId::kScrapeGrain, "scrape_grain", SlotFamily::kGrain, 3, 150.0f, 500.0f, false,
     "One catch. Dry, short, a limb snagging mid-slide. The irregularity, not the rumble"},
    {SlotId::kScrapeLoop, "scrape_loop", SlotFamily::kLoop, 1, 1500.0f, 3000.0f, true,
     "Low-tilted grinding rumble with grain riding on it. NOT a hiss"},
    // A surface-coloured grind is the same layer on a different floor, so it
    // answers to the mute *and* the trim of the loop it is a variant of. A mute
    // per surface would be three ways to silence one thing.
    {SlotId::kScrapeBodyWood, "scrape_body_wood", SlotFamily::kLoop, 1, 1500.0f, 3000.0f, true,
     "The full-weight grind on boards - hollower, more resonant",
     SlotId::kScrapeLoop, SlotId::kScrapeLoop, SlotId::kScrapeLoop},
    {SlotId::kScrapeBodyStone, "scrape_body_stone", SlotFamily::kLoop, 1, 1500.0f, 3000.0f, true,
     "The full-weight grind on flagstone - harder, grittier",
     SlotId::kScrapeLoop, SlotId::kScrapeLoop, SlotId::kScrapeLoop},
    {SlotId::kScrapeLimb, "scrape_limb", SlotFamily::kLoop, 1, 1500.0f, 3000.0f, true,
     "Light, dry, small contact patch. One dragging foot, well under the body grind"},
    {SlotId::kScrapeLimbWood, "scrape_limb_wood", SlotFamily::kLoop, 1, 1500.0f, 3000.0f, true,
     "One limb dragging on boards",
     SlotId::kScrapeLimb, SlotId::kScrapeLimb, SlotId::kScrapeLimb},
    {SlotId::kScrapeLimbStone, "scrape_limb_stone", SlotFamily::kLoop, 1, 1500.0f, 3000.0f, true,
     "One limb dragging on flagstone",
     SlotId::kScrapeLimb, SlotId::kScrapeLimb, SlotId::kScrapeLimb},
    {SlotId::kAirWhoosh, "air_whoosh", SlotFamily::kLoop, 1, 1000.0f, 2000.0f, true,
     "Low airy movement"},
    {SlotId::kHeadImpact, "head_impact", SlotFamily::kAccent, 2, 300.0f, 500.0f, false,
     "Dull skull thud with a granular edge and a slight ring"},
    {SlotId::kSettleRest, "settle_rest", SlotFamily::kAccent, 2, 200.0f, 400.0f, false,
     "Soft final flop. Closes the event"},
    {SlotId::kGruntImpact, "grunt_impact", SlotFamily::kVoice, 0, 300.0f, 600.0f, false,
     "Declared and unfilled. Adding voice later is a config change, not a code change"},
    {SlotId::kScreamBig, "scream_big", SlotFamily::kVoice, 0, 800.0f, 1500.0f, false,
     "Declared and unfilled"},

    // Belongs beside `imp_body`, and sits here because this table is indexed by
    // `SlotId` and a slot's number is an input to the variant hash. New rows go at
    // the end.
    //
    // The impact family was the one place the mod made no distinction it makes
    // everywhere else: the loops have `scrape_loop` against `scrape_limb` and the
    // crunches three tunings, but a faceplant and a forearm came from the same wav.
    //
    // Falls back to `imp_body` and shares its mute, so an install with nothing
    // recorded sounds like one without the feature. Its own trim, for the reason
    // the three crunches have one.
    {SlotId::kImpBodyLimb, "imp_body_limb", SlotFamily::kImpact, 3, 120.0f, 200.0f, false,
     "imp_body out on an arm or a leg: drier, tighter and higher than the torso's, with "
     "less weight under it",
     SlotId::kImpBody, SlotId::kImpBody},

    // The garment. One slot for all four armour classes, since a slot can carry
    // conditional variants, so `heavy` and `cloth` are two placements rather than
    // two slots. No fallback and `expectedVariants = 0`: with nothing recorded it
    // resolves to nothing and the loop never starts.
    {SlotId::kClothRustle, "cloth_rustle", SlotFamily::kLoop, 0, 1500.0f, 3000.0f, true,
     "Fabric and armour shifting under a falling body. Flat and seamless - the engine owns "
     "the envelope, so a designed swish with an arc is unusable"},

    // The mass under a slide, as its own layer. No fallback on purpose: a bed
    // that fell back to `scrape_loop` would play the grind twice, so an install
    // with nothing recorded resolves to nothing and the bed voice never opens.
    // Its own mute and its own trim, because it is its own recording at its own
    // level - and no surface variants, because boards and flagstone change the
    // grit and mass sounds the same under any floor.
    {SlotId::kScrapeLoopRumble, "scrape_loop_rumble", SlotFamily::kLoop, 4, 1500.0f, 4000.0f, true,
     "The mass under a slide: the floor loaded by a body crossing it, with none of the grit "
     "that rides on top. Featureless and seamless - the grind supplies the character"},
};

static_assert(std::size(kSlots) == static_cast<std::size_t>(SlotId::kCount),
              "every SlotId needs a row, or Slot() indexes off the end of the table");

[[nodiscard]] constexpr std::size_t Index(SlotId id) { return static_cast<std::size_t>(id); }

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

std::string_view ToString(SlotFamily family) {
    switch (family) {
        case SlotFamily::kImpact:  return "impact";
        case SlotFamily::kSurface: return "surface";
        case SlotFamily::kArmor:   return "armor";
        case SlotFamily::kGrain:   return "grain";
        case SlotFamily::kLoop:    return "loop";
        case SlotFamily::kAccent:  return "accent";
        case SlotFamily::kVoice:   return "voice";
    }
    return "impact";
}

namespace {

/// Walk a `mutesWith` / `trimsWith` chain to the slot that actually owns the
/// control. Bounded by the slot count so a table edit that accidentally makes a
/// cycle costs a wrong answer rather than a hang - and the loop cannot be
/// entered at all by the shipping table, where no chain is longer than one step.
[[nodiscard]] SlotId Owner(SlotId id, SlotId SlotDesc::*field) {
    for (std::size_t guard = 0; guard < std::size(kSlots); ++guard) {
        const SlotId next = Slot(id).*field;
        if (next == SlotId::kCount || next == id) {
            return id;
        }
        id = next;
    }
    return id;
}

}  // namespace

SlotId MuteOwner(SlotId id) { return Owner(id, &SlotDesc::mutesWith); }
SlotId TrimOwner(SlotId id) { return Owner(id, &SlotDesc::trimsWith); }

SlotId ScrapeSurfaceSlot(SlotId base, SurfaceClass surface) {
    // Only the two loops have surface variants declared, and only for the two
    // surfaces that are recognisably not the default.
    const bool limb = base == SlotId::kScrapeLimb;
    if (base != SlotId::kScrapeLoop && !limb) {
        return base;
    }
    // Thirteen classes and two recorded grinds, so the class walks its parent
    // chain until it reaches one that has a grind or runs out. Ice and glass
    // arrive at stone, snow and gravel arrive at soft and take the base, and a
    // class recorded later needs nothing added here - it just stops walking
    // sooner. kSoft *is* the base, so reaching it means "no variant", which is
    // what the walk running out already means.
    for (SurfaceClass c = surface; c != SurfaceClass::kCount; c = SurfaceParent(c)) {
        if (c == SurfaceClass::kWood) {
            return limb ? SlotId::kScrapeLimbWood : SlotId::kScrapeBodyWood;
        }
        if (c == SurfaceClass::kStone) {
            return limb ? SlotId::kScrapeLimbStone : SlotId::kScrapeBodyStone;
        }
    }
    return base;
}

SlotId SoundBank::PlaysAs(SlotId slot) const {
    // Bounded by walking to a slot that is its own end of the line, so a
    // mis-declared cycle in the manifest costs a few hops rather than the stack.
    for (std::size_t hop = 0; hop < std::size(kSlots); ++hop) {
        const SlotDesc& desc = Slot(slot);
        if (!m_slots[Index(slot)].variants.empty() || desc.fallback == SlotId::kCount ||
            desc.fallback == slot) {
            break;
        }
        slot = desc.fallback;
    }
    return slot;
}

bool SoundBank::HasSound(SlotId slot) const {
    // Files, not intentions. This used to answer yes for any slot the manifest
    // said would one day have variants, because a stand-in covered it until it
    // did; with nothing synthesised, "will be recorded" and "can be heard" are
    // different questions and a strategy is asking the second one.
    return !m_slots[Index(PlaysAs(slot))].variants.empty();
}

SlotId BodySlot(LimbSite site) {
    // Binned through `DamageSiteFor` rather than a second switch: it already
    // answers this question, with the neck counted as spine and an unrecognised
    // skeleton as a limb.
    //
    // The head goes to the torso's layer, not the limb's: a skull has the mass to
    // sound like one, and what makes a faceplant a faceplant is `head_impact`.
    return DamageSiteFor(site) == DamageSite::kLimb ? SlotId::kImpBodyLimb : SlotId::kImpBody;
}

SlotId ArmorSlot(Coverage coverage) {
    switch (coverage) {
        case Coverage::kBare:  return SlotId::kArmorBare;
        case Coverage::kCloth: return SlotId::kArmorCloth;
        case Coverage::kLight: return SlotId::kArmorLight;
        case Coverage::kHeavy: return SlotId::kArmorHeavy;
    }
    return SlotId::kArmorCloth;
}

SlotId SurfaceSlot(SurfaceClass surface) {
    // One slot per class, always - the collapsing that used to happen here
    // (metal playing the stone skin, water playing the soft one) is now the
    // manifest's `fallback` column, so it happens in one place and the config
    // inheritance can follow the same chain. See `SurfaceParent`.
    switch (surface) {
        case SurfaceClass::kSoft:        return SlotId::kSurfSoft;
        case SurfaceClass::kWood:        return SlotId::kSurfWood;
        case SurfaceClass::kStone:       return SlotId::kSurfStone;
        case SurfaceClass::kMetal:       return SlotId::kSurfMetal;
        case SurfaceClass::kWater:       return SlotId::kSurfWater;
        case SurfaceClass::kBody:        return SlotId::kSurfBody;
        case SurfaceClass::kDirt:        return SlotId::kSurfDirt;
        case SurfaceClass::kGravel:      return SlotId::kSurfGravel;
        case SurfaceClass::kSnow:        return SlotId::kSurfSnow;
        case SurfaceClass::kIce:         return SlotId::kSurfIce;
        case SurfaceClass::kGlass:       return SlotId::kSurfGlass;
        case SurfaceClass::kWaterPuddle: return SlotId::kSurfWaterPuddle;
        case SurfaceClass::kBone:        return SlotId::kSurfBone;
        case SurfaceClass::kCount:       break;
    }
    return SlotId::kSurfSoft;
}

SurfaceClass SurfaceOfSlot(SlotId slot) {
    // The inverse of SurfaceSlot, for the two places that hold a slot and need
    // the class's config block: the mute lookup and the trim lookup.
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(SurfaceClass::kCount); ++i) {
        const auto c = static_cast<SurfaceClass>(i);
        if (SurfaceSlot(c) == slot) {
            return c;
        }
    }
    return SurfaceClass::kCount;
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

        // stone. CeramicMedium stays here rather than joining glass: it is
        // brittle, but it is thick-walled and it thuds where glass rings.
        case 3741512247u:  // Stone
        case 1570821952u:  // StoneHeavy
        case 131151687u:   // StoneBroken
        case 899511101u:   // StoneStairs
        case 2892392795u:  // StoneStairsBroken
        case 1886078335u:  // StoneAsStairs
        case 1550912982u:  // BoulderSmall
        case 4283869410u:  // BoulderMedium
        case 1885326971u:  // BoulderLarge
        case 781661019u:   // CeramicMedium
            return SurfaceClass::kStone;

        // glass, out of stone. Both stairs variants come with it.
        case 3739830338u:  // Glass
        case 880200008u:   // GlassStairs
            return SurfaceClass::kGlass;

        // ice, out of stone. IceForm is the conjured kind, which is the same
        // surface with a shorter life.
        case 873356572u:   // Ice
        case 2431524493u:  // IceForm
            return SurfaceClass::kIce;

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
            return SurfaceClass::kWater;

        // a puddle is its own class: a slap with something solid under it,
        // which is a different sound from arriving in a body of water.
        case 3764646153u:  // WaterPuddle
            return SurfaceClass::kWaterPuddle;

        // bodies - another actor, or one of our own limbs
        case 591247106u:   // Skin
        case 2632367422u:  // SkinSmall
        case 2965929619u:  // SkinLarge
        case 2974920155u:  // Organic
        case 1322093133u:  // OrganicLarge
        case 668408902u:   // Insect
        case 220124585u:   // Meat
        case 2518321175u:  // Dragon
        case 1730220269u:  // Alduin
        case 617099282u:   // DLC1DeerSkin
        case 2290050264u:  // DLC1SabreCatPelt
            return SurfaceClass::kBody;

        // bone, out of body. BoneActor is a living creature's skeleton and
        // vanilla routes it to PHYBodyMedium with the flesh; it is here because
        // what we are colouring is the surface that was struck, and that is
        // bone whether or not there is something alive behind it.
        case 2821299363u:  // SkinSkeleton
        case 3049421844u:  // Bone
        case 2058949504u:  // BoneActor
        case 1028101969u:  // DraugrSkeleton
        case 1574477864u:  // DragonSkeleton
            return SurfaceClass::kBone;

        // dirt and mud together, which is the split vanilla's own footstep data
        // makes: `DefaultFootstepWalkLImpactset` maps both onto one impact.
        case 3106094762u:  // Dirt
        case 1486385281u:  // Mud
            return SurfaceClass::kDirt;

        // gravel alone. Sand stays soft - it has none of the rattle that makes
        // gravel worth separating.
        case 428587608u:   // Gravel
            return SurfaceClass::kGravel;

        case 398949039u:   // Snow
        case 1560365355u:  // SnowStairs
            return SurfaceClass::kSnow;

        // named soft, so the mapping is deliberate rather than a fall-through
        case 1286705471u:  // Carpet
        case 3839073443u:  // Cloth
        case 2168343821u:  // Sand
        case 534864873u:   // Ash
        case 1848600814u:  // Grass
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

        // "imp_sub: 0/2 files, nothing to play" is exactly the line that explains
        // why somebody's mod sounds thin, which is why it is at info and not
        // debug. It is the *only* explanation now: an unrecorded slot is a layer
        // that does not sound, and nothing in the mix hints at what is absent.
        if (!slot.variants.empty()) {
            spdlog::info("bank: {}: {}/{} files{}", desc.name, slot.variants.size(),
                         desc.expectedVariants, slot.looping ? ", looping" : "");
        } else if (desc.expectedVariants == 0) {
            spdlog::info("bank: {}: declared and unfilled, skipped silently", desc.name);
        } else if (const SlotId plays = PlaysAs(desc.id);
                   plays != desc.id && !m_slots[Index(plays)].variants.empty()) {
            // Not silent: the surface-coloured scrapes have no recording of
            // their own and play the grind they are a variant of, which is the
            // whole of what makes them a file drop. Reporting them the same way
            // as a slot with nothing behind it would send somebody recording a
            // sound they can already hear.
            spdlog::info("bank: {}: 0/{} files, plays {}", desc.name, desc.expectedVariants,
                         ToString(plays));
        } else {
            spdlog::warn("bank: {}: 0/{} files, nothing to play - record one or give it a "
                         "fallback",
                         desc.name, desc.expectedVariants);
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
        slot.conditions.clear();
        slot.conditionCount = 0;
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
        slot.conditions.clear();
        slot.conditionCount = 0;
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

        for (std::size_t placement = 0; placement < want.files.size(); ++placement) {
            const std::string& file = want.files[placement];
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
            // Parallel to `variants`, and pushed in the same pass so the two
            // cannot drift: a skipped file must not shift every later
            // condition onto the wrong sound. Read by placement, because the
            // same file may be on the slot twice with two different answers -
            // once plain, once tagged - and its name cannot tell them apart.
            const VariantCondition condition = want.ConditionAt(placement);
            slot.conditions.push_back(condition);
            if (!condition.Unconditional()) {
                ++slot.conditionCount;
            }
            ResolvedSound sound{};
            sound.slot = desc.id;
            sound.path = path.string();
            sound.lengthMs = 0.0f;
            if (entry != nullptr) {
                sound.lengthMs = entry->durationMs;
                // The corrections travel with the sound, so the engine never
                // holds the library and Emit never looks anything up.
                sound.pitch = entry->pitch;
                sound.trimDb = entry->trimDb;
                if (entry->Corrected()) {
                    spdlog::info("bank: {} plays '{}' at {:.3f}x, {:+.1f} dB", desc.name, file,
                                 entry->pitch, entry->trimDb);
                }
            }
            if (sound.lengthMs <= 0.0f) {
                sound.lengthMs = WavLengthMs(sound.path);
            }
            if (sound.lengthMs <= 0.0f) {
                sound.lengthMs = desc.maxLengthMs;
                spdlog::warn("bank: {} is not a wav we can measure; assuming {:.0f} ms", sound.path,
                             sound.lengthMs);
            }
            // Last, after every source of a length has had its turn, because it
            // applies to whichever one won. A renderer sizes its mix buffer off
            // this, and pitch here is resampling: a 240 ms impact at 1.09x runs
            // 220, and a buffer sized off the container length would leave 20 ms
            // of tail with nothing to put in it.
            if (sound.pitch > 0.0f) {
                sound.lengthMs /= sound.pitch;
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
    // slot nobody has made a decision about yet, and the shipped pack is the
    // only answer left for it.
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
    // Nothing recorded for this one: play what it says to play instead. Before the
    // pin, the count and either picker, because a slot with no files has nothing to
    // pin, count or shuffle - and because the alternative to falling back is
    // silence.
    //
    // `PlaysAs` is that walk and the only copy of it: a picker falling back by one
    // rule while a label reported another is how "it says it has nothing to play
    // but I can hear the default" happens.
    slot = PlaysAs(slot);

    SlotFiles& files = m_slots[Index(slot)];

    // A pin is a decision, so it comes before the count, before the mutes and
    // before either picker: while it is set this slot has exactly one answer,
    // which is the whole of what auditioning one file against a whole take is.
    if (files.forced != kNoVariant && files.forced < files.variants.size()) {
        files.lastVariant = files.forced;
        return Get(slot, files.forced, out);
    }

    // How many things there are to choose between. Nothing along the chain has a
    // recording when this is zero, and the layer is skipped: there is no
    // stand-in to synthesise any more, so silence is the honest answer and the
    // load log is where it is explained.
    const std::size_t count = files.variants.size();
    if (count == 0) {
        return false;
    }

    // Which of them are in play. Nothing is suspended in the shipping mod and
    // usually nothing is here either, so that case builds no list and indexes
    // straight into the variant numbers.
    std::uint8_t allowed[256];
    std::size_t allowedCount = count;
    bool narrowed = false;
    if (files.mutedCount != 0) {
        allowedCount = 0;
        for (std::size_t i = 0; i < count && allowedCount < std::size(allowed); ++i) {
            if (i >= files.muted.size() || files.muted[i] == 0) {
                allowed[allowedCount++] = static_cast<std::uint8_t>(i);
            }
        }
        narrowed = true;
        if (allowedCount == 0) {
            // Every variant suspended. Silence is what the gesture asked for -
            // falling through to this slot's fallback would answer "mute this"
            // by finding another way to play it.
            return false;
        }
    }

    // The specificity ladder. A file tagged `stone / heavy` is not a candidate
    // anywhere else, and where it *is* one it beats the plain files rather than
    // joining them - otherwise a recording made for one combination would be one
    // option in three on every contact.
    //
    // Three steps: drop what mismatches, keep the most specific tier with anything
    // left in it, hand the rest to the picker unchanged. The picker never learns
    // conditions exist, and neither does `Get` - a variant index is still a
    // position in `files`, so a cue's (slot, variant) round-trips to the same file.
    if (m_conditions && files.conditionCount != 0) {
        std::uint8_t tiered[256];
        std::size_t tieredCount = 0;
        int bestTier = -1;
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < allowedCount; ++i) {
                const std::uint8_t v = narrowed ? allowed[i] : static_cast<std::uint8_t>(i);
                VariantCondition cond =
                    v < files.conditions.size() ? files.conditions[v] : VariantCondition{};
                // A half that is switched off is a half with no opinion, which
                // is exactly what `any` already means - so turning one off
                // collapses the ladder on that axis and leaves the other alone.
                if (!m_surfaceConditions) {
                    cond.surface = SurfaceMatch::kAny;
                }
                if (!m_armorConditions) {
                    cond.coverage = CoverageMatch::kAny;
                }
                if (!Matches(cond.surface, surface) || !Matches(cond.coverage, coverage)) {
                    continue;
                }
                const int tier = cond.Specificity();
                if (pass == 0) {
                    bestTier = std::max(bestTier, tier);
                } else if (tier == bestTier && tieredCount < std::size(tiered)) {
                    tiered[tieredCount++] = v;
                }
            }
            if (pass == 0 && bestTier < 0) {
                // Nothing on this slot can satisfy the contact - a slot whose
                // only file is tagged `stone`, on wood. A condition is a
                // preference and never a mute, so the slot plays its full set
                // rather than going quiet. Without this rule, tagging the only
                // file on a slot would silently delete that layer from most of
                // the game, which is the bug nobody would ever find.
                break;
            }
        }
        if (tieredCount != 0) {
            std::copy(tiered, tiered + tieredCount, allowed);
            allowedCount = tieredCount;
            narrowed = true;
        }
    }

    const auto variantAt = [&](std::size_t index) {
        return narrowed ? allowed[index] : static_cast<std::uint8_t>(index);
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

    // Surface and coverage have already narrowed which variants are candidates.
    // Size has not: keying a condition on the limb site was considered and dropped.
    //
    // Whatever reads it later may only pick between *files*: `Get` reproduces a
    // resolution from the (slot, variant) a cue carries and nothing else, so
    // anything changing the sound rather than the choice would make a recorded cue
    // unreproducible.
    (void)site;

    return Get(slot, pick, out);
}

bool SoundBank::Get(SlotId slot, std::uint8_t variant, ResolvedSound& out) const {
    // A plain lookup into what was loaded. False covers both the slot nobody has
    // recorded and a variant index past the end of one that has been - in either
    // case there is no file, and with nothing left to synthesise there is
    // nothing to hand back.
    const SlotFiles& files = m_slots[Index(slot)];
    if (variant >= files.variants.size()) {
        return false;
    }
    out = files.variants[variant];
    return true;
}

}  // namespace rds
