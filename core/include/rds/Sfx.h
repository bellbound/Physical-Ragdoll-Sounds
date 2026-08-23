#pragma once

// The sound library, and which of its files each slot plays.
//
// Until now a slot's files were whatever `<slot>_<NN>.wav` happened to be in
// the sounds folder, so changing what `imp_body` plays meant renaming files.
// That is fine while one person builds the pack with a python script and wrong
// the moment the choice is something to audition: you want to hear three
// candidates on the same slot without touching the disk between them.
//
// So the pack is split in two:
//
//   library/          every sfx that exists, named whatever it is called, each
//                     with a `<file>.meta.ini` beside it carrying what the
//                     importer measured and what the user wrote in the note
//   RagdollSounds_SFX.ini
//                     which library files each slot plays, in order
//
// The ini is the source of truth for the game. A slot with no entry falls back
// to the old convention scan, so an install that predates this - or a pack
// dropped in by hand - still works and still sounds the same.
//
// Both halves live in core/ because both halves need them: the testbench writes
// the ini and the DLL reads it, and a second parser on either side is how the
// two come to disagree about what a config sounded like.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rds/SlotManifest.h"

namespace rds {

/// Something measured at import that the file may want fixing for.
///
/// Never fatal. The design's own delivery rules (Slots.md §5) are what these
/// check, and a file that breaks one is still worth hearing - a body layer with
/// 40 ms of lead-in is a timing bug, not a silence. `blocking` marks the one
/// class that is: a file the decoder could not read at all.
struct SfxWarning {
    std::string code;    ///< short badge text, e.g. "lead-in"
    std::string detail;  ///< the whole sentence, shown on hover
    bool blocking{};     ///< true only when the file cannot be played at all
};

/// One file in the library, plus everything known about it.
///
/// `file` is the identity - it is what RagdollSounds_SFX.ini names and what
/// survives a rename in the UI. `name` is only ever shown, so renaming is free
/// and cannot break an assignment.
struct SfxEntry {
    std::string file;  ///< filename inside the library directory, with extension
    std::string name;  ///< display name; starts as the stem, editable
    std::string note;  ///< the user's own note. Searched, never interpreted

    // ── what the container says ──────────────────────────────────────────────
    int sampleRate{};
    int channels{};
    int bitsPerSample{};
    float durationMs{};

    // ── what the samples say. All measured the way tools/sfx.py measures them,
    //    because those are the numbers 03-Asset-Status.md quotes ─────────────
    float peakDb{};       ///< full-file peak, dBFS
    /// Silence in front, measured against an absolute floor rather than against
    /// the peak - see the note in MeasureSfx. A relative measure flags every
    /// file that is *specified* to start softly.
    float leadInMs{};
    float usableMs{};     ///< the event itself, lead-in and room tail removed
    float decay20Ms{};    ///< peak to -20 dB. The "-20 dB in" column
    float centroidHz{};   ///< spectral centroid. A weak discriminator, kept for context
    float tiltDb{};       ///< (sub+low)/2 - (high+air)/2. The honest one
    float dcOffset{};     ///< mean sample value
    int loMidTransients{};  ///< peaks in 250-800 Hz. Thud against crunch
    float seamDb{};       ///< level match between the first and last 50 ms
    float steadyDb{};     ///< envelope spread over the live part
    float grainsPerSec{};  ///< peak rate, for the scrape

    /// True when this measures as a sustained texture rather than an event:
    /// steady envelope, matched seam, no single dominant transient. A looping
    /// slot plays these whole and repeats them, so they are judged as loops and
    /// never complained at for being long.
    bool loops{};

    std::vector<SfxWarning> warnings;
    /// Slots this would suit, best first, from the measured character against
    /// each slot's targets. A suggestion, never a restriction.
    std::vector<SlotId> suggested;

    [[nodiscard]] bool Blocked() const;
    /// The stem, which is what a fresh import names it.
    [[nodiscard]] std::string Stem() const;
};

/// Every file under the library directory, with its metadata.
///
/// The game loads this once and only reads it. The testbench also writes it -
/// importing, renaming and note-taking all end in SaveMeta for one entry.
class SfxLibrary {
public:
    /// Scan `directory` for playable files and read each one's `.meta.ini`.
    ///
    /// A file with no sidecar is still listed, with only what the container
    /// says about it: a wav dropped in by hand is a usable sfx, it just has no
    /// measurements until something analyses it. Logs the count at info.
    void Load(const std::filesystem::path& directory);

