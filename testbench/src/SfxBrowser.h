#pragma once

// The library window: everything that exists, what it measures, and picking one
// for a slot.
//
// It opens two ways and they are the same window. From the header bar it is a
// browser - search, listen, rename, annotate, import. From a slot in the sfx
// panel it is a picker, which is the browser plus two things: a pair of preview
// widgets at the top holding what the slot plays now against what is
// highlighted, and an order that puts the sounds of a fitting length first.
//
// Those two extra things are the whole reason this is one window rather than a
// dropdown. Choosing an sfx for `imp_body` is not a lookup, it is an A/B, and an
// A/B needs both sounds one keypress apart.

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

    void Init(rds::SfxLibrary* library, Player* player, int sampleRate);

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

    /// Names and notes edited here are unsaved until this is called. The
    /// measurements are not: those are written the moment they are measured,
    /// because they are not something anybody would want to undo.
    [[nodiscard]] bool Dirty() const { return !m_dirtyFiles.empty(); }
    [[nodiscard]] std::size_t DirtyCount() const { return m_dirtyFiles.size(); }
    void Save();

    /// What the last import run did, for the status line. Cleared when the next
    /// one starts.
    [[nodiscard]] const std::string& ImportNote() const { return m_importNote; }

    /// True once after the set of files in the library changed, so the caller
    /// can rebuild the bank. Consumed by the call: the bank rebuild is not
    /// something to do on every frame that the window happens to be open.
    [[nodiscard]] bool TakeLibraryChanged();

private:
    struct Row {
        std::size_t entry{};  ///< index into the library
        float fit{};          ///< 0..1 against the slot, when picking for one
        bool lengthSuits{};
    };

    void Rebuild();
    void DrawHeader();
    void DrawPickPreviews();
    void DrawList(Pick& pick);
    void DrawRow(const Row& row, int index, Pick& pick);
    void HandleKeys(Pick& pick);
    void RunImport();
    void AdoptPack();
    /// Pack files the library does not already hold, by filename.
    [[nodiscard]] std::vector<std::filesystem::path> PackCandidates() const;
    void MarkDirty(const std::string& file);
    void CommitNote();

    rds::SfxLibrary* m_library{};
    Player* m_player{};
    int m_sampleRate{48000};

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
    std::vector<Row> m_rows;
    int m_highlight{-1};
    bool m_scrollToHighlight{};
    /// True when the library changed under us and the filtered list has to be
    /// rebuilt before the next draw.
    bool m_stale{true};

    /// Which row's note is being typed into, and the buffer it is being typed
    /// into. One at a time: a text box per row would put four hundred live
    /// widgets on screen for a library of four hundred.
    int m_noteRow{-1};
    char m_noteBuffer[512]{};
    int m_renameRow{-1};
    char m_renameBuffer[128]{};

    /// Files whose name or note changed and have not been written yet.
    std::vector<std::string> m_dirtyFiles;

    bool m_fixNames{true};
    std::string m_importNote;
    std::filesystem::path m_packDirectory;
    bool m_libraryChanged{};
};

}  // namespace tb
