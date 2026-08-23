#include "SfxAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <format>
#include <limits>
#include <numbers>
#include <numeric>

namespace tb {
namespace {

constexpr float kSilence = 1e-9f;

[[nodiscard]] float Db(float linear) { return 20.0f * std::log10(std::max(linear, kSilence)); }

// ── the bands ────────────────────────────────────────────────────────────────
//
// tools/sfx.py's BANDS verbatim. Tilt is (sub+low)/2 - (high+air)/2, which is
// the discriminator Slots.md §4 says actually works.

struct Band {
    const char* name;
    float lo;
    float hi;
};

constexpr Band kBands[] = {
    {"sub", 20.0f, 80.0f},      {"low", 80.0f, 250.0f},     {"lomid", 250.0f, 800.0f},
    {"mid", 800.0f, 2500.0f},   {"high", 2500.0f, 8000.0f}, {"air", 8000.0f, 20000.0f},
};

// ── fft ──────────────────────────────────────────────────────────────────────

/// Iterative radix-2, in place. Small enough to read, which is the point: a
/// vendored fft library for six band energies would be more code than this.
void Fft(std::vector<std::complex<float>>& data) {
    const std::size_t n = data.size();
    if (n < 2) {
        return;
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

[[nodiscard]] std::size_t NextPowerOfTwo(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

// ── envelope ─────────────────────────────────────────────────────────────────

struct Envelope {
    std::vector<float> db;
    std::size_t hop{1};
};

/// RMS in `ms`-wide hops, in dB. sfx.py's env_db, same 2 ms default.
[[nodiscard]] Envelope EnvelopeDb(std::span<const float> x, int sampleRate, float ms = 2.0f) {
    Envelope e;
    e.hop = std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate * ms / 1000.0f));
    const std::size_t count = x.size() > e.hop ? (x.size() - e.hop) / e.hop : 0;
    e.db.reserve(std::max<std::size_t>(count, 1));
    for (std::size_t i = 0; i < count; ++i) {
        double sum = 0.0;
        for (std::size_t k = 0; k < e.hop; ++k) {
            const double s = x[i * e.hop + k];
            sum += s * s;
        }
        e.db.push_back(Db(static_cast<float>(std::sqrt(sum / static_cast<double>(e.hop)))));
    }
    if (e.db.empty()) {
        e.db.push_back(Db(0.0f));
    }
    return e;
}

/// Peaks in the envelope more than `floorDb` under the loudest and at least
/// `minGapMs` apart. sfx.py's count_peaks, which is what "grains" and "lo-mid
/// transients" both are.
[[nodiscard]] int CountPeaks(std::span<const float> x, int sampleRate, float floorDb = 20.0f,
                             float minGapMs = 10.0f) {
    const Envelope e = EnvelopeDb(x, sampleRate);
    if (e.db.size() < 3) {
        return 0;
    }
    const float threshold = *std::ranges::max_element(e.db) - floorDb;
    const double gap = minGapMs * sampleRate / 1000.0 / static_cast<double>(e.hop);
    int count = 0;
    double last = -1e9;
    for (std::size_t i = 1; i + 1 < e.db.size(); ++i) {
        if (e.db[i] > threshold && e.db[i] >= e.db[i - 1] && e.db[i] > e.db[i + 1] &&
            (static_cast<double>(i) - last) > gap) {
            ++count;
            last = static_cast<double>(i);
        }
    }
    return count;
}

/// Attack, peak and end of the loudest event, ignoring pre-roll and room tail.
/// sfx.py's find_event, minus the zero-crossing snap - that exists to make a cut
/// open without a click, and nothing here cuts.
struct Event {
    std::size_t start{};
    std::size_t peak{};
    std::size_t end{};
};

[[nodiscard]] Event FindEvent(std::span<const float> x, int sampleRate) {
    Event ev;
    ev.end = x.size();
    const Envelope e = EnvelopeDb(x, sampleRate);
    if (e.db.empty()) {
        return ev;
    }
    const auto peakIt = std::ranges::max_element(e.db);
    const auto pk = static_cast<std::size_t>(std::distance(e.db.begin(), peakIt));
    const float peak = *peakIt;

    std::size_t i = pk;
    while (i > 0 && e.db[i] > peak - 35.0f) {
        --i;
    }
    ev.start = std::min(i * e.hop, x.size());
    ev.peak = std::min(pk * e.hop, x.size());

    const auto need = std::max<std::size_t>(1, static_cast<std::size_t>(30.0 * sampleRate / 1000.0 /
                                                                       static_cast<double>(e.hop)));
    std::size_t quiet = 0;
    std::size_t j = pk;
    while (j + 1 < e.db.size()) {
        ++j;
        quiet = e.db[j] < peak - 40.0f ? quiet + 1 : 0;
        if (quiet >= need) {
            break;
        }
    }
    ev.end = std::min(j * e.hop, x.size());
    if (ev.end <= ev.start) {
        ev.end = x.size();
    }
    return ev;
}

// ── slot targets ─────────────────────────────────────────────────────────────
//
// The measurable half of Slots.md §3, which is `tools/sfx.py`'s SPEC. It is
// copied rather than shared because sfx.py is the authoring tool and this is
// the app: the one place both read is Slots.md, and that is prose. If a number
// here disagrees with §3, §3 is right.
//
// Only used for suggestion and for sorting candidates. Nothing here rejects
// anything - a file that meets none of a slot's targets can still be assigned
// to it, and sometimes should be.

struct SlotTarget {
    rds::SlotId slot;
    float decay20Lo, decay20Hi;    ///< 0,0 for "not checked"
    float centroidLo, centroidHi;
    float tiltMin;                 ///< NaN when the slot has no floor
    float tiltMax;                 ///< NaN when the slot has no ceiling
    int loMidLo, loMidHi;          ///< -1,-1 for "not checked"
    float steadyLo, steadyHi;      ///< loops only
    float grainsLo, grainsHi;      ///< loops only
};

constexpr float kNone = std::numeric_limits<float>::quiet_NaN();

constexpr SlotTarget kTargets[] = {
    {rds::SlotId::kImpTransient, 5, 90, 1500, 9000, kNone, -6, 0, 6, 0, 0, 0, 0},
    {rds::SlotId::kImpBody, 15, 200, 800, 4200, 4, kNone, 0, 5, 0, 0, 0, 0},
    {rds::SlotId::kImpSub, 15, 120, 20, 400, kNone, kNone, -1, -1, 0, 0, 0, 0},
    {rds::SlotId::kSurfWood, 20, 160, 400, 3000, kNone, kNone, -1, -1, 0, 0, 0, 0},
    {rds::SlotId::kSurfStone, 5, 90, 1200, 9000, kNone, -2, -1, -1, 0, 0, 0, 0},
    {rds::SlotId::kSurfSoft, 15, 200, 300, 3200, 3, kNone, -1, -1, 0, 0, 0, 0},
    {rds::SlotId::kLimbTap, 3, 60, 600, 9000, kNone, kNone, 0, 4, 0, 0, 0, 0},
    {rds::SlotId::kCrunchGran, 30, 400, 500, 4500, kNone, kNone, 15, 99, 0, 0, 0, 0},
    {rds::SlotId::kGoreWet, 20, 350, 400, 4000, kNone, kNone, -1, -1, 0, 0, 0, 0},
    {rds::SlotId::kScrapeLoop, 0, 0, 0, 0, 5, 19, -1, -1, 2.5f, 8.0f, 8, 40},
    {rds::SlotId::kFoleyCloth, 0, 0, 0, 0, kNone, kNone, -1, -1, 0.5f, 5.0f, 0, 0},
    {rds::SlotId::kAirWhoosh, 0, 0, 50, 2000, kNone, kNone, -1, -1, 0.5f, 6.0f, 0, 0},
    {rds::SlotId::kHeadImpact, 20, 300, 400, 3500, 2, kNone, 4, 14, 0, 0, 0, 0},
    {rds::SlotId::kSettleRest, 20, 400, 300, 3200, kNone, kNone, -1, -1, 0, 0, 0, 0},
    // The two voice slots are declared and unfilled by design (Slots.md §2).
    // They have no measurable targets and nothing should ever be suggested for
    // them, so they are absent here rather than present with empty ranges.
};

[[nodiscard]] const SlotTarget* TargetFor(rds::SlotId slot) {
    for (const SlotTarget& t : kTargets) {
        if (t.slot == slot) {
            return &t;
        }
    }
    return nullptr;
}

[[nodiscard]] bool InRange(float value, float lo, float hi) { return value >= lo && value <= hi; }

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// measuring
// ═════════════════════════════════════════════════════════════════════════════

void MeasureSfx(std::span<const float> mono, int sampleRate, rds::SfxEntry& entry) {
    entry.sampleRate = sampleRate;
    entry.channels = 1;
    entry.durationMs = sampleRate > 0 ? 1000.0f * static_cast<float>(mono.size()) /
                                            static_cast<float>(sampleRate)
                                      : 0.0f;
    if (mono.empty() || sampleRate <= 0) {
        return;
    }

    float peak = 0.0f;
    double sum = 0.0;
    for (const float s : mono) {
        peak = std::max(peak, std::fabs(s));
        sum += s;
    }
    entry.peakDb = Db(peak);
    entry.dcOffset = static_cast<float>(sum / static_cast<double>(mono.size()));

    // ── loop metrics, measured on the whole file ─────────────────────────────
    //
    // A loop has no attack and no decay to measure; it is judged as a sustained
    // texture. Measured for every file, not only the ones that turn out to
    // loop, because these numbers are what *decide* whether it loops.
    {
        const Envelope e = EnvelopeDb(mono, sampleRate, 10.0f);
        const float top = *std::ranges::max_element(e.db);
        std::vector<float> live;
        live.reserve(e.db.size());
        for (const float v : e.db) {
            if (v > top - 40.0f) {
                live.push_back(v);
            }
        }
        if (!live.empty()) {
            const double mean =
                std::accumulate(live.begin(), live.end(), 0.0) / static_cast<double>(live.size());
            double variance = 0.0;
            for (const float v : live) {
                variance += (v - mean) * (v - mean);
            }
            entry.steadyDb = static_cast<float>(std::sqrt(variance / static_cast<double>(live.size())));

            entry.grainsPerSec = static_cast<float>(CountPeaks(mono, sampleRate, 12.0f, 8.0f)) /
                                 std::max(0.001f, entry.durationMs / 1000.0f);

            // seam: the level match between the first and last 50 ms, which is
            // what a whole-file loop with no crossfade actually hears.
            const auto k = std::min<std::size_t>(mono.size(), static_cast<std::size_t>(0.05 * sampleRate));
            const auto rms = [](std::span<const float> part) {
                double acc = 0.0;
                for (const float s : part) {
                    acc += static_cast<double>(s) * s;
                }
                return static_cast<float>(std::sqrt(acc / std::max<std::size_t>(1, part.size())));
            };
            entry.seamDb = std::fabs(Db(rms(mono.first(k))) - Db(rms(mono.last(k))));

            // How much of the file is *doing something*: the fraction of hops
            // inside the same 40 dB live window `steady` is measured over.
            //
            // This is the discriminator, and it took two wrong ones to find it.
            // The first asked how far the loudest moment stuck out of the
            // median - which reads as "a texture has no single moment" and is
            // true of cloth and wind, but not of a scrape: scrape_loop_01
            // carries 24 grains a second and its loudest grain sits well clear
            // of the bed. The second used a 20 dB window, which a grainy drag
            // falls out of between grains. Both called the shipped scrape an
            // event, handed it a 1.2 s "lead-in" from FindEvent walking back to
            // that grain, and warned at it for a fault it does not have.
            //
            // Duty cycle does not care how spiky a texture is, only whether it
            // keeps going. A one-shot is one moment and then decay - the pack's
            // own composites are 40 dB down inside 200 ms - so it scores low
            // however smooth it is, and a grainy drag scores ~1.0 however
            // jagged it is. Which is the actual question.
            const float duty = static_cast<float>(live.size()) / static_cast<float>(e.db.size());

            entry.loops = entry.durationMs >= 700.0f && entry.seamDb <= 6.0f &&
                          entry.steadyDb <= 8.0f && duty >= 0.75f;
        }
    }

    // ── the event, for everything that is not a loop ─────────────────────────
    std::span<const float> segment = mono;
    if (entry.loops) {
        entry.leadInMs = 0.0f;
        entry.usableMs = entry.durationMs;
        entry.decay20Ms = 0.0f;
    } else {
        const Event ev = FindEvent(mono, sampleRate);
        segment = mono.subspan(ev.start, ev.end - ev.start);
        entry.usableMs = 1000.0f * static_cast<float>(segment.size()) / static_cast<float>(sampleRate);

        // Lead-in is measured against an **absolute** floor, not against where
        // the event was found.
        //
        // 03-Asset-Status.md §6 records this as one of two thresholds that were
        // wrong on their first run, and it is wrong the same way here: a
        // relative measure walks back from the peak to peak-35 dB, so anything
        // specified to start softly - settle_rest by design, and both gore_wet
        // files in practice - reads as hundreds of milliseconds of lead-in it
        // does not have. What the rule is actually about is head *silence*
        // becoming latency, and silence is an absolute quantity.
        constexpr float kSilenceFloorDb = -60.0f;
        const Envelope head = EnvelopeDb(mono, sampleRate);
        std::size_t quietHops = 0;
        while (quietHops < head.db.size() && head.db[quietHops] <= kSilenceFloorDb) {
            ++quietHops;
        }
        entry.leadInMs = 1000.0f * static_cast<float>(quietHops * head.hop) /
                         static_cast<float>(sampleRate);

        const Envelope e = EnvelopeDb(segment, sampleRate);
        const auto peakIt = std::ranges::max_element(e.db);
        const auto pk = static_cast<std::size_t>(std::distance(e.db.begin(), peakIt));
        entry.decay20Ms = 0.0f;
        for (std::size_t i = pk; i < e.db.size(); ++i) {
            if (e.db[i] <= *peakIt - 20.0f) {
                entry.decay20Ms = static_cast<float>(i - pk) * static_cast<float>(e.hop) * 1000.0f /
                                  static_cast<float>(sampleRate);
                break;
            }
        }
    }

    // ── spectrum: centroid, band tilt, lo-mid transient count ────────────────
    if (segment.size() > 64) {
        const std::size_t padded = NextPowerOfTwo(segment.size());
        std::vector<std::complex<float>> spectrum(padded, std::complex<float>(0.0f, 0.0f));
        for (std::size_t i = 0; i < segment.size(); ++i) {
            spectrum[i] = std::complex<float>(segment[i], 0.0f);
        }
        Fft(spectrum);

        const std::size_t half = padded / 2;
        const double binHz = static_cast<double>(sampleRate) / static_cast<double>(padded);

        double weighted = 0.0;
        double total = 0.0;
        std::array<double, std::size(kBands)> energy{};
        for (std::size_t k = 0; k <= half; ++k) {
            const double magnitude = std::abs(spectrum[k]);
            const double hz = static_cast<double>(k) * binHz;
            weighted += magnitude * hz;
            total += magnitude;
            // Parseval: mean-square over the *unpadded* length, so the trailing
            // zeros do not quietly halve every band.
            const double power = magnitude * magnitude * ((k == 0 || k == half) ? 1.0 : 2.0);
            for (std::size_t b = 0; b < std::size(kBands); ++b) {
                if (hz >= kBands[b].lo && hz < kBands[b].hi) {
                    energy[b] += power;
                    break;
                }
            }
        }
        entry.centroidHz = static_cast<float>(weighted / std::max(total, 1e-12));

        std::array<float, std::size(kBands)> level{};
        const double norm = static_cast<double>(padded) * static_cast<double>(segment.size());
        for (std::size_t b = 0; b < std::size(kBands); ++b) {
            level[b] = Db(static_cast<float>(std::sqrt(energy[b] / norm)));
        }
        entry.tiltDb = (level[0] + level[1]) * 0.5f - (level[4] + level[5]) * 0.5f;

        // The 250-800 Hz transient count, which is what separates a thud from a
        // crunch. Band-limited by zeroing everything outside it and coming back.
        std::vector<std::complex<float>> loMid(padded, std::complex<float>(0.0f, 0.0f));
        for (std::size_t i = 0; i < segment.size(); ++i) {
            loMid[i] = std::complex<float>(segment[i], 0.0f);
        }
        Fft(loMid);
        for (std::size_t k = 0; k <= half; ++k) {
            const double hz = static_cast<double>(k) * binHz;
            if (hz < kBands[2].lo || hz >= kBands[2].hi) {
                loMid[k] = 0.0f;
                if (k != 0 && k != half) {
                    loMid[padded - k] = 0.0f;
                }
            }
        }
        // Inverse by conjugation, which saves writing a second transform.
        for (auto& v : loMid) {
            v = std::conj(v);
        }
        Fft(loMid);
        std::vector<float> band(segment.size());
        for (std::size_t i = 0; i < segment.size(); ++i) {
            // ifft(X) = conj(fft(conj(X)))/N, and conj does not move the real part.
            band[i] = loMid[i].real() / static_cast<float>(padded);
        }
        entry.loMidTransients = CountPeaks(band, sampleRate);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// judging
// ═════════════════════════════════════════════════════════════════════════════

void JudgeSfx(rds::SfxEntry& entry) {
    // The blocking one is the caller's - it knows whether the decode worked -
    // so it is carried across rather than recomputed here.
    std::vector<rds::SfxWarning> kept;
    for (const rds::SfxWarning& w : entry.warnings) {
        if (w.blocking) {
            kept.push_back(w);
        }
    }
    entry.warnings = std::move(kept);

    const auto warn = [&entry](std::string code, std::string detail) {
        entry.warnings.push_back({std::move(code), std::move(detail), false});
    };

    if (entry.durationMs <= 0.0f) {
        return;
    }

    // Format. The importer fixes this, so seeing it means the file came in some
    // other way - dropped into the folder by hand, most likely.
    if (entry.sampleRate != 0 && entry.sampleRate != 48000) {
        warn("rate", std::format("{} Hz, not the pack's 48 kHz. The game resamples it, which is "
                                 "audible on anything with a bright transient. Re-import to fix.",
                                 entry.sampleRate));
    }
    if (entry.channels > 1) {
        warn("stereo", std::format("{} channels. The engine plays mono and positions the sound "
                                   "itself, so the second channel is thrown away - and if the two "
                                   "are not correlated, throwing it away comb-filters what is "
                                   "left. Re-import to fix.",
                                   entry.channels));
    }
    if (entry.bitsPerSample != 0 && entry.bitsPerSample != 16) {
        warn("bits", std::format("{}-bit, not the pack's 16. Harmless, but it is not what the rest "
                                 "of the pack is.",
                                 entry.bitsPerSample));
    }

    // Level. Slots.md §5: everything is pitch-shifted at runtime and must not
    // clip there, which is what the headroom rule is for.
    if (entry.peakDb > -0.2f) {
        warn("clipped", std::format("peaks at {:.1f} dBFS. There is no headroom for the runtime "
                                    "pitch scatter, so this will clip in game even though it does "
                                    "not clip here. The pack normalises to -1.5.",
                                    entry.peakDb));
    } else if (entry.peakDb > -0.9f) {
        // -0.9 rather than the -1.5 the pack normalises to, because Slots.md §3
        // makes `imp_sub` an explicit exception at -1.0 - and a warning that
        // fires on every correctly-built sub layer is one nobody reads by the
        // third file.
        warn("hot", std::format("peaks at {:.1f} dBFS against the pack's -1.5 (-1.0 for imp_sub). "
                                "Survivable, but the ±3 semitone scatter has almost nothing to "
                                "work with.",
                                entry.peakDb));
    } else if (entry.peakDb < -30.0f) {
        warn("quiet", std::format("peaks at {:.1f} dBFS. The engine sets the level, so a file this "
                                  "quiet only loses resolution - normalise it.",
                                  entry.peakDb));
    }

    if (std::fabs(entry.dcOffset) > 0.01f) {
        warn("dc", std::format("DC offset {:+.3f}. It eats headroom and thumps when the sound is "
                               "cut off mid-way, which the engine does on a fade.",
                               entry.dcOffset));
    }

    if (entry.loops) {
        // A loop is a texture, so it is never too long and never has a lead-in.
        // Its seam is the thing: the engine repeats it whole with no crossfade,
        // so any level step at the join becomes a rhythm.
        if (entry.seamDb > 6.0f) {
            warn("seam", std::format("{:.1f} dB step between the first and last 50 ms. Loops are "
                                     "played whole with no crossfade, so this becomes an audible "
                                     "pulse once a second in game.",
                                     entry.seamDb));
        }
    } else {
        if (entry.leadInMs > 20.0f) {
            warn("lead-in", std::format("{:.0f} ms of silence before the sound starts. The cue time "
                                        "*is* the attack, so head silence becomes latency and puts "
                                        "the +15/+50/+65 ms layer offsets out.",
                                        entry.leadInMs));
        }
        if (entry.durationMs < 30.0f) {
            warn("very short", std::format("{:.0f} ms. Shorter than every slot's minimum; it will "
                                           "read as a click rather than a contact.",
                                           entry.durationMs));
        }
        if (entry.durationMs > 3000.0f) {
            warn("very long", std::format("{:.1f} s and not a loop. Either it has a baked room tail "
                                          "- which double-counts against the game's own cell "
                                          "reverb - or it is a texture that did not measure as one.",
                                          entry.durationMs / 1000.0f));
        }
        // A tail far longer than the event is the baked-reverb tell.
        if (entry.usableMs > 0.0f && entry.durationMs > entry.usableMs * 2.0f + 200.0f) {
            warn("tail", std::format("{:.0f} ms of event inside a {:.0f} ms file. The rest is "
                                     "room, and the game applies the cell's own - a baked tail "
                                     "sounds like a cave inside a cave.",
                                     entry.usableMs, entry.durationMs));
        }
    }

    if (entry.peakDb <= -60.0f) {
        warn("silent", "nothing audible in the file at all. It will play, and you will hear "
                       "nothing.");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// suggesting
// ═════════════════════════════════════════════════════════════════════════════

bool LengthSuits(float durationMs, rds::SlotId slot, bool slotLoops) {
    const rds::SlotDesc& desc = rds::Slot(slot);
    // ±25%, which is the band the browser sorts to the top. Wide on purpose: a
    // 130 ms transient in a 60-120 ms slot is a fine transient, and the engine
    // truncates rather than refusing it.
    const float lo = desc.minLengthMs * 0.75f;
    const float hi = desc.maxLengthMs * 1.25f;
    if (slotLoops) {
        // A looping slot has no ceiling - it repeats whatever it is given - so
        // only being far too short to loop disqualifies anything.
        return durationMs >= lo;
    }
    return durationMs >= lo && durationMs <= hi;
}

float SlotFit(const rds::SfxEntry& entry, rds::SlotId slot) {
    const SlotTarget* target = TargetFor(slot);
    if (target == nullptr || entry.durationMs <= 0.0f) {
        return 0.0f;
    }
    const rds::SlotDesc& desc = rds::Slot(slot);
    if (!LengthSuits(entry.durationMs, slot, desc.isLoop)) {
        return 0.0f;
    }
    // A loop cannot be an event and an event cannot be a loop - that is the one
    // hard mismatch in this table, and letting it through put every three-second
    // cloth rustle at the top of the imp_body list.
    if (desc.isLoop != entry.loops) {
        return 0.0f;
    }

    int checks = 0;
    int met = 0;
    const auto check = [&](bool applicable, bool ok) {
        if (applicable) {
            ++checks;
            met += ok ? 1 : 0;
        }
    };

    // Inside the declared range rather than merely inside ±25%: the widened
    // band decides candidacy, the exact one decides ranking.
    check(true, InRange(entry.durationMs, desc.minLengthMs, desc.maxLengthMs));
    check(target->centroidHi > 0.0f,
          InRange(entry.centroidHz, target->centroidLo, target->centroidHi));
    check(!std::isnan(target->tiltMin), entry.tiltDb >= target->tiltMin);
    check(!std::isnan(target->tiltMax), entry.tiltDb <= target->tiltMax);

    if (desc.isLoop) {
        check(target->steadyHi > 0.0f, InRange(entry.steadyDb, target->steadyLo, target->steadyHi));
        check(target->grainsHi > 0.0f,
              InRange(entry.grainsPerSec, target->grainsLo, target->grainsHi));
        check(true, entry.seamDb <= 6.0f);
    } else {
        check(target->decay20Hi > 0.0f,
              InRange(entry.decay20Ms, target->decay20Lo, target->decay20Hi));
        check(target->loMidHi >= 0, entry.loMidTransients >= target->loMidLo &&
                                        entry.loMidTransients <= target->loMidHi);
    }

    return checks == 0 ? 0.0f : static_cast<float>(met) / static_cast<float>(checks);
}

std::vector<rds::SlotId> SuggestSlots(const rds::SfxEntry& entry) {
    struct Scored {
        rds::SlotId slot;
        float fit;
    };
    std::vector<Scored> scored;
    for (const SlotTarget& t : kTargets) {
        const float fit = SlotFit(entry, t.slot);
        // Half the targets met is the floor for saying anything at all. Below
        // that the suggestion is noise, and a wrong suggestion is worse than
        // none - it is read as a recommendation.
        if (fit >= 0.5f) {
            scored.push_back({t.slot, fit});
        }
    }
    std::ranges::stable_sort(scored, [](const Scored& a, const Scored& b) { return a.fit > b.fit; });
    if (scored.size() > 3) {
        scored.resize(3);
    }

    std::vector<rds::SlotId> out;
    out.reserve(scored.size());
    for (const Scored& s : scored) {
        out.push_back(s.slot);
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// names
// ═════════════════════════════════════════════════════════════════════════════

std::string TidyName(std::string_view stem) {
    std::string out;
    out.reserve(stem.size());

    // Drop bracketed groups whole. Every one of them so far has been a site
    // stamp or a download counter, and neither is part of what the sound is.
    int depth = 0;
    for (const char c : stem) {
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            depth = std::max(0, depth - 1);
            continue;
        }
        if (depth == 0) {
            out += c;
        }
    }

    // Collapse the separators the removal left behind, and any that were
    // already doubled. `punch--face` and `punch - face` both become `punch-face`.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool lastWasSeparator = false;
    for (const char c : out) {
        const bool separator = c == ' ' || c == '-' || c == '_' || c == '.';
        if (separator) {
            if (!lastWasSeparator && !collapsed.empty()) {
                collapsed += c == '_' ? '_' : '-';
            }
            lastWasSeparator = true;
            continue;
        }
        collapsed += c;
        lastWasSeparator = false;
    }
    while (!collapsed.empty() &&
           (collapsed.back() == '-' || collapsed.back() == '_' || collapsed.back() == ' ')) {
        collapsed.pop_back();
    }

    // Never hand back nothing: a file called `(freesound)` keeps its own stem
    // rather than becoming an entry with no name at all.
    return collapsed.empty() ? std::string(stem) : collapsed;
}

}  // namespace tb
