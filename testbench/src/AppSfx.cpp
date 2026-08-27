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
    rds::SlotId::kImpTransient,
    // The three surfaces with files, then the ten that inherit from them, in
    // the order of the parent they fall back to: soft's children, then stone's,
    // then the two that hang off water and body. Dropping `surf_ice_01.wav` on
    // the ice row is the whole of adding a surface, so the rows want to sit
    // next to the one they currently sound like.
    rds::SlotId::kSurfSoft,        rds::SlotId::kSurfDirt,        rds::SlotId::kSurfGravel,
    rds::SlotId::kSurfSnow,        rds::SlotId::kSurfWater,       rds::SlotId::kSurfWaterPuddle,
    rds::SlotId::kSurfBody,        rds::SlotId::kSurfBone,
    rds::SlotId::kSurfWood,
    rds::SlotId::kSurfStone,       rds::SlotId::kSurfMetal,       rds::SlotId::kSurfIce,
    rds::SlotId::kSurfGlass,
    rds::SlotId::kArmorBare,       rds::SlotId::kArmorCloth,
    rds::SlotId::kArmorLight,     rds::SlotId::kArmorHeavy,
    // Beside the layer it is a variant of, which is what this list is for: it
    // reads in the order an impact arrives, not in SlotId order, so the pressure
    // that keeps `imp_body_limb` at the end of the enum does not apply here.
    rds::SlotId::kImpBody,        rds::SlotId::kImpBodyLimb,     rds::SlotId::kImpSub,
    rds::SlotId::kLimbTap,        rds::SlotId::kCrunchGran,      rds::SlotId::kSpineCrunch,
    rds::SlotId::kLimbCrunch,     rds::SlotId::kHeadImpact,
    rds::SlotId::kGoreWet,        rds::SlotId::kSettleRest,      rds::SlotId::kScrapeLoop,
    rds::SlotId::kScrapeBodyWood, rds::SlotId::kScrapeBodyStone, rds::SlotId::kScrapeLimb,
    rds::SlotId::kScrapeLimbWood, rds::SlotId::kScrapeLimbStone, rds::SlotId::kScrapeGrain,
    // With the other continuous beds rather than at the end where the enum puts
    // it, for the same reason `imp_body_limb` sits beside `imp_body`: this list
    // reads in the order an impact arrives and then through the things that bed
    // it, and the garment is the last of those.
    // The bed sits with the grinds it is under rather than at the end where the
    // enum puts it: this list reads in the order an impact arrives and then
    // through the things that bed it, and the mass under a slide belongs beside
    // the grit that rides on it.
    rds::SlotId::kScrapeLoopRumble,
    rds::SlotId::kAirWhoosh,      rds::SlotId::kClothRustle,     rds::SlotId::kGruntImpact,
    rds::SlotId::kScreamBig,
};

static_assert(std::size(kImpactOrder) == static_cast<std::size_t>(rds::SlotId::kCount),
              "every slot needs a place in the list, or the panel silently hides one");

void Tip(std::string_view text) {
    // Never while a row is in the hand. The drag carries its own preview under
    // the cursor, and it passes over every slot in the list on the way to the
    // one you want - so a second tooltip fighting it for that space is one per
    // slot crossed, on top of the thing you are trying to aim.
    if (ImGui::GetDragDropPayload() != nullptr) {
        return;
    }
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


// ── dragging a placement ────────────────────────────────────────────────────
//
// Putting a sound on a different slot used to be four gestures. Dragging the row
// is the same edit said once.
//
// The payload is the row's *address*, not its filename: a slot may place one wav
// twice, plain and tagged, and only the position says which is in the hand.

constexpr const char* kRowPayload = "rds.sfx.row";

struct RowDrag {
    int slot{-1};
    int index{-1};
};

/// Make the item just submitted the handle for dragging placement `index` of
/// `slot`.
///
/// The preview says which of the two gestures is live and re-reads Ctrl every
/// frame, so the modifier is discoverable from the thing it changes rather than
/// from a tooltip somebody has to already suspect exists.
void RowDragSource(rds::SlotId slot, int index, std::string_view label) {
    if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        return;
    }
    const RowDrag drag{static_cast<int>(slot), index};
    ImGui::SetDragDropPayload(kRowPayload, &drag, sizeof(drag));
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    if (ImGui::GetIO().KeyCtrl) {
        ImGui::TextColored(kSuggest, "copy onto the slot you drop it on");
    } else {
        ImGui::TextDisabled("move onto the slot you drop it on - hold Ctrl to copy");
    }
    ImGui::EndDragDropSource();
}

