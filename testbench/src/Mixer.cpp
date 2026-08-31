#include "Mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "miniaudio.h"

#include "rds/Mix.h"
#include "rds/VanillaTrack.h"

namespace tb {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

std::uint32_t CacheKey(rds::SlotId slot, std::uint8_t variant) {
    return (static_cast<std::uint32_t>(slot) << 8) | variant;
}

float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

/// Equal-power pan from a world position, as heard from the listener. `centred`
/// is a voice hung on a bone: at arm's length the player's own ragdoll is not a
/// direction.
///
/// **Direction only, and there is no distance term anywhere on this side.** Not
/// because the engine put one in `gainDb` - it explicitly does not, see the note
/// at Engine.cpp's `Emit` - but because Skyrim attenuates a positioned voice
/// itself through the output model the renderer attaches. So what is tuned here
/// is the un-attenuated cue, in both halves, and falloff is the game's alone.
void PanForPoint(const rds::Vec3& position, bool centred, const rds::ListenerState& listener,
                 float& gl, float& gr) {
    float pan = 0.0f;
    if (!centred) {
        // right = facing x up, with up = +z
        const rds::Vec3 f = listener.facing;
        const float rx = f.y, ry = -f.x;
        const float rlen = std::sqrt(rx * rx + ry * ry);
        if (rlen > 1e-4f) {
            const float dx = position.x - listener.position.x;
            const float dy = position.y - listener.position.y;
            const float lateral = (dx * rx + dy * ry) / rlen;
            pan = std::clamp(lateral / 220.0f, -1.0f, 1.0f) * 0.75f;  // ~3 m to full width
        }
    }
    const float a = (pan + 1.0f) * 0.25f * kPi;  // 0..pi/2
    gl = std::cos(a);
    gr = std::sin(a);
}

/// One-shot: read `src` at `pitch` with linear interpolation, into the mix.
///
/// The vanilla track's, now. A cue goes through `rds::MixComposite` with the rest
/// of its moment; a vanilla row is one file the game played on its own and has no
/// moment to belong to.
void AddOneShot(std::vector<float>& mix, std::size_t frames, const std::vector<float>& src,
                double startSec, int sr, float pitch, float gain, float gl, float gr, float fadeMs) {
    if (src.empty() || gain <= 0.0f) return;
    const double step = std::max(0.01f, pitch);
    const double srcLen = static_cast<double>(src.size());
    const std::size_t start = static_cast<std::size_t>(std::max(0.0, startSec) * sr);
    const int fadeSamples = static_cast<int>(fadeMs * 0.001f * static_cast<float>(sr));

    double pos = 0.0;
    for (std::size_t i = start; i < frames; ++i) {
        if (pos >= srcLen - 1.0) break;
        const std::size_t i0 = static_cast<std::size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));
        const float s = src[i0] * (1.0f - frac) + src[i0 + 1] * frac;
        float g = gain;
        if (fadeSamples > 0) {
            const int done = static_cast<int>(i - start);
            if (done < fadeSamples) g *= static_cast<float>(done) / static_cast<float>(fadeSamples);
        }
        mix[i * 2] += s * g * gl;
        mix[i * 2 + 1] += s * g * gr;
        pos += step;
    }
}

/// A loop's life, gathered so it can be rendered in one pass with the source
/// phase carried across gain and pitch changes.
struct LoopPoint {
    double timeSec{};
    float gain{};
    float pitch{1.0f};
    float gl{1.0f}, gr{1.0f};
    bool stop{};
    float fadeMs{};
};

