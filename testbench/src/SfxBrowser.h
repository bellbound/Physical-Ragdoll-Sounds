#pragma once

// The library window: everything that exists, what it measures, and picking one
// for a slot.
//
// It opens two ways and they are the same window. From the header bar it is a
// browser - search, sort, listen, rename, import. From a slot in the sfx
// panel it is a picker, which is the browser plus three things: a pair of
// preview widgets at the top holding what the slot plays now against what is
// highlighted, an order that puts the sounds of a fitting length first, and the
// in-take audition - the highlighted file dropped into the slot so the take
// plays with it in place.
//
// Those three things are the whole reason this is one window rather than a
// dropdown. Choosing an sfx for `imp_body` is not a lookup, it is an A/B, and an
// A/B needs both sounds one keypress apart - and needs them heard where they
// land, because a candidate that is right on its own is regularly wrong under
// the transient that arrives 10 ms before it.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Player.h"
#include "rds/Sfx.h"
#include "rds/SlotManifest.h"

namespace tb {

/// Pieces the slot panel draws too, so a library row and a slot's row read as
/// the same kind of thing without being the same widget.
namespace sfxui {

/// Text with every occurrence of `needle` in the search colour.
///
/// Drawn rather than formatted because ImGui has no rich text: the string is cut
/// at each match and the pieces are laid out on one line. `needle` empty draws
/// the text plainly.
void HighlightedText(std::string_view text, std::string_view needle);

/// The metadata badges: duration, loop, warnings, suggested slots. Returns the
/// height it used so a caller laying out rows by hand can account for it.
void Badges(const rds::SfxEntry& entry, bool showSuggestions);

/// A play/stop button for one library file, wired to the player's preview voice.
/// `id` disambiguates two buttons for the same file in one frame - the picker
/// draws the current sound both at the top and in the list.
void PreviewButton(const char* id, const rds::SfxLibrary& library, const std::string& file,
                   Player& player, int sampleRate);

/// `1.2 s` / `340 ms`, whichever reads better at that length.
[[nodiscard]] std::string Duration(float ms);

/// Decode one library file at `sampleRate`, ready for the preview voice. Null
/// when it will not decode, which is what the `unreadable` warning is about.
[[nodiscard]] std::shared_ptr<PreviewClip> LoadPreview(const rds::SfxLibrary& library,
                                                       const std::string& file, int sampleRate);

}  // namespace sfxui

class SfxBrowser {
public:
    /// What the browser hands back when the user picks something.
    struct Pick {
        bool made{};
        rds::SlotId slot{};
        /// Which of the slot's files to replace, or -1 to add one.
        int variant{-1};
        std::string file;
    };

    /// What the highlight should be heard as, while picking for a slot.
    ///
    /// The caller substitutes `file` for everything that slot plays and re-mixes
    /// the take, so arrowing down the list auditions each candidate in its real
    /// place. Inactive while browsing, while nothing is highlighted, and when
    /// the audition checkbox is off - which is there because the substitution
    /// costs a re-mix of the whole take on every keypress.
    struct Audition {
        bool active{};
        rds::SlotId slot{};
        std::string file;
    };
    [[nodiscard]] Audition InTakeAudition() const;

    /// `assignments` is read and never written: the window needs it to say how
    /// many slots a file is on before deleting it, and taking it off them is
    /// the caller's job because that edit belongs on the undo stack.
    /// `previewGainDb` is the audition volume, owned by the caller so it can be
    /// written into the ui prefs beside every other switch that is remembered.
    void Init(rds::SfxLibrary* library, const rds::SfxAssignments* assignments, Player* player,
              int sampleRate, float* previewGainDb);

    /// The built pack, for the one-click adopt. Its `<slot>_<NN>.wav` files are
    /// the sounds the mod is playing today, so importing them is how a fresh
    /// library stops being empty without anybody hunting for the folder.
    void SetPackDirectory(std::filesystem::path directory);

    /// Open as a browser, with nothing selected for anything.
    void Open();

    /// Open to choose a sound for one slot. `current` is what that variant plays
    /// now, shown at the top to compare against; empty when adding a new one.
    void OpenForSlot(rds::SlotId slot, int variant, std::string current, bool slotLoops);

    void Close();
    [[nodiscard]] bool IsOpen() const { return m_open; }

    /// Draw the window if it is open. The returned Pick is set on the one frame
    /// the user chose something.
    [[nodiscard]] Pick Draw();

    /// True while the window has focus and is using the arrow keys and the
    /// space bar, so the main window stands its own shortcuts down. Without
    /// this, space would toggle the take's transport at the same time as it
    /// auditions the highlighted sound.
    [[nodiscard]] bool WantsKeys() const;

    /// Names and mutes edited here are unsaved until this is called. The
    /// measurements are not: those are written the moment they are measured,
    /// because they are not something anybody would want to undo.
    [[nodiscard]] bool Dirty() const { return !m_dirtyFiles.empty(); }
    [[nodiscard]] std::size_t DirtyCount() const { return m_dirtyFiles.size(); }
    void Save();

    /// What the last import, adopt, re-measure or delete did, for the status
    /// line. Cleared when the next one starts.
    [[nodiscard]] const std::string& ImportNote() const { return m_importNote; }

    /// Files deleted since this was last called, in the order they went.
    ///
    /// Emptied by the call. The library and the disk are already done with
    /// them - what is left is the assignments, which the browser does not own:
    /// taking a file off its slots is an edit somebody will want back with
    /// Ctrl+Z, and the undo stack lives with the panel that owns it.
    [[nodiscard]] std::vector<std::string> TakeDeleted();