/// True on the frame a row was dropped on the item just submitted, with `out`
/// set to where it came from.
///
/// Targets nest - a row sits inside its slot - and that is deliberate rather
/// than tolerated: ImGui hands the drop to the smallest rectangle under the
/// cursor, so pointing at a row means that row's condition and pointing at the
/// space around them means the slot itself.
[[nodiscard]] bool AcceptRowDrop(RowDrag& out) {
    bool dropped = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kRowPayload);
            payload != nullptr && payload->DataSize == static_cast<int>(sizeof(RowDrag))) {
            std::memcpy(&out, payload->Data, sizeof(RowDrag));
            dropped = true;
        }
        ImGui::EndDragDropTarget();
    }
    return dropped;
}


// ── conditions ──────────────────────────────────────────────────────────────
//
// A file on a slot can ask something of the contact before it is a candidate, and
// where it applies it beats the plain files rather than joining them - the
// difference between a slot that has five sounds and one that knows which of them
// belongs where.
//
// The engine has done this since the armour build; until now the only way to set
// one was to type it into RagdollSounds_SFX.ini by hand.

/// The two axes, in the order the popover lays them out. `any` first on both,
/// because the top-left cell is "no opinion" and reading right or down is
/// asking for more.
constexpr rds::SurfaceMatch kCondSurfaces[] = {
    rds::SurfaceMatch::kAny,   rds::SurfaceMatch::kSoft,  rds::SurfaceMatch::kWood,
    rds::SurfaceMatch::kStone, rds::SurfaceMatch::kMetal, rds::SurfaceMatch::kWater,
    rds::SurfaceMatch::kBody,
};
constexpr rds::CoverageMatch kCondArmour[] = {
    rds::CoverageMatch::kAny,   rds::CoverageMatch::kBare,  rds::CoverageMatch::kCloth,
    rds::CoverageMatch::kLight, rds::CoverageMatch::kHeavy,
};

[[nodiscard]] bool SameCondition(rds::VariantCondition a, rds::VariantCondition b) {
    return a.surface == b.surface && a.coverage == b.coverage;
}

/// `stone / heavy` - the tag exactly as the ini spells it.
///
/// Deliberately the ini's own words rather than something friendlier: the panel
/// is the second way to set these and the file is the first, and somebody who
/// learns the grid should be able to read the file afterwards.
[[nodiscard]] std::string ConditionTag(rds::VariantCondition cond) {
    return std::format("{} / {}", rds::ToString(cond.surface), rds::ToString(cond.coverage));
}

[[nodiscard]] std::string_view SurfaceWords(rds::SurfaceMatch m) {
    switch (m) {
        case rds::SurfaceMatch::kSoft:  return "soft ground";
        case rds::SurfaceMatch::kWood:  return "wood";
        case rds::SurfaceMatch::kStone: return "stone";
        case rds::SurfaceMatch::kMetal: return "metal";
        case rds::SurfaceMatch::kWater: return "water";
        case rds::SurfaceMatch::kBody:  return "another body";
        default:                        return "any surface";
    }
}

[[nodiscard]] std::string_view ArmourWords(rds::CoverageMatch m) {
    switch (m) {
        case rds::CoverageMatch::kBare:  return "bare skin";
        case rds::CoverageMatch::kCloth: return "clothing";
        case rds::CoverageMatch::kLight: return "light armour";
        case rds::CoverageMatch::kHeavy: return "heavy armour";
        default:                         return "any armour";
    }
}

