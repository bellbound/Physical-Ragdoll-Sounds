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
//                     importer measured and when it came in
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
/// Mostly not fatal. The design's own delivery rules (Slots.md §5) are what
/// these check, and a file that breaks one is still worth hearing - a body layer
/// with 40 ms of lead-in is a timing bug, not a silence. Two flags mark the
/// exceptions, and they are different in kind:
///
///   `blocking`  the decoder could not read it. There is no sound here at all.
///   `dead`      it plays, and nothing anyone can do makes it usable: the
///               samples are already flat-topped, the hiss is 29 dB under the
///               hero, the two channels are different takes. Slots.md's own
///               language for this is "the take is dead" - the answer is to
///               re-source or regenerate, never to process.
///
/// Everything else is advisory, and everything the import pass can fix is fixed
/// on the way in rather than reported - so a warning that is here at all is one
/// that survived the repair.
struct SfxWarning {
    std::string code;    ///< short badge text, e.g. "lead-in"
    std::string detail;  ///< the whole sentence, shown on hover
    bool blocking{};     ///< true only when the file cannot be played at all
    bool dead{};         ///< true when it plays but cannot be made right
};

/// One file in the library, plus everything known about it.
///
/// `file` is the identity - it is what RagdollSounds_SFX.ini names and what
/// survives a rename in the UI. `name` is only ever shown, so renaming is free
/// and cannot break an assignment.
struct SfxEntry {
    std::string file;  ///< filename inside the library directory, with extension
    std::string name;  ///< display name; starts as the stem, editable

    /// When this file entered the library, in seconds since the unix epoch.
    ///
    /// Written once, at import, and never moved after: a re-measure keeps it and
    /// so does a rename, because both are things done to a file that is already
    /// here. It is what the browser's newest-first order sorts on, which is the
    /// order that matters the evening after a download - the twelve sounds that
    /// just arrived, together, at the top of the list.
    ///
    /// Zero only while nothing knows. Anything that predates this field gets the
    /// audio file's own modification time when the library loads, which for a
    /// file the importer copied in is the moment it was copied - the same
    /// instant this would have recorded.
    std::int64_t importedAt{};

    /// Muted: still in the library, still named by whatever slots name it, and
    /// not played.
    ///
    /// The difference between this and taking the file off a slot is that this
    /// one is not a decision about the slot. `x` in the panel forgets which slot
    /// the sound was on; this suspends it everywhere at once and remembers all
    /// of it, so a sound that turned out to be wrong can be silenced without
    /// losing the work of having placed it. The bank skips these when it fills a
    /// slot and the browser sorts them to the bottom of the list.
    bool disabled{};

    // ── the corrections. Yours, like `name` and `disabled` ───────────────────
    //
    // A correction to *the recording*, which is why it lives here and not on the
    // assignment. `RagdollSounds_SFX.ini`'s own header draws that line already:
    // a mute "is about the sound, not about one entry in the list", while a
    // condition "is about one entry in `Sfx`, not about the sound". A scrape
    // loop pitched too low is wrong wherever it is used, so it is corrected once
    // - exactly like `disabled`, which suspends a sound everywhere at once.
    //
    // If two slots genuinely want one file at two levels, that is a decision
    // about the slots and `SlotGain:f<Slot>` is the control for it.
    //
    // Both are identities at their defaults, so a library that has never been
    // touched sounds exactly as it did.

    /// Playback rate, x. 1 is the file as recorded; 1.09 is it a semitone and a
    /// half up. Multiplies the pitch the engine already chose - the per-cue
    /// scatter, the intensity bias and the armour bias - rather than replacing
    /// it, so a correction and a variation stay separate things.
    ///
    /// **This changes a one-shot's length**, because pitch here is resampling
    /// and not a formant shift. `EffectiveDurationMs()` is the number to check a
    /// length spec against; `durationMs` stays what the container says.
    float pitch{1.0f};

    /// Level, dB. Applied at Stage 5 with the other trims.
    ///
    /// It cannot change what was chosen, and that is structural rather than a
    /// promise: the bank does not resolve a layer to a *file* until `Emit`, long
    /// after Stage 4 has sorted, rate-capped and burst-shaped. There is no path
    /// by which this number could reach `Proposal::levelDb`. See `config.md` -
    /// "this wav is hot" is that file's own example of a Trim.
    float trimDb{};

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

    // ── the technical rule-outs. All measured the way tools/sfx.py and
    //    tools/triage_batch.py measure them, because their thresholds are what
    //    03-Asset-Status.md §7 ruled four cuts out on ────────────────────────

