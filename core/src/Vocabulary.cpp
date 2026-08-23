// The ToString overloads that belong to Feed.h and Cue.h.
//
// Kept out of Types.cpp because those two headers pull in <vector> and
// SlotManifest.h, and Types.cpp is the one translation unit that should stay
// cheap enough to include from anywhere.

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

}  // namespace rds
