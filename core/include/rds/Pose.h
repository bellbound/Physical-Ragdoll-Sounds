#pragma once

// The pose sidecar: every ragdoll limb, every tick.
//
// The impacts CSV is a *sparse* record meant to stay readable in a spreadsheet and
// diffable in git. Pose is the opposite: eighteen limbs at sixty hertz, ten times
// the row count, uninteresting by eye. So it lives beside the take as
// `<stem>_pose.bin`, found by stem like `<stem>.yaml` and `<stem>.mp4` are.
//
// An *encoding*, not a second input format: Read() hands back the same
// `kLimbSample` FeedEvents the live path pushes through the ring, so the engine
// cannot tell them apart.
//
// Frame-oriented rather than record-oriented, because that is how it is written
// and consumed, and because it amortises the timestamp and actor id over eighteen
// limbs: 516 bytes per frame per actor against 792.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rds/Feed.h"
#include "rds/Types.h"

namespace rds::pose {

inline constexpr char kMagic[8] = {'R', 'D', 'S', 'P', 'O', 'S', 'E', '1'};
/// What this build writes. v2 appends `LimbRecordExt` to each limb; v1 files are
/// still read, with those fields left at zero.
///
/// The magic keeps its trailing '1' deliberately: it identifies the *format
/// family*, not the version, and changing it would make every existing sidecar
/// unrecognisable rather than merely older. `Header::version` is what says which
/// one this is, and `Header::limbStride` is what makes reading either safe.
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint32_t kVersionMin = 1;

/// Little-endian, MSVC x64 - the same assumption Link.h already makes about the
/// wire. Field order is chosen so every struct is padding-free; the asserts
/// below are what stops a well-meaning edit from silently changing the layout.
struct Header {
    char magic[8]{};
    std::uint32_t version{kVersion};
    std::uint32_t frameCount{};
    /// Bytes per limb record. Read rather than assumed, so a v2 that adds a
    /// field can be skipped over by a v1 reader instead of misparsed.
    std::uint32_t limbStride{};
    std::uint32_t reserved{};
    /// The take's t0. Frames carry f32 offsets from this, which keeps ~0.004 ms
    /// of precision over a sixty-second take.
    double originMs{};
};

struct FrameHeader {
    float timeMs{};  ///< relative to Header::originMs
    ActorId actorId{};
    std::uint16_t limbCount{};
    /// The actor's ActorPhase at the tick that produced this frame. Carried so a
    /// decoded sample is byte-for-byte the event the live path pushed, rather
    /// than one missing a field nobody noticed was missing.
    std::uint16_t phase{};
};

/// A limb carries its own index rather than being placed by position in the
/// frame. Four bytes to avoid a format that is silently wrong the first time a
/// limb is missing from a tick - a rebuilt ragdoll does exactly that.
struct LimbRecord {
    std::uint16_t limbIndex{};
    std::uint16_t reserved{};
    float pos[3]{};
    float vel[3]{};
};

/// What v2 added, written immediately after each `LimbRecord`.
///
/// A trailing extension rather than two more fields inside `LimbRecord`, which is
/// what lets one build read both versions: `limbStride` comes from the header and
/// bytes past what the reader understands are skipped, so a v1 file loads with
/// these two at zero rather than being refused.
///
/// They exist because the garment's rotation term had nothing to read:
/// `angularSpeed` is published on every live limb sample but the sidecar never
/// stored it, so the term measured 0 on the whole corpus while being live in the
/// game - the one thing the seam rule forbids.
struct LimbRecordExt {
    float angularSpeed{};  ///< rad/s
    float radius{};        ///< objectRadius, units. Turns radians into surface speed
};

static_assert(sizeof(Header) == 32, "the pose header is a fixed 32 bytes on disk");
static_assert(sizeof(FrameHeader) == 12, "the frame header is a fixed 12 bytes on disk");
static_assert(sizeof(LimbRecord) == 28, "a limb record is a fixed 28 bytes on disk");
static_assert(sizeof(LimbRecordExt) == 8, "the v2 extension is a fixed 8 bytes on disk");

/// What this build writes. A v1 file has 28 and is still read.
inline constexpr std::uint32_t kLimbStride = sizeof(LimbRecord) + sizeof(LimbRecordExt);
/// The smallest stride any version this build understands can have.
inline constexpr std::uint32_t kLimbStrideV1 = sizeof(LimbRecord);

/// A per-tick pose sample, as opposed to the two snapshots the older captures
/// carry in their CSV at `ragdoll_start` and `ragdoll_end`.
///
/// Those two are a launch pose, not a signal, and they keep living in the CSV
/// exactly where they always were - which is what stops a cut of an old take
/// from writing a two-frame pose file and reporting itself as having pose data.
/// The discriminator is the state text: a snapshot is stamped with the state
/// that produced it, a tick sample carries none.
[[nodiscard]] inline bool IsTickSample(const FeedEvent& event) {
    return event.kind == EventKind::kLimbSample && event.text[0] == '\0';
}

/// Write every per-tick sample in `events` whose time falls inside [loMs, hiMs].
/// Rows are grouped into frames by (timeMs, actorId) over the sorted stream, which
/// is how they arrive. Times are re-based against `originMs`, so a slice shares its
/// CSV's clock and a `_cut_N` take carries pose data without a second slicer.
///
/// Writing no frames is not an error and leaves no file behind: a take captured
/// with sampling off should look like one with no pose data, not a corrupt one.
[[nodiscard]] bool Write(const std::filesystem::path& file, const std::vector<FeedEvent>& events,
                         double originMs, double loMs, double hiMs, std::size_t& framesOut,
                         std::string& error);

/// Decode `file`, appending one `kLimbSample` FeedEvent per limb per frame.
///
/// A missing file is not an error - it is a take recorded before pose existed,
/// and the caller reports that as a warning rather than a failure. A file that
/// exists and is malformed *is* an error, because that is a bug or a truncated
/// write and silently reading it as "no pose" would hide both.
[[nodiscard]] bool Read(const std::filesystem::path& file, std::vector<FeedEvent>& out,
                        std::size_t& framesOut, std::string& error);

/// Just the header, for the take list - which wants to know whether a take has
/// pose data without paying to decode a few thousand frames per row.
[[nodiscard]] bool Probe(const std::filesystem::path& file, std::size_t& framesOut);

}  // namespace rds::pose
