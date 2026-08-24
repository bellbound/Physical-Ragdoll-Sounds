#pragma once

// Getting a sound file into the library, in the shape the pack wants.
//
// The rule is that an import never refuses. Whatever arrives is made playable
// and then measured, and anything wrong with it comes back as a warning you can
// read and ignore. That is deliberate: the reason to audition a file at all is
// usually that you are not sure about it, and a gate that says no is a gate that
// stops you finding out. The one exception is a file nothing can decode, which
// is not a judgement - there is simply no sound there.
//
// What "the shape the pack wants" means is Slots.md §5: mono, 48 kHz, 16-bit
// PCM. Anything else is converted on the way in rather than complained at,
// because that conversion is exactly what `sfx.py make` would have done and the
// game is fussier than the testbench about being handed something else.
//
// ffmpeg does the container work and miniaudio does the decode. Testbench-only:
// the game has a library of finished files and never imports anything.

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

/// The multi-select open dialog. Empty when the user cancelled.
[[nodiscard]] std::vector<std::filesystem::path> PickAudioFiles();

/// Send files to the recycle bin, as one undoable operation.
///
/// The other direction from an import, and here for the same reason the import
/// is: this is the file that knows how to talk to the shell. Deleting an sfx
/// takes its audio and its sidecar together, so they go in one call - one
/// entry in the bin, restored as a pair by one Ctrl+Z in Explorer.
///
/// The bin rather than std::filesystem::remove because the browser's delete is
/// the one button in the window that cannot be undone from inside the app, and
/// a sound somebody spent an evening auditioning is worth the recoverable
/// version of that. Missing paths are skipped rather than failed - deleting a
/// file with no sidecar is the normal case for anything dropped in by hand.
/// False with `error` set when the shell refused; nothing is assumed about how
/// much of the list went.
[[nodiscard]] bool RecycleFiles(const std::vector<std::filesystem::path>& files,
                                std::string& error);

}  // namespace tb