    /// True once after the set of files in the library changed, so the caller
    /// can rebuild the bank. Consumed by the call: the bank rebuild is not
    /// something to do on every frame that the window happens to be open.
    [[nodiscard]] bool TakeLibraryChanged();

private:
    /// Where the search hit, which is the order the list comes back in: a name
    /// match is what somebody typing three letters meant, and a badge match is
    /// the whole `imp_body` band arriving on top of it.
    enum class Match { kName = 0, kTag = 1 };

    /// What orders the list inside whatever bands the search and the picker
    /// impose. Newest first is the default: the sound you are looking for is
    /// nearly always one you just brought in, and no part of a name says so.
    /// `kName` is the library's own alphabetical order, for when it is.
    ///
    /// This never outranks a band. A slot's fitting lengths still come first
    /// while picking, a name hit still beats a badge hit while searching, and
    /// muted is still last - newest only decides the order *within* each of
    /// those, which is the one place the date is the useful question.
    enum class Sort { kName = 0, kNewest = 1, kOldest = 2 };

    struct Row {
        std::size_t entry{};  ///< index into the library
        float fit{};          ///< 0..1 against the slot, when picking for one
        bool lengthSuits{};
        Match match{Match::kName};
        bool disabled{};  ///< muted, and therefore last whatever else it is
    };

    void Rebuild();
    void DrawHeader();
    void DrawPickPreviews();
    void DrawList(Pick& pick);
    void DrawRow(const Row& row, int index, Pick& pick);
    void HandleKeys(Pick& pick);
    void RunImport();
    void AdoptPack();
    /// The confirmation, drawn every frame and open only when something asked
    /// for it. A modal, because the question is "is this the file you meant"
    /// and the answer is unreadable from a row that has already scrolled.
    void DrawDeleteConfirm();
    /// Recycle one library file and its sidecar, drop the entry, and queue the
    /// name for TakeDeleted. A shell refusal leaves everything as it was.
    void DeleteEntry(const std::string& file);
    /// Slots that name `file`, by name, for the confirmation's warning.
    [[nodiscard]] std::vector<std::string> SlotsUsing(const std::string& file) const;
    /// Pack files the library does not already hold, by filename.
    [[nodiscard]] std::vector<std::filesystem::path> PackCandidates() const;
    void MarkDirty(const std::string& file);
    /// `imported 2026-08-23 19:41 - 2 days ago`, or the empty string for an
    /// entry whose date nothing knows.
    [[nodiscard]] static std::string ImportedLine(const rds::SfxEntry& entry);
    /// Mute or unmute one library entry: writes the flag, re-sorts the list and
    /// tells the caller to rebuild the bank, because muting is a change to what
    /// the take plays and not just to what the window looks like.
    void ToggleDisabled(std::size_t entry);
    /// The `disable` / `enable` button, drawn the same in the list and in the
    /// pair of previews at the top.
    void DisableButton(std::size_t entry);
    /// The two corrections - pitch and trim - on their own line under the name.
    ///
    /// A correction to the recording, so it belongs to the file and not to the
    /// assignment: it applies wherever the sound is used, exactly like the mute
    /// beside it. Writes to the metadata file, so it wants a Ctrl+S like a
    /// rename does, and rebuilds the bank so the next block is the new sound.
    void CorrectionRow(std::size_t entry);
    /// Write the row being renamed back into the library, if there is one. Called
    /// before anything that moves the rows out from under it.
    void CommitEdits();

    rds::SfxLibrary* m_library{};
    const rds::SfxAssignments* m_assignments{};
    Player* m_player{};
    int m_sampleRate{48000};
    /// The audition volume in dB, owned by the caller. Null until Init.
    float* m_previewGainDb{};

    bool m_open{};
    bool m_focusSearch{};
    bool m_hasFocus{};

    /// Set when the window was opened for a slot rather than to browse.
    bool m_picking{};
    rds::SlotId m_slot{};
    int m_variant{-1};
    std::string m_current;
    bool m_slotLoops{};

    char m_search[128]{};
    Sort m_sort{Sort::kNewest};
    std::vector<Row> m_rows;
    int m_highlight{-1};
    bool m_scrollToHighlight{};
    /// True when the library changed under us and the filtered list has to be
    /// rebuilt before the next draw.
    bool m_stale{true};

    /// Which row is being renamed, and the buffer it is being typed into. One at
    /// a time: a text box per row would put four hundred live widgets on screen
    /// for a library of four hundred.
    int m_renameRow{-1};
    char m_renameBuffer[128]{};

    /// Files whose name or mute changed and have not been written yet.
    std::vector<std::string> m_dirtyFiles;

    bool m_fixNames{true};
    /// Drop the highlighted file into the slot and re-mix the take. On, because
    /// it is the reason the picker knows which slot it was opened from.
    bool m_auditionInTake{true};
    std::string m_importNote;
    std::filesystem::path m_packDirectory;
    bool m_libraryChanged{};

    /// The file the delete confirmation is asking about, and the one-frame flag
    /// that opens it. Two fields because ImGui wants OpenPopup called from the
    /// window's own id scope and the button that asks is inside a row's.
    std::string m_confirmFile;
    bool m_openConfirm{};
    /// Deleted and not yet handed to the caller.
    std::vector<std::string> m_deleted;
};

}  // namespace tb
