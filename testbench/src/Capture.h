#pragma once

// Writing takes, and everything else that edits the files a take is made of.
//
// The testbench used to be read-only over Research/NewRecordings; with the devbench
// link it also *makes* takes, so it writes the same two files QuickModMenuNG
// writes and reads them back identically. The CSV column list here is the
// recorder's own and the sidecar carries the keys Recording.cpp looks up: a take
// this program wrote must be indistinguishable from one the game wrote.
//
// Everything here is blocking and file-system-bound, called from the UI thread - a
// few milliseconds for a write, a few seconds for anything that runs ffmpeg.

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

/// Where a written take's clock came from, for a caller that has to line
/// something else up with it - the video, in practice.
///
/// A take is re-based onto its first surviving row rather than onto the window
/// it was asked for, and that is right: a selection dragged by eye starts in
/// dead air, and re-basing onto the request would open every cut take with a
/// second of nothing. But it means the two are not the same number, and
/// anything cut to match the take has to be cut against this one. Cutting
/// against the request instead is a de-sync of exactly the dead air.
struct TakeWindow {
    double originMs{};    ///< source time that became the new take's zero
    double durationMs{};  ///< the new take's own length, closing row included
};

/// Write `<stem>.csv` and `<stem>.yaml` into `directory`, keeping only the
/// events inside `[loMs, hiMs]`.
///
/// Returns the CSV's path, or an empty path with `error` filled. The times in
/// the file start at `kLeadInMs` rather than at zero: a take whose first row is
/// t_ms 0 gives the phase machine no frame before the first contact, and the
/// captures the game writes all carry a lead-in of their own.
///
/// `window`, when given, comes back with the clock this landed on.
[[nodiscard]] std::filesystem::path WriteTake(const std::filesystem::path& directory,
                                              const std::string& stem, const TakeSource& source,
                                              double loMs, double hiMs, std::string& error,
                                              TakeWindow* window = nullptr);

// ═════════════════════════════════════════════════════════════════════════════
// lining a take up with its video
// ═════════════════════════════════════════════════════════════════════════════
//
// Sync works differently here than for the takes in Research/, and the difference
// makes the numbers mean opposite things.
//
// QuickModMenuNG recorded against an OBS recording that was usually already
// running and cut down afterwards. Its sync rows pair the take's clock with *that*
// recording's, the cut point was written nowhere, and the fitted intercept is
// useless against the mp4 on disk - hence the hand-measured per-take nudges in
// framecache/video-offsets.ini.
//
// A devbench take owns its recording: this program starts and stops OBS, and the
// file that comes out is the whole video with nothing cut off. So the intercept is
// the answer outright, and the sidecar says so by naming an `obs.output_path`.
//
// What has to be crossed instead is a process boundary: the events are stamped on
// the game's session clock and OBS answers on its own, so a row is (the game's
// clock at the midpoint of the round trip) against (OBS's reported output
// duration). The rtt column says how much each row is worth.

/// One row of the sync track, before it is rebased onto a take's own clock.
struct SyncSample {
    double gameMs{};  ///< the game's session clock at the instant OBS answered
    double obsMs{};   ///< OBS's output duration at that instant
    double rttMs{};   ///< how long the round trip took, and so what the row is worth
};

/// Write `<stem>_sync.csv`, rebasing each row's game clock onto the take's own by
/// subtracting `originMs` - the number `WriteTake` hands back in its `TakeWindow`.
///
/// Rows from before the take's zero are kept rather than dropped: a negative t_ms
/// is a perfectly good point on the same line and it lengthens the lever arm the
/// slope is fitted over.
[[nodiscard]] bool WriteSyncTrack(const std::filesystem::path& directory,
                                  const std::string& stem,
                                  const std::vector<SyncSample>& samples, double originMs,
                                  std::string& error);

/// What the sidecar's `obs:` block says about a take's video.
struct ObsTakeInfo {
    /// The mp4 as it now sits beside the take. Empty means the take has no video
    /// and no block is written at all.
    std::string outputPath;
    std::string syncCsv;  ///< empty when no sync track could be written
    /// OBS's output clock at the take's zero - the fitted intercept, or 0 when
    /// there was nothing to fit.
    double offsetMs{};
    std::string obsVersion;
    std::string recordDirectory;
};

/// Append the `obs:` block to a take's sidecar, in the shape QuickModMenuNG
/// writes it and `Recording.cpp` reads it.
///
/// Appended rather than written by `WriteTake`, because the offset is not known
/// until the video has been stopped, moved and fitted - which is three steps
/// after the CSV exists.
void AppendObsBlock(const std::filesystem::path& yaml, const ObsTakeInfo& info);

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
                                              std::string& error, TakeWindow* window = nullptr);

/// Turn a live capture into a TakeSource. Picks the subject actor, and carries
/// the cell and the name off its profile.
[[nodiscard]] TakeSource SourceFromCapture(const std::vector<rds::FeedEvent>& events,
                                           const rds::link::ProfileMessage* subject,
                                           std::string note);

/// Milliseconds of silence written in front of the first event of a take.
inline constexpr double kLeadInMs = 100.0;

}  // namespace tb
