#pragma once

// Reading a capture back in as a Feed.
//
// The files are QuickModMenuNG's: one CSV of events, one YAML sidecar of
// everything the rows do not carry, and optionally a two-row _sync.csv and an
// mp4. Research/02-Data-Dictionary.md is the column reference and says what each
// one is worth; the loader implements its warnings rather than trusting the
// columns:
//
//   - sort by t_ms, never by seq. The session_stop row is written out of band
//     with seq 0, so sorting by seq puts the last row first
//   - take the CSV's mass, not the sidecar's. Three of twelve takes snapshot the
//     sidecar while the bodies are keyframed and every limb reads 0
//   - coverage that is nameless and weightless is `bare`, not `clothing`. TNG's
//     skin is a real TESObjectARMO occupying five slots
//   - read the `coverage:` map, not slot occupancy. Four slots emit a stub with
//     no armour_type on a stripped subject
//
// This is a replay of the game's own capture, not a second input format. It
// fills the same FeedEvent the live path fills, which is what keeps one backend
// one backend.

#include <filesystem>
#include <string>
#include <vector>

#include "rds/Feed.h"

namespace rds {

// ── the CSV's vocabulary, the way out ────────────────────────────────────────
//
// The loader turns the recorder's names back into ids; these turn them forwards
// again, so the testbench can write a take the loader will read. They live here
// rather than beside the writer because this file already owns the mapping, and
// a second table is how a written take stops round-tripping.

/// The MATERIAL_ID's name as RE::MaterialIDToString gives it. "-" when nothing
/// in the table matches, which is what the loader reads back as "unresolved".
[[nodiscard]] std::string_view MaterialName(std::uint32_t materialId);

/// A canonical name per layer. The loader maps several names onto one layer -
/// Static and AnimStatic both mean kStatic - so this picks one and the round
/// trip is lossless in the direction that matters.
[[nodiscard]] std::string_view LayerName(ColLayer layer);
[[nodiscard]] std::string_view SourceName(MaterialSource source);
[[nodiscard]] std::string_view PhaseName(ActorPhase phase);

/// What a take says about itself, for the testbench's recording list.
struct RecordingInfo {
    std::string stem;   ///< "Proventus_Avenicci_impacts_log_7"
    std::string note;   ///< recording.note - the take's *intent*, not a description of it
    std::string actorName;
    std::string cell;
    TimeMs durationMs{};
    std::uint32_t impacts{};
    std::uint32_t dropped{};  ///< non-zero means the take is incomplete
    bool complete{};

    /// True when `<stem>_pose.bin` is beside the take and carries frames.
    ///
    /// False is not a failure - the take replays exactly as it always did - but
    /// it is worth saying out loud, because air time then has no measurement
    /// behind it and falls back to inferring flight from the gaps between
    /// contacts. Every take captured before pose existed reads false; the
    /// `limb_sample` rows those CSVs carry are two snapshots at ragdoll_start
    /// and ragdoll_end, which is a launch pose, not a signal.
    bool hasBodySamples{};
    std::size_t poseFrames{};

    /// Rows in `<stem>_vanilla.csv` - what Skyrim's own impact system played
    /// during the take. Zero for every take recorded before the hook existed, and
    /// for any recorded on a runtime where it could not be installed.
    std::size_t vanillaRows{};

