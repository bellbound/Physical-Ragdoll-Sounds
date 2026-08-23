#pragma once

// One logging setup for both builds.
//
// Same shape as the other SKSE mods here - a spdlog default logger written into
// SKSE's log directory - with SkyrimNet's startup rotating sink behind
// EnableLogRotation, so the log from the session that crashed survives the next
// launch. The testbench uses the identical calls and writes beside its exe.
//
// What goes where:
//
//   info   liberal, and aimed at somebody else's log. Init, both ini paths, the
//          values that differ from default, the sound bank's per-slot resolution,
//          vanilla suppression naming every form it touched, one summary line per
//          knockdown (contacts in, cues out, bursts, peak, duration), voice cap
//          hits, and every refusal to start (disabled, no bank, bad file)
//   debug  the firehose. Per-contact admit/reject with the reason, phase
//          transitions, every arbitration drop with the margin, every cue emitted
//   warn   clamped config values with both numbers, unknown ini keys, an
//          unrecognised skeleton falling back to generic sizing
//   error  something the user has to fix, and what we did instead
//
// The rule is that a user's info-level log should be enough to tell whether the
// mod is running, what it is configured to, and whether it heard the knockdown -
// without asking them to change a setting and do it again.

#include <filesystem>
#include <string>

#include "rds/Config.h"

#ifdef RDS_STANDALONE
    #include <spdlog/spdlog.h>
#endif

namespace rds {
struct EngineStats;
}

namespace rds::log {

struct Options {
    std::filesystem::path directory;  ///< SKSE's log dir in game, the exe's dir in the testbench
    std::string name{"RagdollSounds"};
    LogLevel level{LogLevel::kInfo};
    bool rotate{true};
    std::size_t maxFiles{5};
    bool alsoStdout{false};  ///< testbench only
};

/// Install the default spdlog logger. Safe to call once; a second call is
/// logged and ignored.
void Setup(const Options& options);

/// Change the level after the config has been read, without reopening the file.
void SetLevel(LogLevel level);

/// The one line that says what a knockdown cost. Called at info when an actor
/// reaches Rest.
void Summary(const std::string& actorName, const EngineStats& stats, double durationMs);

}  // namespace rds::log
