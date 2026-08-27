#pragma once

// The sound library, and which of its files each slot plays.
//
// A slot's files used to be whatever `<slot>_<NN>.wav` happened to be in the
// sounds folder, so changing what `imp_body` plays meant renaming files - fine
// while a script builds the pack, wrong the moment you want to audition three
// candidates on one slot. So the pack is split in two:
//
//   library/          every sfx that exists, named whatever it is called, each
//                     with a `<file>.meta.ini` beside it carrying what the
//                     importer measured and when it came in
//   RagdollSounds_SFX.ini
//                     which library files each slot plays, in order
//
// The ini is the source of truth for the game. A slot with no entry falls back to
// the old convention scan, so an install that predates this still sounds the same.
//
// Both halves live in core/ because the testbench writes the ini and the DLL reads
// it, and a second parser on either side is how the two come to disagree.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rds/SlotManifest.h"

namespace rds {

/// Something measured at import that the file may want fixing for. Mostly not
/// fatal - these check Slots.md §5's delivery rules, and a body layer with 40 ms
/// of lead-in is a timing bug, not a silence. Two flags mark the exceptions:
///
///   `blocking`  the decoder could not read it. There is no sound here at all.
///   `dead`      it plays, and nothing anyone can do makes it usable: already
///               flat-topped, hiss 29 dB under the hero, two channels from
///               different takes. The answer is to re-source, never to process.
///
/// Everything else is advisory, and anything the import pass can fix is fixed on
/// the way in - so a warning here is one that survived the repair.
struct SfxWarning {
    std::string code;    ///< short badge text, e.g. "lead-in"
    std::string detail;  ///< the whole sentence, shown on hover
    bool blocking{};     ///< true only when the file cannot be played at all
    bool dead{};         ///< true when it plays but cannot be made right
};

/// One file in the library, plus everything known about it. `file` is the identity
/// - what RagdollSounds_SFX.ini names - and `name` is only ever shown, so renaming
/// is free and cannot break an assignment.
struct SfxEntry {
    std::string file;  ///< filename inside the library directory, with extension
    std::string name;  ///< display name; starts as the stem, editable

    /// When this file entered the library, in seconds since the unix epoch. Written
    /// once at import and never moved: a re-measure keeps it and so does a rename.
    /// The browser's newest-first order sorts on it, which is the order that
    /// matters the evening after a download.
    ///
    /// Zero only while nothing knows. Anything predating this field gets the audio
    /// file's modification time, which for an imported file is when it was copied.
    std::int64_t importedAt{};

    /// Muted: still in the library, still named by whatever slots name it, and not
    /// played. Unlike taking the file off a slot, this is not a decision about the
    /// slot - it suspends the sound everywhere at once and remembers every
    /// placement, so a sound that turned out wrong can be silenced without losing
    /// the work of having placed it.
    bool disabled{};

    // ── the corrections. Yours, like `name` and `disabled` ───────────────────
    //
    // A correction to *the recording*, which is why it lives here and not on the
    // assignment: a scrape loop pitched too low is wrong wherever it is used, so it
    // is corrected once. If two slots genuinely want one file at two levels, that
    // is a decision about the slots and `SlotGain:f<Slot>` is the control.
    //
    // Both are identities at their defaults.

    /// Playback rate, x. 1 is the file as recorded, 1.09 a semitone and a half up.
    /// Multiplies the pitch the engine already chose rather than replacing it, so a
    /// correction and a variation stay separate things.
    ///
    /// **This changes a one-shot's length** - pitch here is resampling, not a
    /// formant shift. Check length specs against `EffectiveDurationMs()`.
    float pitch{1.0f};

    /// Level, dB. Applied at Stage 5 with the other trims. It cannot change what
    /// was chosen, structurally: the bank does not resolve a layer to a *file*
    /// until `Emit`, long after Stage 4 has sorted, rate-capped and burst-shaped.
    float trimDb{};

    // ── what the container says ──────────────────────────────────────────────
    int sampleRate{};
    int channels{};
    int bitsPerSample{};
    float durationMs{};

    // ── what the samples say. All measured the way tools/sfx.py measures them,
    //    because those are the numbers 03-Asset-Status.md quotes ─────────────
    float peakDb{};       ///< full-file peak, dBFS
    /// Silence in front, measured against an absolute floor rather than the peak
    /// (see MeasureSfx): a relative measure flags every file *specified* to start
    /// softly.
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

    // ── the technical rule-outs. All measured the way tools/sfx.py and
    //    tools/triage_batch.py measure them, because their thresholds are what
    //    03-Asset-Status.md §7 ruled four cuts out on ────────────────────────

    /// Peak minus the quietest 50 ms in front of the attack, in dB - `sfx.py`'s
    /// `snr`, whose 30 dB gate dropped `twisting…gristle_variation2`. 99 when there
    /// is no pre-roll long enough to measure in, which is not the same as clean.
    float noiseFloorDb{99.0f};

