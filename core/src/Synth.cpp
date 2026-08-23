#include "rds/Synth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rds {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

/// How much of a loop is spent wrapping its tail over its head. The buffer is
/// rendered this much longer than the slot asks for and trimmed back, so the
/// length a caller was handed is the length it gets - a renderer sizes its mix
/// buffer from it, and a buffer short by the fade truncates the tail.
constexpr float kLoopFadeSec = 0.15f;

/// xorshift again, and seeded from the slot and variant rather than the clock,
/// so a stand-in is the same sound every session. A procedural layer that
/// changed shape every run would make the offline A/B meaningless.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    [[nodiscard]] std::uint64_t Next() {
        m_state ^= m_state >> 12;
        m_state ^= m_state << 25;
        m_state ^= m_state >> 27;
        return m_state * 0x2545F4914F6CDD1DULL;
    }

    /// -1 .. 1
    [[nodiscard]] float Bipolar() {
        return static_cast<float>(static_cast<std::int64_t>(Next() >> 11)) / 4503599627370496.0f -
               1.0f;
    }

    /// 0 .. 1
    [[nodiscard]] float Unit() { return 0.5f * (Bipolar() + 1.0f); }

private:
    std::uint64_t m_state;
};

/// One-pole low-pass. Enough for a tilt; nothing here needs a steep filter, and
/// the eventual authored files will carry their own EQ anyway.
class OnePole {
public:
    void SetCutoff(float hz, float sampleRate) {
        const float x = std::exp(-2.0f * kPi * std::clamp(hz, 1.0f, sampleRate * 0.45f) / sampleRate);
        m_a = 1.0f - x;
        m_b = x;
    }
    [[nodiscard]] float LowPass(float in) {
        m_z = m_a * in + m_b * m_z;
        return m_z;
    }
    [[nodiscard]] float HighPass(float in) { return in - LowPass(in); }
    void Reset() { m_z = 0.0f; }

private:
    float m_a{1.0f};
    float m_b{0.0f};
    float m_z{0.0f};
};

/// A resonant mode, for the hollow knock in wood and the slight ring the head
/// accent is allowed. Excited by an impulse and left to decay.
class Mode {
public:
    void Set(float hz, float decayMs, float sampleRate) {
        m_omega = 2.0f * kPi * hz / sampleRate;
        m_decay = std::exp(-1.0f / std::max(1.0f, decayMs * 0.001f * sampleRate));
        m_phase = 0.0f;
        m_amp = 1.0f;
    }
    [[nodiscard]] float Next() {
        const float value = m_amp * std::sin(m_phase);
        m_phase += m_omega;
        if (m_phase > 2.0f * kPi) {
            m_phase -= 2.0f * kPi;
        }
        m_amp *= m_decay;
        return value;
    }

private:
    float m_omega{};
    float m_decay{};
    float m_phase{};
    float m_amp{};
};

[[nodiscard]] float ExpEnvelope(float t, float attackSec, float decaySec) {
    if (t < attackSec) {
        return attackSec <= 0.0f ? 1.0f : t / attackSec;
    }
    return std::exp(-(t - attackSec) / std::max(1.0e-4f, decaySec));
}

/// Gentle saturation for the 2nd and 3rd harmonic, which is what keeps the sub
/// audible on a laptop speaker. The brief is explicit that the answer to "it
/// disappears on the laptop" is more saturation and not more level.
[[nodiscard]] float Saturate(float x, float drive) {
    return std::tanh(x * drive) / std::tanh(drive);
}

void Normalise(std::vector<float>& samples, float peakDb) {
    float peak = 0.0f;
    for (const float s : samples) {
        peak = std::max(peak, std::fabs(s));
    }
    if (peak <= 1.0e-6f) {
        return;
    }
    const float target = std::pow(10.0f, peakDb / 20.0f);
    const float scale = target / peak;
    for (float& s : samples) {
        s *= scale;
    }
}

