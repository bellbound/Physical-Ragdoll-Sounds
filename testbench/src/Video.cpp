#include "Video.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include <windows.h>

#include <GL/gl.h>

#include "stb_image.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(line);
    while (std::getline(ss, cur, ',')) out.push_back(cur);
    return out;
}

double ToDouble(const std::string& s, double fallback = 0.0) {
    try {
        return std::stod(s);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

// ── running ffmpeg ───────────────────────────────────────────────────────────

int RunProcess(const std::wstring& commandLine) {
    std::wstring mutableLine = commandLine;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

int RunProcessCapture(const std::wstring& commandLine, std::string& output) {
    output.clear();

    // A temp file rather than a pipe. A pipe needs a reader thread or a bounded
    // read - a child that fills the 4 KB buffer while nobody is draining it
    // deadlocks - and the one caller here is ffprobe printing a single number.
    wchar_t tempDir[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) == 0 ||
        GetTempFileNameW(tempDir, L"rds", 0, tempFile) == 0) {
        return -1;
    }

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE sink = CreateFileW(tempFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (sink == INVALID_HANDLE_VALUE) {
        DeleteFileW(tempFile);
        return -1;
    }

    std::wstring mutableLine = commandLine;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = sink;
    si.hStdError = sink;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    int code = -1;
    if (CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        code = static_cast<int>(exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(sink);

    std::ifstream in(tempFile, std::ios::binary);
    if (in) {
        std::ostringstream text;
        text << in.rdbuf();
        output = text.str();
    }
    in.close();
    DeleteFileW(tempFile);
    return code;
}

// ── frame caches and clips ───────────────────────────────────────────────────

fs::path FrameCacheDir(const fs::path& cacheRoot, const std::string& stem) {
    return cacheRoot / stem;
}

bool HasFrameCache(const fs::path& cacheRoot, const std::string& stem) {
    std::error_code ec;
    return fs::exists(FrameCacheDir(cacheRoot, stem) / "cache.txt", ec);
}

bool BuildFrameCache(const fs::path& mp4, const fs::path& cacheRoot, const std::string& stem,
                     std::string& error) {
    std::error_code ec;
    if (mp4.empty() || !fs::exists(mp4, ec)) {
        error = "no video to build from";
        return false;
    }
    const fs::path dir = FrameCacheDir(cacheRoot, stem);
    fs::create_directories(dir, ec);

    std::wostringstream cmd;
    cmd << L"ffmpeg -y -v error -i \"" << mp4.wstring() << L"\" -vf \"fps=" << kCacheFps
        << L",scale=-2:" << kCacheHeight << L"\" -q:v 5 \"" << (dir / L"%05d.jpg").wstring()
        << L"\"";
    const int rc = RunProcess(cmd.str());
    if (rc != 0) {
        error = std::format("ffmpeg failed (rc {}) - is it on PATH?", rc);
        return false;
    }
    std::ofstream stamp(dir / "cache.txt");
    stamp << "fps=" << kCacheFps << "\n";
    return true;
}

void ClearFrameCache(const fs::path& cacheRoot, const std::string& stem) {
    std::error_code ec;
    fs::remove_all(FrameCacheDir(cacheRoot, stem), ec);
}

std::size_t ClearAllFrameCaches(const fs::path& cacheRoot) {
    std::error_code ec;
    std::size_t gone = 0;
    if (!fs::exists(cacheRoot, ec)) {
        return 0;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(cacheRoot, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        fs::remove_all(e.path(), ec);
        if (!ec) {
            ++gone;
        }
    }
    return gone;
}

bool CutVideo(const fs::path& in, const fs::path& out, double startMs, double endMs,
              std::string& error) {
    std::error_code ec;
    if (in.empty() || !fs::exists(in, ec)) {
        error = "no source video";
        return false;
    }
    if (endMs <= startMs) {
        error = "the selection has no length";
        return false;
    }
    fs::create_directories(out.parent_path(), ec);

    // -ss before -i so ffmpeg seeks rather than decodes its way there, and
    // -accurate_seek (the default in that position since 2.1) so it still lands
    // on the right frame. -t rather than -to, because -to after -i is measured
    // against the *output* timeline and the two disagree once -ss has moved it.
    std::wostringstream cmd;
    cmd << L"ffmpeg -y -v error -ss " << std::fixed << std::setprecision(3) << (startMs / 1000.0)
        << L" -i \"" << in.wstring() << L"\" -t " << ((endMs - startMs) / 1000.0)
        << L" -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p -an \"" << out.wstring()
        << L"\"";
    const int rc = RunProcess(cmd.str());
    if (rc != 0) {
        error = std::format("ffmpeg failed (rc {}) - is it on PATH?", rc);
        return false;
    }
    return true;
}

double ProbeDurationMs(const fs::path& video) {
    std::error_code ec;
    if (video.empty() || !fs::exists(video, ec)) {
        return 0.0;
    }
    std::wostringstream cmd;
    cmd << L"ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 \""
        << video.wstring() << L"\"";
    std::string output;
    if (RunProcessCapture(cmd.str(), output) != 0) {
        return 0.0;
    }
    return ToDouble(output, 0.0) * 1000.0;
}

// ── the sync fit ─────────────────────────────────────────────────────────────

SyncModel FitSync(const fs::path& syncCsv) {
    SyncModel m;
    std::ifstream in(syncCsv);
    if (!in) return m;

    struct Row {
        double t, obs, rtt;
    };
    std::vector<Row> rows;
    std::string line;
    bool sawHeader = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!sawHeader && line.find("t_ms") != std::string::npos) {
            sawHeader = true;
            continue;
        }
        const std::vector<std::string> f = SplitCsv(line);
        if (f.size() < 3) continue;
        rows.push_back({ToDouble(f[0]), ToDouble(f[1]), ToDouble(f[2])});
    }
    m.rowsTotal = static_cast<int>(rows.size());
    if (rows.empty()) return m;

    // "Low rtt" is relative: take everything within 5 ms of the best round trip
    // in this take, so a take whose best row is 12 ms is not thrown away whole.
    double minRtt = rows.front().rtt;
    for (const Row& r : rows) minRtt = std::min(minRtt, r.rtt);
    m.minRttMs = minRtt;

    std::vector<Row> good;
    for (const Row& r : rows)
        if (r.rtt <= minRtt + 5.0) good.push_back(r);
    if (good.empty()) good = rows;
    m.rowsUsed = static_cast<int>(good.size());

    if (good.size() == 1) {
        m.slope = 1.0;
        m.intercept = good[0].obs - good[0].t;
    } else {
        // Ordinary least squares. Two rows is the common case here and gives the
        // exact line through them, which is what a "piecewise fit" degenerates
        // to at this sample count.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const double n = static_cast<double>(good.size());
        for (const Row& r : good) {
            sx += r.t;
            sy += r.obs;
            sxx += r.t * r.t;
            sxy += r.t * r.obs;
        }
        const double den = n * sxx - sx * sx;
        if (std::fabs(den) < 1e-9) {
            m.slope = 1.0;
            m.intercept = (sy - sx) / n;
        } else {
            m.slope = (n * sxy - sx * sy) / den;
            m.intercept = (sy - m.slope * sx) / n;
        }
    }
    m.driftMsPerSec = (m.slope - 1.0) * 1000.0;
    m.valid = true;
    return m;
}

// ── the persisted nudge ──────────────────────────────────────────────────────

void OffsetStore::Load(const fs::path& file) {
    m_file = file;
    m_values.clear();
    m_lines.clear();
    std::ifstream in(file);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        m_lines.push_back(line);
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m_values[line.substr(0, eq)] = ToDouble(line.substr(eq + 1));
    }
}

void OffsetStore::Save() const {
    if (m_file.empty()) return;
    std::error_code ec;
    fs::create_directories(m_file.parent_path(), ec);
    std::ofstream out(m_file, std::ios::trunc);
    if (!out) return;

    // Rewrite through the file as it was read, so every comment survives. The
    // previous version regenerated three fixed header lines and dropped the rest,
    // which threw away the explanation of why these numbers are not all 2000 and
    // the record of how each one was measured - the only place either was written
    // down. A hand-editable file the tool silently strips is not hand-editable.
    if (m_lines.empty()) {
        out << "# Per-take video offset in milliseconds. video position = take time + offset.\n"
            << "#\n"
            << "# Hand-editable: nudge a line and the app will keep it. Comments survive a save.\n";
        for (const auto& [k, v] : m_values) {
            out << k << "=" << v << "\n";
        }
        return;
    }

    std::set<std::string> written;
    for (const std::string& line : m_lines) {
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == '[' || eq == std::string::npos) {
            out << line << "\n";
            continue;
        }
        const std::string key = line.substr(0, eq);
        const auto it = m_values.find(key);
        if (it == m_values.end()) {
            out << line << "\n";  // someone removed it in memory; leave the file's copy alone
            continue;
        }
        out << key << "=" << it->second << "\n";
        written.insert(key);
    }
    for (const auto& [k, v] : m_values) {
        if (!written.count(k)) {
            out << k << "=" << v << "\n";
        }
    }
}

double OffsetStore::Get(const std::string& stem, double fallback) const {
    auto it = m_values.find(stem);
    return it == m_values.end() ? fallback : it->second;
}

void OffsetStore::Set(const std::string& stem, double value) {
    m_values[stem] = value;
    Save();
}

void OffsetStore::Erase(const std::string& stem) {
    if (m_values.erase(stem) != 0) {
        Save();
    }
}

// ── frames ───────────────────────────────────────────────────────────────────

VideoTake::~VideoTake() { Close(); }

void VideoTake::Close() {
    m_stopWorker.store(true, std::memory_order_relaxed);
    m_requestCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_stopWorker.store(false, std::memory_order_relaxed);

    for (auto& [idx, t] : m_textures) {
        const GLuint id = t.id;
        glDeleteTextures(1, &id);
    }
    m_textures.clear();
    m_frames.clear();
    m_mp4.clear();
    m_dir.clear();
    m_status[0] = 0;
    m_direct = false;
    m_unbuilt = false;
    m_requested = -1;
    m_lastDecoded.store(-1, std::memory_order_relaxed);
    m_ready.store(false, std::memory_order_release);
    m_durationMs = 0.0;
    m_width = m_height = 0;
}

void VideoTake::Open(const std::string& stem, const fs::path& mp4, const fs::path& cacheRoot,
                     Mode mode) {
    Close();
    m_stem = stem;
    if (mp4.empty() || !fs::exists(mp4)) {
        // No mp4 is not the same as no frames: a take whose cache was built and
        // whose video was then deleted still scrubs, and that is the ordinary
        // state of every take once it has been kept.
        m_dir = FrameCacheDir(cacheRoot, stem);
        if (HasFrameCache(cacheRoot, stem)) {
            m_worker = std::thread([this] { Decode(); });
            return;
        }
        m_dir.clear();
        std::snprintf(m_status, sizeof(m_status), "no video for this take");
        m_ready.store(true, std::memory_order_release);
        return;
    }
    m_mp4 = mp4;
    m_dir = FrameCacheDir(cacheRoot, stem);

    const bool cached = HasFrameCache(cacheRoot, stem);
    m_unbuilt = !cached;
    // Direct is a preference, not an instruction: with the frames already on
    // disk there is nothing to be gained by seeking the mp4, and the cache is
    // both faster and exact.
    m_direct = mode == Mode::kDirect && !cached;

    if (m_direct) {
        m_worker = std::thread([this] { DirectWorker(); });
    } else {
        m_worker = std::thread([this] { Decode(); });
    }
}

void VideoTake::Decode() {
    std::error_code ec;
    const fs::path cacheRoot = m_dir.parent_path();
    const bool cached = HasFrameCache(cacheRoot, m_stem);

    if (!cached) {
        std::string error;
        if (!BuildFrameCache(m_mp4, cacheRoot, m_stem, error)) {
            std::snprintf(m_status, sizeof(m_status), "%s", error.c_str());
            m_ready.store(true, std::memory_order_release);
            return;
        }
    }

    for (const fs::directory_entry& e : fs::directory_iterator(m_dir, ec)) {
        if (e.path().extension() == ".jpg") m_frames.push_back(e.path());
    }
    std::sort(m_frames.begin(), m_frames.end());
    m_fps = kCacheFps;
    m_durationMs = m_frames.empty() ? 0.0 : static_cast<double>(m_frames.size()) * 1000.0 / m_fps;
    m_unbuilt = false;

    if (m_frames.empty()) {
        std::snprintf(m_status, sizeof(m_status), "no frames decoded");
    } else {
        int w = 0, h = 0, c = 0;
        if (stbi_info(m_frames.front().string().c_str(), &w, &h, &c)) {
            m_width = w;
            m_height = h;
        }
        std::snprintf(m_status, sizeof(m_status), "%zu frames at %d fps, %dx%d (%s)",
                      m_frames.size(), kCacheFps, m_width, m_height, cached ? "cached" : "decoded");
    }
    m_ready.store(true, std::memory_order_release);
}

fs::path VideoTake::DirectFramePath(int index) const {
    return m_dir / "direct" / std::format("{:05}.jpg", index);
}

void VideoTake::DirectWorker() {
    // The length first, so the transport and the offset heuristic have something
    // to work with before a single frame has been pulled.
    m_durationMs = ProbeDurationMs(m_mp4);
    m_fps = kCacheFps;
    std::error_code ec;
    fs::create_directories(m_dir / "direct", ec);
    std::snprintf(m_status, sizeof(m_status), "direct from %s (%.1f s), no cache built",
                  m_mp4.filename().string().c_str(), m_durationMs / 1000.0);
    m_ready.store(true, std::memory_order_release);

    while (!m_stopWorker.load(std::memory_order_relaxed)) {
        int index = -1;
        {
            std::unique_lock lock{m_requestMutex};
            m_requestCv.wait(lock, [this] {
                return m_requested >= 0 || m_stopWorker.load(std::memory_order_relaxed);
            });
            if (m_stopWorker.load(std::memory_order_relaxed)) return;
            index = m_requested;
            m_requested = -1;
        }
        const fs::path out = DirectFramePath(index);
        if (fs::exists(out, ec)) {
            m_lastDecoded.store(index, std::memory_order_release);
            continue;
        }

        // -ss before -i so the seek is a seek and not a decode from zero; a
        // frame two minutes in would otherwise take a second and a half and the
        // whole mode would be unusable.
        std::wostringstream cmd;
        cmd << L"ffmpeg -y -v error -ss " << std::fixed << std::setprecision(3)
            << (static_cast<double>(index) / kCacheFps) << L" -i \"" << m_mp4.wstring()
            << L"\" -frames:v 1 -vf \"scale=-2:" << kCacheHeight << L"\" -q:v 5 \"" << out.wstring()
            << L"\"";
        if (RunProcess(cmd.str()) == 0) {
            m_lastDecoded.store(index, std::memory_order_release);
        }
    }
}

void VideoTake::Request(int index) {
    {
        std::lock_guard lock{m_requestMutex};
        // Latest wins. While the scrub bar is being dragged every request is
        // superseded before ffmpeg has finished, and a queue would spend a
        // minute catching up on frames nobody is looking at any more.
        m_requested = index;
    }
    m_requestCv.notify_one();
}

unsigned VideoTake::LoadFile(const fs::path& file) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(file.string().c_str(), &w, &h, &ch, 4);
    if (!pixels) return 0;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    m_width = w;
    m_height = h;
    return id;
}

unsigned VideoTake::LoadFrame(int index) {
    if (index < 0 || index >= static_cast<int>(m_frames.size())) return 0;
    return LoadFile(m_frames[static_cast<std::size_t>(index)]);
}

unsigned VideoTake::TextureAt(double videoTimeMs) {
    if (!Ready()) return 0;

    int index = static_cast<int>(std::llround(videoTimeMs * 0.001 * m_fps));
    if (!m_direct) {
        if (m_frames.empty()) return 0;
        index = std::clamp(index, 0, static_cast<int>(m_frames.size()) - 1);
    } else if (index < 0) {
        return 0;
    }

    ++m_clock;
    auto it = m_textures.find(index);
    if (it != m_textures.end()) {
        it->second.used = m_clock;
        return it->second.id;
    }

    unsigned id = 0;
    if (m_direct) {
        std::error_code ec;
        const fs::path file = DirectFramePath(index);
        if (!fs::exists(file, ec)) {
            Request(index);
            // Show the freshest thing we do have rather than nothing. A black
            // rectangle every time the playhead moves reads as broken; a frame a
            // few tenths late reads as a decode catching up, which is what it is.
            const int fallback = m_lastDecoded.load(std::memory_order_acquire);
            if (const auto cached = m_textures.find(fallback); cached != m_textures.end()) {
                cached->second.used = m_clock;
                return cached->second.id;
            }
            if (fallback >= 0 && fs::exists(DirectFramePath(fallback), ec)) {
                const unsigned late = LoadFile(DirectFramePath(fallback));
                if (late != 0) {
                    m_textures[fallback] = Tex{late, m_clock};
                    return late;
                }
            }
            return m_textures.empty() ? 0u : m_textures.begin()->second.id;
        }
        id = LoadFile(file);
    } else {
        id = LoadFrame(index);
    }
    if (!id) return 0;
    m_textures[index] = Tex{id, m_clock};

    while (m_textures.size() > 48) {
        auto oldest = m_textures.begin();
        for (auto i = m_textures.begin(); i != m_textures.end(); ++i)
            if (i->second.used < oldest->second.used) oldest = i;
        const GLuint dead = oldest->second.id;
        glDeleteTextures(1, &dead);
        m_textures.erase(oldest);
    }
    return id;
}

bool VideoTake::BuildCacheAndDropVideo(const fs::path& cacheRoot, std::string& error) {
    if (m_mp4.empty()) {
        error = "this take has no video to build from";
        return false;
    }
    const fs::path mp4 = m_mp4;
    const std::string stem = m_stem;

    if (!BuildFrameCache(mp4, cacheRoot, stem, error)) return false;

    // The video goes. That is the whole point of the cache: 30 fps at 360p is
    // everything the timeline can show, the mp4 is two orders of magnitude
    // bigger, and a folder of takes that each kept both fills a disk in an
    // afternoon. Anything that wants the original extracts it before building.
    Close();
    std::error_code ec;
    fs::remove(mp4, ec);
    fs::remove_all(FrameCacheDir(cacheRoot, stem) / "direct", ec);

    Open(stem, {}, cacheRoot, Mode::kFrameCache);
    return true;
}

}  // namespace tb
