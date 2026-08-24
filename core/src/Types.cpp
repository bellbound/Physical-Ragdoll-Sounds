#include "rds/Types.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace rds {
namespace {

/// Lowercasing one char at a time rather than building a lowered copy: this runs
/// once per ragdoll attach, not per contact, but it is also called from the
/// recording loader over every row of every take and there is no reason for it
/// to allocate.
[[nodiscard]] constexpr char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool Contains(std::string_view haystack, std::string_view needleLower) {
    if (needleLower.empty() || haystack.size() < needleLower.size()) {
        return false;
    }
    const std::size_t last = haystack.size() - needleLower.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        for (; j < needleLower.size(); ++j) {
            if (Lower(haystack[i + j]) != needleLower[j]) {
                break;
            }
        }
        if (j == needleLower.size()) {
            return true;
        }
    }
    return false;
}

/// Left or right, from the bone name rather than from the limb index.
///
/// Vanilla writes " L " / " R " as a whole word ("NPC L Forearm [LLar]") and
/// repeats it as the first letter of the bracket tag. Matching the spaced word
/// first keeps "Calf" from reading as a right-side anything.
enum class Side { kNone, kLeft, kRight };

[[nodiscard]] Side SideOf(std::string_view bone) {
    if (Contains(bone, " l ")) {
        return Side::kLeft;
    }
    if (Contains(bone, " r ")) {
        return Side::kRight;
    }
    if (Contains(bone, "[l")) {
        return Side::kLeft;
    }
    if (Contains(bone, "[r")) {
        return Side::kRight;
    }
    return Side::kNone;
}

}  // namespace

float Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

float Distance(const Vec3& a, const Vec3& b) {
    const Vec3 d{a.x - b.x, a.y - b.y, a.z - b.z};
    return Length(d);
}

std::string_view ToString(SurfaceClass c) {
    switch (c) {
        case SurfaceClass::kSoft: return "soft";
        case SurfaceClass::kWood: return "wood";
        case SurfaceClass::kStone: return "stone";
        case SurfaceClass::kMetal: return "metal";
        case SurfaceClass::kWater: return "water";
        case SurfaceClass::kBody: return "body";
        case SurfaceClass::kCount: break;
    }
    return "soft";
}

std::string_view ToString(LimbSite s) {
    switch (s) {
        case LimbSite::kUnknown: return "unknown";
        case LimbSite::kHead: return "head";
        case LimbSite::kNeck: return "neck";
        case LimbSite::kTorso: return "torso";
        case LimbSite::kUpperArm: return "upperarm";
        case LimbSite::kForearm: return "forearm";
        case LimbSite::kHand: return "hand";
        case LimbSite::kThigh: return "thigh";
        case LimbSite::kCalf: return "calf";
        case LimbSite::kFoot: return "foot";
        case LimbSite::kCount: break;
    }
    return "unknown";
}

std::string_view ToString(Coverage c) {
    switch (c) {
        case Coverage::kBare: return "bare";
        case Coverage::kCloth: return "cloth";
        case Coverage::kLight: return "light";
        case Coverage::kHeavy: return "heavy";
    }
    return "bare";
}

std::string_view ToString(Motion m) {
    switch (m) {
        case Motion::kLaunch: return "Launch";
        case Motion::kAirborne: return "Airborne";
        case Motion::kTumble: return "Tumble";
        case Motion::kSlide: return "Slide";
        case Motion::kResting: return "Resting";
        case Motion::kCount: break;
    }
    return "Resting";
}

std::string_view ToString(SlideExit e) {
    switch (e) {
        case SlideExit::kNone: return "none";
        case SlideExit::kRested: return "rested";
        case SlideExit::kLaunched: return "launched";
        case SlideExit::kStruck: return "struck";
    }
    return "none";
}

std::string_view ToString(Moment m) {
    switch (m) {
        case Moment::kOrdinary: return "Ordinary";
        case Moment::kHero: return "Hero";
        case Moment::kCount: break;
    }
    return "Ordinary";
}

