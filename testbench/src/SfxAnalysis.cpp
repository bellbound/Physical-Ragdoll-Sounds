#include "SfxAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
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

/// Peak magnitude in `ms`-wide hops, in dB.
///
/// The onset detector's envelope, and it is a peak rather than the RMS every
/// other measurement here uses for one reason: a 2 ms RMS window over a 30 Hz
/// tone is a window inside one sixth of a cycle, so it swings tens of dB twice
/// per cycle and every one of those swings looks like a contact. A 10 ms peak
/// window over the same tone varies by under 5 dB, which is under the 8 dB an
/// onset has to rise - so `imp_sub`'s sweep reads as the one contact it is.
[[nodiscard]] Envelope PeakEnvelopeDb(std::span<const float> x, int sampleRate, float ms = 10.0f) {
    Envelope e;
    e.hop = std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate * ms / 1000.0f));
    const std::size_t count = x.size() > e.hop ? (x.size() - e.hop) / e.hop : 0;
    e.db.reserve(std::max<std::size_t>(count, 1));
    for (std::size_t i = 0; i < count; ++i) {
        float peak = 0.0f;
        for (std::size_t k = 0; k < e.hop; ++k) {
            peak = std::max(peak, std::fabs(x[i * e.hop + k]));
        }
        e.db.push_back(Db(peak));
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

/// Contact positions, in envelope hops. sfx.py's `onsets()`: a rise of at least
/// 8 dB inside 12 ms, at least 46 ms apart, and within 32 dB of the loudest.
///
/// The 46 ms is not a tuning constant. It is the onset-gap floor measured in
/// three independent reference clips (04-Reference-Analysis.md §2) - about where
/// hearing stops resolving two impacts as two - so anything closer is one
/// contact, not two, and the engine's own rate cap is the same number.
[[nodiscard]] std::vector<std::size_t> OnsetHops(const Envelope& e, int sampleRate) {
    std::vector<std::size_t> hits;
    if (e.db.size() < 3) {
        return hits;
    }
    const float top = *std::ranges::max_element(e.db);
    const float threshold = top - 32.0f;
    const auto look = std::max<std::size_t>(
        1, static_cast<std::size_t>(12.0 * sampleRate / 1000.0 / static_cast<double>(e.hop)));
    const double gap = 46.0 * sampleRate / 1000.0 / static_cast<double>(e.hop);
    double last = -1e9;

    // A contact is a rise, so a file that *starts* on one has nothing to rise
    // from and the detector walks straight past it. sfx.py can live with that -
    // it reads takes, and a take has room tone in front of it - but a library
    // file has had its head silence trimmed on the way in, which is precisely a
    // file that starts on its own attack. Without this, a trimmed one-shot with
    // a satellite in it reports the satellite as its only contact.
    if (e.db.front() >= top - 12.0f) {
        hits.push_back(0);
        last = 0.0;
    }

    for (std::size_t i = look; i + 1 < e.db.size(); ++i) {
        if (e.db[i] < threshold || e.db[i] < e.db[i + 1]) {
            continue;
        }
        const auto first = e.db.begin() + static_cast<std::ptrdiff_t>(i - look);
        const auto here = e.db.begin() + static_cast<std::ptrdiff_t>(i);
        if (e.db[i] - *std::min_element(first, here) < 8.0f) {
            continue;
        }
        if (static_cast<double>(i) - last < gap) {
            // Two contacts inside the resolution floor are one contact, and it
            // is the louder of the pair.
            if (!hits.empty() && e.db[i] > e.db[hits.back()]) {
                hits.back() = i;
                last = static_cast<double>(i);
            }
            continue;
        }
        hits.push_back(i);
        last = static_cast<double>(i);
    }
    return hits;
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
    // The spine and limb crunches are the same brief on a different bone, so
    // they are held to the same density floor. The limb's band runs higher and
    // its tail shorter: a snap out on an arm is drier and tighter than a skull
    // being crushed, and holding it to the head's centroid window would pass a
    // file that sounds like the wrong bone.
    {rds::SlotId::kSpineCrunch, 30, 400, 400, 4000, kNone, kNone, 15, 99, 0, 0, 0, 0},
    {rds::SlotId::kLimbCrunch, 25, 350, 700, 6000, kNone, kNone, 15, 99, 0, 0, 0, 0},
    {rds::SlotId::kGoreWet, 20, 350, 400, 4000, kNone, kNone, -1, -1, 0, 0, 0, 0},
    {rds::SlotId::kScrapeLoop, 0, 0, 0, 0, 5, 19, -1, -1, 2.5f, 8.0f, 8, 40},
    // The surface variants are the same brief on a different floor, so they are
    // held to the same measurements. The limb grind is not: a small contact
    // patch has no low shelf to speak of and catches far more often per second,
    // so its tilt floor comes off and its grain range goes up.
    {rds::SlotId::kScrapeBodyWood, 0, 0, 0, 0, 5, 19, -1, -1, 2.5f, 8.0f, 8, 40},
    {rds::SlotId::kScrapeBodyStone, 0, 0, 0, 0, 5, 19, -1, -1, 2.5f, 8.0f, 8, 40},
    {rds::SlotId::kScrapeLimb, 0, 0, 0, 0, kNone, 8, -1, -1, 2.0f, 8.0f, 25, 120},
    {rds::SlotId::kScrapeLimbWood, 0, 0, 0, 0, kNone, 8, -1, -1, 2.0f, 8.0f, 25, 120},
    {rds::SlotId::kScrapeLimbStone, 0, 0, 0, 0, kNone, 8, -1, -1, 2.0f, 8.0f, 25, 120},
    {rds::SlotId::kScrapeGrain, 40, 400, 300, 6000, kNone, kNone, 6, 40, 0, 0, 0, 0},
    // The bed, and the one loop in the set held to the *opposite* of the grinds'
    // tilt window. A grind is judged for having a low shelf at all - +5 to +19 -
    // and this is judged for having almost nothing else: over +20, with a
    // centroid down where the references put the loudest band of a slide. Its
    // steadiness band is the whoosh's rather than a grind's, because the one
    // thing it must not have is features - a bump in a bed becomes a pulse the
    // moment the file loops, and there is no grit here to hide it. No grain
    // range: counting grit peaks in a layer whose whole definition is having
    // none would fail every correct file.
    {rds::SlotId::kScrapeLoopRumble, 0, 0, 20, 1200, 20, kNone, -1, -1, 0.5f, 6.0f, 0, 0},
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
    // Cleared rather than assumed clear: the importer measures a file, repairs
    // it and measures it again into the same entry, and a count that accumulates
    // across the two would double. Everything below either assigns or is reset
    // here first.
    entry.clipRuns = 0;
    entry.clipPct = 0.0f;
    entry.contacts = 0;
    entry.satelliteDb = 0.0f;
    entry.satelliteAtMs = 0.0f;
    entry.noiseFloorDb = 99.0f;
    entry.topOctaveDb = 0.0f;
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

    // ── flat tops ────────────────────────────────────────────────────────────
    //
    // Clipping, measured as clipping rather than as a peak reading. A peak over
    // -0.2 dBFS is a *headroom* fault and the import pass normalises it away;
    // what it cannot touch is a wave whose top is already gone, and that is what
    // this counts: runs of samples sitting at the file's own maximum.
    //
    // Against the maximum, not against full scale, so the measurement survives
    // the normalise - a plateau is still a plateau 1.5 dB down. Five samples at
    // 48 kHz (104 us) with 0.02% tolerance is the shortest run that a bass note
    // cannot produce on its own: a 100 Hz sine falls 0.05% off its crest in that
    // time, and anything slower than about 50 Hz is the one blind spot, which
    // `imp_sub`'s decaying sweep only reaches on its single loudest crest.
    if (peak > 0.05f) {
        const float ceiling = peak * 0.9998f;
        std::size_t run = 0;
        std::size_t flat = 0;
        for (std::size_t i = 0; i <= mono.size(); ++i) {
            const bool at = i < mono.size() && std::fabs(mono[i]) >= ceiling;
            if (at) {
                ++run;
                continue;
            }
            if (run >= 5) {
                ++entry.clipRuns;
                flat += run;
            }
            run = 0;
        }
        entry.clipPct = 100.0f * static_cast<float>(flat) / static_cast<float>(mono.size());
    }

    // ── identity ─────────────────────────────────────────────────────────────
    //
    // FNV-1a over the samples quantised to 16 bits, which is what the file
    // holds: two downloads of the same sound under two names hash the same, and
    // a re-encode of one of them does not have to.
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const float s : mono) {
            const auto q = static_cast<std::uint16_t>(
                static_cast<std::int16_t>(std::lrint(std::clamp(s, -1.0f, 1.0f) * 32767.0f)));
            hash = (hash ^ static_cast<std::uint8_t>(q & 0xFF)) * 1099511628211ULL;
            hash = (hash ^ static_cast<std::uint8_t>(q >> 8)) * 1099511628211ULL;
        }
        entry.contentHash = hash;
    }

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

        // ── the noise floor, sfx.py's `snr` ──────────────────────────────────
        //
        // The quietest 50 ms in front of the attack, against the peak. Measured
        // in the pre-roll rather than in the tail because the tail is the sound
        // decaying into the floor and there is no line between the two; the room
        // tone before the hit is the floor on its own. Nothing to measure it in
        // leaves 99, which reads as "not known" and never trips the gate - a
        // file trimmed to its attack has no floor to measure, and inventing one
        // out of its decay would fail every short dry one-shot in the pack.
        //
        // The attack is found here rather than taken from FindEvent, which walks
        // back to peak-35 dB: on the one file this measurement exists for - the
        // one whose hiss sits inside 30 dB of the hit - that walk goes all the
        // way to sample 0 and hands back no pre-roll at all. The first hop
        // within 12 dB of the peak cannot do that, and 20 ms in front of it
        // keeps the foot of the rise out of the measurement.
        std::size_t before = 0;
        {
            const Envelope head = PeakEnvelopeDb(mono, sampleRate, 2.0f);
            const float top = *std::ranges::max_element(head.db);
            std::size_t attack = 0;
            while (attack + 1 < head.db.size() && head.db[attack] < top - 12.0f) {
                ++attack;
            }
            const auto guard = static_cast<std::size_t>(0.02 * sampleRate);
            const std::size_t at = attack * head.hop;
            before = at > guard ? at - guard : 0;
        }
        if (before > static_cast<std::size_t>(0.1 * sampleRate)) {
            const auto window = static_cast<std::size_t>(0.05 * sampleRate);
            float quietest = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i + window <= before; i += window) {
                double acc = 0.0;
                for (std::size_t k = 0; k < window; ++k) {
                    acc += static_cast<double>(mono[i + k]) * mono[i + k];
                }
                quietest = std::min(quietest,
                                    static_cast<float>(std::sqrt(acc / static_cast<double>(window))));
            }
            if (quietest != std::numeric_limits<float>::max()) {
                // Capped at the sentinel: digital silence in front measures as
                // -180 dB, and "the floor is 178 dB down" is a number nobody
                // needs to read past "there is no floor".
                entry.noiseFloorDb = std::min(99.0f, entry.peakDb - Db(quietest));
            }
        }

        // ── how many contacts, and what is riding under the hero ─────────────
        //
        // Both come off the same onset list, and neither is a fault on its own:
        // a take with four contacts in it is four one-shots that have not been
        // cut apart yet (03-Asset-Status.md §3.3, and every shipped limb_tap),
        // while one 20+ dB down is the satellite §7 found in 39 takes of 100.
        const Envelope whole = PeakEnvelopeDb(mono, sampleRate);
        const std::vector<std::size_t> hits = OnsetHops(whole, sampleRate);
        entry.contacts = static_cast<int>(hits.size());
        if (hits.size() > 1) {
            std::size_t hero = hits.front();
            for (const std::size_t h : hits) {
                if (whole.db[h] > whole.db[hero]) {
                    hero = h;
                }
            }
            const std::size_t* loudest = nullptr;
            for (const std::size_t& h : hits) {
                if (h != hero && (loudest == nullptr || whole.db[h] > whole.db[*loudest])) {
                    loudest = &h;
                }
            }
            if (loudest != nullptr) {
                entry.satelliteDb = whole.db[*loudest] - whole.db[hero];
                entry.satelliteAtMs = (static_cast<float>(*loudest) - static_cast<float>(hero)) *
                                      static_cast<float>(whole.hop) * 1000.0f /
                                      static_cast<float>(sampleRate);
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
        double topOctave = 0.0;
        std::array<double, std::size(kBands)> energy{};
        for (std::size_t k = 0; k <= half; ++k) {
            const double magnitude = std::abs(spectrum[k]);
            const double hz = static_cast<double>(k) * binHz;
            weighted += magnitude * hz;
            total += magnitude;
            // Parseval: mean-square over the *unpadded* length, so the trailing
            // zeros do not quietly halve every band.
            const double power = magnitude * magnitude * ((k == 0 || k == half) ? 1.0 : 2.0);
            // Everything a lossy encoder or a 32 kHz source takes off the top
            // lands above 16 kHz, so it is worth its own accumulator rather than
            // being averaged into an air band that starts at 8 k.
            if (hz >= 16000.0) {
                topOctave += power;
            }
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

        // Against the 2.5-8 kHz band rather than against the whole file, so a
        // sound that simply has no top end - a sub layer, a body thud - is not
        // read as one that had it taken away.
        entry.topOctaveDb =
            Db(static_cast<float>(std::sqrt(topOctave / norm))) - level[4];

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

    // Two kinds, and what separates them is not severity - it is whether there
    // is anything to do about it.
    //
    // Everything `warn` reports has a fix, and the fix is the same one twice:
    // the repair pass, which runs on every import and is one click on a row for
    // anything that arrived another way. So a `warn` badge on a file that came
    // through the importer means the repair could not reach it - a length, a
    // seam, a second contact - not that nobody has pressed the button.
    //
    // `dead` is the other list: a wave whose top is already gone, hiss 29 dB
    // under the hero, two channels that are different takes. Processing does
    // not recover any of them and the answer is another file. They are still
    // assignable, because the library never refuses.
    const auto warn = [&entry](std::string code, std::string detail) {
        entry.warnings.push_back({std::move(code), std::move(detail), false, false});
    };
    const auto dead = [&entry](std::string code, std::string detail) {
        entry.warnings.push_back({std::move(code), std::move(detail), false, true});
    };

    if (entry.durationMs <= 0.0f) {
        return;
    }

    // ── format ───────────────────────────────────────────────────────────────
    //
    // The importer converts, so these only appear on a file that arrived some
    // other way: dropped into the folder by hand, written by sfx.py at another
    // rate, or imported on a machine with no ffmpeg.
    if (entry.sampleRate != 0 && entry.sampleRate != 48000) {
        warn("rate", std::format("{} Hz, not the pack's 48 kHz. The game resamples it at load, "
                                 "which is audible on anything with a bright transient.\n\n"
                                 "Fix: `repair` on this row, or import it again. Both convert "
                                 "through ffmpeg, so both need it on PATH.",
                                 entry.sampleRate));
    }
    if (entry.channels > 1) {
        warn("stereo", std::format("{} channels. The engine plays mono and positions the sound "
                                   "itself, so the second channel is thrown away - and if the two "
                                   "are not correlated, throwing it away comb-filters what is "
                                   "left.\n\nFix: `repair`, which folds it the way the importer "
                                   "would have.",
                                   entry.channels));
    }
    if (entry.bitsPerSample != 0 && entry.bitsPerSample != 16) {
        warn("bits", std::format("{}-bit, not the pack's 16. Harmless in itself - everything "
                                 "downstream is float - but it is not what the rest of the pack "
                                 "is.\n\nFix: `repair` writes it back at 16.",
                                 entry.bitsPerSample));
    }

    // ── level ────────────────────────────────────────────────────────────────
    //
    // Peak is a repaired quantity now: the import pass normalises to -1.5 dBFS
    // unless the file already sits in the band that holds both pack targets, so
    // these two fire on files the repair has not been run on. The band is
    // `sfxrepair`'s and the warnings are its edges, so there is no gap between
    // "left alone" and "flagged" for a file to fall into.
    if (entry.peakDb > sfxrepair::kPeakHotDb) {
        warn("hot", std::format("peaks at {:.1f} dBFS against the pack's -1.5 (-1.0 for imp_sub). "
                                "Everything is pitch-scattered +/-3 semitones at runtime and has "
                                "to survive it, and this has almost nothing to work with.\n\n"
                                "Fix: `repair` normalises it to -1.5.",
                                entry.peakDb));
    } else if (entry.peakDb < sfxrepair::kPeakQuietDb) {
        warn("quiet", std::format("peaks at {:.1f} dBFS, under the pack's -1.5. The engine sets "
                                  "the level, so a quiet file does not play quietly - it plays at "
                                  "the same level with less resolution behind it, and reads as "
                                  "softer than its neighbours in the same slot.\n\n"
                                  "Fix: `repair` normalises it.",
                                  entry.peakDb));
    }

    // Clipping proper, which is the half of "too loud" that no amount of gain
    // undoes. Slots.md §5 wants everything pre-limited and dry; a squared-off
    // wave is that rule broken at the source, and the odd harmonics it leaves
    // ride the +/-3 semitone scatter down as well as up.
    if (entry.clipRuns >= 5) {
        const bool bad = entry.clipRuns >= 40 || entry.clipPct >= 0.2f;
        std::string detail =
            std::format("the waveform is squared off in {} places, {:.2f}% of the samples sitting "
                        "flat at the maximum.\n\nThis is not the headroom rule - headroom is "
                        "normalised on the way in and this file is at {:.1f} dBFS. It is the tops "
                        "of the waves already gone before the file was written, and the buzz that "
                        "leaves is in the samples. Turning it down turns down the buzz with it.{}",
                        entry.clipRuns, entry.clipPct, entry.peakDb,
                        bad ? "\n\nNothing here can repair it: re-source the file, or generate the "
                              "take again with the output level down."
                            : "\n\nA handful of flats on the loudest crest is worth hearing "
                              "before judging - a heavily limited take reads the same way and can "
                              "still be the right sound.");
        if (bad) {
            dead("clipped", std::move(detail));
        } else {
            warn("clipped", std::move(detail));
        }
    }

    if (std::fabs(entry.dcOffset) > sfxrepair::kDcOffset * 2.0f) {
        warn("dc", std::format("DC offset {:+.3f}. It eats headroom on one side and thumps when "
                               "the sound is cut off mid-way, which the engine does on every "
                               "fade.\n\nFix: `repair` subtracts it.",
                               entry.dcOffset));
    }

    // ── the noise floor ──────────────────────────────────────────────────────
    //
    // 30 dB is tools/sfx.py's gate and it is what ruled `twisting…gristle` out
    // of the 2026-08-23 batch. Measured in the pre-roll, so a loop - which has
    // none - is never asked.
    if (!entry.loops && entry.noiseFloorDb < 40.0f) {
        if (entry.noiseFloorDb < 30.0f) {
            dead("noise floor",
                 std::format("the room tone in front of the hit is only {:.0f} dB under it. "
                             "tools/sfx.py fails a take at 30 and this is under that.\n\n"
                             "A slot plays its variants over and over, so the bed comes back every "
                             "time and reads as hiss rather than as the room. Nothing takes "
                             "broadband noise out without taking the transient with it - "
                             "re-source it, or generate the take again.",
                             entry.noiseFloorDb));
        } else {
            warn("noisy", std::format("{:.0f} dB between the hit and the room tone in front of it. "
                                      "Past the 30 dB gate, so it is usable, but it will be "
                                      "audible under a quiet cell and it stacks with every other "
                                      "layer in the impact.\n\nNothing repairs this; it is worth "
                                      "knowing before it goes on `surf_soft`, which fires on "
                                      "everything unresolved.",
                                      entry.noiseFloorDb));
        }
    }

    // ── what else is in the file ─────────────────────────────────────────────
    if (!entry.loops && entry.contacts > 1) {
        if (entry.satelliteDb <= -20.0f) {
            warn("satellite",
                 std::format("a second contact {:+.0f} ms from the hero, {:.0f} dB under it.\n\n"
                             "03-Asset-Status.md §7 found one of these in 39 takes out of 100 - "
                             "usually a bright debris wash the generator bolted on. Left in, it "
                             "lands on top of whatever the engine schedules next and reads as a "
                             "flam; on an impact layer it collides with `imp_sub`'s own arrival at "
                             "+55-75 ms.\n\nFix: cut it. `python tools/sfx.py split <file>` cuts at "
                             "the 46 ms contact floor, and tools/triage_batch.py drops satellites "
                             "at exactly this 20 dB threshold and records every one.",
                             entry.satelliteAtMs, entry.satelliteDb));
        } else if (entry.loMidTransients < 15) {
            // Under the density `crunch_gran` is specified at. Over it, the
            // "contacts" are the grains: Slots.md §3 asks that slot for 15-99
            // transients in 250-800 Hz and §2 for "many small crackles
            // overlapping into one texture", and both shipped files land 5 and 3
            // onsets apart on this measure. Telling somebody to split a bone
            // crunch into its crackles is advice that would ruin it.
            warn("contacts",
                 std::format("{} contacts at least 46 ms apart, the loudest pair {:.0f} dB "
                             "apart.\n\nNot a fault - it is several one-shots in one file, and "
                             "cutting them is where all four shipped `limb_tap` files came from "
                             "(03-Asset-Status.md §3.3). But assigned whole it plays whole: the "
                             "slot fires once and every contact in the file sounds.\n\n"
                             "Fix: `python tools/sfx.py split <file>`, then import the pieces.",
                             entry.contacts, -entry.satelliteDb));
        }
    }

    // ── what the top end says about where it came from ───────────────────────
    //
    // A container that claims 48 kHz proves nothing: an upsample and a lossy
    // encode both leave the same hole, and neither is undoable. Only asked of
    // files that have room for a top octave in the first place.
    if (entry.sampleRate >= 40000 && entry.topOctaveDb < -70.0f && entry.peakDb > -40.0f) {
        warn("band-limited",
             std::format("nothing above 16 kHz - it sits {:.0f} dB under the 2.5-8 kHz band. The "
                         "file says {} Hz, but it was upsampled from something lower or squeezed "
                         "through a lossy encoder before it got here, and a conversion cannot put "
                         "a top octave back.\n\nFine for a body or a sub layer, which live below "
                         "4 kHz anyway. Worth re-sourcing for `imp_transient` or `surf_stone`, "
                         "where the top end is the whole character.",
                         entry.topOctaveDb, entry.sampleRate));
    }

    // ── length and shape ─────────────────────────────────────────────────────
    if (entry.loops) {
        // A loop is a texture, so it is never too long and never has a lead-in.
        // Its seam is the thing: the engine repeats it whole with no crossfade,
        // so any level step at the join becomes a rhythm.
        if (entry.seamDb > 6.0f) {
            warn("seam", std::format("{:.1f} dB step between the first and last 50 ms. Loops are "
                                     "played whole with no crossfade, so this becomes an audible "
                                     "pulse once a second in game.\n\nFix: `python tools/sfx.py "
                                     "make --slot scrape_loop <file>` sweeps for the quietest seam "
                                     "and folds the tail back over the head with an equal-power "
                                     "crossfade. That is what took scrape_loop_01 from 3.9 dB to "
                                     "0.02. The repair pass deliberately leaves loops alone - a "
                                     "fade at the end of one is a hole, not a fix.",
                                     entry.seamDb));
        }
    } else {
        if (entry.leadInMs > sfxrepair::kLeadInMs) {
            warn("lead-in", std::format("{:.0f} ms of silence before the sound starts. The cue time "
                                        "*is* the attack, so head silence becomes latency and puts "
                                        "the +15/+50/+65 ms layer offsets out.\n\n"
                                        "Fix: `repair` trims it.",
                                        entry.leadInMs));
        }
        if (entry.durationMs < 30.0f) {
            warn("very short", std::format("{:.0f} ms. Shorter than every slot's minimum; it will "
                                           "read as a click rather than as a contact.\n\nNothing "
                                           "lengthens a sound that is not there - this is a "
                                           "fragment, most likely a split that cut too tight.",
                                           entry.durationMs));
        }
        if (entry.durationMs > 3000.0f) {
            warn("very long", std::format("{:.1f} s and it does not measure as a texture. Either it "
                                          "has a baked room tail or it is several sounds in one "
                                          "file.\n\nA long file is not wrong on its own: Slots.md "
                                          "§6 wants textures cut both ways, and the only "
                                          "`scrape_loop` to pass the 2026-08-23 batch was a 9.9 s "
                                          "long cut. It is wrong in a one-shot slot, which plays "
                                          "the whole of it.\n\nFix: `sfx.py make` windows it.",
                                          entry.durationMs / 1000.0f));
        }
        // A tail far longer than the event is the baked-reverb tell.
        if (entry.usableMs > 0.0f && entry.durationMs > entry.usableMs * 2.0f + 200.0f) {
            warn("tail", std::format("{:.0f} ms of event inside a {:.0f} ms file. The rest is room, "
                                     "and the game applies the cell's own on top - a baked tail "
                                     "sounds like a cave inside a cave, and it turns overlapping "
                                     "impacts to mud.\n\nFix: `sfx.py make --len` cuts to the "
                                     "event, or generate the take dry. The repair pass will not "
                                     "do it: where a decay stops being the sound and starts being "
                                     "the room is a judgement, and trimming it wrong takes the "
                                     "body off the hit.",
                                     entry.usableMs, entry.durationMs));
        }
    }

    if (entry.peakDb <= -60.0f) {
        dead("silent", "nothing audible in the file at all. It will play, and you will hear "
                       "nothing. Whatever was meant to be here did not survive the export.");
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
