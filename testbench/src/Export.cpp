#include "Export.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <format>

#include "App.h"
#include "Mixer.h"
#include "Video.h"

#include "rds/ConfigManager.h"
#include "rds/ConfigSchema.h"
#include "rds/Engine.h"

namespace tb {
namespace {

std::string Stamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/// One row of the merged timeline. Everything that happened, whatever produced
/// it, so the question "why is there sound here" is answered by reading down.
struct Row {
    double timeMs{};
    int rank{};  ///< orders things that share a timestamp: state, contact, cue
    std::string text;
};

// The phase names used to be spelled out again here. They live in Recording.h
// now - the CSV writer needs them too - and a second copy in this namespace is
// ambiguous with the one ADL finds on rds::ActorPhase anyway.
using rds::PhaseName;

/// The last state change at or before `ms`, or nullptr. A window that starts
/// mid-ragdoll would otherwise open with no phase at all, and the phase is the
/// first thing every one of these questions turns on.
const rds::FeedEvent* StateAt(const rds::Recording& rec, double ms) {
    const rds::FeedEvent* found = nullptr;
    for (const auto& e : rec.Events()) {
        if (e.kind != rds::EventKind::kState || e.timeMs > ms) continue;
        found = &e;
    }
    return found;
}

void AppendState(const rds::Recording& rec, std::vector<Row>& rows, double lo, double hi) {
    for (const auto& e : rec.Events()) {
        if (e.kind != rds::EventKind::kState || e.timeMs < lo || e.timeMs > hi) {
            continue;
        }
        rows.push_back({e.timeMs, 0,
                        std::format("STATE     {:<22} phase={}", e.text, PhaseName(e.phase))});
    }
}

void AppendContacts(const rds::Recording& rec, std::vector<Row>& rows, double lo, double hi) {
    for (const auto& e : rec.Events()) {
        if (e.kind != rds::EventKind::kImpact || e.timeMs < lo || e.timeMs > hi) {
            continue;
        }
        const auto* limb = rec.Profile(e.actorId) ? rec.Profile(e.actorId)->Limb(e.limbIndex)
                                                  : nullptr;
        rows.push_back(
            {e.timeMs, 1,
             std::format("  contact  {:<22} {:7.1f} u/s  tan {:6.1f}  body {:6.1f}  ang {:6.1f}"
                         "  {}  seq {}",
                         limb ? limb->boneName : std::string("?"), e.impactSpeed, e.tangentSpeed,
                         e.bodySpeed, e.angularSpeed,
                         e.otherLayer == rds::ColLayer::kDeadBip ? "DeadBip" : "Static",
                         e.sourceSeq)});
    }
}

/// The file a cue plays, for the columns that name it.
///
/// A pure lookup, never Resolve: the bank has already made this choice and
/// re-resolving would advance the shuffle bag, so the report would name a
/// different file than the one the cue list was mixed from.
std::string CueFile(const rds::SoundBank* bank, const rds::Cue& cue) {
    rds::ResolvedSound resolved{};
    if (bank == nullptr || !bank->Get(cue.slot, cue.variant, resolved)) {
        return "-";
    }
    if (resolved.path.empty()) {
        return "(no recording)";
    }
    return std::filesystem::path(resolved.path).stem().string();
}

void AppendCues(const rds::OfflineResult& result, std::vector<Row>& rows, double lo, double hi) {
    for (const auto& c : result.cues) {
        if (c.timeMs < lo || c.timeMs > hi) continue;
        const char* op = c.op == rds::CueOp::kPlayOneShot ? "play"
                         : c.op == rds::CueOp::kStartLoop ? "loop+"
                         : c.op == rds::CueOp::kUpdateLoop ? "loop~"
                                                           : "loop-";
        rows.push_back({c.timeMs, 2,
                        std::format("    CUE    {:<14} {:<5} {:6.1f} dB  pitch {:4.2f}  {:<16} "
                                    "{:<12} int {:4.2f}  seq {}",
                                    rds::ToString(c.slot), op, c.gainDb, c.pitch,
                                    rds::ToString(c.reason), rds::ToString(c.motion, c.moment), c.intensity,
                                    c.sourceSeq)});
    }
}

}  // namespace

std::filesystem::path WriteExport(const ExportRequest& r, const std::filesystem::path& directory,
                                  std::string& error) {
    if (!r.recording || !r.info || !r.config || !r.result) {
        error = "nothing loaded to export";
        return {};
    }

    const double lo = r.windowLoMs;
    const double hi = r.windowHiMs;

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    // The window is in the name: two exports of the same take taken minutes
    // apart are otherwise told apart only by a timestamp, and the thing that
    // actually distinguishes them is which slice of it they hold.
    const auto path =
        r.windowIsRegion
            ? directory / std::format("export_{}_{:.0f}-{:.0f}ms_{}.txt", r.info->stem, lo, hi,
                                      Stamp())
            : directory / std::format("export_{}_{}.txt", r.info->stem, Stamp());

    std::ofstream out(path);
    if (!out) {
        error = std::format("could not open {}", path.string());
        return {};
    }

    // ── take ─────────────────────────────────────────────────────────────────
    out << "TAKE\n";
    out << std::format("  stem            {}\n", r.info->stem);
    out << std::format("  note            {}\n", r.info->note);
    out << std::format("  actor           {}\n", r.info->actorName);
    out << std::format("  cell            {}\n", r.info->cell);
    out << std::format("  duration        {:.0f} ms\n", r.info->durationMs);
    out << std::format("  impacts         {} (dropped {}, complete {})\n", r.info->impacts,
                       r.info->dropped, r.info->complete ? "yes" : "no");
    out << std::format("  playhead        {:.0f} ms\n", r.playheadMs);
    out << std::format("  seed            {}\n", r.seed);
    out << std::format("  config          {}{} (side {})\n", r.configName,
                       r.configUnsaved ? " - EDITED, not what that file holds" : "", r.side);

    if (r.windowIsRegion) {
        out << "\nWINDOW - this export covers the loop region only\n";
        out << std::format("  region          {:.0f} - {:.0f} ms  ({:.0f} ms of {:.0f})\n", lo, hi,
                           hi - lo, r.info->durationMs);
        if (const rds::FeedEvent* at = StateAt(*r.recording, lo)) {
            out << std::format("  phase at start  {} ({}, set at {:.0f} ms)\n", PhaseName(at->phase),
                               at->text, at->timeMs);
        } else {
            out << "  phase at start  no state change before this point\n";
        }
        out << "  NOTE  TIMELINE, CUES and DECISIONS below are cut to this window. SOUND BANK\n"
               "        and FUNNEL are the whole take - the funnel counters are accumulated\n"
               "        over the run and cannot be re-cut after the fact.\n";
    }

    out << "\nVIDEO\n";
    if (r.info->videoPath.empty()) {
        out << "  (this take has no video)\n";
    } else {
        out << std::format("  file            {}\n", r.info->videoPath.filename().string());
        if (r.video) {
            out << std::format("  clip length     {:.0f} ms at {:.2f} fps, {}\n",
                               r.video->DurationMs(), r.video->Fps(),
                               r.video->Ready() ? "decoded" : "still decoding");
        }
        out << std::format("  offset in use   {:+.0f} ms   (video position = take time + offset)\n",
                           r.videoOffsetMs);
        if (r.windowIsRegion) {
            // Where to scrub to. The whole reason the offset is in this file is
            // that a take time cannot be typed into a video player.
            const double vlo = lo + r.videoOffsetMs;
            const double vhi = hi + r.videoOffsetMs;
            out << std::format("  window in video {:.0f} - {:.0f} ms  ({:.2f} - {:.2f} s)\n", vlo,
                               vhi, vlo * 0.001, vhi * 0.001);
            if (r.video && r.video->Fps() > 1.0) {
                out << std::format("  window frames   {:.0f} - {:.0f} at {:.2f} fps\n",
                                   vlo * 0.001 * r.video->Fps(), vhi * 0.001 * r.video->Fps(),
                                   r.video->Fps());
            }
        }
        if (r.sync && r.sync->valid) {
            out << std::format("  sync fit        slope {:.6f}, intercept {:.0f} ms, drift "
                               "{:+.2f} ms/s, min rtt {:.0f} ms\n",
                               r.sync->slope, r.sync->intercept, r.sync->driftMsPerSec,
                               r.sync->minRttMs);
            if (r.offsetMeasured) {
                out << "  NOTE  `offset in use` is the measured value from "
                       "framecache/video-offsets.ini,\n"
                       "        which overrides the fit above.\n";
            } else {
                out << "  NOTE  the intercept is against the UNCUT OBS recording, and a clipped take"
                       "\n        is cut out of it at a point recorded nowhere - so `offset in use`"
                       "\n        above is a guess until it is measured into video-offsets.ini.\n";
            }
        } else {
            out << "  sync fit        none - no usable _sync.csv rows\n";
        }
    }

    // ── config ───────────────────────────────────────────────────────────────
    //
    // The short form here and the whole ini at the end of the file. This one is
    // what gets read: ninety keys at their defaults say nothing, and the six
    // that moved are the answer to "what is this take being heard through".
    out << "\nCONFIG - values differing from default\n";
    // Opened surfaces only: a closed class holds its parent's values, so
    // listing it would report one edit to stone as four changes.
    const auto deltas = rds::Deltas(r.config, App::AlgorithmAndOpenedSurfaces(*r.config));
    if (deltas.empty()) {
        out << "  (all defaults)\n";
    } else {
        for (const auto& d : deltas) {
            out << "  " << d << "\n";
        }
    }

    // ── sound bank ───────────────────────────────────────────────────────────
    out << "\nSOUND BANK\n";
    if (r.bank) {
        for (const auto& slot : rds::Slots()) {
            const auto n = r.bank->FileCount(slot.id);
            rds::ResolvedSound s{};
            const bool any = r.bank->Get(slot.id, 0, s);
            if (!any) {
                // Either declared and unfilled by design, or unrecorded and
                // silent. `HasSound` is what tells the two apart, because it
                // walks the fallback the way the engine does.
                out << std::format("  {:<14} {}\n", slot.name,
                                   r.bank->HasSound(slot.id)
                                       ? std::format("0/{} files, plays {}", slot.expectedVariants,
                                                     rds::ToString(r.bank->PlaysAs(slot.id)))
                                       : std::format("0/{} files, nothing to play",
                                                     slot.expectedVariants));
            } else {
                out << std::format("  {:<14} {}/{} files, wav, {:.0f} ms\n", slot.name, n,
                                   slot.expectedVariants, s.lengthMs);
            }
        }
    }

    // ── stats ────────────────────────────────────────────────────────────────
    const auto& s = r.result->stats;
    out << (r.windowIsRegion ? "\nFUNNEL - contacts in, cues out (WHOLE TAKE, not the window)\n"
                             : "\nFUNNEL - contacts in, cues out\n");
    out << std::format("  events read           {}\n", s.eventsIn);
    out << std::format("  contacts              {}\n", s.contactsIn);
    out << std::format("    rejected below floor {}\n", s.rejectedBelowFloor);
    out << std::format("    rejected blow-up     {}\n", s.rejectedBlowup);
    out << std::format("    mirrored self-hit    {}\n", s.droppedMirror);
    out << std::format("    manifold collapsed   {}\n", s.collapsedManifold);
    out << std::format("    self-contacts dropped {}\n", s.droppedSelfContact);
    out << std::format("  proposed cues         {}\n", s.proposedCues);
    out << std::format("    dropped rate cap     {}\n", s.droppedRateCap);
    out << std::format("    dropped chain merge  {}\n", s.droppedChainMerge);
    out << std::format("    dropped masking      {}\n", s.droppedMasking);
    out << std::format("    dropped burst cap    {}\n", s.droppedBurstCap);
    out << std::format("    muted by layer       {}\n", s.mutedCues);
    out << std::format("    compressed by class  {}\n", s.compressedCues);
    out << std::format("  hero moments          {} (+{} re-anchored)\n", s.heroes,
                       s.heroReanchors);
    // What the head's floor relief bought. Zero with the option switched on says the
    // threshold is out of reach, which the hero count on its own cannot tell you.
    out << std::format("    on head relief       {}\n", s.heroHeadRelief);
    // Flights something was pushing. Not a fault on its own - a leashed actor
    // hauled off a balcony is a legitimate thing to happen - but if this is
    // non-zero and the landing after it sounded thin, this is the first line to
    // read.
    out << std::format("    driven flights       {}\n", s.drivenFlights);
    // Slides the entry test found at all.
    out << std::format("  slides                {}\n", s.slides);
    out << std::format("  emitted               {} in {} bursts\n", s.emittedCues, s.bursts);
    out << std::format("  first cue             {:.0f} ms\n", s.firstCueMs);
    out << std::format("  last cue              {:.0f} ms\n", s.lastCueMs);
    out << std::format("  peak contact          {:.1f} u/s\n", s.peakSpeed);

    if (r.audio) {
        out << "\nAUDIO\n";
        out << std::format("  {:.0f} ms at {} Hz, {} frames\n", r.audio->durationMs,
                           r.audio->sampleRate, r.audio->Frames());
        out << std::format("  peak {:.3f} heard, {:.3f} before the limiter ({})\n", r.audio->peak,
                           r.audio->rawPeak, r.limiter ? "limiter on" : "limiter off");
        if (r.audio->rawPeak > 1.0f) {
            out << "  NOTE  over 1.0 before the limiter. The game has no limiter, so this "
                   "config would clip in Skyrim.\n";
        }
    }

    // ── what sounds here, by slot ────────────────────────────────────────────
    //
    // The same count the app shows under the timeline, so a screenshot and a
    // dump of the same moment cannot disagree.
    {
        std::vector<std::pair<rds::SlotId, int>> counts;
        int total = 0;
        for (const auto& c : r.result->cues) {
            if (c.timeMs < lo || c.timeMs > hi) continue;
            ++total;
            const auto it = std::find_if(counts.begin(), counts.end(),
                                         [&](const auto& e) { return e.first == c.slot; });
            if (it == counts.end()) {
                counts.emplace_back(c.slot, 1);
            } else {
                ++it->second;
            }
        }
        out << std::format("\nCUES BY SLOT{} - {} cue{}\n",
                           r.windowIsRegion ? " IN THE WINDOW" : "", total, total == 1 ? "" : "s");
        if (counts.empty()) {
            out << "  (nothing sounds here)\n";
        }
        for (const auto& [slot, n] : counts) {
            out << std::format("  {:<14} {}\n", rds::ToString(slot), n);
        }
    }

    // ── the merged timeline ──────────────────────────────────────────────────
    //
    // The section this file exists for. State changes, every contact the solver
    // reported, and every cue we emitted, in one column - so "why is there a
    // sound at 900 ms" is answered by looking at 900 ms.
    out << "\nTIMELINE - state, contacts and cues in one column\n";
    out << "  (a cue's `seq` is the contact row that produced it)\n\n";
    std::vector<Row> rows;
    AppendState(*r.recording, rows, lo, hi);
    AppendContacts(*r.recording, rows, lo, hi);
    AppendCues(*r.result, rows, lo, hi);
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.timeMs != b.timeMs ? a.timeMs < b.timeMs : a.rank < b.rank;
    });
    for (const auto& row : rows) {
        out << std::format("{:9.1f} ms  {}\n", row.timeMs, row.text);
    }

    // ── cues, in full ────────────────────────────────────────────────────────
    out << "\nCUES\n";
    out << "  (`sfx` is the file this cue resolved to; `held` is how many dB its class's "
           "compressor took off it)\n\n";
    out << std::format(
        "{:>9}  {:<14} {:<22} {:<5} {:>7} {:>5} {:>6}  {:<16} {:<12} {:>5} {:>6} {:>8} {:>4}\n",
        "time", "slot", "sfx", "op", "gain", "held", "pitch", "reason", "phase", "int", "limb",
        "site", "seq");
    for (const auto& c : r.result->cues) {
        if (c.timeMs < lo || c.timeMs > hi) continue;
        const char* op = c.op == rds::CueOp::kPlayOneShot ? "play"
                         : c.op == rds::CueOp::kStartLoop ? "loop+"
                         : c.op == rds::CueOp::kUpdateLoop ? "loop~"
                                                           : "loop-";
        out << std::format("{:9.1f}  {:<14} {:<22} {:<5} {:7.1f} {:>5} {:6.2f}  {:<16} {:<12} "
                           "{:5.2f} {:6} {:>8} {:4}\n",
                           c.timeMs, rds::ToString(c.slot), CueFile(r.bank, c), op, c.gainDb,
                           c.compressCutDb < 0.0f ? std::format("-{:.1f}", -c.compressCutDb)
                                                  : std::string{},
                           c.pitch, rds::ToString(c.reason), rds::ToString(c.motion, c.moment), c.intensity,
                           c.limbIndex, rds::ToString(c.site), c.sourceSeq);
    }

    // ── the trace ────────────────────────────────────────────────────────────
    if (!r.result->trace.empty()) {
        out << "\nDECISIONS - every contact that reached arbitration, and what became of it\n";
        for (const auto& t : r.result->trace) {
            if (t.timeMs < lo || t.timeMs > hi) continue;
            out << std::format("{:9.1f} ms  seq {:5}  limb {:2}  {:7.1f} u/s  int {:4.2f}  {:<12} "
                               "{}\n",
                               t.timeMs, t.sourceSeq, t.limbIndex, t.impactSpeed, t.intensity,
                               rds::ToString(t.motion, t.moment), t.outcome);
        }
    }

    // ── the config in full ───────────────────────────────────────────────────
    //
    // Last, and only on request. It is four hundred lines, and what somebody
    // opens this file to find is nearly always in the timeline - but a report
    // that names a config rather than carrying it is worthless six saves later,
    // when the file of that name is a different config.
    if (r.includeConfigs) {
        out << "\n\nCONFIG INI - the config this export was rendered through, in full\n";
        out << std::format("  {}{}\n", r.configName,
                           r.configUnsaved
                               ? " - as it stands in memory, which is NOT what the file holds"
                               : " - matches the file of that name");
        out << "  Paste this block into RagdollSounds_Algorithm.ini and the mod plays what this\n"
               "  export is a report of. Every key carries what it changes perceptually.\n";
        out << "\n"
               "; ----------------------------------------------------------------------------\n";
        out << rds::ConfigManager::ToIniText(r.config,
                                             App::AlgorithmAndOpenedSurfaces(*r.config),
                                             "RagdollSounds_Algorithm.ini - the sound engine");
        out << "; ----------------------------------------------------------------------------\n";
    }

    out.flush();
    return path;
}

}  // namespace tb