std::string_view ToString(Motion m, Moment moment) {
    if (moment != Moment::kHero) {
        return ToString(m);
    }
    switch (m) {
        case Motion::kLaunch: return "Launch+Hero";
        case Motion::kAirborne: return "Airborne+Hero";
        case Motion::kTumble: return "Tumble+Hero";
        case Motion::kSlide: return "Slide+Hero";
        case Motion::kResting: return "Resting+Hero";
        case Motion::kCount: break;
    }
    return "Resting+Hero";
}

std::string_view ToString(DistanceTier t) {
    switch (t) {
        case DistanceTier::kFull: return "full";
        case DistanceTier::kSimplified: return "simplified";
        case DistanceTier::kCulled: return "culled";
    }
    return "full";
}

LimbSite SiteFromBoneName(std::string_view boneName) {
    // Order is not arbitrary: "UpperArm" and "Forearm" both contain "arm", and
    // the ragdoll bone for the pelvis is called "COM", which is a substring of
    // nothing else here. Longest and most specific first.
    if (Contains(boneName, "head")) return LimbSite::kHead;
    if (Contains(boneName, "neck")) return LimbSite::kNeck;
    if (Contains(boneName, "upperarm")) return LimbSite::kUpperArm;
    if (Contains(boneName, "forearm")) return LimbSite::kForearm;
    if (Contains(boneName, "hand")) return LimbSite::kHand;
    if (Contains(boneName, "thigh")) return LimbSite::kThigh;
    if (Contains(boneName, "calf")) return LimbSite::kCalf;
    if (Contains(boneName, "foot")) return LimbSite::kFoot;
    if (Contains(boneName, "spine")) return LimbSite::kTorso;
    if (Contains(boneName, "com")) return LimbSite::kTorso;
    if (Contains(boneName, "pelvis")) return LimbSite::kTorso;
    // A draugr, a creature, a modded skeleton. kUnknown still sounds - it sizes
    // off limbRadius instead - which is the point of not indexing blindly.
    return LimbSite::kUnknown;
}

LimbChain ChainFromBoneName(std::string_view boneName) {
    const LimbSite site = SiteFromBoneName(boneName);
    switch (site) {
        case LimbSite::kHead:
        case LimbSite::kNeck:
            return LimbChain::kHead;
        case LimbSite::kTorso:
            return LimbChain::kTorso;
        case LimbSite::kUpperArm:
        case LimbSite::kForearm:
        case LimbSite::kHand:
            return SideOf(boneName) == Side::kRight ? LimbChain::kRightArm : LimbChain::kLeftArm;
        case LimbSite::kThigh:
        case LimbSite::kCalf:
        case LimbSite::kFoot:
            return SideOf(boneName) == Side::kRight ? LimbChain::kRightLeg : LimbChain::kLeftLeg;
        case LimbSite::kUnknown:
        case LimbSite::kCount:
            break;
    }
    return LimbChain::kNone;
}

float NominalMass(LimbSite site) {
    // Anthropometric segment fractions of a 70-unit body, NOT the solver's own
    // masses. 07 §6: vanilla's ragdoll has R Forearm at 6.0 against L Forearm at
    // 2.0 and a hand heavier than an upper arm, so a KE-based loudness off it
    // would be three times louder on the right arm for identical movement. The
    // trunk fraction (43 %) is split across the four spine bodies, which is why
    // torso reads 8 rather than 30.
    switch (site) {
        case LimbSite::kHead: return 4.8f;
        case LimbSite::kNeck: return 1.1f;
        case LimbSite::kTorso: return 7.5f;
        case LimbSite::kUpperArm: return 1.9f;
        case LimbSite::kForearm: return 1.1f;
        case LimbSite::kHand: return 0.4f;
        case LimbSite::kThigh: return 7.0f;
        case LimbSite::kCalf: return 3.3f;
        case LimbSite::kFoot: return 1.0f;
        case LimbSite::kUnknown:
        case LimbSite::kCount:
            break;
    }
    // Mid-limb, so an unrecognised skeleton is neither silent nor a cannon. The
    // sizing that actually varies on that path is objectRadius.
    return 2.5f;
}

}  // namespace rds
