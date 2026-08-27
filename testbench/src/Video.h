#pragma once

// The take's video, and everything done to a video file.
//
// Two clocks have to be reconciled: `video_time_ms = t_ms + offset`, with the
// offset fitted through the *low-rtt* rows of the take's _sync.csv rather than
// taken from the first row, because the two clocks drift over a long take. The
// mp4s are also cuts of a longer OBS recording whose cut point is recorded nowhere
// (05 §9), so the fit gets the drift and a persisted per-take nudge gets the cut.
//
// ── two ways to get a frame ──────────────────────────────────────────────────
//
// **Frame cache** is the shipping path: ffmpeg decodes the whole clip to jpgs
// once and scrubbing is a file read. Fast, survives a restart, and why the mp4 can
// then be deleted - a 200 MB take becomes 8 MB of frames.
//
// **Direct** is experimental, behind Options -> Enable Video Sync: it keeps the
// mp4 and pulls single frames out with ffmpeg on demand, so a take recorded thirty
// seconds ago is scrubbable immediately. The cost is a seek per frame, so it
// decodes on a worker, shows the last frame until the new one lands, and falls
// back to the frame cache whenever a clip has one.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tb {

/// obs_duration_ms = slope * t_ms + intercept, fitted through the rows worth
/// trusting. With one usable row it degenerates to that row's own offset.
struct SyncModel {
    bool valid{};
    double slope{1.0};
    double intercept{0.0};
    int rowsTotal{};
    int rowsUsed{};
    double minRttMs{};
    double driftMsPerSec{};  ///< (slope - 1) * 1000, the number the fit exists for

    [[nodiscard]] double ObsMs(double takeMs) const { return slope * takeMs + intercept; }
};

[[nodiscard]] SyncModel FitSync(const std::filesystem::path& syncCsv);

/// The per-take nudge, persisted beside the frame cache so a take opened
/// tomorrow lines up the way it was left today.
class OffsetStore {
public:
    void Load(const std::filesystem::path& file);
    void Save() const;
    [[nodiscard]] bool Has(const std::string& stem) const { return m_values.count(stem) != 0; }
    [[nodiscard]] double Get(const std::string& stem, double fallback) const;
    void Set(const std::string& stem, double value);
    void Erase(const std::string& stem);

private:
    std::filesystem::path m_file;
    std::map<std::string, double> m_values;
    /// The file exactly as it was read. Save() rewrites through this rather than
    /// regenerating, so the comments survive - and this file is mostly comments,
    /// because a bare list of numbers does not explain why they are not all 2000.
    std::vector<std::string> m_lines;
};

// ═════════════════════════════════════════════════════════════════════════════
// what can be done to a video file
// ═════════════════════════════════════════════════════════════════════════════

/// Run a command line without cmd.exe in the middle.
///
/// std::system() would put the string through `cmd /c`, and cmd's quoting rules
/// plus its habit of expanding `%` mangle exactly the two things these commands
/// need: paths with spaces, and ffmpeg's `%05d` output pattern. Exposed because
/// building a cache, cutting a clip and pulling one frame are all one ffmpeg
/// call and they do not all live in this file.
int RunProcess(const std::wstring& commandLine);

/// The same, with stdout collected. Used for ffprobe, which answers on stdout
/// and nowhere else.
int RunProcessCapture(const std::wstring& commandLine, std::string& output);

/// Where a take's frames live.
[[nodiscard]] std::filesystem::path FrameCacheDir(const std::filesystem::path& cacheRoot,
                                                  const std::string& stem);
[[nodiscard]] bool HasFrameCache(const std::filesystem::path& cacheRoot, const std::string& stem);

/// Decode `mp4` to jpgs under `<cacheRoot>/<stem>`. Blocking, and the caller is
/// expected to be a worker thread or to accept the stall.
[[nodiscard]] bool BuildFrameCache(const std::filesystem::path& mp4,
                                   const std::filesystem::path& cacheRoot, const std::string& stem,
                                   std::string& error);

void ClearFrameCache(const std::filesystem::path& cacheRoot, const std::string& stem);
/// Every cache under `cacheRoot`. Returns how many directories went.
std::size_t ClearAllFrameCaches(const std::filesystem::path& cacheRoot);

/// Cut `[startMs, endMs)` out of `in` into `out`.
///
/// Re-encoded rather than stream-copied, deliberately: `-c copy` can only cut on
/// a keyframe and OBS writes one about every two seconds, so a copy cut lands up
/// to two seconds away from where it was asked for - which is the whole quantity
/// the video is here to measure.
[[nodiscard]] bool CutVideo(const std::filesystem::path& in, const std::filesystem::path& out,
                            double startMs, double endMs, std::string& error);

