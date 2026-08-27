#pragma once

// Getting a sound file into the library, in the shape the pack wants.
//
// **An import never refuses.** Whatever arrives is made playable and then
// measured, and anything wrong comes back as a warning you can read and ignore -
// the reason to audition a file is usually that you are not sure about it, and a
// gate that says no stops you finding out. The one exception is a file nothing can
// decode, where there is simply no sound.
//
// "The shape the pack wants" is Slots.md §5: mono, 48 kHz, 16-bit PCM. Anything
// else is converted rather than complained at, because that is what `sfx.py make`
// would have done.
//
// §5's delivery rules are mechanical - a peak of -1.5 dBFS, no DC, no head
// silence, no click at the end - so the import *repairs* them silently:
//
//   - the peak is normalised to -1.5 dBFS, the headroom the runtime's +/-3
//     semitone scatter needs. Files already inside the band that holds both pack
//     targets are left alone, so adopting the built pack does not pull `imp_sub`
//     off its own -1.0
//   - DC is subtracted, head silence and trailing digital silence are trimmed, and
//     a one-shot that stops dead gets `sfx.py make`'s 6 ms cosine fade
//   - a stereo source whose channels do not correlate has its left channel taken
//     rather than the two folded (03 §3.1)
//
// All of it is idempotent - a repaired file repairs to itself. What is left is
// what a badge is *for*: faults that need a decision, and ones nothing can mend.
//
// ffmpeg does the container work and miniaudio the decode. Testbench-only.

#include <filesystem>
#include <string>
#include <vector>

#include "rds/Sfx.h"

namespace tb {

/// Whether an ffmpeg and an ffprobe could be found on PATH. Cached after the
/// first look. Without them the import button still works for wav sources -
/// miniaudio decodes those on its own - and says so for everything else.
[[nodiscard]] bool FfmpegAvailable();

/// The resolved executables, for the UI and the log. Empty when there are none.
[[nodiscard]] const std::string& FfmpegPath();
[[nodiscard]] const std::string& FfprobePath();

/// What one import did.
struct ImportOutcome {
    std::filesystem::path source;
    std::string file;   ///< the library filename it became, empty on failure
    std::string error;  ///< non-empty when nothing was written
    bool converted{};   ///< true when ffmpeg had to change the format
    bool renamed{};     ///< true when the FMTS fix changed the name

    /// What the repair pass did, comma-separated, for the log.
    ///
    /// Only for the log. The point of repairing something is that nobody has to
    /// think about it, and a note in the window saying "normalised, trimmed 40
    /// ms" is exactly the thinking it was meant to save - but a silent edit that
    /// leaves no trace anywhere is not something to build either, so it goes
    /// where an edit that turns out to have been wrong can be found afterwards.
    std::string repairs;
};

struct ImportOptions {
    /// The FMTS fix. `punch-face-hard-3(fromnoisetosound.com)` becomes
    /// `punch-face-hard-3`. On by default because it is right almost every
    /// time, and off in one click when it is not.
    bool fixNames{true};
};

/// Bring one file into `library`, analyse it, and write its sidecar.
///
/// On success the library holds a new entry and `outcome.file` names it. On
/// failure nothing is written and `outcome.error` says why - which is only ever
/// "could not read it" or "could not write it".
[[nodiscard]] ImportOutcome ImportSfx(const std::filesystem::path& source, rds::SfxLibrary& library,
                                      const ImportOptions& options);

/// Re-decode a file already in the library and measure it again.
///
/// For entries that arrived without a sidecar - dropped into the folder by hand,
/// or written by `sfx.py make` - and for re-judging after the slot targets
/// change. Never converts and never renames: the file is already in the library
/// and moving it under an assignment that names it would break that assignment.
[[nodiscard]] bool MeasureExisting(rds::SfxLibrary& library, const std::string& file,
                                   std::string& error);

/// Convert, repair and re-measure a file already in the library, in place - the
/// import pass run on something that did not come through the importer. Everything
/// MeasureExisting does plus the container conversion and the sample repairs,
/// which is why it is its own button: a re-measure reads, this writes.
///
/// Idempotent on a file that is already right, which is what makes it safe to
/// press on the whole library: a pack file at -1.0 dBFS is inside the leave-alone
/// band and comes back byte-identical.
///
/// The name, the mute and the import date survive. False with `error` set when the
/// file could not be read or written.
[[nodiscard]] bool RepairExisting(rds::SfxLibrary& library, const std::string& file,
                                  std::string& error, std::string* repairs = nullptr);

/// True when `entry` carries a warning the repair pass would clear. What the
/// browser shows the `repair` button for: offering it on a file whose only
/// fault is a baked reverb tail is offering something that does nothing.
[[nodiscard]] bool NeedsRepair(const rds::SfxEntry& entry);

/// The multi-select open dialog. Empty when the user cancelled.
[[nodiscard]] std::vector<std::filesystem::path> PickAudioFiles();

/// Send files to the recycle bin, as one undoable operation. Here because this is
/// the file that knows how to talk to the shell. Deleting an sfx takes its audio
/// and sidecar together in one call, so they are one entry in the bin restored by
/// one Ctrl+Z.
///
/// The bin rather than `std::filesystem::remove`, because the browser's delete is
/// the one button in the window that cannot be undone from inside the app. Missing
/// paths are skipped rather than failed. False with `error` set when the shell
/// refused; nothing is assumed about how much of the list went.
[[nodiscard]] bool RecycleFiles(const std::vector<std::filesystem::path>& files,
                                std::string& error);

}  // namespace tb
