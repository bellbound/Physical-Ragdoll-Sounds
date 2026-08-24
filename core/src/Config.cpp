#include "rds/Config.h"

namespace rds {

bool* LayerMute(AlgorithmConfig& config, SlotId slot) {
    switch (slot) {
        case SlotId::kImpTransient: return &config.layers.impTransient;
        case SlotId::kImpBody:      return &config.layers.impBody;
        case SlotId::kImpSub:       return &config.layers.impSub;

        // The surface skins are muted from the surface section, not from the
        // layer block - one panel owns the floor. See SurfaceConfig.
        case SlotId::kSurfWood:     return &config.surfaces.wood;
        case SlotId::kSurfStone:    return &config.surfaces.stone;
        case SlotId::kSurfSoft:     return &config.surfaces.soft;

        case SlotId::kLimbTap:      return &config.layers.limbTap;
        case SlotId::kCrunchGran:   return &config.layers.crunchGran;
        case SlotId::kGoreWet:      return &config.layers.goreWet;

        case SlotId::kScrapeLoop:   return &config.layers.scrapeLoop;
        case SlotId::kFoleyCloth:   return &config.layers.foleyCloth;
        case SlotId::kAirWhoosh:    return &config.layers.airWhoosh;

        case SlotId::kHeadImpact:   return &config.layers.headImpact;
        case SlotId::kSettleRest:   return &config.layers.settleRest;

        case SlotId::kGruntImpact:
        case SlotId::kScreamBig:
        case SlotId::kCount:
            break;
    }
    return nullptr;
}

const bool* LayerMute(const AlgorithmConfig& config, SlotId slot) {
    return LayerMute(const_cast<AlgorithmConfig&>(config), slot);
}

}  // namespace rds
