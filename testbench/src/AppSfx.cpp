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
    m_browser.Init(&m_library, &m_sfx, &m_player, 48000, &m_sfxPreviewGainDb);
    m_browser.SetPackDirectory(m_paths.sounds);
    ApplySfx();
}

void App::ApplySfx() {
    m_bank.LoadAssigned(m_library, m_sfx, m_paths.sounds.string());
    m_bank.Seed(m_seed);
    // The load cleared the bank's overrides, because it renumbered the variants
    // they name. This puts back the ones that still point at something.
    ApplySfxSession();
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

// ═════════════════════════════════════════════════════════════════════════════
// forcing and muting
// ═════════════════════════════════════════════════════════════════════════════
//
// Two ways to point a whole take at one file, and they are not the same kind of
// thing - which is the reason they are stored in two different places.
//
// A **force** pins one file: every cue the slot emits plays it, so a candidate
// can be heard against the take it will live in rather than against a preview
// button. That is a way to listen and nothing else. It lives in m_sfxSession,
// is written nowhere, is not pushed to the game, is not an undo step, and dies
// with the process.
//
// A **mute** suspends one: the slot carries on without it, which is how you find
// out whether the one you dislike is the one you keep hearing - and once you
// know, the answer is "never play this here", which is a decision about the
// pack. So it lives in m_sfx beside the file list, saves into
// RagdollSounds_SFX.ini, undoes and redoes with every other assignment edit, and
// goes over the link so a running game stops playing it too. The file keeps its
// place in the list, so its variant index is untouched and unmuting puts the
// take back exactly as it was.

namespace {

/// The bank's variant index for a library file on a slot, or -1.
///
/// By path rather than by position in the assignment: a file the bank skipped -
/// disabled in the library, or missing from disk - shifts every index after it,
/// so counting rows in the panel would silence the wrong sound.
[[nodiscard]] int VariantOfFile(const rds::SoundBank& bank, const rds::SfxLibrary& library,
                                rds::SlotId slot, std::string_view file) {
    const std::string path = library.PathOf(file).string();
    const std::size_t count = bank.FileCount(slot);
    for (std::size_t i = 0; i < count; ++i) {
        rds::ResolvedSound sound;
        if (bank.Get(slot, static_cast<std::uint8_t>(i), sound) && sound.path == path) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

void App::ApplySfxSession() {
    // Only the pins. The mutes are part of the assignment now, so
    // SoundBank::LoadAssigned has already applied them by the time this runs -
    // and applying them a second time here is how the two copies would drift.
    for (std::size_t i = 0; i < m_sfxSession.size(); ++i) {
        const auto slot = static_cast<rds::SlotId>(i);
        SlotSession& session = m_sfxSession[i];
        if (session.forced.empty()) {
            continue;
        }
        const int variant = VariantOfFile(m_bank, m_library, slot, session.forced);
        // Gone from the slot: dropped rather than remembered, so the count on
        // "Unforce all" is the number of slots actually pinned and not the
        // number of times somebody pressed the button.
        if (variant < 0) {
            session.forced.clear();
        } else {
            m_bank.ForceVariant(slot, static_cast<std::uint8_t>(variant));
        }
    }
}

bool App::SfxForced(rds::SlotId slot, std::string_view file) const {
    return m_sfxSession[static_cast<std::size_t>(slot)].forced == file;
}

bool App::SfxMuted(rds::SlotId slot, std::string_view file) const {
    return m_sfx.For(slot).Muted(file);
}

void App::ToggleSfxForce(rds::SlotId slot, std::string_view file) {
    std::string& forced = m_sfxSession[static_cast<std::size_t>(slot)].forced;
    forced = (forced == file) ? std::string{} : std::string(file);
    ApplySfx();
}

void App::ToggleSfxMute(rds::SlotId slot, std::string_view file) {
    const rds::SfxAssignments before = m_sfx;
    std::vector<std::string>& muted = m_sfx.For(slot).muted;
    if (const auto it = std::ranges::find(muted, file); it != muted.end()) {
        muted.erase(it);
    } else {
        muted.emplace_back(file);
    }
    // Through the assignment history like every other edit to the pack, so
    // Ctrl+Z reaches it and the unsaved marker lights. PushSfxEdit calls
    // ApplySfx, which rebuilds the bank and re-runs the take.
    PushSfxEdit(before, std::string(rds::Slot(slot).name) + " / " +
                            (muted.size() > before.For(slot).muted.size() ? "mute" : "unmute"));
}

int App::SfxForceCount() const {
    return static_cast<int>(std::ranges::count_if(
        m_sfxSession, [](const SlotSession& s) { return !s.forced.empty(); }));
}

int App::SfxMuteCount() const {
    int total = 0;
    for (const rds::SlotDesc& desc : rds::Slots()) {
        total += static_cast<int>(m_sfx.For(desc.id).muted.size());
    }
    return total;
}

void App::ClearSfxForces() {
    for (SlotSession& session : m_sfxSession) {
        session.forced.clear();
    }
    ApplySfx();
}

void App::ClearSfxMutes() {
    const rds::SfxAssignments before = m_sfx;
    for (const rds::SlotDesc& desc : rds::Slots()) {
        m_sfx.For(desc.id).muted.clear();
    }
    PushSfxEdit(before, "unmute all");
}

bool App::SlotInTimeline(rds::SlotId slot) const {
    // The remembered answer, not the live one. Recomputing it from the cue
    // list every frame meant that muting a slot - or any config change that
    // silenced one - reordered the panel while the cursor was still on the
    // control that caused it. See m_slotSeen.
    const auto index = static_cast<std::size_t>(slot);
    return index < m_slotSeen.size() && m_slotSeen[index];
}

CueSound App::SoundOf(const rds::Cue& cue) const {
    CueSound out;
    out.variant = cue.variant;
    out.variantCount = static_cast<int>(m_bank.FileCount(cue.slot));
    out.forced = m_bank.ForcedVariant(cue.slot) != rds::SoundBank::kNoVariant;

    rds::ResolvedSound resolved{};
    if (!m_bank.Get(cue.slot, cue.variant, resolved)) {
        // A declared-and-unfilled voice slot. The engine skips these, so it is
        // not a cue anybody should be looking at - but say so rather than
        // leaving the cell blank, which reads as a bug in this column.
        out.label = "(unfilled)";
        return out;
    }
    if (resolved.procedural || resolved.path.empty()) {
        out.procedural = true;
        out.label = "(procedural)";
        return out;
    }

    out.file = fs::path(resolved.path).filename().string();
    // The library's display name, which is what the panel and the browser call
    // it. Renaming is free there precisely so a sound can be called what it
    // sounds like, and this is the column where that pays off.
    if (const rds::SfxEntry* entry = m_library.Find(out.file); entry != nullptr &&
                                                              !entry->name.empty()) {
        out.label = entry->name;
    } else {
        out.label = fs::path(resolved.path).stem().string();
    }
    return out;
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

    // Only while there is something to undo. A force or a mute is invisible from
    // anywhere but the row it is on - the slot may not even be in this take -
    // so the way out of one has to be at the top where it cannot be missed.
    if (const int forced = SfxForceCount(); forced > 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::Button(std::format("Unforce all ({})###unforceall", forced).c_str())) {
            ClearSfxForces();
        }
        ImGui::PopStyleColor();
        Tip(std::format("{} slot(s) are pinned to one file. This puts every one of them back to "
                        "picking between its variants.",
                        forced));
    }
    if (const int muted = SfxMuteCount(); muted > 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::Button(std::format("Unmute all ({})###unmuteall", muted).c_str())) {
            ClearSfxMutes();
        }
        ImGui::PopStyleColor();
        Tip(std::format("{} file(s) are muted on a slot. This puts all of them back in play, as "
                        "one undo step. It does not touch anything disabled in the library, which "
                        "is the everywhere-at-once version of the same idea.",
                        muted));
    }

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

    // The whole slot's mute, beside the files it would have played.
    //
    // Not the same switch as the per-file mutes below it: those suspend one
    // recording and let the slot pick another, which answers "is this the right
    // file". This silences the layer, which answers "does the mix need this
    // layer at all" - and that is the question the design keeps asking, because
    // its central claim is that muting imp_sub should take the gnarl with it.
    //
    // It writes the same flag the [Layers] and [Surfaces] panels do, through
    // rds::LayerMute, so the two are one switch shown twice rather than two
    // switches that can disagree. Like every mute it lands at render, which is
    // what makes it an honest A/B: arbitration made the same decisions either
    // way and only the sound is gone.
    if (const bool* mute = rds::LayerMute(m_side[m_focusSide].cfg, slot); mute != nullptr) {
        const bool audible = *mute;
        ImGui::SameLine();
        if (!audible) ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::SmallButton(audible ? "mute" : "muted")) {
            ConfigSide& s = m_side[m_focusSide];
            const rds::AlgorithmConfig before = s.cfg;
            *rds::LayerMute(s.cfg, slot) = !audible;
            s.dirty = true;
            PushEdit(m_focusSide, before,
                     std::string(desc.name) + (audible ? " / muted" : " / unmuted"));
        }
        if (!audible) ImGui::PopStyleColor();
        Tip(std::format(
            "Silence every cue this slot emits, on side {}. Arbitration is untouched - the "
            "same cues are chosen and paid for, and only the sound is gone - so what you "
            "hear is this take without this layer rather than a different take.\n\n"
            "The same flag as [Layers] / [Surfaces] in the config panel. With split A/B on, "
            "muting one side gives you with-and-without on alternate loops, which is the "
            "fastest way to ask whether a layer is earning its place.",
            static_cast<char>('A' + m_focusSide)));
    }

    ImGui::SameLine();
    bool looping = assignment.looping;
    if (ImGui::Checkbox("loop", &looping)) {
        const rds::SfxAssignments before = m_sfx;
        m_sfx.For(slot).looping = looping;
        PushSfxEdit(before, std::string(desc.name) + " / looping");
    }
    Tip("This slot's sound is a sustained texture the engine repeats, not an event. Looping slots "
        "are never judged for being too long, and a long file assigned to one is not a warning.");

    if (!m_sfxSession[static_cast<std::size_t>(slot)].forced.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kDirty, "[forced]");
        Tip("Every cue this slot emits is playing one file. The slot is not picking between its "
            "variants until that is cleared.");
    }

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
            // Greyed rather than hidden while muted: the point of suspending one
            // variant is to keep looking at it while it is not playing.
            const bool quiet = SfxMuted(slot, file);
            if (quiet) ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
            ImGui::TextUnformatted(entry->name.c_str());
            if (quiet) ImGui::PopStyleColor();
            Tip(std::format("{}\n\nvariant {} of {}{}. The order is the variant index a recorded "
                            "cue carries, so re-ordering changes which file a given cue plays.",
                            entry->file, i, assignment.files.size(), quiet ? ", muted" : ""));
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

        const bool forced = SfxForced(slot, file);
        const bool muted = SfxMuted(slot, file);

        ImGui::SameLine();
        if (forced) ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::SmallButton(forced ? "forced" : "force")) {
            ToggleSfxForce(slot, file);
        }
        if (forced) ImGui::PopStyleColor();
        Tip(forced ? "Stop pinning this one. The slot goes back to picking between its variants, "
                     "with the same seed, so the take returns to exactly what it was."
                   : "Play this file for every cue on this slot, so a candidate can be heard "
                     "against the whole take instead of against the preview button. One pin per "
                     "slot - pinning another moves it - and it beats a mute on the same file, "
                     "because pinning something you muted is asking to hear it. Lasts until you "
                     "clear it or close the testbench; nothing is written and the game is not "
                     "told.");

        ImGui::SameLine();
        if (muted) ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::SmallButton(muted ? "muted" : "mute")) {
            ToggleSfxMute(slot, file);
        }
        if (muted) ImGui::PopStyleColor();
        Tip(muted ? "Put it back in play on this slot. The file never left its place in the "
                    "list, so the take goes back to exactly what it was."
                  : "Never choose this one, while leaving it on the slot. The rest of the "
                    "variants carry the take, which is how you find out whether the one you "
                    "dislike is the one you keep hearing; a slot with all of its files muted goes "
                    "silent rather than falling back to a stand-in.\n\n"
                    "This slot only, and it is saved: it writes into RagdollSounds_SFX.ini, "
                    "undoes with Ctrl+Z, goes over the link to a running game, and is still there "
                    "next launch. The library's own disable is the everywhere-at-once version.");

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
        rds::SlotAssignment& target = m_sfx.For(slot);
        target.Unmute(target.files[static_cast<std::size_t>(removeAt)]);
        target.files.erase(target.files.begin() + removeAt);
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
