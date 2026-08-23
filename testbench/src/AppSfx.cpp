// The sfx half of the main window: which sound each slot plays.
//
// Split out of App.cpp because it is a self-contained piece of UI over a
// self-contained piece of state. Everything here reads and writes exactly two
// things - `m_library` and `m_sfx` - and everything that changes the second one
// goes through ApplySfx(), which is what makes an assignment audible on the next
// block rather than on the next reload.

#include "App.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "imgui.h"

#include "SfxAnalysis.h"
#include "rds/ConfigManager.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

constexpr ImVec4 kSuggest{0.55f, 0.82f, 1.0f, 1.0f};
constexpr ImVec4 kQuiet{0.62f, 0.64f, 0.70f, 1.0f};
constexpr ImVec4 kDirty{1.0f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kBad{1.0f, 0.45f, 0.42f, 1.0f};

/// The order a slot list reads best in: the order the layers of an impact
/// arrive, not the order SlotId happens to declare them.
///
/// Slots.md §1 is the whole of this - transient at 0 ms, the surface skin at
/// +0-15, the body at +10-30, the sub at +55-75 - and then the things that ride
/// on top of an impact, then the things that close or bed it. Reading down this
/// list is reading through one hit.
constexpr rds::SlotId kImpactOrder[] = {
    rds::SlotId::kImpTransient, rds::SlotId::kSurfSoft,   rds::SlotId::kSurfWood,
    rds::SlotId::kSurfStone,    rds::SlotId::kImpBody,    rds::SlotId::kImpSub,
    rds::SlotId::kLimbTap,      rds::SlotId::kCrunchGran, rds::SlotId::kHeadImpact,
    rds::SlotId::kGoreWet,      rds::SlotId::kSettleRest, rds::SlotId::kScrapeLoop,
    rds::SlotId::kFoleyCloth,   rds::SlotId::kAirWhoosh,  rds::SlotId::kGruntImpact,
    rds::SlotId::kScreamBig,
};

static_assert(std::size(kImpactOrder) == static_cast<std::size_t>(rds::SlotId::kCount),
              "every slot needs a place in the list, or the panel silently hides one");

void Tip(std::string_view text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(440.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

[[nodiscard]] std::string Lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// state
// ═════════════════════════════════════════════════════════════════════════════

void App::LoadSfx() {
    m_library.Load(m_paths.library);

    m_sfx = rds::SfxAssignments{};
    const std::size_t slots = m_sfx.Load(m_paths.sfxIni);
    if (slots == 0) {
        // Nothing assigned yet. The library is almost always the built pack at
        // this point, and its files are named `<slot>_<NN>` - so the first run
        // starts from the assignment the pack already implied rather than from
        // an empty table that would make every slot procedural.
        m_sfx.SeedFromNames(m_library);
        spdlog::info("sfx: no assignments in {}, seeded {} slot(s) from the library's filenames",
                     m_paths.sfxIni.string(), m_sfx.AssignedSlots());
    }
    m_sfxSaved = m_sfx;

    m_sfxUndo.clear();
    m_sfxRedo.clear();
    m_browser.Init(&m_library, &m_player, 48000);
    m_browser.SetPackDirectory(m_paths.sounds);
    ApplySfx();
}

void App::ApplySfx() {
    m_bank.LoadAssigned(m_library, m_sfx, m_paths.sounds.string());
    m_bank.Seed(m_seed);
    // The cache is keyed on (slot, variant), and that key now means a different
    // file. Without this the panel would say one thing and the speakers another
    // until the next restart, which is the exact bug this feature exists to make
    // impossible.
    m_sources.Invalidate();
    for (ConfigSide& side : m_side) {
        side.dirty = true;
        side.verifyRun = false;
    }
    RerunDirty();
}

void App::SaveSfx() {
    if (m_sfx.Save(m_paths.sfxIni)) {
        m_sfxSaved = m_sfx;
        spdlog::info("sfx: wrote {} slot assignment(s) to {}", m_sfx.AssignedSlots(),
                     m_paths.sfxIni.string());
    }
}

void App::PushSfxEdit(const rds::SfxAssignments& before, std::string_view label) {
    if (before == m_sfx) {
        return;
    }
    m_sfxUndo.push_back({before, std::string(label), ++m_editSeq});
    if (m_sfxUndo.size() > 128) {
        m_sfxUndo.erase(m_sfxUndo.begin());
    }
    // A new step of any kind ends the branch you could redo, the same way a
    // config edit and a region change do.
    m_sfxRedo.clear();
    m_regionRedo.clear();
    m_side[0].redo.clear();
    m_side[1].redo.clear();
    ApplySfx();
}

void App::UndoSfx() {
    if (m_sfxUndo.empty()) {
        return;
    }
    const SfxEdit step = m_sfxUndo.back();
    m_sfxUndo.pop_back();
    m_sfxRedo.push_back({m_sfx, step.label, ++m_editSeq});
    m_sfx = step.state;
    ApplySfx();
}

void App::RedoSfx() {
    if (m_sfxRedo.empty()) {
        return;
    }
    const SfxEdit step = m_sfxRedo.back();
    m_sfxRedo.pop_back();
    m_sfxUndo.push_back({m_sfx, step.label, ++m_editSeq});
    m_sfx = step.state;
    ApplySfx();
}

bool App::SlotInTimeline(rds::SlotId slot) const {
    const ConfigSide& side = m_side[m_player.ActiveSide()];
    return std::ranges::any_of(side.result.cues,
                               [slot](const rds::Cue& cue) { return cue.slot == slot; });
}

std::vector<rds::SlotId> App::SlotOrder() const {
    std::vector<rds::SlotId> used;
    std::vector<rds::SlotId> rest;
    for (const rds::SlotId slot : kImpactOrder) {
        (SlotInTimeline(slot) ? used : rest).push_back(slot);
    }
    used.insert(used.end(), rest.begin(), rest.end());
    return used;
}

// ═════════════════════════════════════════════════════════════════════════════
// the panel
// ═════════════════════════════════════════════════════════════════════════════

void App::DrawSfxPanel(float height) {
    ImGui::BeginChild("sfxpanel", ImVec2(0, height), ImGuiChildFlags_Borders);

    const bool unsaved = !(m_sfx == m_sfxSaved);

    // Same header shape as the config panel above it: what this is, where it
    // saves to, and the three buttons that act on it.
    ImGui::TextColored(kSuggest, "SFX");
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", m_paths.sfxIni.filename().string().c_str(), unsaved ? " *" : "");
    Tip(std::format("{}\n\nWritten straight into the deployed mod, because unlike an algorithm "
                    "config there is only ever one of these. The game reads it at load: a slot "
                    "listed here plays those files, and a slot left empty falls back to scanning "
                    "sounds\\ for <slot>_<NN>.wav.",
                    m_paths.sfxIni.string()));

    ImGui::SameLine();
    if (ImGui::Button("Save##sfx")) {
        SaveSfx();
    }
    Tip("Write RagdollSounds_SFX.ini. Ctrl+S does this and everything else unsaved at once.");

    ImGui::SameLine();
    ImGui::BeginDisabled(m_sfxUndo.empty());
    if (ImGui::Button("Undo##sfx")) {
        UndoSfx();
    }
    ImGui::EndDisabled();
    Tip(m_sfxUndo.empty() ? std::string("Nothing to undo here. Ctrl+Z undoes whichever happened "
                                        "last - a slider, a loop region or an assignment.")
                          : "Undo \"" + m_sfxUndo.back().label + "\" (" +
                                std::to_string(m_sfxUndo.size()) + " step" +
                                (m_sfxUndo.size() == 1 ? "" : "s") + ").");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_sfxRedo.empty());
    if (ImGui::Button("Redo##sfx")) {
        RedoSfx();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Library...")) {
        m_browser.Open();
    }
    Tip("Everything in the library, with what it measures. Also where importing happens.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-4.0f);
    ImGui::InputTextWithHint("##sfxfilter", "filter slots", m_sfxFilter, sizeof(m_sfxFilter));

    ImGui::Separator();

    ImGui::BeginChild("slots", ImVec2(0, 0));
    const std::string needle = Lower(m_sfxFilter);
    bool drewDivider = false;
    const std::vector<rds::SlotId> order = SlotOrder();
    for (const rds::SlotId slot : order) {
        const rds::SlotDesc& desc = rds::Slot(slot);
        if (!needle.empty()) {
            std::string hay = Lower(desc.name) + " " + Lower(desc.role) + " " + Lower(desc.character);
            for (const std::string& file : m_sfx.For(slot).files) {
                if (const rds::SfxEntry* entry = m_library.Find(file); entry != nullptr) {
                    hay += " " + Lower(entry->name);
                }
            }
            if (hay.find(needle) == std::string::npos) {
                continue;
            }
        }
        // One line where the slots this take used stop. The list is long and
        // most of it is irrelevant to whatever you are listening to.
        if (!drewDivider && !SlotInTimeline(slot)) {
            drewDivider = true;
            if (slot != order.front()) {
                ImGui::Spacing();
                ImGui::TextDisabled("--- not in this take ---");
            }
        }
        DrawSlotWidget(slot);
    }
    ImGui::EndChild();

    ImGui::EndChild();
}

void App::DrawSlotWidget(rds::SlotId slot) {
    const rds::SlotDesc& desc = rds::Slot(slot);
    rds::SlotAssignment& assignment = m_sfx.For(slot);
    ImGui::PushID(static_cast<int>(slot));

    ImGui::BeginGroup();

    const bool inTake = SlotInTimeline(slot);
    ImGui::TextColored(inTake ? kSuggest : kQuiet, "%s", std::string(desc.name).c_str());
    Tip(std::string(desc.character));
    ImGui::SameLine();
    // The recommended length, on the slot rather than buried in a tooltip: it is
    // the number you are checking a candidate against, so it belongs next to the
    // candidates.
    ImGui::TextDisabled("%.0f-%.0f ms", desc.minLengthMs, desc.maxLengthMs);
    Tip(std::format("What this slot wants, from Slots.md §3. The picker sorts anything within a "
                    "quarter either side of it to the top, and everything else still assignable "
                    "below that."));

    ImGui::SameLine();
    bool looping = assignment.looping;
    if (ImGui::Checkbox("loop", &looping)) {
        const rds::SfxAssignments before = m_sfx;
        m_sfx.For(slot).looping = looping;
        PushSfxEdit(before, std::string(desc.name) + " / looping");
    }
    Tip("This slot's sound is a sustained texture the engine repeats, not an event. Looping slots "
        "are never judged for being too long, and a long file assigned to one is not a warning.");

    ImGui::SameLine();
    if (ImGui::SmallButton("+ add")) {
        m_browser.OpenForSlot(slot, -1, {}, assignment.looping);
    }
    Tip("Assign another file to this slot. The engine picks between a slot's files with a shuffle "
        "bag, so three files means no immediate repeats - which is what imp_body_01/02/03 was "
        "doing before this panel existed.");

    if (assignment.files.empty()) {
        ImGui::Indent(14.0f);
        ImGui::TextDisabled("nothing assigned - falls back to sounds\\%s_NN.wav",
                            std::string(desc.name).c_str());
        Tip("An empty slot is not a silent one. The bank scans the built pack for files named "
            "after the slot, which is what the mod did before this file existed.");
        ImGui::Unindent(14.0f);
    }

    int removeAt = -1;
    for (int i = 0; i < static_cast<int>(assignment.files.size()); ++i) {
        const std::string& file = assignment.files[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::Indent(14.0f);

        sfxui::PreviewButton("play", m_library, file, m_player, 48000);
        ImGui::SameLine();

        const rds::SfxEntry* entry = m_library.Find(file);
        if (entry == nullptr) {
            ImGui::TextColored(kBad, "%s", file.c_str());
            Tip("Named by the ini but not in the library - deleted, renamed on disk, or the "
                "library folder moved. The slot plays one fewer variant than it says.");
        } else {
            ImGui::TextUnformatted(entry->name.c_str());
            Tip(std::format("{}\n\nvariant {} of {}. The order is the variant index a recorded cue "
                            "carries, so re-ordering changes which file a given cue plays.",
                            entry->file, i, assignment.files.size()));
            ImGui::SameLine();
            sfxui::Badges(*entry, false);
            // The one badge the library view cannot show, because it depends on
            // the slot: whether this length suits the slot it landed in.
            if (!LengthSuits(entry->durationMs, slot, assignment.looping)) {
                ImGui::SameLine();
                ImGui::TextColored(kDirty, "(long for the slot)");
                Tip(std::format("{} against the slot's {:.0f}-{:.0f} ms. Not a problem in itself - "
                                "the engine plays what it is given - but it is worth knowing.",
                                sfxui::Duration(entry->durationMs), desc.minLengthMs,
                                desc.maxLengthMs));
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("change")) {
            m_browser.OpenForSlot(slot, i, file, assignment.looping);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = i;
        }
        Tip("Take this file off the slot.");

        ImGui::Unindent(14.0f);
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        const rds::SfxAssignments before = m_sfx;
        auto& files = m_sfx.For(slot).files;
        files.erase(files.begin() + removeAt);
        PushSfxEdit(before, std::string(desc.name) + " / remove");
    }

    ImGui::EndGroup();

    // Hovering the widget lights up this slot's bars in the timeline, which is
    // the whole point of having both on screen: "what does this slot actually
    // do in this take" is otherwise a question you answer by squinting.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        m_hoverSlotPending = static_cast<int>(slot);
    }

    ImGui::Separator();
    ImGui::PopID();
}

}  // namespace tb
