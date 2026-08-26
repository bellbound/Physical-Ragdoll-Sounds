#include "rds/Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <format>
#include <memory>
#include <vector>

#include "rds/Engine.h"
#include "rds/detail/StartupRotatingFileSink.h"

namespace rds::log {
namespace {

bool g_installed = false;

[[nodiscard]] spdlog::level::level_enum ToSpdlog(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace: return spdlog::level::trace;
        case LogLevel::kDebug: return spdlog::level::debug;
        case LogLevel::kInfo: return spdlog::level::info;
        case LogLevel::kWarn: return spdlog::level::warn;
        case LogLevel::kError: return spdlog::level::err;
        case LogLevel::kOff: return spdlog::level::off;
    }
    return spdlog::level::info;
}

}  // namespace

void Setup(const Options& options) {
    if (g_installed) {
        spdlog::info("log::Setup called twice; keeping the logger that is already open");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.directory, ec);
    const auto file = options.directory / std::format("{}.log", options.name);

    std::vector<spdlog::sink_ptr> sinks;
    if (options.rotate) {
        // max_file_size 0 means "rotate every launch regardless of size", which
        // is the behaviour that matters: the log from the session that crashed
        // has to survive the next one.
        sinks.push_back(std::make_shared<detail::startup_rotating_file_sink_mt>(
            file.string(), 0, options.maxFiles));
    } else {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(), true));
    }
    if (options.alsoStdout) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    auto logger = std::make_shared<spdlog::logger>("rds", sinks.begin(), sinks.end());
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_default_logger(std::move(logger));
    SetLevel(options.level);
    g_installed = true;

    spdlog::info("Physical Ragdoll Sounds - log opened at {}", file.string());

    // Say what rotation actually did, because "the log from the session that
    // crashed has to survive the next one" is a claim this file makes and has
    // never checked. A session's log went missing between two launches with
    // rotation switched on and no rotated file anywhere, and there was nothing
    // in the log to say whether the sink had rotated, failed to rotate, or never
    // been asked to.
    if (options.rotate) {
        std::size_t kept = 0;
        const auto stem = file.stem().string();
        for (const auto& entry : std::filesystem::directory_iterator(options.directory, ec)) {
            if (ec) {
                break;
            }
            const auto& path = entry.path();
            if (path == file || path.extension() != file.extension()) {
                continue;
            }
            const auto other = path.stem().string();
            if (other.size() > stem.size() + 1 && other.compare(0, stem.size(), stem) == 0 &&
                other[stem.size()] == '.') {
                ++kept;
            }
        }
        spdlog::info("log: rotation is on, keeping {} file(s); {} previous log(s) are on disk "
                     "beside this one",
                     options.maxFiles == 0 ? std::string{"every"}
                                           : std::to_string(options.maxFiles),
                     kept);
    } else {
        spdlog::info("log: rotation is off - this file was truncated, and last session's log is "
                     "gone");
    }
}

void SetLevel(LogLevel level) {
    const auto mapped = ToSpdlog(level);
    spdlog::set_level(mapped);
    // Flushing per line is what makes the log useful after a crash: the last
    // thing the mod did is on disk, not in a buffer that went down with the
    // process. Affordable only because levels under the active one cost nothing.
    spdlog::flush_on(mapped);
}

void Summary(const std::string& actorName, const EngineStats& stats, double durationMs) {
    // The one line a user's info-level log has to carry per knockdown. The
    // reduction ratio is the number to watch: the design's target is roughly
    // 10:1, four to six audible moments against 30-60 collisions.
    spdlog::info(
        "knockdown [{}] {:.0f} ms: {} contacts in, {} cues out in {} bursts, {:.1f}:1, peak "
        "{:.0f} u/s | dropped rate {} chain {} mask {} burst {} | rejected blowup {} "
        "floor {} mirror {} manifold {} | self {} | heroes {} (+{} re-anchored, {} on head "
        "relief)",
        actorName, durationMs, stats.contactsIn, stats.emittedCues, stats.bursts,
        static_cast<double>(stats.ReductionRatio()), static_cast<double>(stats.peakSpeed),
        stats.droppedRateCap, stats.droppedChainMerge, stats.droppedMasking, stats.droppedBurstCap,
        stats.rejectedBlowup, stats.rejectedBelowFloor, stats.droppedMirror,
        stats.collapsedManifold, stats.droppedSelfContact, stats.heroes, stats.heroReanchors,
        stats.heroHeadRelief);
}

}  // namespace rds::log