void AddLoop(std::vector<float>& mix, std::size_t frames, const std::vector<float>& src,
             const std::vector<LoopPoint>& points, int sr) {
    if (src.empty() || points.empty()) return;
    const double srcLen = static_cast<double>(src.size());
    if (srcLen < 2.0) return;

    // The seam, the way `MixLoop` makes it in the game: the lap turns over a
    // crossfade before the end of the file and the tail is mixed back across the
    // head. Wrapping hard - which is what this did - put a step the size of the
    // signal into `scrape_loop` once every two seconds, and only here: the
    // shipped file's ends are a full -25 dBFS apart, which is its own noise floor.
    const double fadeFrames =
        std::min(srcLen * 0.5, static_cast<double>(rds::kLoopSeamMs) * 0.001 * sr);
    const double period = std::max(1.0, srcLen - fadeFrames);
    bool wrapped = false;

    const auto read = [&](double at) {
        const std::size_t a = static_cast<std::size_t>(at);
        if (a + 1 >= src.size()) return src[src.size() - 1];
        const float f = static_cast<float>(at - static_cast<double>(a));
        return src[a] * (1.0f - f) + src[a + 1] * f;
    };

    const std::size_t start = static_cast<std::size_t>(std::max(0.0, points.front().timeSec) * sr);

    // The *first* stop ends the loop, not only a trailing one.
    //
    // A voice's points arrive in cue-list order, and the offline runner sorts
    // that list by time - so a stop whose timestamp is older than the updates
    // emitted before it does not come last. Testing `points.back()` alone then
    // read "no stop at all" and ran the loop to the end of the mix at whatever
    // gain it last held: a single grind droning under the remaining two minutes
    // of a take the engine had actually stopped. The engine no longer stamps a
    // stop in the past, and this no longer depends on it not doing so.
    //
    // Anything after the stop is dead by construction - the render walks frames
    // up to `end` and never reaches those points - so the first one is the whole
    // answer.
    std::size_t end = frames;
    float outFadeMs = 0.0f;
    for (const LoopPoint& point : points) {
        if (!point.stop) continue;
        end = std::min(frames, static_cast<std::size_t>(std::max(0.0, point.timeSec) * sr));
        outFadeMs = point.fadeMs;
        break;
    }
    if (end <= start) return;

    const int inFade = static_cast<int>(points.front().fadeMs * 0.001f * static_cast<float>(sr));
    const int outFade = static_cast<int>(outFadeMs * 0.001f * static_cast<float>(sr));

    // Ramp gain and pitch between control points rather than stepping: the game
    // fades an update over the cue's own fadeMs, and a step is a click. 20 ms
    // when the cue does not ask for anything.
    const int defaultRamp = std::max(1, sr / 50);
    std::size_t pi = 0;
    float gain = points[0].gain, pitch = points[0].pitch, gl = points[0].gl, gr = points[0].gr;
    float targetGain = gain, targetPitch = pitch, targetGl = gl, targetGr = gr;
    int rampLeft = 0;

    double pos = 0.0;
    for (std::size_t i = start; i < end; ++i) {
        const double now = static_cast<double>(i) / sr;
        while (pi + 1 < points.size() && points[pi + 1].timeSec <= now) {
            ++pi;
            if (!points[pi].stop) {
                targetGain = points[pi].gain;
                targetPitch = points[pi].pitch;
                targetGl = points[pi].gl;
                targetGr = points[pi].gr;
                rampLeft = points[pi].fadeMs > 0.0f
                               ? std::max(1, static_cast<int>(points[pi].fadeMs * 0.001f * sr))
                               : defaultRamp;
            }
        }
        if (rampLeft > 0) {
            const float a = 1.0f / static_cast<float>(rampLeft);
            gain += (targetGain - gain) * a;
            pitch += (targetPitch - pitch) * a;
            gl += (targetGl - gl) * a;
            gr += (targetGr - gr) * a;
            --rampLeft;
        } else {
            gain = targetGain;
            pitch = targetPitch;
            gl = targetGl;
            gr = targetGr;
        }

        float env = 1.0f;
        const int since = static_cast<int>(i - start);
        if (inFade > 0 && since < inFade) env *= static_cast<float>(since) / static_cast<float>(inFade);
        const int until = static_cast<int>(end - i);
        if (outFade > 0 && until < outFade) env *= static_cast<float>(until) / static_cast<float>(outFade);

        // Only after the first lap: the head of a loop is heard as it was
        // recorded the first time round, which is what the game's first tile does
        // too.
        float s = read(pos);
        if (wrapped && fadeFrames > 0.0 && pos < fadeFrames) {
            const float t = static_cast<float>(pos / fadeFrames);
            s = s * t + read(pos + period) * (1.0f - t);
        }

        mix[i * 2] += s * gain * env * gl;
        mix[i * 2 + 1] += s * gain * env * gr;

        pos += std::max(0.01f, pitch);
        while (pos >= period) {
            pos -= period;
            wrapped = true;
        }
    }
}

}  // namespace