    /// Percentage of samples sitting inside a full-scale flat top - the one level
    /// fault the import pass cannot repair. Gain moves a peak; it does not put back
    /// the tops of waves squared off before the file was written.
    float clipPct{};
    /// How many separate flat tops. The discriminator, not the percentage: a
    /// decaying sub sweep touches its maximum once, a clipped impact on every crest.
    int clipRuns{};

    /// Contacts at least 46 ms apart and within 32 dB of the loudest - the
    /// reference rate floor (04 §2), where two impacts stop resolving as two. More
    /// than one is not a fault: it is what `sfx.py split` exists for.
    int contacts{};

    /// The loudest thing after the hero contact, relative to it, and when. 39 takes
    /// in 100 carry one, 21-34 dB down and usually a bright debris wash (03 §7);
    /// left in, it lands on whatever the engine schedules next and reads as a flam.
    float satelliteDb{};
    float satelliteAtMs{};

    /// Energy above 16 kHz against the 2.5-8 kHz band, in dB. Far down means the
    /// file was upsampled or heavily lossy whatever its container claims.
    float topOctaveDb{};

    /// FNV-1a over the decoded samples. Two files with the same hash are the same
    /// sound under two names, which 03 §7 hit twice in one batch of 102.
    std::uint64_t contentHash{};

    /// True when this measures as a sustained texture rather than an event: steady
    /// envelope, matched seam, no single dominant transient. Judged as a loop, so
    /// never complained at for being long.
    bool loops{};

    std::vector<SfxWarning> warnings;
    /// Slots this would suit, best first, from the measured character against
    /// each slot's targets. A suggestion, never a restriction.
    std::vector<SlotId> suggested;

    [[nodiscard]] bool Blocked() const;
    /// Carries a warning nothing can repair. Still assignable - the library never
    /// refuses - but the honest answer is another file.
    [[nodiscard]] bool Dead() const;
    /// The stem, which is what a fresh import names it.
    [[nodiscard]] std::string Stem() const;

    /// How long this file actually plays for, `pitch` included - the number a
    /// length spec has to be checked against. `durationMs` stays what the container
    /// says, but pitch here is resampling, so a one-shot corrected to 1.09x is 8%
    /// shorter than the wav on disk.
    [[nodiscard]] float EffectiveDurationMs() const {
        return pitch > 0.0f ? durationMs / pitch : durationMs;
    }

    /// True when either correction is doing anything. The ordinary case is
    /// neither, and both are identities there.
    [[nodiscard]] bool Corrected() const { return pitch != 1.0f || trimDb != 0.0f; }
};

/// `2026-08-23 19:41` in local time, for an `importedAt`. Empty for 0, so a caller
/// printing an unknown import time gets nothing rather than 1970.
[[nodiscard]] std::string FormatImportTime(std::int64_t unixSeconds);

/// Every file under the library directory, with its metadata. The game loads it
/// once and only reads it; the testbench also writes it, since importing,
/// renaming and muting all end in SaveMeta for one entry.
class SfxLibrary {
public:
    /// Scan `directory` for playable files and read each one's `.meta.ini`. A file
    /// with no sidecar is still listed with only what the container says: a wav
    /// dropped in by hand is a usable sfx, it just has no measurements yet.
    void Load(const std::filesystem::path& directory);

    [[nodiscard]] const std::filesystem::path& Directory() const { return m_directory; }
    [[nodiscard]] std::span<const SfxEntry> Entries() const { return m_entries; }
    /// For the browser, which edits names and mutes in place and writes on focus
    /// loss. Nothing on the game side has any business here.
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

    /// Drop one entry from the index. False when the library did not hold it.
    ///
    /// The index only: nothing on disk is touched. Erasing the audio is the
    /// caller's, because the game side never removes anything and the testbench
    /// sends files to the recycle bin - a Win32 shell call with no business here.
    bool Remove(std::string_view file);

    /// Write one entry's `.meta.ini`. Called on its own when only the name or
    /// the mute changed and nothing needs re-measuring.
    bool SaveMeta(const SfxEntry& entry) const;

    /// Read one sidecar into `out`, which must already carry `file`. False when
    /// there is no sidecar; `out` is left as it was.
    [[nodiscard]] static bool LoadMeta(const std::filesystem::path& wav, SfxEntry& out);

    /// The sidecar's path for a library file: `<file>.meta.ini`, so it sorts next
    /// to the audio and a directory listing pairs them.
    [[nodiscard]] static std::filesystem::path MetaPathFor(const std::filesystem::path& wav);

private:
    std::filesystem::path m_directory;
    std::vector<SfxEntry> m_entries;
};

/// What one slot plays. A row of `files` is a *placement*, not a sound: the same
/// wav may sit on the slot twice - once plain, once tagged for one kind of contact
/// - which is why everything about a condition below is addressed by position and
/// never by filename.
struct SlotAssignment {
    /// Library filenames, in order. The order is the variant index a cue carries,
    /// so re-ordering changes which file a given cue plays. A name may appear more
    /// than once: two placements are two candidates the picker treats separately.
    std::vector<std::string> files;

