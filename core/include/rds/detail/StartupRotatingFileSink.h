#pragma once

#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

// Copied verbatim from skse/SkyrimNet/include/common/utils/StartupRotatingFileSink.h,
// with only the namespace changed. It is header-only and touches nothing but spdlog and
// Win32, so it is portable in the sense core/ needs: the testbench builds it with no game
// anywhere near it.

namespace rds::detail {

// Sink that rotates the log file exactly once — on open — then writes without
// any in-session size checks.
//
// Rotation behavior:
//   max_file_size == 0  →  always rotate the existing file on open
//   max_file_size  > 0  →  only rotate if the existing file is >= max_file_size bytes
//
// The rotated filename uses the file's creation time (via GetFileTime) to produce
// e.g. "SkyrimNet.2024-01-15_14-30-22.log". Falls back to index increment
// ("SkyrimNet.1.log", "SkyrimNet.2.log", …) if GetFileTime fails.
//
// max_files == 0  →  keep all rotated files (unlimited)
// max_files  > 0  →  after rotation, delete oldest rotated files until count <= max_files
template <typename Mutex>
class startup_rotating_file_sink final : public spdlog::sinks::base_sink<Mutex> {
public:
    explicit startup_rotating_file_sink(spdlog::filename_t base_filename,
                                        std::size_t max_file_size = 0,
                                        std::size_t max_files = 0,
                                        const spdlog::file_event_handlers& event_handlers = {})
        : base_filename_(std::move(base_filename))
        , max_file_size_(max_file_size)
        , max_files_(max_files)
        , event_handlers_(event_handlers)
        , file_helper_(event_handlers_) {
        const bool rotated = rotate_on_open_();
        file_helper_.open(base_filename_, false);
        if (rotated) reset_creation_time_(std::filesystem::path(base_filename_));
        prune_old_logs_(std::filesystem::path(base_filename_));
    }

    spdlog::filename_t filename() {
        std::lock_guard<Mutex> lock(spdlog::sinks::base_sink<Mutex>::mutex_);
        return file_helper_.filename();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        file_helper_.write(formatted);
    }

    void flush_() override { file_helper_.flush(); }

private:
    bool rotate_on_open_() {
        namespace fs = std::filesystem;
        const fs::path base_path(base_filename_);

        if (!fs::exists(base_path)) return false;

        if (max_file_size_ > 0) {
            std::error_code ec;
            if (fs::file_size(base_path, ec) < max_file_size_ && !ec) return false;
        }

        fs::path dest = timestamp_name_(base_path);
        if (dest.empty()) dest = index_name_(base_path);

        std::error_code ec;
        fs::rename(base_path, dest, ec);
        // On rename failure, open() below appends to the existing file — acceptable fallback.
        return !ec;
    }

    // After rotation, Windows NTFS tunnel-caching reuses the old file's creation timestamp
    // for any new file created at the same path within ~15 seconds. Reset it to now so the
    // next startup generates a unique rotated filename instead of colliding with this one.
    static void reset_creation_time_(const std::filesystem::path& p) {
#if defined(_WIN32)
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        SetFileTime(h, &now, nullptr, nullptr);
        CloseHandle(h);
#endif
    }

    // Deletes the oldest rotated files (matching <stem>.<anything><ext> in the same directory)
    // until the count is within max_files_. Sorted by last write time, oldest first.
    // max_files_ == 0 means unlimited — no pruning.
    void prune_old_logs_(const std::filesystem::path& base_path) {
        if (max_files_ == 0) return;

        namespace fs = std::filesystem;
        const auto dir        = base_path.parent_path();
        const auto ext        = base_path.extension();
        const auto base_stem  = base_path.stem().string();  // ASCII-safe for log filenames

        std::vector<fs::path> candidates;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            std::error_code ecEntry;
            if (!entry.is_regular_file(ecEntry) || ecEntry) continue;
            const auto& p = entry.path();
            if (p == base_path) continue;
            if (p.extension() != ext) continue;
            const auto s = p.stem().string();
            // Match "<base_stem>.<anything>" — same prefix, dot separator, non-empty suffix
            if (s.size() <= base_stem.size() + 1) continue;
            if (s.compare(0, base_stem.size(), base_stem) != 0) continue;
            if (s[base_stem.size()] != '.') continue;
            candidates.push_back(p);
        }

        if (candidates.size() <= max_files_) return;

        std::sort(candidates.begin(), candidates.end(), [](const fs::path& a, const fs::path& b) {
            std::error_code ea, eb;
            auto ta = fs::last_write_time(a, ea);
            auto tb = fs::last_write_time(b, eb);
            // On error, treat the file as newest so it is never incorrectly deleted.
            if (ea) ta = fs::file_time_type::max();
            if (eb) tb = fs::file_time_type::max();
            return ta < tb;
        });

        const std::size_t to_delete = candidates.size() - max_files_;
        for (std::size_t i = 0; i < to_delete; ++i) {
            std::error_code ec2;
            fs::remove(candidates[i], ec2);
        }
    }

    // Returns <stem>.<YYYY-MM-DD_HH-MM-SS><ext> using the file's creation time,
    // or empty path on any failure.
    static std::filesystem::path timestamp_name_(const std::filesystem::path& p) {
#if defined(_WIN32)
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};

        FILETIME ft{};
        const bool ok = GetFileTime(h, &ft, nullptr, nullptr);
        CloseHandle(h);
        if (!ok) return {};

        SYSTEMTIME st{};
        if (!FileTimeToSystemTime(&ft, &st)) return {};

        char buf[32];
        snprintf(buf, sizeof(buf), ".%04d-%02d-%02d_%02d-%02d-%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return splice_suffix_(p, buf);
#else
        return {};
#endif
    }

    // Returns the first <stem>.<N><ext> path that does not already exist.
    static std::filesystem::path index_name_(const std::filesystem::path& p) {
        for (std::size_t i = 1; i < 100'000; ++i) {
            auto candidate = splice_suffix_(p, "." + std::to_string(i));
            if (!std::filesystem::exists(candidate)) return candidate;
        }
        return splice_suffix_(p, ".old");
    }

    // "foo.log" + ".1"  →  "foo.1.log"
    static std::filesystem::path splice_suffix_(const std::filesystem::path& p,
                                                const std::string& ascii_suffix) {
        const std::filesystem::path sfx(ascii_suffix);
        return p.parent_path() /
               (p.stem().native() + sfx.native() + p.extension().native());
    }

    spdlog::filename_t base_filename_;
    std::size_t max_file_size_;
    std::size_t max_files_;
    spdlog::file_event_handlers event_handlers_;
    spdlog::details::file_helper file_helper_;  // must be declared after event_handlers_
};

using startup_rotating_file_sink_mt = startup_rotating_file_sink<std::mutex>;
using startup_rotating_file_sink_st = startup_rotating_file_sink<spdlog::details::null_mutex>;

}  // namespace rds::detail
