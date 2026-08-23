#pragma once

// Writing takes, and everything else that edits the files a take is made of.
//
// The testbench used to be read-only over Research/NewRecordings: QuickModMenuNG
// wrote the takes and this program replayed them. With the devbench link it also
// *makes* them - from a live game, and from a stretch of one it already has - so
// it needs to write the same two files QuickModMenuNG writes, and read back
// identically. That is why the CSV column list here is the recorder's own and
// the sidecar carries the keys Recording.cpp actually looks up: a take this
// program wrote must be indistinguishable from one the game wrote, or a
// verification run over the folder means nothing.
//
// Everything here is blocking and file-system-bound. Called from the UI thread,
// which is a stall of a few milliseconds for a write and a few seconds for
// anything that runs ffmpeg - see the notes on each.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "rds/Feed.h"
#include "rds/Link.h"
#include "rds/Recording.h"

namespace tb {

/// Which takes are in the next/prev cycle, persisted beside the frame cache.
///
/// A separate file from video-offsets.ini rather than a column in it: one is a
/// measurement of a clip and the other is a working preference, and a take that
/// is deleted should lose the second without anybody having to remember that the
/// first lived in the same place.
class TakeFlags {
public:
    void Load(const std::filesystem::path& file);
    void Save() const;

    /// Enabled unless something said otherwise, so a folder of takes that
    /// predates this file is entirely enabled.
    [[nodiscard]] bool Enabled(const std::string& stem) const;
    void SetEnabled(const std::string& stem, bool enabled);
    void Erase(const std::string& stem);

private:
    std::filesystem::path m_file;
    std::map<std::string, bool> m_disabled;
};

// ═════════════════════════════════════════════════════════════════════════════
// writing a take
// ═════════════════════════════════════════════════════════════════════════════

/// Everything needed to write one. `events` carry absolute times on whatever
/// clock they came off; the writer re-bases them so the take starts near zero.
struct TakeSource {
    std::vector<rds::FeedEvent> events;
    const rds::ActorProfile* profile{};  ///< the limb table. Null writes an empty one
    std::string actorName;
    std::string formId;  ///< eight hex digits
    std::string cell;
    std::string note;    ///< recording.note - the take's *intent*
};

/// Write `<stem>.csv` and `<stem>.yaml` into `directory`, keeping only the
/// events inside `[loMs, hiMs]`.
///
/// Returns the CSV's path, or an empty path with `error` filled. The times in
/// the file start at `kLeadInMs` rather than at zero: a take whose first row is
/// t_ms 0 gives the phase machine no frame before the first contact, and the
/// captures the game writes all carry a lead-in of their own.
[[nodiscard]] std::filesystem::path WriteTake(const std::filesystem::path& directory,
                                              const std::string& stem, const TakeSource& source,
                                              double loMs, double hiMs, std::string& error);

/// The first `<base>_<n>` with no CSV in `directory`.
[[nodiscard]] std::string NextTakeStem(const std::filesystem::path& directory,
                                       std::string_view base);

/// A filename-safe version of an actor's name, for a stem. "Lennald the Brash
/// [Whiterun Guard]" becomes "Lennald_the_Brash_Whiterun_Guard".
[[nodiscard]] std::string SafeStem(std::string_view name);

// ═════════════════════════════════════════════════════════════════════════════
// editing one that exists
// ═════════════════════════════════════════════════════════════════════════════

/// Drop every row of `csv` whose `t_ms` falls inside `[loMs, hiMs]`.
///
/// Rewrites the file in place, through a temporary, and leaves the sidecar's
/// impact count updated. The remaining rows keep their original timestamps: the
/// alternative is closing the gap, which would put every row after the cut out
/// of step with the video and with every export already written about this take.
[[nodiscard]] bool DeleteEventRange(const std::filesystem::path& csv, double loMs, double hiMs,
                                    std::size_t& removed, std::string& error);

/// Everything a take owns: csv, yaml, `_sync.csv`, mp4, and its frame cache.
/// Returns how many files went.
std::size_t DeleteTake(const std::filesystem::path& csv, const std::filesystem::path& cacheRoot,
                       std::string& error);

/// The take a selection of another take would become, without the video half.
/// `WriteTake` with the source's own profile and sidecar values filled in.
[[nodiscard]] std::filesystem::path SliceTake(const rds::Recording& recording,
                                              const rds::RecordingInfo& info,
                                              const std::filesystem::path& directory,
                                              const std::string& stem, double loMs, double hiMs,
                                              std::string& error);

/// Turn a live capture into a TakeSource. Picks the subject actor, and carries
/// the cell and the name off its profile.
[[nodiscard]] TakeSource SourceFromCapture(const std::vector<rds::FeedEvent>& events,
                                           const rds::link::ProfileMessage* subject,
                                           std::string note);

/// Milliseconds of silence written in front of the first event of a take.
inline constexpr double kLeadInMs = 100.0;

}  // namespace tb
