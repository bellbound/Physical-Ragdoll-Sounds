#include "VanillaSuppression.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace rds::game {
namespace {

/// The body impact sets, by editor id. These are the sets a humanoid ragdoll
/// actually uses; between them they map 57-75 materials each onto one to three
/// distinct `BGSImpactData` records, which is why nulling about eight records
/// covers the whole ragdoll path.
constexpr std::string_view kBodyImpactSets[] = {
    "PHYBodyMedium",
    "PHYBodyLargeImpactSet",
    "PHYBodySmallImpactSet",
    "PHYBodyBones",
    "PHYBodyMetalLargeImpactSet",
    "PHYBodyMetalSmallImpactSet",
    "PHYMaterialArmorHeavyImpactSet",
    "PHYMaterialArmorLightImpactSet",
    "PHYMeatImpactSet",
};

/// What we took, so it can be put back.
///
/// Kept beside the flag rather than re-read from the file, because another mod
/// may have replaced these between load and now and re-reading would restore
/// vanilla's over the top of theirs.
struct Saved {
    RE::BGSImpactData* impact{};
    RE::BGSSoundDescriptorForm* sound1{};
    RE::BGSSoundDescriptorForm* sound2{};
};

std::vector<Saved> g_saved;
bool g_suppressed = false;

/// `TESForm::LookupByEditorID` is used nowhere else in this workspace and leans
/// on an editor-id cache that vanilla `PHY*` records only get from po3_Tweaks, so
/// a load order without it would silently suppress nothing. Walking the form
/// array asks the game for what it actually loaded.
[[nodiscard]] RE::BGSImpactDataSet* FindSet(std::string_view editorId) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (handler == nullptr) {
        return nullptr;
    }
    for (auto* set : handler->GetFormArray<RE::BGSImpactDataSet>()) {
        if (set == nullptr) {
            continue;
        }
        const char* id = set->GetFormEditorID();
        if (id != nullptr && editorId == id) {
            return set;
        }
    }
    return nullptr;
}

}  // namespace

void SuppressVanillaBodyImpacts() {
    if (g_suppressed) {
        return;
    }
    g_suppressed = true;

    std::size_t setsFound = 0;
    std::size_t setsMissing = 0;

    for (const std::string_view editorId : kBodyImpactSets) {
        RE::BGSImpactDataSet* set = FindSet(editorId);
        if (set == nullptr) {
            ++setsMissing;
            spdlog::warn("suppression: no impact set called {} in this load order", editorId);
            continue;
        }
        ++setsFound;

        // The map is keyed by the material, not by a MATERIAL_ID, and the same
        // BGSImpactData is reached from dozens of materials - PHYBodyMedium alone
        // maps 60 onto three - so the same record arrives over and over. Null it
        // once and remember it once, or the restore table fills with duplicates
        // and the second one restores what the first already put back.
        for (const auto& [material, impact] : set->impactMap) {
            if (impact == nullptr) {
                continue;
            }
            const bool seen = std::ranges::any_of(
                g_saved, [&](const Saved& s) { return s.impact == impact; });
            if (seen) {
                continue;
            }
            if (impact->sound1 == nullptr && impact->sound2 == nullptr) {
                continue;  // already silent; nothing to take and nothing to give back
            }

            g_saved.push_back(Saved{impact, impact->sound1, impact->sound2});
            impact->sound1 = nullptr;
            impact->sound2 = nullptr;

            const char* impactId = impact->GetFormEditorID();
            spdlog::info("suppression: silenced {:08X} {} (via {})", impact->GetFormID(),
                         impactId != nullptr ? impactId : "?", editorId);
        }
    }

    // The line a user who wonders where their impact sounds went should find,
    // without being asked to turn anything on and do it again.
    spdlog::info("suppression: {} impact record(s) silenced across {} of {} body impact sets{}",
                 g_saved.size(), setsFound, std::size(kBodyImpactSets),
                 setsMissing != 0 ? " - see the warnings above for the ones that were missing" : "");

    if (g_saved.empty()) {
        spdlog::warn("suppression: nothing was silenced, so vanilla's body impacts will still play "
                     "under ours and every ragdoll contact will be heard twice");
    }
}

void RestoreVanillaBodyImpacts() {
    if (!g_suppressed) {
        return;
    }
    for (const Saved& saved : g_saved) {
        if (saved.impact == nullptr) {
            continue;
        }
        saved.impact->sound1 = saved.sound1;
        saved.impact->sound2 = saved.sound2;
    }
    spdlog::info("suppression: put back {} impact record(s)", g_saved.size());
    g_saved.clear();
    g_suppressed = false;
}

}  // namespace rds::game