// ── SoundSource ──────────────────────────────────────────────────────────────

namespace {

/// One decode, at a fixed channel count. Both public decoders are this with a
/// different channel count, and neither wants to own the ma_decoder lifetime.
bool DecodeFile(const std::string& path, int channels, int wantRate, std::vector<float>& out,
                int& sampleRate) {
    out.clear();
    ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_f32, static_cast<ma_uint32>(channels),
                               static_cast<ma_uint32>(std::max(0, wantRate)));
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
        return false;
    }
    ma_uint64 frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
    // Ten minutes. A cap rather than a check: an sfx that long is a mistake, and
    // reading it would be a gigabyte of float before anybody noticed.
    const ma_uint64 limit = static_cast<ma_uint64>(48000) * 60 * 10;
    bool ok = false;
    if (frames > 0 && frames < limit) {
        out.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&decoder, out.data(), frames, &read);
        out.resize(static_cast<std::size_t>(read) * static_cast<std::size_t>(channels));
        ok = read > 0;
    }
    sampleRate = static_cast<int>(decoder.outputSampleRate);
    ma_decoder_uninit(&decoder);
    return ok;
}

}  // namespace

bool DecodeMonoFile(const std::string& path, std::vector<float>& out, int& sampleRate) {
    // Native rate, not the mixer's: the analysis reports what the file *is*, and
    // resampling on the way in would have it report what we made of it.
    sampleRate = 0;
    return DecodeFile(path, 1, 0, out, sampleRate);
}

bool DecodeInterleaved(const std::string& path, int channels, std::vector<float>& out,
                       int& sampleRate) {
    sampleRate = 0;
    return DecodeFile(path, std::max(1, channels), 0, out, sampleRate);
}

void SoundSource::SetBank(const rds::SoundBank* bank, int sampleRate) {
    m_bank = bank;
    m_sampleRate = sampleRate;
    m_cache.clear();
}

const rds::PcmBuffer& SoundSource::Get(rds::SlotId slot, std::uint8_t variant) {
    // Ahead of the cache rather than in it: the audition is not keyed on a
    // variant, and putting it in the cache would mean invalidating a slot's
    // worth of entries every time the highlight moved one row.
    if (m_auditionSlot == static_cast<int>(slot) && !m_auditionSamples.Empty()) {
        return m_auditionSamples;
    }
    const std::uint32_t key = CacheKey(slot, variant);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second;

    rds::PcmBuffer e;
    e.sampleRate = m_sampleRate;
    rds::ResolvedSound resolved;
    // Get, never Resolve: Resolve advances the shuffle bag and would hand back a
    // different variant than the cue names, so the audio would not be the cue
    // list the arbitrator emitted.
    if (m_bank != nullptr && m_bank->Get(slot, variant, resolved) && !resolved.path.empty()) {
        int rate = 0;
        DecodeFile(resolved.path, 1, m_sampleRate, e.samples, rate);
    }
    // Empty when the slot has no recording, and cached that way on purpose: the
    // mix is then what the shipped mod plays, which is this layer missing. The
    // stand-in that used to fill it made the testbench the one place the mod
    // sounded complete.
    return m_cache.emplace(key, std::move(e)).first->second;
}