/// What the tag means, in words: `only on stone, in heavy armour`.
[[nodiscard]] std::string ConditionWords(rds::VariantCondition cond) {
    const bool surface = cond.surface != rds::SurfaceMatch::kAny;
    const bool armour = cond.coverage != rds::CoverageMatch::kAny;
    if (!surface && !armour) {
        return "no condition - in play everywhere";
    }
    if (surface && !armour) {
        return std::format("only on {}", SurfaceWords(cond.surface));
    }
    if (!surface && armour) {
        return std::format("only in {}", ArmourWords(cond.coverage));
    }
    return std::format("only on {}, in {}", SurfaceWords(cond.surface),
                       ArmourWords(cond.coverage));
}

/// The popover's grid: every combination of surface and armour, one click each. A
/// table rather than two dropdowns, because "which corner of the space is this
/// recording for" is a thing you point at and two combos would make `stone / heavy`
/// two decisions. True on the frame a cell was clicked, with `out` set to it.
///
/// `allowPlain` is the top-left cell, which asks nothing. Off while adding, where
/// it means the same as `+ add`; on while re-tagging, where it takes a tag off.
[[nodiscard]] bool DrawConditionGrid(rds::VariantCondition& out, bool allowPlain) {
    bool picked = false;
    if (ImGui::BeginTable("cond", 1 + static_cast<int>(std::size(kCondSurfaces)),
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("");
        for (const rds::SurfaceMatch surface : kCondSurfaces) {
            ImGui::TableSetupColumn(std::string(rds::ToString(surface)).c_str());
        }
        ImGui::TableHeadersRow();
        for (const rds::CoverageMatch armour : kCondArmour) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", std::string(rds::ToString(armour)).c_str());
            for (const rds::SurfaceMatch surface : kCondSurfaces) {
                ImGui::TableNextColumn();
                const rds::VariantCondition cell{surface, armour};
                const bool plain = cell.Unconditional();
                const bool off = plain && !allowPlain;
                ImGui::BeginDisabled(off);
                if (ImGui::Selectable(std::format(" + ##{}_{}", static_cast<int>(surface),
                                                  static_cast<int>(armour))
                                          .c_str())) {
                    out = cell;
                    picked = true;
                }
                ImGui::EndDisabled();
                if (!off) {
                    Tip(plain ? std::string("No condition at all: a plain variant, in play "
                                            "everywhere. This is how a tag comes back off.")
                              : std::format("{}.\n\nWritten `{}` in RagdollSounds_SFX.ini. A "
                                            "tagged file beats the plain ones where it matches "
                                            "and is invisible where it does not - and if nothing "
                                            "on the slot can match, the slot plays its full set "
                                            "rather than going quiet.",
                                            ConditionWords(cell), ConditionTag(cell)));
                }
            }
        }
        ImGui::EndTable();
    }
    return picked;
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
// Two ways to point a whole take at one file, and not the same kind of thing -
// which is why they are stored in two places.
//
// A **force** pins one file so a candidate can be heard against the take it will
// live in rather than against a preview button. A way to listen and nothing else:
// it lives in m_sfxSession, is written nowhere, is not pushed to the game, is not
// an undo step, and dies with the process.
//
// A **mute** suspends one and lets the slot carry on without it, which is a
// decision about the pack. It lives in m_sfx beside the file list, saves into
// RagdollSounds_SFX.ini, undoes with every other assignment edit, and goes over
// the link. The file keeps its place in the list, so unmuting puts the take back
// exactly as it was.

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

// ═════════════════════════════════════════════════════════════════════════════
// moving a placement
// ═════════════════════════════════════════════════════════════════════════════

void App::DropSfxPlacement(rds::SlotId from, int index, rds::SlotId to,
                           rds::VariantCondition condition, bool copy) {
    if (static_cast<std::size_t>(from) >= static_cast<std::size_t>(rds::SlotId::kCount)) {
        return;
    }
    rds::SlotAssignment& source = m_sfx.For(from);
    if (index < 0 || index >= static_cast<int>(source.files.size())) {
        return;
    }
    // Both by value: the vectors under them are about to move.
    const std::string file = source.files[static_cast<std::size_t>(index)];
    const rds::VariantCondition was = source.ConditionAt(static_cast<std::size_t>(index));

    // Dropped back where it already is. Not an edit and never an undo step -
    // even for a copy, where it would mean a second identical placement, which
    // is a way to weight a file and not a thing a slip of the mouse should do.
    if (from == to && SameCondition(was, condition)) {
        return;
    }

    const rds::SfxAssignments before = m_sfx;
    if (from == to) {
        // Onto another condition on its own slot. A move here is a re-tag and
        // must go through SetConditionAt rather than through remove-and-add:
        // the index is the variant a recorded cue carries, and sending the row
        // to the end of the list would renumber every take in the corpus.
        if (copy) {
            m_sfx.For(to).Add(file, condition);
        } else {
            m_sfx.For(to).SetConditionAt(static_cast<std::size_t>(index), condition);
        }
    } else {
        m_sfx.For(to).Add(file, condition);
        if (!copy) {
            // RemoveAt takes the row's condition with it, and the mute when
            // that was the file's last placement here.
            m_sfx.For(from).RemoveAt(static_cast<std::size_t>(index));
        }
    }
    // Nothing carries the mute across. It says "not this sound, *here*", and
    // here was the slot it left: dragging a file you silenced onto another slot
    // is asking whether it works there. A destination that already mutes the
    // name is the one exception, and that is the name-keyed mute saying what it
    // has always said.

    std::string label;
    if (from == to) {
        const std::string what =
            condition.Unconditional() ? std::string("anything") : ConditionTag(condition);
        label = std::format("{} / {}{} for {}", rds::Slot(to).name, copy ? "copy " : "", file,
                            what);
    } else {
        const std::string where =
            condition.Unconditional()
                ? std::string(rds::Slot(to).name)
                : std::format("{} ({})", rds::Slot(to).name, ConditionTag(condition));
        label = std::format("{} / {} to {}", rds::Slot(from).name, copy ? "copy" : "move", where);
    }
    PushSfxEdit(before, label);
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

    // Where the files are, which is not always the slot the cue names. The
    // engine writes the resolved slot into every cue it emits, so this is
    // usually `cue.slot` itself - but a stop carries the ask rather than the
    // resolution, and an unrecorded `scrape_limb_wood` would then report a
    // stand-in for a voice that spent its whole life playing `scrape_limb`.
    out.plays = m_bank.PlaysAs(cue.slot);
    out.fellBack = out.plays != cue.slot;

    out.variantCount = static_cast<int>(m_bank.FileCount(out.plays));
    out.forced = m_bank.ForcedVariant(out.plays) != rds::SoundBank::kNoVariant;

    rds::ResolvedSound resolved{};
    if (!m_bank.Get(out.plays, cue.variant, resolved) || resolved.path.empty()) {
        // Nothing recorded for it and nothing to fall back on, so this cue makes
        // no sound. The engine skips it, and saying so beats leaving the cell
        // blank - which reads as a bug in this column rather than a gap in the
        // pack.
        out.silent = true;
        out.label = "(no recording)";
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
    // Same argument as the two buttons around it: a slot hidden from the
    // timeline is invisible from anywhere but the row it is on, and the take it
    // was hidden for is long since gone. An empty stretch of lane looks exactly
    // like a take with nothing in it, so the count has to be somewhere it cannot
    // be missed.
    if (const auto hidden = static_cast<int>(std::ranges::count(m_slotHidden, true)); hidden > 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::Button(std::format("Show all ({})###showallintimeline", hidden).c_str())) {
            m_slotHidden.fill(false);
        }
        ImGui::PopStyleColor();
        Tip(std::format("{} slot(s) are hidden from the timeline. This puts all of them back on "
                        "it. Drawing only - nothing was silenced, so nothing changes but the "
                        "lane.",
                        hidden));
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
            std::string hay =
                Lower(desc.name) + " " + Lower(rds::ToString(desc.family)) + " " +
                Lower(desc.character);
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

    // Dragging a row to the edge of the view scrolls it.
    //
    // The list is twenty-nine slots long and a drag is one button held down, so
    // without this the only reachable slots are the ones already on screen -
    // and the slot you want is below the fold about as often as not. The filter
    // box is the other way to shorten the trip, and it is above the child, so
    // it cannot be typed into mid-drag.
    if (const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        payload != nullptr && payload->IsDataType(kRowPayload)) {
        const ImVec2 min = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        constexpr float kEdge = 32.0f;
        const bool inside = mouse.x >= min.x && mouse.x <= min.x + size.x &&
                            mouse.y >= min.y - kEdge && mouse.y <= min.y + size.y + kEdge;
        if (inside) {
            const float step = 900.0f * ImGui::GetIO().DeltaTime;
            if (mouse.y < min.y + kEdge) {
                ImGui::SetScrollY(ImGui::GetScrollY() - step);
            } else if (mouse.y > min.y + size.y - kEdge) {
                ImGui::SetScrollY(ImGui::GetScrollY() + step);
            }
        }
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
    // Not the same switch as the per-file mutes below: those answer "is this the
    // right file", this answers "does the mix need this layer at all" - the
    // question the design keeps asking, since its central claim is that muting
    // imp_sub should take the gnarl with it.
    //
    // Writes the same flag the [Layers] and [Surfaces] panels do, through
    // rds::LayerMute, so the two are one switch shown twice. Like every mute it
    // lands at render, which is what makes it an honest A/B.
    if (const bool* mute = rds::LayerMute(m_side[m_focusSide].cfg, slot); mute != nullptr) {
        const bool audible = *mute;
        ImGui::SameLine();
        if (!audible) ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::SmallButton(audible ? "mute" : "muted")) {
            ConfigSide& s = m_side[m_focusSide];
            const rds::AlgorithmConfig before = s.cfg;
            *rds::LayerMute(s.cfg, slot) = !audible;
            // A surface skin's mute lives in that class's block, and writing to
            // a *closed* block is writing somewhere the next Resolve overwrites.
            // Silencing one floor is a setting of its own, so say so: open it.
            // Without this the button would appear to work and then quietly
            // undo itself the next time anything else was touched.
            if (const rds::SurfaceClass surface = rds::SurfaceOfSlot(rds::MuteOwner(slot));
                surface != rds::SurfaceClass::kCount) {
                s.cfg.surfaces.opened[static_cast<std::size_t>(surface)] = true;
                s.cfg.surfaces.Resolve();
            }
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

    // Whether this slot's cues are drawn on the timeline.
    //
    // Beside the mute on purpose, because the two questions are asked in the
    // same breath and answered in opposite places: "does the mix need this
    // layer" is a mute and reaches the render, "can I see past this layer" is
    // this and reaches nothing. A slide hidden here is still audible, still in
    // the table, still in the export - the lane just stops drawing over the
    // impacts underneath it.
    if (const auto index = static_cast<std::size_t>(slot); index < m_slotHidden.size()) {
        ImGui::SameLine();
        bool shown = !m_slotHidden[index];
        if (!shown) ImGui::PushStyleColor(ImGuiCol_Text, kDirty);
        if (ImGui::Checkbox("timeline", &shown)) {
            m_slotHidden[index] = !shown;
        }
        if (!shown) ImGui::PopStyleColor();
        Tip(std::format(
            "Draw {}'s cues on the timeline. Off hides its bars and, for a loop slot, "
            "its envelope - which is the point of it: a long slide's scrape envelopes "
            "cover most of the lane and the impacts you are tuning are underneath "
            "them.\n\nDrawing only. Nothing here changes the cue list, the table, the "
            "export or what you hear - unlike `mute`, which silences the layer for "
            "real. Remembered between launches.",
            desc.name));
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
        // Whatever the popover last put there is not what this button means.
        m_pendingConditionSlot = -1;
        m_browser.OpenForSlot(slot, -1, {}, assignment.looping);
    }
    Tip("Assign another file to this slot. The engine picks between a slot's files with a shuffle "
        "bag, so three files means no immediate repeats - which is what imp_body_01/02/03 was "
        "doing before this panel existed.");

    // The same gesture with the condition chosen first.
    //
    // First the grid, then the library, in that order on purpose: what a
    // recording is *for* is the thing you know before you go looking for it,
    // and asking afterwards would mean picking a sound and then being
    // interrogated about it. The picker that opens is the ordinary one, so the
    // A/B against what the slot plays now still works - it is the landing that
    // differs, not the choosing.
    ImGui::SameLine();
    if (ImGui::SmallButton("+ variant")) {
        ImGui::OpenPopup("addvariant");
    }
    Tip("Assign a file that only plays on some contacts - one recording for stone, or for plate, "
        "or for plate on stone. It beats the plain files where it matches and is invisible where "
        "it does not.\n\nA condition is a preference and never a mute: if nothing on the slot "
        "matches the contact, the slot plays its whole set rather than going quiet.\n\n"
        "It may be a file the slot already plays. That puts it on the slot twice - once plain, "
        "once tagged - so it stays one option among the rest everywhere and becomes the only one "
        "where the tag matches. The two rows are separate: re-tag or remove one and the other "
        "stays as it was.");
    if (ImGui::BeginPopup("addvariant")) {
        ImGui::TextDisabled("what is the new one for?");
        ImGui::Spacing();
        rds::VariantCondition chosen{};
        if (DrawConditionGrid(chosen, false)) {
            m_pendingConditionSlot = static_cast<int>(slot);
            m_pendingCondition = chosen;
            m_browser.OpenForSlot(slot, -1, {}, assignment.looping);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (assignment.files.empty()) {
        ImGui::Indent(14.0f);
        const rds::SlotId plays = m_bank.PlaysAs(slot);
        if (plays != slot) {
            // What the game is doing right now, which is neither silence nor a
            // stand-in. Said in the panel where the slot is filled, because this
            // is where somebody decides whether it still needs a recording.
            ImGui::TextDisabled("nothing assigned - plays %s until one is",
                                std::string(rds::Slot(plays).name).c_str());
            Tip(std::format(
                "An unrecorded surface variant is not a silent slot and not a synthesised one: it "
                "resolves to {}, which is what makes colouring a floor a file drop and nothing "
                "else.\n\nBefore that it scans the built pack for sounds\\{}_NN.wav, so a file "
                "named after the slot still wins.",
                rds::Slot(plays).name, desc.name));
        } else {
            ImGui::TextDisabled("nothing assigned - falls back to sounds\\%s_NN.wav",
                                std::string(desc.name).c_str());
            Tip("An empty slot is not a silent one. The bank scans the built pack for files named "
                "after the slot, which is what the mod did before this file existed.");
        }
        ImGui::Unindent(14.0f);
    }

    // Drawn in groups - the plain files first, then one block per condition -
    // while every row keeps the index it has in `files`. Grouping is a way of
    // reading the list and must not be a way of re-ordering it: that index is
    // the variant a recorded cue carries, and moving one would change which
    // sound every take in the corpus plays.
    std::vector<int> order;
    order.reserve(assignment.files.size());
    std::vector<rds::VariantCondition> groups;
    for (int i = 0; i < static_cast<int>(assignment.files.size()); ++i) {
        const rds::VariantCondition cond = assignment.ConditionAt(static_cast<std::size_t>(i));
        if (cond.Unconditional()) {
            order.push_back(i);
        } else if (std::ranges::none_of(groups, [&](const rds::VariantCondition& seen) {
                       return SameCondition(seen, cond);
                   })) {
            groups.push_back(cond);
        }
    }
    for (const rds::VariantCondition& group : groups) {
        for (int i = 0; i < static_cast<int>(assignment.files.size()); ++i) {
            if (SameCondition(assignment.ConditionAt(static_cast<std::size_t>(i)), group)) {
                order.push_back(i);
            }
        }
    }

    const rds::SlotResolutionConfig& resolution = m_side[m_focusSide].cfg.slots;
    int removeAt = -1;
    // Where a dropped row came from, and what this slot is about to ask of it.
    // Collected rather than acted on, for the same reason `removeAt` is: the
    // edit renumbers the list the loop is walking.
    RowDrag dropFrom{};
    rds::VariantCondition dropCond{};
    rds::VariantCondition heading{};
    for (const int i : order) {
        const std::string& file = assignment.files[static_cast<std::size_t>(i)];
        const rds::VariantCondition cond = assignment.ConditionAt(static_cast<std::size_t>(i));
        if (!cond.Unconditional() && !SameCondition(cond, heading)) {
            // Whether this group is being honoured *right now*, on the side in
            // focus. A grid full of overrides and a switch off in [Slots] looks
            // exactly like a grid full of overrides that work, and the panel is
            // the only place the two can be told apart.
            const bool honoured =
                resolution.conditionalVariants &&
                (cond.surface == rds::SurfaceMatch::kAny || resolution.surfaceConditions) &&
                (cond.coverage == rds::CoverageMatch::kAny || resolution.armorConditions);
            ImGui::Indent(14.0f);
            if (honoured) {
                ImGui::TextDisabled("- %s -", ConditionWords(cond).c_str());
            } else {
                ImGui::TextColored(kDirty, "- %s - ignored, see [Slots] -",
                                   ConditionWords(cond).c_str());
            }
            // The heading is a target of its own, so a group with no rows you
            // want to point at is still somewhere you can drop onto.
            if (RowDrag from; AcceptRowDrop(from)) {
                dropFrom = from;
                dropCond = cond;
            }
            Tip(honoured
                    ? std::format("`{}` in the ini. These beat the plain files above where the "
                                  "contact matches, and drop out where it does not.\n\nTwo files "
                                  "tagged the same take turns with each other, exactly as the "
                                  "plain ones do.",
                                  ConditionTag(cond))
                    : std::format("`{}` in the ini, and side {} is not honouring it: one of "
                                  "bConditionalVariants, bSurfaceConditions or bArmorConditions is "
                                  "off in [Slots]. These files are still on the slot and still "
                                  "picked - just as though they were plain.",
                                  ConditionTag(cond), static_cast<char>('A' + m_focusSide)));
            ImGui::Unindent(14.0f);
        }
        heading = cond;
        ImGui::PushID(i);
        ImGui::Indent(14.0f);
        // The row as one item, so it can be dropped onto as one thing.
        ImGui::BeginGroup();

        sfxui::PreviewButton("play", m_library, file, m_player, 48000);
        ImGui::SameLine();

        const rds::SfxEntry* entry = m_library.Find(file);
        if (entry == nullptr) {
            ImGui::TextColored(kBad, "%s", file.c_str());
            RowDragSource(slot, i, file);
            Tip("Named by the ini but not in the library - deleted, renamed on disk, or the "
                "library folder moved. The slot plays one fewer variant than it says.");
        } else {
            // Greyed rather than hidden while muted: the point of suspending one
            // variant is to keep looking at it while it is not playing.
            const bool quiet = SfxMuted(slot, file);
            if (quiet) ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
            ImGui::TextUnformatted(entry->name.c_str());
            if (quiet) ImGui::PopStyleColor();
            RowDragSource(slot, i, entry->name);
            Tip(std::format("{}\n\nvariant {} of {}{}. The order is the variant index a recorded "
                            "cue carries, so re-ordering changes which file a given cue plays."
                            "\n\nDrag this name onto another slot to move it there, or onto one "
                            "of a slot's `- only on ... -` headings to move it there as that "
                            "variant. Hold Ctrl while dropping to copy instead, which is how one "
                            "recording becomes both a plain option and the tagged one.",
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

        // What this file is for, and the way to change it. On every row rather
        // than only on the tagged ones, because "this one is really the stone
        // take" is a thing you find out by listening - long after the file was
        // assigned - and there would otherwise be no way to say so.
        ImGui::SameLine();
        const bool tagged = !cond.Unconditional();
        if (tagged) ImGui::PushStyleColor(ImGuiCol_Text, kSuggest);
        if (ImGui::SmallButton(tagged ? ConditionTag(cond).c_str() : "for...")) {
            ImGui::OpenPopup("retag");
        }
        if (tagged) ImGui::PopStyleColor();
        Tip(tagged ? std::format("{}. Click to change it, or clear it with the top-left cell.",
                                 ConditionWords(cond))
                   : std::string("Plain: this one is a candidate on every contact. Click to make "
                                 "it a variant for one kind of contact instead."));
        if (ImGui::BeginPopup("retag")) {
            ImGui::TextDisabled("what is this one for?");
            ImGui::Spacing();
            rds::VariantCondition chosen{};
            if (DrawConditionGrid(chosen, true)) {
                const rds::SfxAssignments before = m_sfx;
                // This row, not this filename: the slot may play the same file
                // twice, plain in the set and tagged for one kind of contact,
                // and re-tagging one of them must leave the other alone.
                m_sfx.For(slot).SetConditionAt(static_cast<std::size_t>(i), chosen);
                PushSfxEdit(before, std::format("{} / {} for {}", desc.name, file,
                                                chosen.Unconditional() ? std::string("anything")
                                                                       : ConditionTag(chosen)));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("change")) {
            m_pendingConditionSlot = -1;
            m_browser.OpenForSlot(slot, i, file, assignment.looping);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = i;
        }
        Tip("Take this file off the slot.");

        ImGui::EndGroup();
        // Dropping on a row means "here": the file lands under the same heading
        // this row sits under, which is the one you were pointing at.
        if (RowDrag from; AcceptRowDrop(from)) {
            dropFrom = from;
            dropCond = cond;
        }

        ImGui::Unindent(14.0f);
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        const rds::SfxAssignments before = m_sfx;
        // Takes the row's condition with it, and the mute only when that was
        // the last placement of the file - the two rules live in RemoveAt so
        // every caller gets them.
        m_sfx.For(slot).RemoveAt(static_cast<std::size_t>(removeAt));
        PushSfxEdit(before, std::string(desc.name) + " / remove");
    }

    ImGui::EndGroup();

    // Hovering the widget lights up this slot's bars in the timeline, which is
    // the whole point of having both on screen: "what does this slot actually
    // do in this take" is otherwise a question you answer by squinting.
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // The whole widget, so the name, the buttons and the space beside the rows
    // are all somewhere a drop lands - and landing there the file is plain,
    // whatever the rows underneath happen to ask for. A row or a heading is a
    // smaller rectangle inside this one and wins where the cursor is over it.
    if (RowDrag from; AcceptRowDrop(from)) {
        dropFrom = from;
        dropCond = {};
    }

    if (hovered) {
        m_hoverSlotPending = static_cast<int>(slot);
    }

    // After the loop and after both targets, because it renumbers the list the
    // loop walked. Ctrl is read at the drop rather than at the pick-up: the
    // preview tooltip has been saying which of the two this is the whole way
    // across, so the answer is whatever it said last.
    if (dropFrom.index >= 0) {
        DropSfxPlacement(static_cast<rds::SlotId>(dropFrom.slot), dropFrom.index, slot, dropCond,
                         ImGui::GetIO().KeyCtrl);
    }

    ImGui::Separator();
    ImGui::PopID();
}

}  // namespace tb
