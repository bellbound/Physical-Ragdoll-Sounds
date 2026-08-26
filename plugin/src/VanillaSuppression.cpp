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

/// One body impact record, as the hook's filter wants it.
///
/// The editor id is copied at data load rather than asked for at the moment of a
/// play: `GetFormEditorID` reads a cache the hook has no business touching from
/// the impact manager's thread, and the answer cannot change once the game is
/// running anyway.
struct Indexed {
    const RE::BGSImpactData* impact{};
    std::string editorId;
};

/// Built once at data load and never written afterwards, which is the whole of
/// its thread safety. A linear scan over about eight entries beats a hash lookup
/// and, more to the point, needs no lock on a path that runs per collision.
std::vector<Indexed> g_index;
bool g_indexed = false;

/// Walking the form array asks the game for what it actually loaded, rather than
/// going through `TESForm::LookupByEditorID` - which is used nowhere else in this
/// workspace and needs a lookup cache to have been populated.
///
/// **Both routes want the same thing and neither is free of it.**
/// `BGSImpactDataSet` does not implement `GetFormEditorID` - only about fifteen
/// form types do - so in a stock game every set here answers with an empty
/// string and nothing matches. po3_Tweaks is what normally keeps the name. That
/// dependency is real either way; walking the array only avoids the *second* one
/// on the cache being warm. `BuildBodyImpactIndex` says so out loud when nothing
/// resolves, which is the difference between a mod that is quiet and a mod that
/// is quiet for a reason nobody can see.
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

/// Which sets outside kBodyImpactSets point at the records we just silenced.
///
/// Set -> record is many-to-many, and nothing stops a set we do not own - one of
/// vanilla's, or one another mod adds - from reaching a record we null. Nulling
/// is per record, not per set, so that set goes quiet too, and it surfaces a long
/// way from here: a weapon hit that stops making a noise reads as an unrelated
/// bug in somebody else's mod. We do not un-null it - owning the mix is the whole
/// point, and which of the two a given install wants is not ours to guess - but
/// the log should say it happened, naming both ends, so the search that starts at
/// "my flesh hits went silent" ends here instead of in a forum thread.
void ReportSharedImpactRecords(const std::vector<RE::BGSImpactDataSet*>& ours) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (handler == nullptr) {
        return;
    }

    std::size_t shared = 0;
    for (auto* set : handler->GetFormArray<RE::BGSImpactDataSet>()) {
        if (set == nullptr || std::ranges::find(ours, set) != ours.end()) {
            continue;
        }

        // Same reason the suppression loop dedupes: one record arrives from dozens
        // of materials in the one set, and the map is unordered, so remember what
        // has been named rather than comparing against the previous entry.
        std::vector<RE::BGSImpactData*> named;
        for (const auto& [material, impact] : set->impactMap) {
            if (impact == nullptr) {
                continue;
            }
            const bool isOurs = std::ranges::any_of(
                g_saved, [&](const Saved& s) { return s.impact == impact; });
            if (!isOurs || std::ranges::find(named, impact) != named.end()) {
                continue;
            }
            named.push_back(impact);
            ++shared;

            const char* setId = set->GetFormEditorID();
            const char* impactId = impact->GetFormEditorID();
            spdlog::warn(
                "suppression: {:08X} {} is also reached from {:08X} {}, which is not one of ours - "
                "that set loses its sound too",
                impact->GetFormID(), impactId != nullptr ? impactId : "?", set->GetFormID(),
                setId != nullptr ? setId : "?");
        }
    }

    if (shared == 0) {
        spdlog::info("suppression: no impact set outside the body sets shares what we silenced");
    }
}

/// Every distinct record the body impact sets reach, deduped.
///
/// The map is keyed by material and the same record arrives from dozens of them -
/// PHYBodyMedium alone maps 60 onto three - so dedupe or the filter scans the
/// same pointer sixty times per lookup.
void ForEachBodyImpact(const auto& visit) {
    for (const std::string_view editorId : kBodyImpactSets) {
        RE::BGSImpactDataSet* set = FindSet(editorId);
        if (set == nullptr) {
            continue;
        }
        for (const auto& [material, impact] : set->impactMap) {
            if (impact != nullptr) {
                visit(impact, editorId);
            }
        }
    }
}

}  // namespace

void BuildBodyImpactIndex() {
    if (g_indexed) {
        return;
    }
    g_indexed = true;

    ForEachBodyImpact([](RE::BGSImpactData* impact, std::string_view) {
        const bool seen = std::ranges::any_of(
            g_index, [&](const Indexed& i) { return i.impact == impact; });
        if (seen) {
            return;
        }
        const char* id = impact->GetFormEditorID();
        g_index.push_back(Indexed{impact, id != nullptr ? std::string{id} : std::string{"?"}});
    });

    spdlog::info("suppression: indexed {} body impact record(s) across {} set(s)", g_index.size(),
                 std::size(kBodyImpactSets));
    if (g_index.empty()) {
        spdlog::warn(
            "suppression: no body impact records were found, so nothing will be recognised as "
            "ours and vanilla's impacts will play under ours. The usual cause is not a missing "
            "mod but a missing *name*: BGSImpactDataSet does not implement GetFormEditorID, so "
            "without po3_Tweaks (Powerof3's Tweaks) every set in this load order answers with an "
            "empty string and none of the PHY* names above can match");
    }
}

bool IsBodyImpact(const RE::BGSImpactData* impact) {
    if (impact == nullptr) {
        return false;
    }
    for (const Indexed& entry : g_index) {
        if (entry.impact == impact) {
            return true;
        }
    }
    return false;
}

std::string_view BodyImpactEditorId(const RE::BGSImpactData* impact) {
    for (const Indexed& entry : g_index) {
        if (entry.impact == impact) {
            return entry.editorId;
        }
    }
    return "?";
}

void SuppressVanillaBodyImpacts() {
    if (g_suppressed) {
        return;
    }
    g_suppressed = true;

    std::size_t setsMissing = 0;

    // The sets we own, for the sharing scan below to exclude. Kept as pointers
    // rather than looked up again by editor id: FindSet already paid for that.
    std::vector<RE::BGSImpactDataSet*> ours;

    for (const std::string_view editorId : kBodyImpactSets) {
        RE::BGSImpactDataSet* set = FindSet(editorId);
        if (set == nullptr) {
            ++setsMissing;
            spdlog::warn("suppression: no impact set called {} in this load order", editorId);
            continue;
        }
        ours.push_back(set);

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
                 g_saved.size(), ours.size(), std::size(kBodyImpactSets),
                 setsMissing != 0 ? " - see the warnings above for the ones that were missing" : "");

    if (g_saved.empty()) {
        spdlog::warn("suppression: nothing was silenced, so vanilla's body impacts will still play "
                     "under ours and every ragdoll contact will be heard twice");
        return;
    }

    ReportSharedImpactRecords(ours);
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