    /// Peak minus the quietest 50 ms in front of the attack, in dB. `sfx.py`'s
    /// `snr`, and its 30 dB gate is what dropped `twisting…gristle_variation2`.
    /// 99 when there is no pre-roll long enough to measure it in, which is not
    /// the same as clean and is why the sentinel is high rather than 0.
    float noiseFloorDb{99.0f};

    /// Percentage of samples sitting inside a full-scale flat top.
    ///
    /// The one level fault the import pass cannot repair. Gain moves a peak; it
    /// does not put back the tops of waves that were squared off before the file
    /// was written, and the odd-harmonic buzz that leaves is what "clipped"
    /// should have always meant here. Nothing is a peak over -0.2 dBFS any more:
    /// that is a headroom question and headroom is normalised on the way in.
    float clipPct{};
    /// How many separate flat tops. The discriminator, not the percentage: a
    /// decaying sub sweep touches its own maximum once and a clipped impact
    /// touches it on every crest for as long as it is loud.
    int clipRuns{};

    /// Contacts at least 46 ms apart and within 32 dB of the loudest - the
    /// reference rate floor from 04-Reference-Analysis.md §2, which is where two
    /// impacts stop resolving as two. More than one is not a fault: it is what
    /// `sfx.py split` exists for, and all four shipped `limb_tap` files came out
    /// of it.
    int contacts{};

    /// The loudest thing after the hero contact, relative to it, and when.
    ///
    /// 03-Asset-Status.md §7: 39 takes in 100 carry one of these, 21-34 dB down
    /// and usually a bright debris wash. Left in, it lands on top of whatever
    /// the engine schedules next and reads as a flam. 0 for neither.
    float satelliteDb{};
    float satelliteAtMs{};

    /// Energy above 16 kHz against the 2.5-8 kHz band, in dB. Far down means the
    /// file was upsampled or heavily lossy whatever its container claims - the
    /// one thing a 48 kHz conversion cannot put back.
    float topOctaveDb{};

    /// FNV-1a over the decoded samples. Two library files with the same hash are
    /// the same sound under two names, which 03-Asset-Status.md §7 hit twice in
    /// one batch of 102.
    std::uint64_t contentHash{};

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
    /// Carries a warning nothing can repair. Still assignable - the library
    /// never refuses - but the honest answer to one of these is another file.
    [[nodiscard]] bool Dead() const;
    /// The stem, which is what a fresh import names it.
    [[nodiscard]] std::string Stem() const;

    /// How long this file actually plays for, `pitch` included.
    ///
    /// The number a length spec has to be checked against. `durationMs` is what
    /// the container says and stays that way; pitch here is resampling, so a
    /// one-shot corrected to 1.09x is 8 % shorter than the wav on disk - and a
    /// spec, a slot's min/max, or `03-Asset-Status.md`'s tables read against the
    /// container length would all be grading a file that is not what plays.
    [[nodiscard]] float EffectiveDurationMs() const {
        return pitch > 0.0f ? durationMs / pitch : durationMs;
    }

    /// True when either correction is doing anything. The ordinary case is
    /// neither, and both are identities there.
    [[nodiscard]] bool Corrected() const { return pitch != 1.0f || trimDb != 0.0f; }
};

/// `2026-08-23 19:41` in local time, for an `importedAt`. Empty for 0, which is
/// what an entry whose import time nothing knows carries - so a caller can print
/// it and get nothing rather than 1970.
[[nodiscard]] std::string FormatImportTime(std::int64_t unixSeconds);

/// Every file under the library directory, with its metadata.
///
/// The game loads this once and only reads it. The testbench also writes it -
/// importing, renaming and muting all end in SaveMeta for one entry.
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
    /// For the browser, which edits names and mutes in place and writes them on
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

    /// Drop one entry from the index. False when the library did not hold it.
    ///
    /// The index only: nothing on disk is touched. Erasing the audio and its
    /// sidecar is the caller's, because the two halves want different
    /// treatment - the game side never removes anything, and the testbench
    /// sends the files to the recycle bin rather than unlinking them, which is
    /// a Win32 shell call that has no business in core.
    bool Remove(std::string_view file);

    /// Write one entry's `.meta.ini`. Called on its own when only the name or
    /// the mute changed and nothing needs re-measuring.
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
///
/// A row of `files` is a *placement*, not a sound: the same wav may sit on the
/// slot twice - once plain, once tagged for one kind of contact - and the two
/// are different entries with different conditions. That is what "this is
/// slot-based, not sound-effect-based" means, and it is why everything about a
/// condition below is addressed by position and never by filename.
struct SlotAssignment {
    /// Library filenames, in order. The order is the variant index a cue
    /// carries, so re-ordering this changes which file a given cue plays.
    ///
    /// A name may appear more than once. Two placements of one file are two
    /// candidates the picker treats separately, which is what lets one
    /// recording be both a plain member of the set and the one tagged for
    /// plate.
    std::vector<std::string> files;