/// The engine gives whole-file looping only, so the loop point is the asset's
/// problem: any audible pulse at the seam becomes a rhythm in-game. Wrapping the
/// tail back over the head is the cheapest way to make one seamless.
void CrossfadeEnds(std::vector<float>& samples, std::size_t fadeSamples) {
    if (samples.size() < fadeSamples * 3 || fadeSamples == 0) {
        return;
    }
    const std::size_t tailStart = samples.size() - fadeSamples;
    for (std::size_t i = 0; i < fadeSamples; ++i) {
        const float w = static_cast<float>(i) / static_cast<float>(fadeSamples);
        samples[i] = samples[i] * w + samples[tailStart + i] * (1.0f - w);
    }
    samples.resize(tailStart);
}

// ── the individual briefs ────────────────────────────────────────────────────

/// 01 §1's measured sweep, and the recipe in 02-SFX §3, which is the same thing
/// written as a synthesis patch: 150 Hz to 45 Hz over 60 ms exponentially, then
/// 45 Hz to 28 Hz over the next 200 ms; amplitude up in 3 ms and 20 dB down by
/// 40 ms; saturate, then low-pass at 300 Hz; and the 2 ms of un-faded attack is
/// the only click, because imp_transient is the click.
void RenderSub(std::vector<float>& out, float sampleRate, std::uint8_t variant) {
    const bool big = (variant % 2) == 1;
    const float startHz = big ? 110.0f : 150.0f;
    const float midHz = 45.0f;
    const float endHz = 28.0f;
    const float sweepSec = big ? 0.080f : 0.060f;
    const float tailSec = big ? 0.240f : 0.200f;

    OnePole lowPass;
    lowPass.SetCutoff(300.0f, sampleRate);

    float phase = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        float hz;
        if (t < sweepSec) {
            hz = startHz * std::pow(midHz / startHz, t / sweepSec);
        } else {
            const float u = std::min(1.0f, (t - sweepSec) / tailSec);
            hz = midHz * std::pow(endHz / midHz, u);
        }
        phase += 2.0f * kPi * hz / sampleRate;
        if (phase > 2.0f * kPi) {
            phase -= 2.0f * kPi;
        }
        // -20 dB at 40 ms is a 17 ms time constant; silent by 300 ms follows.
        const float amp = ExpEnvelope(t, 0.003f, 0.0174f);
        out[i] = lowPass.LowPass(Saturate(std::sin(phase) * amp, 2.2f));
    }
    // No high-pass. This layer owns the bottom exclusively, and it is the
    // loudest element in the mod - hence -1.0 rather than -1.5 dBFS.
    Normalise(out, -1.0f);
}

/// Bright, fast attack, 20 dB down inside 90 ms, high-passed at 400 Hz so it
/// cannot compete with the body layer.
void RenderTransient(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole highPass;
    highPass.SetCutoff(400.0f, sampleRate);
    OnePole tone;
    tone.SetCutoff(6000.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.0006f, 0.0195f);
        out[i] = highPass.HighPass(tone.LowPass(rng.Bipolar())) * amp;
    }
    Normalise(out, -1.5f);
}

/// Low-mid flesh and mass, tilted up in 250-800 Hz, 20 dB down in 145-155 ms,
/// and deliberately smooth: a plain body hit has only two or three low-mid
/// transient peaks. Anything with crackle in it is a crunch, not a body.
void RenderBody(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole highPass;
    highPass.SetCutoff(120.0f, sampleRate);
    OnePole band;
    band.SetCutoff(520.0f, sampleRate);
    OnePole smooth;
    smooth.SetCutoff(1400.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.004f, 0.0326f);
        const float noise = smooth.LowPass(rng.Bipolar());
        // The band-passed part carries the mass; a little of the wider noise
        // keeps it from sounding like a filter sweep.
        out[i] = (band.LowPass(highPass.HighPass(noise)) * 2.2f + noise * 0.25f) * amp;
    }
    Normalise(out, -1.5f);
}