    [[nodiscard]] const std::filesystem::path& Directory() const { return m_directory; }
    [[nodiscard]] std::span<const SfxEntry> Entries() const { return m_entries; }
    /// For the browser, which edits names and notes in place and writes them on
    /// focus loss. Nothing on the game side has any business here.
    [[nodiscard]] std::span<SfxEntry> MutableEntries() { return m_entries; }
    [[nodiscard]] std::size_t Size() const { return m_entries.size(); }

    /// By filename, which is the identity the ini stores. Null when the library
    /// does not have it - an assignment naming a file somebody deleted.
    [[nodiscard]] const SfxEntry* Find(std::string_view file) const;
    [[nodiscard]] SfxEntry* Find(std::string_view file);

    /// Absolute path of a library file, whether or not it is indexed.
    [[nodiscard]] std::filesystem::path PathOf(std::string_view file) const;

    /// Add or replace one entry and write its sidecar. Used by the importer and
    /// by every edit in the browser.
    void Upsert(const SfxEntry& entry);

    /// Write one entry's `.meta.ini`. Called on its own when only the note or
    /// the name changed and nothing needs re-measuring.
    bool SaveMeta(const SfxEntry& entry) const;

    /// Read one sidecar into `out`, which must already carry `file`. False when
    /// there is no sidecar; `out` is left as it was.
    [[nodiscard]] static bool LoadMeta(const std::filesystem::path& wav, SfxEntry& out);

    /// The sidecar's path for a library file: `<file>.meta.ini`, so it sorts
    /// next to the audio and a directory listing pairs them without thinking.
    [[nodiscard]] static std::filesystem::path MetaPathFor(const std::filesystem::path& wav);

private:
    std::filesystem::path m_directory;
    std::vector<SfxEntry> m_entries;
};

/// What one slot plays.
struct SlotAssignment {
    /// Library filenames, in order. The order is the variant index a cue
    /// carries, so re-ordering this changes which file a given cue plays.
    std::vector<std::string> files;

    /// Whether this slot's sound is a sustained texture the engine repeats.
    ///
    /// Defaults to the manifest's own `isLoop` and is overridable per slot,
    /// because "is this a loop" is a property of what got assigned as much as
    /// of the slot: a sliding or wind-like sound wants repeating and must not
    /// be complained at for being long, and a slot fed one wants to say so.
    bool looping{};

    [[nodiscard]] bool Empty() const { return files.empty(); }
};

/// The slot-to-file table, as read from and written to RagdollSounds_SFX.ini.
class SfxAssignments {
public:
    /// Every slot at its manifest default: no files, `looping` from the slot.
    SfxAssignments();

    /// Read `file`. Returns how many slots carried an assignment; 0 means the
    /// file was missing or empty, which is the signal to fall back to the old
    /// convention scan rather than to play nothing.
    std::size_t Load(const std::filesystem::path& file);

    /// Write every slot, with the slot's brief and its recommended length as
    /// comments - the same self-documenting shape the other two inis have.
    bool Save(const std::filesystem::path& file) const;

    [[nodiscard]] const SlotAssignment& For(SlotId slot) const;
    [[nodiscard]] SlotAssignment& For(SlotId slot);

    /// True when no slot names a file, so nothing here can drive the bank.
    [[nodiscard]] bool Empty() const;

    /// How many slots name at least one file.
    [[nodiscard]] std::size_t AssignedSlots() const;

    /// Fill from `<slot>_<NN>` filenames in a library, which is how the
    /// existing pack becomes a set of assignments without anybody clicking
    /// twenty-nine times. Only fills slots that are currently empty.
    void SeedFromNames(const SfxLibrary& library);

    /// Every file named by any slot. Used to tell an orphan in the library from
    /// something in use.
    [[nodiscard]] bool IsUsed(std::string_view file) const;

    [[nodiscard]] bool operator==(const SfxAssignments& other) const;

private:
    SlotAssignment m_slots[static_cast<std::size_t>(SlotId::kCount)];
};

}  // namespace rds
