#pragma once

// The vanilla track: what Skyrim's own impact system played, beside what we did.
//
// It lives next to the take as `<stem>_vanilla.csv`, found by stem exactly like
// `<stem>.yaml`, `<stem>_pose.bin` and `<stem>.mp4` already are, and for the same
// reason pose does: the impacts CSV is the *recorder's* format and a take this
// program writes has to be indistinguishable from one the game wrote. A column
// added there would break that; a sibling file cannot.
//
// Unlike pose it is text, because it is the opposite shape. Pose is eighteen
// limbs at sixty hertz and nobody reads it; this is a few dozen rows a take, one
// per collision vanilla decided to sound, and reading it by eye is most of what
// it is for.
//
// ── what a row is worth ──────────────────────────────────────────────────────
//
// Each row is one call into `BGSImpactManager::PlayImpactDataSounds` on the
// collision path, observed at the moment it happened (VanillaImpactHook.h). It
// carries what vanilla *chose*: the impact record, the descriptor that fired,
// which half of the light/heavy pair that was, and the descriptor's gain and
// variance settings.
//
// It does not carry what vanilla then *drew*. Which of the descriptor's `files`
// wavs the audio engine picked, and the dB and frequency it rolled inside
// `db_variance` and `freq_variance`, happen after the handle is built, in
// BSGameSound, whose layout CommonLib does not have. So the honest reading of a
// row is "one of these N files, at this attenuation, plus or minus this much" -
// which is enough to reconstruct the event to within vanilla's own randomness,
// and not enough to reproduce one particular take of it sample for sample.
//
// `heard` is the other half of the same honesty. A row is written whether or not
// the sound was let through, so a take made with vanilla audio on and a take made
// with ours carry the same track; the column says which it was.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rds/Feed.h"

namespace rds::vanilla {

/// Write every `kVanillaSound` event in `events` whose time falls inside
/// [loMs, hiMs], re-based against `originMs` so the file shares its CSV's clock.
///
/// Writing no rows is not an error and leaves no file behind: a take from a
/// runtime where the hook could not be installed, or one recorded before this
/// existed, should look like a take with no vanilla track rather than a broken
/// one.
[[nodiscard]] bool Write(const std::filesystem::path& file, const std::vector<FeedEvent>& events,
                         double originMs, double loMs, double hiMs, std::size_t& rowsOut,
                         std::string& error);

/// Read one back, appending `kVanillaSound` FeedEvents in file order.
///
/// A missing file is not an error - it is a take with no vanilla track, which is
/// most of them. A file that is there and malformed *is*, for the reason pose
/// gives: that is a truncated write or a bug, and reading it as "no track" hides
/// both.
[[nodiscard]] bool Read(const std::filesystem::path& file, std::vector<FeedEvent>& out,
                        std::string& error);

/// Just the row count, for the take list - which wants to know whether a take
/// has a vanilla track without paying to parse it.
[[nodiscard]] bool Probe(const std::filesystem::path& file, std::size_t& rowsOut);

/// "light" or "heavy" - which half of the SNAM/NAM1 pair a row fired.
[[nodiscard]] std::string_view BranchName(const VanillaSoundInfo& info);

/// The descriptor's static attenuation in dB. Stored as the CK value times 100,
/// which is a number nobody can read off a spreadsheet.
[[nodiscard]] float AttenuationDb(const VanillaSoundInfo& info);

}  // namespace rds::vanilla
