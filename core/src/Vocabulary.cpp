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
        case EventKind::kVanillaSound: return "vanilla_sound";
    }
    return "impact";
}

std::string_view ToString(CueReason r) {
    switch (r) {
        case CueReason::kImpactComposite: return "composite";
        case CueReason::kSurfaceSkin: return "surface";
        case CueReason::kArmorSkin: return "armor";
        case CueReason::kHeadImpact: return "head";
        case CueReason::kCrunch: return "crunch";
        case CueReason::kGore: return "gore";
        case CueReason::kLimbTap: return "tap";
        case CueReason::kScrape: return "scrape";
        case CueReason::kRustle: return "rustle";
    }
    return "composite";
}

bool IsDamageLayer(CueReason r) {
    return r == CueReason::kCrunch || r == CueReason::kGore;
}

std::string_view ToString(CompressBand band) {
    // The ini key rather than a word for it: every use of this is a tooltip
    // saying which slider held a cue down, and "fBodyDb" is the thing you then
    // go and find. A prose name would need translating back.
    switch (band) {
        case CompressBand::kTransient: return "fTransientDb";
        case CompressBand::kBody:      return "fBodyDb";
        case CompressBand::kBass:      return "fBassDb";
        case CompressBand::kBodyLimb:  return "fBodyLimbDb";
        case CompressBand::kImpact:    return "fImpactDb";
        case CompressBand::kTap:       return "fTapDb";
        case CompressBand::kHead:      return "fHeadDb";
        case CompressBand::kCrunch:    return "fCrunchDb";
        case CompressBand::kGore:      return "fGoreDb";
        case CompressBand::kScrape:    return "fScrapeDb";
    }
    return "fImpactDb";
}

CompressBand CompressBandFor(SlotId slot, CueReason reason) {
    // The slot first, and only for the four impact layers. All four carry
    // `kImpactComposite`, so asking the reason first would collapse the split
    // this function exists for before it ever got to look.
    switch (slot) {
        case SlotId::kImpTransient: return CompressBand::kTransient;
        case SlotId::kImpBody:      return CompressBand::kBody;
        case SlotId::kImpSub:       return CompressBand::kBass;
        case SlotId::kImpBodyLimb:  return CompressBand::kBodyLimb;
        default:                    break;
    }
    switch (reason) {
        // Neither skin is a class of moment: each is only ever a layer inside a
        // composite, riding the body about twelve decibels under it. So they
        // answer with the composite's catch-all rather than wanting a line of
        // their own in the ini.
        case CueReason::kImpactComposite:
        case CueReason::kSurfaceSkin:
        case CueReason::kArmorSkin:    return CompressBand::kImpact;
        case CueReason::kLimbTap:      return CompressBand::kTap;
        case CueReason::kHeadImpact:   return CompressBand::kHead;
        case CueReason::kCrunch:       return CompressBand::kCrunch;
        case CueReason::kGore:         return CompressBand::kGore;
        // The rustle answers to the scrape's threshold rather than owning a
        // line of its own. Both are continuous beds whose level is already a
        // ramp on a measured quantity, so what the compressor has to decide
        // about them is the same decision, and a second key would be a second
        // place for it to be made differently.
        case CueReason::kScrape:
        case CueReason::kRustle:       return CompressBand::kScrape;
    }
    return CompressBand::kImpact;
}

float CompressThresholdDb(const CompressConfig& cfg, CompressBand band) {
    switch (band) {
        case CompressBand::kTransient: return cfg.transientDb;
        case CompressBand::kBody:      return cfg.bodyDb;
        case CompressBand::kBass:      return cfg.bassDb;
        case CompressBand::kBodyLimb:  return cfg.bodyLimbDb;
        case CompressBand::kImpact:    return cfg.impactDb;
        case CompressBand::kTap:       return cfg.tapDb;
        case CompressBand::kHead:      return cfg.headDb;
        case CompressBand::kCrunch:    return cfg.crunchDb;
        case CompressBand::kGore:      return cfg.goreDb;
        case CompressBand::kScrape:    return cfg.scrapeDb;
    }
    return cfg.impactDb;
}

}  // namespace rds
