#pragma once

// The pose sidecar: every ragdoll limb, every tick.
//
// The impacts CSV is a *sparse* record - one row per collision - and it is meant
// to stay readable in a spreadsheet and diffable in git. Pose is the opposite
// shape: eighteen limbs at sixty hertz, dense, uninteresting to read by eye, and
// ten times the row count. Putting it in the CSV would cost the one property
// that file has.
//
// So it lives beside the take as `<stem>_pose.bin`, found by stem exactly like
// `<stem>.yaml`, `<stem>_sync.csv` and `<stem>.mp4` already are.
//
// This is an *encoding*, not a second input format. Read() hands back the same
// `kLimbSample` FeedEvents the live path pushes through the ring, so by the time
// the engine sees anything the two are indistinguishable - which is what keeps
// one backend one backend, and what makes tuning offline mean something about
// the game.
//
// Frame-oriented rather than record-oriented, because that is how it is written
// and how it is consumed, and because it amortises the timestamp and the actor
// id over eighteen limbs: 516 bytes per frame per actor against 792.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rds/Feed.h"
#include "rds/Types.h"

namespace rds::pose {

inline constexpr char kMagic[8] = {'R', 'D', 'S', 'P', 'O', 'S', 'E', '1'};
inline constexpr std::uint32_t kVersion = 1;

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

static_assert(sizeof(Header) == 32, "the pose header is a fixed 32 bytes on disk");
static_assert(sizeof(FrameHeader) == 12, "the frame header is a fixed 12 bytes on disk");
static_assert(sizeof(LimbRecord) == 28, "a limb record is a fixed 28 bytes on disk");

inline constexpr std::uint32_t kLimbStride = sizeof(LimbRecord);

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
///
/// Rows are grouped into frames by (timeMs, actorId) over the sorted stream,
/// which is how they arrive: one tick publishes one contiguous run per actor.
/// Times are re-based against `originMs`, so a slice shares its CSV's clock -
/// that is what lets a `_cut_N` take carry pose data without a second slicer.
///
/// Writing no frames is not an error and leaves no file behind: a take captured
/// with sampling off should look like a take with no pose data, not like a
/// corrupt one.
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