void RenderSurface(std::vector<float>& out, float sampleRate, SlotId slot, Rng& rng) {
    OnePole tone;
    OnePole highPass;
    highPass.SetCutoff(150.0f, sampleRate);
    Mode modeA;
    Mode modeB;
    float decaySec = 0.03f;
    float modeMix = 0.0f;

    switch (slot) {
        case SlotId::kSurfWood:
            // A hollow knock is two low box modes over a short noise contact.
            tone.SetCutoff(2600.0f, sampleRate);
            modeA.Set(185.0f, 55.0f, sampleRate);
            modeB.Set(430.0f, 32.0f, sampleRate);
            modeMix = 0.55f;
            decaySec = 0.030f;
            break;
        case SlotId::kSurfStone:
            // Hard, short, almost no decay, and no modes at all - stone does not
            // ring, which is exactly what separates it from wood by ear.
            tone.SetCutoff(7000.0f, sampleRate);
            modeA.Set(900.0f, 6.0f, sampleRate);
            modeB.Set(1700.0f, 4.0f, sampleRate);
            modeMix = 0.12f;
            decaySec = 0.014f;
            break;
        default:
            // Dull, dead, no ring. The default for anything unresolved, which is
            // every natural ground surface until somebody records on snow.
            tone.SetCutoff(700.0f, sampleRate);
            modeA.Set(120.0f, 20.0f, sampleRate);
            modeB.Set(240.0f, 14.0f, sampleRate);
            modeMix = 0.20f;
            decaySec = 0.038f;
            break;
    }

    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.0008f, decaySec);
        const float noise = highPass.HighPass(tone.LowPass(rng.Bipolar())) * amp;
        const float ring = (modeA.Next() * 0.7f + modeB.Next() * 0.3f) * modeMix;
        out[i] = noise + ring;
    }
    Normalise(out, -1.5f);
}

/// Burst filler. Tiny, dry, and neutral, because the plugin scatters it hard in
/// pitch at runtime - these are the quiet nine of every ten events, under the
/// cliff, not heroes.
void RenderTap(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole tone;
    tone.SetCutoff(3200.0f, sampleRate);
    OnePole highPass;
    highPass.SetCutoff(220.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.0005f, 0.010f);
        out[i] = highPass.HighPass(tone.LowPass(rng.Bipolar())) * amp;
    }
    Normalise(out, -1.5f);
}

/// The most commonly mis-briefed sound in the set: a bone break is *density*,
/// not a snap. 01 §6 counts 24 separate transients in the 250-800 Hz band across
/// 420 ms, ten times the density of a plain thud in that band. So this is a
/// granular bed at ~57 grains a second, band-limited to 250-800 Hz, with no
/// single grain allowed to dominate.
void RenderCrunch(std::vector<float>& out, float sampleRate, Rng& rng) {
    std::ranges::fill(out, 0.0f);
    const float lengthSec = static_cast<float>(out.size()) / sampleRate;
    const int grains = std::max(15, static_cast<int>(std::lround(57.0f * lengthSec)));

    OnePole band;
    band.SetCutoff(800.0f, sampleRate);
    OnePole highPass;
    highPass.SetCutoff(250.0f, sampleRate);

    for (int g = 0; g < grains; ++g) {
        // Spread over the whole window with jitter rather than on a grid: an
        // even grid reads as a buzz at this density.
        const float centre = (static_cast<float>(g) + rng.Unit()) / static_cast<float>(grains);
        const auto start = static_cast<std::size_t>(centre * static_cast<float>(out.size()));
        const auto span = static_cast<std::size_t>((0.004f + 0.008f * rng.Unit()) * sampleRate);
        const float level = 0.35f + 0.65f * rng.Unit();
        for (std::size_t i = 0; i < span && start + i < out.size(); ++i) {
            const float t = static_cast<float>(i) / sampleRate;
            out[start + i] += rng.Bipolar() * level * ExpEnvelope(t, 0.0003f, 0.0025f);
        }
    }
    // The +4 dB wide bell the post-pass asks for, done as a band emphasis: that
    // band is where the granularity has to live or it sounds like a stick.
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float wide = out[i];
        const float t = static_cast<float>(i) / sampleRate;
        const float shape = ExpEnvelope(t, 0.010f, 0.180f);
        out[i] = (band.LowPass(highPass.HighPass(wide)) * 1.6f + wide * 0.35f) * shape;
    }
    Normalise(out, -1.5f);
}