void SoundSource::SetAudition(rds::SlotId slot, const std::string& path) {
    if (m_auditionSlot == static_cast<int>(slot) && m_auditionPath == path) {
        return;
    }
    m_auditionSlot = static_cast<int>(slot);
    m_auditionPath = path;
    m_auditionSamples.samples.clear();
    m_auditionSamples.sampleRate = m_sampleRate;
    int rate = 0;
    if (!DecodeFile(path, 1, m_sampleRate, m_auditionSamples.samples, rate)) {
        // Nothing to hear rather than a stand-in: a file the decoder cannot read
        // is one the browser is already flagging, and synthesising something in
        // its place would have the audition answer for a sound that does not
        // exist.
        m_auditionSamples.samples.clear();
    }
}

void SoundSource::ClearAudition() {
    m_auditionSlot = -1;
    m_auditionPath.clear();
    m_auditionSamples.samples.clear();
}

void SoundSource::Invalidate() { m_cache.clear(); }

// ── the mix ──────────────────────────────────────────────────────────────────

MixedAudio MixCues(const std::vector<rds::Cue>& cues, double audioDurationMs,
                   const rds::ListenerState& listener, SoundSource& sources, int sampleRate,
                   float masterGainDb, bool limiter) {
    MixedAudio out;
    out.sampleRate = sampleRate;

    double durMs = audioDurationMs;
    for (const rds::Cue& c : cues) durMs = std::max(durMs, c.timeMs + 1200.0);
    durMs = std::clamp(durMs, 250.0, 300000.0);
    out.durationMs = durMs;

    const std::size_t frames = static_cast<std::size_t>(durMs * 0.001 * sampleRate) + 1;
    out.stereo.assign(frames * 2, 0.0f);

    const float master = DbToLin(masterGainDb);

    // One-shots are grouped, never played one at a time. `Engine::Emit` stamps
    // every layer of a proposal with the same (actorId, sourceSeq) inside one
    // tick, which is the composite id `GameRenderer` groups on; the damage layers
    // are a second group inside the same moment because the game opens them on
    // their own sound category. Both halves take the moment's earliest cue as
    // frame zero, so the split moves no timing.
    struct GroupKey {
        rds::ActorId actorId{};
        std::uint32_t sourceSeq{};
        bool damage{};
        bool operator==(const GroupKey&) const = default;
    };
    const auto momentOf = [](const rds::Cue& c) {
        return (static_cast<std::uint64_t>(c.actorId) << 32) | c.sourceSeq;
    };
    std::vector<std::pair<GroupKey, std::vector<rds::Cue>>> groups;
    std::map<std::uint64_t, rds::TimeMs> bases;

    // Loops: gather each voiceId's control points, then render once so the
    // source phase carries across the updates.
    std::map<std::uint32_t, std::vector<LoopPoint>> loops;
    std::map<std::uint32_t, std::pair<rds::SlotId, std::uint8_t>> loopSlot;

    for (const rds::Cue& c : cues) {
        if (c.op == rds::CueOp::kPlayOneShot) {
            const GroupKey key{c.actorId, c.sourceSeq, rds::IsDamageLayer(c.reason)};
            const auto it = std::find_if(groups.begin(), groups.end(),
                                         [&](const auto& g) { return g.first == key; });
            if (it == groups.end()) {
                groups.emplace_back(key, std::vector<rds::Cue>{c});
            } else {
                it->second.push_back(c);
            }
            // The base spans both halves of the moment, so a crunch 20 ms behind
            // the transient keeps that 20 ms as leading silence in its own buffer
            // rather than starting at the head of it.
            const auto base = bases.find(momentOf(c));
            if (base == bases.end()) {
                bases.emplace(momentOf(c), c.timeMs);
            } else {
                base->second = std::min(base->second, c.timeMs);
            }
            continue;
        }

        float gl = 1.0f, gr = 1.0f;
        PanForPoint(c.position, c.boneIndex >= 0, listener, gl, gr);
        const float gain = DbToLin(c.gainDb) * master;

        switch (c.op) {
            case rds::CueOp::kPlayOneShot:
                break;  // grouped above
            case rds::CueOp::kStartLoop:
                loopSlot[c.voiceId] = {c.slot, c.variant};
                loops[c.voiceId].push_back({c.timeMs * 0.001, gain, c.pitch, gl, gr, false, c.fadeMs});
                break;
            case rds::CueOp::kUpdateLoop:
                if (loops.count(c.voiceId))
                    loops[c.voiceId].push_back({c.timeMs * 0.001, gain, c.pitch, gl, gr, false, c.fadeMs});
                break;
            case rds::CueOp::kStopLoop:
                if (loops.count(c.voiceId))
                    loops[c.voiceId].push_back({c.timeMs * 0.001, gain, c.pitch, gl, gr, true, c.fadeMs});
                break;
        }
    }

    // Each group into one buffer, through the game's own mixer. The audition level
    // goes on afterwards and never before: the soft clip inside `MixComposite` is
    // part of what the game builds, and folding a monitor level into it would move
    // the knee every time the slider did.
    rds::MixParams params;
    params.sampleRate = sampleRate;
    rds::MixBuffer mixed;
    for (const auto& [key, group] : groups) {
        const auto base = bases.find((static_cast<std::uint64_t>(key.actorId) << 32) |
                                     key.sourceSeq);
        const std::optional<rds::TimeMs> timeBase =
            base == bases.end() ? std::nullopt : std::optional<rds::TimeMs>{base->second};
        if (!rds::MixComposite(group, sources, params, mixed, timeBase) || mixed.Empty()) {
            continue;
        }

        // One point for the whole composite, because the game hangs one voice on
        // it. `MixComposite` picks the loudest cue's, which is the same rule the
        // renderer follows.
        float gl = 1.0f, gr = 1.0f;
        PanForPoint(mixed.position, mixed.boneIndex >= 0, listener, gl, gr);

        const auto start =
            static_cast<std::size_t>(std::max(0.0, mixed.startMs) * 0.001 * sampleRate);
        for (std::size_t i = 0; i < mixed.samples.size(); ++i) {
            const std::size_t f = start + i;
            if (f >= frames) break;
            const float s = mixed.samples[i] * master;
            out.stereo[f * 2] += s * gl;
            out.stereo[f * 2 + 1] += s * gr;
        }
    }

    for (auto& [voiceId, points] : loops) {
        auto it = loopSlot.find(voiceId);
        if (it == loopSlot.end()) continue;
        AddLoop(out.stereo, frames, sources.Get(it->second.first, it->second.second).samples, points,
                sampleRate);
    }

    // The references are compressed and limited; a soft clip at the end is
    // closer to what a game mix does than letting a stacked burst wrap.
    // The raw peak is kept and shown: the soft clip is a testbench convenience
    // and the game has no limiter, so a config that only sounds controlled here
    // is a config that will clip in Skyrim.
    float peak = 0.0f, raw = 0.0f;
    for (float& s : out.stereo) {
        raw = std::max(raw, std::fabs(s));
        if (limiter) s = std::tanh(s * 0.9f) * 1.111f;
        peak = std::max(peak, std::fabs(s));
    }
    out.peak = peak;
    out.rawPeak = raw;
    return out;
}