    /// Files that are on this slot and must never be picked, by filename.
    ///
    /// Kept as names rather than as variant indices on purpose: an index is a
    /// position in `files`, and re-ordering the list - which is a thing the
    /// panel does - would silently move the mute onto a different sound.
    ///
    /// A muted file keeps its place in `files`, so it keeps its variant index
    /// and unmuting puts the take back exactly as it was. That is the
    /// difference from the library's own `SfxEntry::disabled`, which suspends a
    /// sound *everywhere* and drops it out of the slot's variant list
    /// altogether: this one is a decision about this slot.
    ///
    /// By name, so a file placed twice on one slot is muted in both places at
    /// once: a mute says "not this sound, here", and the two placements are the
    /// same sound. Unlike a condition, which is about what a *position* on the
    /// slot is for, there is nothing a per-placement mute could express that
    /// taking the placement off the slot does not already say.
    ///
    /// Saved, and read by the game. A slot with every file muted goes silent
    /// rather than falling back to what it would play with no files at all -
    /// muting something is not a request to find a replacement for it.
    std::vector<std::string> muted;

    /// What each placement asks of the contact before it is a candidate,
    /// parallel to `files` and one entry per row of it.
    ///
    /// By position and never by filename. A filename is not an identity here:
    /// the same wav may be on the slot twice, and a condition keyed on the name
    /// would land on both copies - which is one recording claiming to be both
    /// the plain option and the plate-only one, and shows up in the panel as
    /// the same file listed twice under the same heading. The runtime has
    /// always been positional (`SoundBank::SlotFiles::conditions`); this is the
    /// same list.
    ///
    /// Short - or empty, which is the shipping case - means the rest are
    /// unconditional, so a pack from before conditions existed loads and
    /// behaves the same. A condition is a *preference*: if nothing satisfies it
    /// the slot plays its full set rather than going silent. Silencing a file is
    /// `muted`, which is stored separately precisely so the two cannot be
    /// confused.
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

    /// Change which file the placement at `index` plays.
    ///
    /// The condition stays: a tag is what this position on the slot is *for* -
    /// "the stone one" - and changing which recording serves it does not change
    /// the job. The mute does not: it went with the sound that was here.
    void ReplaceAt(std::size_t index, std::string file);

    /// Take the placement at `index` off the slot, with its condition, and drop
    /// the mute when that was the last placement of the file.
    void RemoveAt(std::size_t index);

    /// One condition per placement, trimming or padding as needed. Called after
    /// anything that sets `files` wholesale, which is the parsers.
    void NormalizeConditions();

    /// How a written file names the placement at `index`: the filename, or
    /// `file.wav#2` when the slot places that name more than once. The suffix
    /// is 1-based and only ever appears where it has to, so a slot without
    /// duplicates writes exactly what it always wrote.
    [[nodiscard]] std::string PlacementTag(std::size_t index) const;

    /// The placement such a tag names, or -1 when nothing on the slot matches.
    /// A bare name means the first placement of it, which is how a file written
    /// before duplicates were expressible still reads correctly.
    [[nodiscard]] int PlacementOf(std::string_view tag) const;

    /// Whether this slot's sound is a sustained texture the engine repeats.
    ///
    /// Defaults to the manifest's own `isLoop` and is overridable per slot,
    /// because "is this a loop" is a property of what got assigned as much as
    /// of the slot: a sliding or wind-like sound wants repeating and must not
    /// be complained at for being long, and a slot fed one wants to say so.
    bool looping{};

    [[nodiscard]] bool Empty() const { return files.empty(); }
    [[nodiscard]] bool Muted(std::string_view file) const;
    /// Drop a name from `muted`. Called whenever a file leaves the slot, so a
    /// mute cannot outlive the sound it was about and come back to life the day
    /// somebody assigns that file here again.
    void Unmute(std::string_view file);
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

    /// How many slots name `file`. What a "this is used by three slots" warning
    /// counts, and what Forget returns after the fact.
    [[nodiscard]] std::size_t UseCount(std::string_view file) const;

    /// Take `file` off every slot that names it, mutes included, and say how
    /// many slots that was.
    ///
    /// For a file that has stopped existing. An assignment naming a missing
    /// file is not fatal - the bank skips it - but it is a line in the ini
    /// pointing at nothing, and it comes back to life the day somebody imports
    /// a different sound under the same name.
    std::size_t Forget(std::string_view file);

    [[nodiscard]] bool operator==(const SfxAssignments& other) const;

private:
    SlotAssignment m_slots[static_cast<std::size_t>(SlotId::kCount)];
};

}  // namespace rds