    /// Present when the take has video beside it.
    std::filesystem::path videoPath;
    /// video_time_ms = t_ms + offsetMs + driftMsPerSec * t_ms / 1000.
    ///
    /// Both terms are fitted by least squares through the low-rtt rows of the
    /// sync csv rather than taken from the first row, because the two clocks
    /// genuinely drift: measured at +0.29 to +2.92 ms per second across these
    /// takes, which is up to 20 ms of slip over a seven-second one - more than a
    /// frame, and enough to put a hit on the wrong side of the picture. Whether
    /// the intercept can be believed as it stands depends on `videoIsWholeOutput`.
    double videoOffsetMs{};
    double videoDriftMsPerSec{};
    bool hasSync{};
    /// True when the mp4 beside the take is OBS's own output for this take, and
    /// not a clip cut out of a longer recording.
    ///
    /// The distinction is the whole difference between the two sync regimes. A
    /// cut clip's sync clocks belong to the recording it was cut from, and the
    /// cut point is written down nowhere - so the intercept is useless against
    /// the file on disk and the offset has to be nudged by hand. A whole output
    /// starts where the sync track says it starts, so the intercept *is* the
    /// offset. Read off the sidecar rather than guessed: `obs.output_path` names
    /// a file the take owns, and `obs.external_recording` says the take merely
    /// rode a recording somebody else had already started.
    bool videoIsWholeOutput{};
};

/// A loaded take, ready to replay.
class Recording final : public IFeed {
public:
    /// `csvPath` is the take's CSV; the YAML and _sync.csv are found beside it
    /// by stem. Returns false and fills `error` on a malformed file.
    [[nodiscard]] bool Load(const std::filesystem::path& csvPath, std::string& error);

    [[nodiscard]] const RecordingInfo& Info() const { return m_info; }

    /// Every take in a directory, sorted by name. Reads only the YAML headers,
    /// so it is cheap enough to run at startup over the whole research folder.
    [[nodiscard]] static std::vector<RecordingInfo> Scan(const std::filesystem::path& directory);

    // ── IFeed ────────────────────────────────────────────────────────────────
    bool Drain(TimeMs untilMs, std::vector<FeedEvent>& out) override;
    [[nodiscard]] const ActorProfile* Profile(ActorId actor) const override;
    [[nodiscard]] const ListenerState& Listener() const override;
    [[nodiscard]] float FrameTimeSec() const override;

    /// Rewind to the start. The testbench does this every loop.
    void Rewind();

    /// The frame boundaries, derived from the gaps between contact batches - a
    /// gap over `frameGapMs` starts a new frame. The offline runner ticks the
    /// engine at exactly these, so a replay steps the same way the game did.
    ///
    /// The quiet stretches are filled back in at the take's own median frame
    /// interval: the game ran frames there too, and the engine does its phase
    /// work on the tick, so a replay that only ticked where the solver spoke
    /// would leave a fall stuck mid-phase for as long as the body was in the air.
    [[nodiscard]] const std::vector<TimeMs>& FrameBoundaries() const { return m_frames; }

    /// The take's median frame interval, in ms. What the runner steps by past the
    /// end of the recording, so the closing cue still gets a tick to land on.
    [[nodiscard]] double FrameStepMs() const { return m_frameStepMs; }

    [[nodiscard]] const std::vector<FeedEvent>& Events() const { return m_events; }

    /// The vanilla track, in file order. Empty when the take has none.
    ///
    /// Held apart from `m_events` rather than merged into the replay stream, and
    /// that is deliberate: these rows are not something the solver saw, they are
    /// what the *game* did with what it saw. Merging them would put them in front
    /// of the engine, into `eventsIn`, and into the frame-boundary derivation - a
    /// reference track quietly becoming an input.
    [[nodiscard]] const std::vector<FeedEvent>& VanillaTrack() const { return m_vanilla; }

private:
    RecordingInfo m_info;
    ActorProfile m_profile;
    ListenerState m_listener;
    std::vector<FeedEvent> m_events;  ///< sorted by timeMs
    std::vector<FeedEvent> m_vanilla;  ///< the vanilla track, in file order
    std::vector<TimeMs> m_frames;
    std::size_t m_cursor{};
    /// How far into `m_frames` the frame-time derivation has already walked.
    /// Separate from `m_cursor` because the two advance on different things -
    /// one on events consumed, one on frame boundaries passed - and a tick with
    /// no contacts in it moves the second without moving the first.
    std::size_t m_frameCursor{};
    float m_frameTimeSec{};
    double m_frameStepMs{16.6};
};

}  // namespace rds