// ── the vanilla mix ──────────────────────────────────────────────────────────

namespace {

/// Which of a descriptor's files this row draws.
///
/// Vanilla draws uniformly at the moment of the play and does not tell anyone
/// which it drew (VanillaTrack.h). So this draws too - from the seed and the
/// row, which makes it repeatable across runs and across machines without
/// pretending to be the draw that actually happened. A hash rather than a
/// running counter, so trimming the take's front does not reshuffle its tail.
[[nodiscard]] std::size_t DrawIndex(std::uint32_t seed, std::uint32_t row, std::size_t count) {
    if (count <= 1) {
        return 0;
    }
    std::uint32_t h = seed * 2654435761u + row * 2246822519u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h % count;
}

/// One decoded file per path, for the length of one mix.
///
/// Local rather than a member of anything: the vanilla side is re-mixed when the
/// checkbox moves and not per frame, a take touches a handful of distinct files,
/// and a cache that outlived the call would have to be invalidated when the root
/// changed - one more thing to get wrong for a saving nobody would measure.
class Decoded {
public:
    Decoded(int sampleRate) : m_sampleRate(sampleRate) {}

    const std::vector<float>& Get(const std::string& path) {
        const auto it = m_cache.find(path);
        if (it != m_cache.end()) {
            return it->second;
        }
        std::vector<float> samples;
        int rate = 0;
        // Empty on failure and cached that way, exactly as SoundSource does for a
        // slot with no recording: an xwm the decoder will not read is silence
        // here, and silence is the honest answer rather than a stand-in.
        if (!DecodeFile(path, 1, m_sampleRate, samples, rate)) {
            samples.clear();
        }
        return m_cache.emplace(path, std::move(samples)).first->second;
    }

private:
    int m_sampleRate;
    std::map<std::string, std::vector<float>> m_cache;
};

}  // namespace