    /// Files that are on this slot and must never be picked, by filename.
    ///
    /// Names rather than variant indices: an index is a position in `files`, and
    /// re-ordering the list - which the panel does - would move the mute onto a
    /// different sound. A muted file keeps its place and its variant index, so
    /// unmuting puts the take back exactly as it was. (`SfxEntry::disabled` is the
    /// other one: it suspends a sound *everywhere*.)
    ///
    /// By name, so a file placed twice on one slot is muted in both places: a mute
    /// says "not this sound, here", and the two placements are the same sound.
    ///
    /// Saved, and read by the game. A slot with every file muted goes silent rather
    /// than falling back - muting is not a request to find a replacement.
    std::vector<std::string> muted;

    /// What each placement asks of the contact before it is a candidate, parallel
    /// to `files`.
    ///
    /// By position and never by filename: the same wav may be on the slot twice,
    /// and a condition keyed on the name would land on both copies - one recording
    /// claiming to be both the plain option and the plate-only one. The runtime has
    /// always been positional (`SoundBank::SlotFiles::conditions`).
    ///
    /// Short - or empty, the shipping case - means the rest are unconditional, so a
    /// pack from before conditions existed behaves the same. A condition is a
    /// *preference*: if nothing satisfies it the slot plays its full set rather
    /// than going silent. Silencing a file is `muted`.
    std::vector<VariantCondition> conditions;

    /// The condition on the placement at `index`, unconditional when it has
    /// none and when `index` is past the end.
    [[nodiscard]] VariantCondition ConditionAt(std::size_t index) const;

    /// Tag the placement at `index`, or clear its tag when `condition` asks
    /// nothing. Out of range does nothing.
    void SetConditionAt(std::size_t index, VariantCondition condition);

    /// Put `file` on the slot as a new placement, tagged or not. The only way
    /// files grow, so `conditions` cannot fall out of step with it.
    void Add(std::string file, VariantCondition condition = {});

    /// Change which file the placement at `index` plays. The condition stays - a
    /// tag is what this position is *for*, and changing which recording serves it
    /// does not change the job. The mute does not: it went with the old sound.
    void ReplaceAt(std::size_t index, std::string file);

    /// Take the placement at `index` off the slot, with its condition, and drop
    /// the mute when that was the last placement of the file.
    void RemoveAt(std::size_t index);

    /// One condition per placement, trimming or padding as needed. Called after
    /// anything that sets `files` wholesale, which is the parsers.
    void NormalizeConditions();

    /// How a written file names the placement at `index`: the filename, or
    /// `file.wav#2` when the slot places that name more than once. 1-based, and
    /// only where it has to be, so a slot without duplicates writes what it always
    /// wrote.
    [[nodiscard]] std::string PlacementTag(std::size_t index) const;

    /// The placement such a tag names, or -1 when nothing on the slot matches.
    /// A bare name means the first placement of it, which is how a file written
    /// before duplicates were expressible still reads correctly.
    [[nodiscard]] int PlacementOf(std::string_view tag) const;

    /// Whether this slot's sound is a sustained texture the engine repeats.
    /// Defaults to the manifest's `isLoop` and is overridable per slot, because "is
    /// this a loop" is a property of what got assigned as much as of the slot.
    bool looping{};

    [[nodiscard]] bool Empty() const { return files.empty(); }
    [[nodiscard]] bool Muted(std::string_view file) const;
    /// Drop a name from `muted`. Called whenever a file leaves the slot, so a mute
    /// cannot outlive the sound it was about and come back to life when somebody
    /// assigns that file here again.
    void Unmute(std::string_view file);
};

/// The slot-to-file table, as read from and written to RagdollSounds_SFX.ini.
class SfxAssignments {
public:
    /// Every slot at its manifest default: no files, `looping` from the slot.
    SfxAssignments();

    /// Read `file`. Returns how many slots carried an assignment; 0 means missing
    /// or empty, the signal to fall back to the convention scan rather than to play
    /// nothing.
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

    /// Fill from `<slot>_<NN>` filenames in a library, which is how an existing
    /// pack becomes a set of assignments. Only fills slots that are empty.
    void SeedFromNames(const SfxLibrary& library);

    /// Every file named by any slot. Used to tell an orphan in the library from
    /// something in use.
    [[nodiscard]] bool IsUsed(std::string_view file) const;

    /// How many slots name `file`. What a "this is used by three slots" warning
    /// counts, and what Forget returns after the fact.
    [[nodiscard]] std::size_t UseCount(std::string_view file) const;

    /// Take `file` off every slot that names it, mutes included, and say how many
    /// slots that was. For a file that has stopped existing: an assignment naming a
    /// missing file is not fatal, but it is a line in the ini pointing at nothing
    /// that comes back to life when somebody imports a different sound under the
    /// same name.
    std::size_t Forget(std::string_view file);

    [[nodiscard]] bool operator==(const SfxAssignments& other) const;

private:
    SlotAssignment m_slots[static_cast<std::size_t>(SlotId::kCount)];
};

}  // namespace rds
