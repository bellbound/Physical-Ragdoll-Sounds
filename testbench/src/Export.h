#pragma once

// Dump everything the testbench is holding, so a disagreement about what it is
// doing can be settled by reading rather than by guessing.
//
// Written because "the sound does not match the video" has at least four
// possible causes - the physics data, the engine's decisions, the mix, or the
// video alignment - and from the outside they look identical. The dump puts all
// four side by side on one timeline.

#include <filesystem>
#include <string>
#include <vector>

#include "rds/Config.h"
#include "rds/Offline.h"
#include "rds/Recording.h"
#include "rds/SlotManifest.h"

namespace tb {

struct MixedAudio;
struct SyncModel;
class VideoTake;

/// Everything about one moment in the testbench's life.
///
/// Pointers rather than copies: this is assembled at the instant the button is
/// pressed and written immediately, and nothing here outlives the call.
struct ExportRequest {
    const rds::Recording* recording{};
    const rds::RecordingInfo* info{};
    const rds::ConfigSet* config{};
    const rds::OfflineResult* result{};
    const rds::SoundBank* bank{};
    const MixedAudio* audio{};
    const SyncModel* sync{};
    const VideoTake* video{};

    std::string configName;
    /// True when the config in memory is not what the file of that name holds,
    /// so `configName` above is where it came from and not what it is.
    bool configUnsaved{};
    /// Write the whole config out as an ini, with every key's description, at
    /// the end of the report. Off leaves only the list of what differs from
    /// default, which is shorter and says nothing about the other eighty keys.
    bool includeConfigs{};
    double videoOffsetMs{};
    /// True when the offset came from framecache/video-offsets.ini rather than
    /// from a fallback. Changes the note under it from a caveat to a fact.
    bool offsetMeasured{};
    double playheadMs{};

    /// The stretch of the take to write about. When a loop region is set this is
    /// that region and `windowIsRegion` is true, and every time-ordered section
    /// below is cut to it: the contacts, the cues, the decisions, the state the
    /// actor was already in. Defaults to the whole take.
    ///
    /// The point is that a region is drawn around the thing being argued about,
    /// and a full dump of a 100-second take buries the 200 ms in question.
    double windowLoMs{};
    double windowHiMs{1e30};
    bool windowIsRegion{};

    std::uint32_t seed{};
    bool limiter{};
    int side{};  ///< which config side this is, in split mode
};

/// Write a human-readable report. Not JSON: the point is to be read by eye and
/// pasted into a conversation, and the interesting part is a single merged
/// timeline that no object model would present better than columns do.
///
/// Sections:
///   take       what the recording says about itself, and its video sync fit
///   config     every parameter that differs from its default
///   bank       which slots resolved to files and which to stand-ins
///   stats      the funnel from contacts in to cues out, with every drop counted
///   timeline   the merged view - state changes, raw contacts, and cues emitted,
///              in one time-ordered column, which is what settles a "why is
///              there sound here" question
///   cues       every cue with its full provenance
///   config ini the whole config as it stood in memory, saved or not, with each
///              key's description - so the report carries the settings it is a
///              report *of* rather than the name of a file that may since have
///              been edited. Last, because it is four hundred lines and the
///              question is nearly always further up
///
/// Returns the path written, or empty with `error` filled.
std::filesystem::path WriteExport(const ExportRequest& request,
                                  const std::filesystem::path& directory, std::string& error);

}  // namespace tb