/// Thick liquid burst. Low-mid, no bright edge, amplitude wobble rather than
/// transients.
void RenderGore(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole tone;
    tone.SetCutoff(1200.0f, sampleRate);
    OnePole wobble;
    wobble.SetCutoff(28.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.006f, 0.075f);
        const float modulation = 0.6f + 0.8f * std::fabs(wobble.LowPass(rng.Bipolar())) * 6.0f;
        out[i] = tone.LowPass(rng.Bipolar()) * amp * std::clamp(modulation, 0.2f, 1.6f);
    }
    Normalise(out, -1.5f);
}

/// 01 §7: the slide is a low rumble, not a hiss. The measured band tilt goes
/// *down* - sub -37 dB against air -56 - and there are ~65 grain peaks a second
/// riding on it. A bright scrape is the easiest way to get this wrong.
void RenderScrapeLoop(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole lowTilt;
    lowTilt.SetCutoff(220.0f, sampleRate);
    OnePole grainTone;
    grainTone.SetCutoff(1500.0f, sampleRate);
    OnePole airCut;
    airCut.SetCutoff(4000.0f, sampleRate);

    for (std::size_t i = 0; i < out.size(); ++i) {
        const float noise = rng.Bipolar();
        // Low shelf up, high shelf down: the rumble is the body of it and the
        // top is only there so it does not sound like a sine.
        const float low = lowTilt.LowPass(noise) * 3.2f;
        const float mid = airCut.LowPass(noise) * 0.55f;
        out[i] = low + mid;
    }

    const float lengthSec = static_cast<float>(out.size()) / sampleRate;
    const int grains = static_cast<int>(std::lround(65.0f * lengthSec));
    for (int g = 0; g < grains; ++g) {
        const float centre = (static_cast<float>(g) + rng.Unit()) / static_cast<float>(grains);
        const auto start = static_cast<std::size_t>(centre * static_cast<float>(out.size()));
        const auto span = static_cast<std::size_t>(0.004f * sampleRate);
        const float level = 0.25f + 0.5f * rng.Unit();
        for (std::size_t i = 0; i < span && start + i < out.size(); ++i) {
            const float t = static_cast<float>(i) / sampleRate;
            out[start + i] += grainTone.LowPass(rng.Bipolar()) * level *
                              ExpEnvelope(t, 0.0002f, 0.0015f);
        }
    }
    CrossfadeEnds(out, static_cast<std::size_t>(kLoopFadeSec * sampleRate));
    Normalise(out, -1.5f);
}

/// Part of the continuous bed, so no peak may stand more than a few dB above it:
/// a transient here pokes through the mix at the moment everything else is quiet.
void RenderCloth(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole tone;
    tone.SetCutoff(2400.0f, sampleRate);
    OnePole body;
    body.SetCutoff(600.0f, sampleRate);
    OnePole envelope;
    envelope.SetCutoff(6.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float noise = tone.LowPass(rng.Bipolar()) - body.LowPass(rng.Bipolar()) * 0.4f;
        const float slow = 0.6f + 2.5f * std::fabs(envelope.LowPass(rng.Bipolar()));
        out[i] = noise * std::clamp(slow, 0.3f, 1.3f);
    }
    CrossfadeEnds(out, static_cast<std::size_t>(kLoopFadeSec * sampleRate));
    Normalise(out, -1.5f);
}

/// Flat and loopable, not a designed sweep with a built-in climax: the airborne
/// rise is driven as a parameter, so any shape baked in here would fight it.
void RenderWhoosh(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole tone;
    tone.SetCutoff(700.0f, sampleRate);
    OnePole cap;
    cap.SetCutoff(2000.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = cap.LowPass(tone.LowPass(rng.Bipolar()) * 3.0f);
    }
    CrossfadeEnds(out, static_cast<std::size_t>(kLoopFadeSec * sampleRate));
    Normalise(out, -1.5f);
}

