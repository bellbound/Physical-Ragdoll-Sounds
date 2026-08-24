// The ToString overloads that belong to Feed.h and Cue.h.
//
// Kept out of Types.cpp because those two headers pull in <vector> and
// SlotManifest.h, and Types.cpp is the one translation unit that should stay
// cheap enough to include from anywhere.

#include "rds/Config.h"
#include "rds/Cue.h"
#include "rds/Feed.h"

namespace rds {

std::string_view ToString(EventKind k) {
    switch (k) {
        case EventKind::kImpact: return "impact";
        case EventKind::kTouch: return "touch";
        case EventKind::kSeparate: return "separate";
        case EventKind::kState: return "state";
        case EventKind::kLimbSample: return "limb_sample";
        case EventKind::kListener: return "listener";
    }
    return "impact";
}

std::string_view ToString(CueReason r) {
    switch (r) {
        case CueReason::kImpactComposite: return "composite";
        case CueReason::kSurfaceSkin: return "surface";
        case CueReason::kHeadImpact: return "head";
        case CueReason::kCrunch: return "crunch";
        case CueReason::kGore: return "gore";
        case CueReason::kLimbTap: return "tap";
        case CueReason::kScrape: return "scrape";
        case CueReason::kFoleyBed: return "bed";
        case CueReason::kAirborneRise: return "rise";
        case CueReason::kSettleClose: return "settle";
    }
    return "composite";
}

float CompressThresholdDb(const CompressConfig& cfg, CueReason reason) {
    switch (reason) {
        // The surface skin is not a class of moment: it is only ever a layer
        // inside a composite, which is one moment and takes one cut. So it
        // answers with the composite's threshold rather than wanting a line of
        // its own in the ini.
        case CueReason::kImpactComposite:
        case CueReason::kSurfaceSkin:  return cfg.impactDb;
        case CueReason::kLimbTap:      return cfg.tapDb;
        case CueReason::kHeadImpact:   return cfg.headDb;
        case CueReason::kCrunch:       return cfg.crunchDb;
        case CueReason::kGore:         return cfg.goreDb;
        case CueReason::kScrape:       return cfg.scrapeDb;
        case CueReason::kFoleyBed:     return cfg.foleyDb;
        case CueReason::kAirborneRise: return cfg.airborneDb;
        case CueReason::kSettleClose:  return cfg.settleDb;
    }
    return cfg.impactDb;
}

}  // namespace rds
