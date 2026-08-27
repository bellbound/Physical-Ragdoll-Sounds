#include "rds/Config.h"

#include "rds/SlotManifest.h"

namespace rds {

bool* LayerMute(AlgorithmConfig& config, SlotId slot) {
    // A slot that is a variant of another answers to that slot's mute, and the
    // manifest is where that is written down. It used to be written here, and
    // in three other switches, and the four had to be kept in agreement by
    // hand - which is exactly the kind of agreement that quietly stops holding.
    // The surface skins are muted from their own block in the surfaces list, not
    // from the layer block - one panel owns the floor. Handled ahead of the
    // switch because there are thirteen of them and they are a table, not a set
    // of named fields. See SurfaceConfig.
    if (const SurfaceClass surface = SurfaceOfSlot(MuteOwner(slot));
        surface != SurfaceClass::kCount) {
        return &config.surfaces.skins[static_cast<std::size_t>(surface)].enabled;
    }

    switch (MuteOwner(slot)) {
        case SlotId::kImpTransient: return &config.layers.impTransient;
        case SlotId::kImpBody:      return &config.layers.impBody;
        case SlotId::kImpSub:       return &config.layers.impSub;

        case SlotId::kLimbTap:      return &config.layers.limbTap;

        // Silencing one part's damage is `Damage:b<Part>Enabled`, which stops
        // the cue being proposed instead of muting it after the arbitrator made
        // room for it. The spine and limb crunches arrive here as `kCrunchGran`,
        // because that is what the manifest says they mute with.
        case SlotId::kCrunchGran:   return &config.layers.crunchGran;
        case SlotId::kGoreWet:      return &config.layers.goreWet;
        case SlotId::kScrapeGrain:  return &config.layers.scrapeGrain;

        // The surface-coloured scrapes arrive here as the loop they colour. A
        // mute per surface would be three ways to silence one thing, and the
        // surface A/B already exists - it is Surfaces:bEnabled.
        case SlotId::kScrapeLoop:   return &config.layers.scrapeLoop;
        case SlotId::kScrapeLimb:   return &config.layers.scrapeLimb;
        case SlotId::kScrapeLoopRumble: return &config.layers.scrapeLoopRumble;

        // The armour skins are muted from the armour section, not from the layer
        // block - one panel owns what the body was wearing, exactly as one panel
        // owns the floor. See ArmorConfig.
        case SlotId::kArmorBare:    return &config.armor.bare;
        case SlotId::kArmorCloth:   return &config.armor.cloth;
        case SlotId::kArmorLight:   return &config.armor.light;
        case SlotId::kArmorHeavy:   return &config.armor.heavy;

        case SlotId::kHeadImpact:   return &config.layers.headImpact;
        case SlotId::kSettleRest:   return &config.layers.settleRest;

        // The declared-and-unfilled slots have no mute, because nothing ever
        // resolves to them and a control over silence is a lie. `kAirWhoosh`
        // joined them when the airborne rise was removed: the slot, its files and
        // its sfx block are still here, and nothing proposes it.
        case SlotId::kGruntImpact:
        case SlotId::kScreamBig:
        case SlotId::kAirWhoosh:

        // Every surface skin is answered by the table above, before the switch.
        // Listed so this stays exhaustive and a fourteenth class is a compile
        // error here rather than a floor that silently cannot be muted.
        case SlotId::kSurfWood:
        case SlotId::kSurfStone:
        case SlotId::kSurfSoft:
        case SlotId::kSurfMetal:
        case SlotId::kSurfWater:
        case SlotId::kSurfBody:
        case SlotId::kSurfDirt:
        case SlotId::kSurfGravel:
        case SlotId::kSurfSnow:
        case SlotId::kSurfIce:
        case SlotId::kSurfGlass:
        case SlotId::kSurfWaterPuddle:
        case SlotId::kSurfBone:
        // Unreachable: MuteOwner only ever returns a real slot. Listed so the
        // switch stays exhaustive and a new SlotId is a compile error here.
        case SlotId::kSpineCrunch:
        case SlotId::kLimbCrunch:
        case SlotId::kScrapeBodyWood:
        case SlotId::kScrapeBodyStone:
        case SlotId::kScrapeLimbWood:
        case SlotId::kScrapeLimbStone:
        case SlotId::kCount:
            break;
    }
    return nullptr;
}

const bool* LayerMute(const AlgorithmConfig& config, SlotId slot) {
    return LayerMute(const_cast<AlgorithmConfig&>(config), slot);
}

}  // namespace rds