MixedAudio MixVanilla(const std::vector<rds::FeedEvent>& track, double audioDurationMs,
                      const rds::ListenerState& listener, const VanillaLibrary& library,
                      int sampleRate, float masterGainDb, bool limiter, std::uint32_t seed,
                      std::size_t& played, std::size_t& misses) {
    played = 0;
    misses = 0;

    MixedAudio out;
    out.sampleRate = sampleRate;

    double durMs = audioDurationMs;
    for (const rds::FeedEvent& e : track) durMs = std::max(durMs, e.timeMs + 1200.0);
    durMs = std::clamp(durMs, 250.0, 300000.0);
    out.durationMs = durMs;

    const std::size_t frames = static_cast<std::size_t>(durMs * 0.001 * sampleRate) + 1;
    out.stereo.assign(frames * 2, 0.0f);

    const float master = DbToLin(masterGainDb);
    Decoded decoded{sampleRate};
    std::uint32_t row = 0;

    for (const rds::FeedEvent& event : track) {
        if (event.kind != rds::EventKind::kVanillaSound) continue;
        const std::uint32_t thisRow = row++;
        // A row with no name is a miss, not a skip. The game could not name the
        // descriptor (VanillaSoundFlag::kNameMissing), so nothing can find its
        // file - and counting it as anything other than a miss would report the
        // vanilla side as complete while it was playing half the take.
        if (event.text[0] == '\0') {
            ++misses;
            continue;
        }

        const std::vector<std::string>& files = library.Files(event.text);
        if (files.empty()) {
            ++misses;
            continue;
        }

        const std::vector<float>& src =
            decoded.Get(files[DrawIndex(seed, thisRow, files.size())]);
        if (src.empty()) {
            ++misses;
            continue;
        }

        // Static attenuation only, and it is an *attenuation*: the CK field is
        // how much quieter the descriptor is, so it subtracts. Everything else
        // vanilla would do to this voice - the output model's distance curve, the
        // acoustic space, the dB roll - is either the game's to apply or is not
        // knowable, and is left out rather than approximated.
        const float gain = DbToLin(-rds::vanilla::AttenuationDb(event.vanilla)) * master;

        float gl = 1.0f, gr = 1.0f;
        // Never centred: a vanilla row has no bone to be attached to.
        PanForPoint(event.position, false, listener, gl, gr);
        AddOneShot(out.stereo, frames, src, event.timeMs * 0.001, sampleRate, 1.0f, gain, gl, gr,
                   0.0f);
        ++played;
    }

    float peak = 0.0f, raw = 0.0f;
    for (float& s : out.stereo) {
        raw = std::max(raw, std::fabs(s));
        if (limiter) s = std::tanh(s * 0.9f) * 1.111f;
        peak = std::max(peak, std::fabs(s));
    }
    out.peak = peak;
    out.rawPeak = raw;
    return out;
}

}  // namespace tb