/// The clip's length in milliseconds via ffprobe, or 0 when it cannot be read.
[[nodiscard]] double ProbeDurationMs(const std::filesystem::path& video);

/// One take's frames. Decoding runs on a background thread so selecting a
/// recording never stalls the UI; frames are cached on disk across runs.
class VideoTake {
public:
    ~VideoTake();

    VideoTake() = default;
    VideoTake(const VideoTake&) = delete;
    VideoTake& operator=(const VideoTake&) = delete;

    enum class Mode {
        /// Decode the whole clip to jpgs once, then read files. The default.
        kFrameCache,
        /// Keep the mp4 and pull single frames on demand. Experimental.
        kDirect,
    };

    /// Starts (or reuses) the frames for `mp4`. Safe to call with an empty path:
    /// the take simply has no video and everything else still works.
    ///
    /// `kDirect` falls back to the frame cache when one already exists - there is
    /// nothing to be gained by seeking an mp4 when the frames are on disk - so
    /// asking for it is a preference, not an instruction.
    void Open(const std::string& stem, const std::filesystem::path& mp4,
              const std::filesystem::path& cacheRoot, Mode mode = Mode::kFrameCache);
    void Close();

    [[nodiscard]] bool HasVideo() const { return !m_mp4.empty(); }
    [[nodiscard]] bool Ready() const { return m_ready.load(std::memory_order_acquire); }
    /// True when this take is being served straight out of the mp4.
    [[nodiscard]] bool Direct() const { return m_direct; }
    /// True when the mp4 is there and no cache has been built from it - an
    /// "unbuilt" take, which is what the video window offers a button for.
    [[nodiscard]] bool Unbuilt() const { return m_unbuilt; }

    /// Only meaningful once Ready(): the worker publishes it with the flag.
    [[nodiscard]] const char* Status() const { return Ready() ? m_status : "decoding frames..."; }
    [[nodiscard]] double DurationMs() const { return m_durationMs; }
    [[nodiscard]] double Fps() const { return m_fps; }
    [[nodiscard]] int FrameCount() const { return static_cast<int>(m_frames.size()); }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }
    [[nodiscard]] const std::filesystem::path& Path() const { return m_mp4; }

    /// GL texture for the frame at `videoTimeMs`, or 0. Decodes on demand and
    /// keeps a small LRU, which is cheap enough at 30 fps and keeps a nine-take
    /// folder from living in VRAM.
    ///
    /// In direct mode a miss returns the *nearest frame already decoded* rather
    /// than nothing, and queues the one that was asked for - so scrubbing shows
    /// a slightly stale picture instead of a black rectangle.
    [[nodiscard]] unsigned TextureAt(double videoTimeMs);

    /// Build this take's frame cache and, on success, delete the mp4 - the whole
    /// point of the cache is that the 200 MB file stops being needed. Blocking;
    /// the caller shows a spinner or accepts the stall.
    [[nodiscard]] bool BuildCacheAndDropVideo(const std::filesystem::path& cacheRoot,
                                              std::string& error);

private:
    void Decode();
    void DirectWorker();
    unsigned LoadFrame(int index);
    unsigned LoadFile(const std::filesystem::path& file);
    [[nodiscard]] std::filesystem::path DirectFramePath(int index) const;
    void Request(int index);

    std::filesystem::path m_mp4;
    std::filesystem::path m_dir;
    std::string m_stem;
    char m_status[160]{};
    bool m_direct{};
    bool m_unbuilt{};

    std::vector<std::filesystem::path> m_frames;
    double m_fps{30.0};
    double m_durationMs{};
    int m_width{}, m_height{};

    struct Tex {
        unsigned id{};
        std::uint64_t used{};
    };
    std::map<int, Tex> m_textures;
    std::uint64_t m_clock{};

    std::thread m_worker;
    std::atomic<bool> m_ready{false};

    // ── direct mode's extractor ──────────────────────────────────────────────
    //
    // One worker and a single pending index rather than a queue: while you drag
    // the scrub bar every frame is superseded by the next one before ffmpeg has
    // finished, and a queue would spend a minute catching up on frames nobody is
    // looking at any more. Latest wins.
    std::mutex m_requestMutex;
    std::condition_variable m_requestCv;
    int m_requested{-1};
    std::atomic<bool> m_stopWorker{false};
    std::atomic<int> m_lastDecoded{-1};
};

/// The decoded frame rate and the max width the cache is built at. 30 fps is
/// two frames per 60 Hz UI refresh, which is as much as scrubbing can show.
inline constexpr int kCacheFps = 30;
inline constexpr int kCacheHeight = 360;

}  // namespace tb