/// The one file allowed a tonal tail: a dull skull thud with a granular edge and
/// a slight low resonance for 100-150 ms. Seven low-mid peaks - more than a plain
/// thud, far fewer than a spine break.
void RenderHeadImpact(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole tone;
    tone.SetCutoff(900.0f, sampleRate);
    Mode ring;
    ring.Set(245.0f, 130.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.003f, 0.045f);
        out[i] = tone.LowPass(rng.Bipolar()) * amp * 1.6f + ring.Next() * 0.35f;
    }
    // Seven small cracks, no more - the granular edge, not a crunch bed.
    for (int g = 0; g < 7; ++g) {
        const auto start = static_cast<std::size_t>(
            (0.02f + 0.10f * rng.Unit()) * sampleRate + static_cast<float>(g) * 0.004f * sampleRate);
        const auto span = static_cast<std::size_t>(0.006f * sampleRate);
        for (std::size_t i = 0; i < span && start + i < out.size(); ++i) {
            const float t = static_cast<float>(i) / sampleRate;
            out[start + i] += rng.Bipolar() * 0.5f * ExpEnvelope(t, 0.0002f, 0.0018f);
        }
    }
    Normalise(out, -1.5f);
}

/// The only impact-family sound that should NOT have an instant transient. It is
/// punctuation: a soft flop of flesh and cloth that closes the event.
void RenderSettle(std::vector<float>& out, float sampleRate, Rng& rng) {
    OnePole tone;
    tone.SetCutoff(900.0f, sampleRate);
    OnePole body;
    body.SetCutoff(260.0f, sampleRate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float amp = ExpEnvelope(t, 0.018f, 0.060f);
        const float noise = tone.LowPass(rng.Bipolar());
        out[i] = (body.LowPass(noise) * 2.0f + noise * 0.3f) * amp;
    }
    Normalise(out, -1.5f);
}

}  // namespace

SynthBuffer Synthesise(SlotId slot, std::uint8_t variant, float lengthMs, float sampleRate) {
    SynthBuffer buffer;
    buffer.sampleRate = sampleRate;

    const SlotDesc& desc = Slot(slot);
    if (desc.expectedVariants == 0) {
        // The declared-and-unfilled voice slots. Nothing to stand in for.
        return buffer;
    }

    const float length = lengthMs > 0.0f ? lengthMs
                                         : 0.5f * (desc.minLengthMs + desc.maxLengthMs);
    const auto count = static_cast<std::size_t>(std::max(16.0f, length * 0.001f * sampleRate));
    // A loop is rendered long by exactly the crossfade it will lose, so what
    // comes back is the length that was asked for.
    const std::size_t fade =
        desc.isLoop ? static_cast<std::size_t>(kLoopFadeSec * sampleRate) : 0u;
    buffer.samples.assign(count + fade, 0.0f);

    // Seeded from the slot and its variant so a stand-in is the same sound every
    // session, and different between variants.
    Rng rng{0xC0FFEEULL * (static_cast<std::uint64_t>(slot) + 1) + variant * 0x9E3779B9ULL + 1};

    switch (slot) {
        case SlotId::kImpSub:
            RenderSub(buffer.samples, sampleRate, variant);
            break;
        case SlotId::kImpTransient:
            RenderTransient(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kImpBody:
            RenderBody(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kSurfWood:
        case SlotId::kSurfStone:
        case SlotId::kSurfSoft:
            RenderSurface(buffer.samples, sampleRate, slot, rng);
            break;
        case SlotId::kLimbTap:
            RenderTap(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kCrunchGran:
            RenderCrunch(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kGoreWet:
            RenderGore(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kScrapeLoop:
            RenderScrapeLoop(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kFoleyCloth:
            RenderCloth(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kAirWhoosh:
            RenderWhoosh(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kHeadImpact:
            RenderHeadImpact(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kSettleRest:
            RenderSettle(buffer.samples, sampleRate, rng);
            break;
        case SlotId::kGruntImpact:
        case SlotId::kScreamBig:
        case SlotId::kCount:
            buffer.samples.clear();
            break;
    }
    return buffer;
}

SynthBuffer Synthesise(const ResolvedSound& sound, float sampleRate) {
    return Synthesise(sound.slot, sound.variant, sound.lengthMs, sampleRate);
}

}  // namespace rds
