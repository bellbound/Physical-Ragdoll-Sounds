#include "rds/Pcm.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string_view>

#include "rds/Synth.h"

namespace rds {
namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

/// Where a data chunk starts and how long it is, alongside the format that
/// describes it. Everything both ProbeWav and ReadWavMono need out of one walk
/// of the file.
struct WavLayout {
    WavInfo info;
    std::uint64_t dataOffset{};
    std::uint32_t dataBytes{};
};

/// One pass over the chunk list.
///
/// The format and data chunks may arrive in either order and a file may carry
/// chunks we do not care about between them - LIST, fact, the cue a DAW leaves
/// behind - so this reads to the end rather than stopping at the first data
/// chunk. Odd-sized chunks are followed by a pad byte, which is the detail that
/// silently desynchronises a naive walk halfway through a file.
[[nodiscard]] bool ReadLayout(std::istream& file, WavLayout& out) {
    char riff[12]{};
    file.read(riff, sizeof(riff));
    if (!file || std::string_view{riff, 4} != "RIFF" || std::string_view{riff + 8, 4} != "WAVE") {
        return false;
    }

    bool haveFormat = false;
    bool haveData = false;

    while (file) {
        char id[4]{};
        std::uint32_t size = 0;
        file.read(id, sizeof(id));
        file.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (!file) {
            break;
        }
        const std::string_view chunk{id, 4};
        const std::streamoff advance = static_cast<std::streamoff>(size) + (size & 1u);

        if (chunk == "fmt " && size >= 16) {
            char header[16]{};
            file.read(header, sizeof(header));
            if (!file) {
                break;
            }
            std::uint16_t format = 0;
            std::uint16_t channels = 0;
            std::uint32_t sampleRate = 0;
            std::uint16_t bits = 0;
            std::memcpy(&format, header + 0, sizeof(format));
            std::memcpy(&channels, header + 2, sizeof(channels));
            std::memcpy(&sampleRate, header + 4, sizeof(sampleRate));
            std::memcpy(&bits, header + 14, sizeof(bits));

            // WAVE_FORMAT_EXTENSIBLE hides the real format in a GUID whose first
            // two bytes are the tag it stands in for. Anything that writes 24-bit
            // uses it, so this is not an exotic case.
            if (format == kFormatExtensible && size >= 40) {
                char extension[24]{};
                file.read(extension, sizeof(extension));
                if (file) {
                    std::memcpy(&format, extension + 8, sizeof(format));
                }
                file.seekg(static_cast<std::streamoff>(size) - 40, std::ios::cur);
            } else if (size > 16) {
                file.seekg(static_cast<std::streamoff>(size) - 16, std::ios::cur);
            }
            if (size & 1u) {
                file.seekg(1, std::ios::cur);
            }

            out.info.sampleRate = static_cast<int>(sampleRate);
            out.info.channels = static_cast<int>(channels);
            out.info.bitsPerSample = static_cast<int>(bits);
            out.info.floatFormat = format == kFormatFloat;
            haveFormat = format == kFormatPcm || format == kFormatFloat;
            if (!haveFormat) {
                return false;
            }
        } else if (chunk == "data") {
            out.dataOffset = static_cast<std::uint64_t>(file.tellg());
            out.dataBytes = size;
            haveData = true;
            file.seekg(advance, std::ios::cur);
        } else {
            file.seekg(advance, std::ios::cur);
        }
    }

    if (!haveFormat || !haveData || out.info.channels <= 0 || out.info.sampleRate <= 0) {
        return false;
    }
    const std::uint32_t frameBytes = static_cast<std::uint32_t>(out.info.channels) *
                                     static_cast<std::uint32_t>(out.info.bitsPerSample / 8);
    if (frameBytes == 0) {
        return false;
    }
    out.info.frames = out.dataBytes / frameBytes;
    return true;
}

/// One sample, whatever width it arrived at, as float in [-1, 1].
[[nodiscard]] float DecodeSample(const std::uint8_t* p, int bits, bool isFloat) {
    if (isFloat) {
        if (bits == 32) {
            float value = 0.0f;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }
        if (bits == 64) {
            double value = 0.0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value);
        }
        return 0.0f;
    }
    switch (bits) {
        case 8:
            // 8-bit wav is unsigned, alone among the widths.
            return (static_cast<float>(*p) - 128.0f) / 128.0f;
        case 16: {
            std::int16_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value) / 32768.0f;
        }
        case 24: {
            const std::int32_t value =
                (static_cast<std::int32_t>(p[2]) << 24 | static_cast<std::int32_t>(p[1]) << 16 |
                 static_cast<std::int32_t>(p[0]) << 8) >>
                8;
            return static_cast<float>(value) / 8388608.0f;
        }
        case 32: {
            std::int32_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value) / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

}  // namespace

WavInfo ProbeWav(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    WavLayout layout;
    if (!ReadLayout(file, layout)) {
        return {};
    }
    return layout.info;
}

float WavLengthMs(const std::string& path) {
    const WavInfo info = ProbeWav(path);
    if (!info.Valid()) {
        return 0.0f;
    }
    return 1000.0f * static_cast<float>(info.frames) / static_cast<float>(info.sampleRate);
}

PcmBuffer ReadWavMono(const std::string& path, int targetRate) {
    PcmBuffer out;
    out.sampleRate = targetRate;
    if (targetRate <= 0) {
        return out;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return out;
    }
    WavLayout layout;
    if (!ReadLayout(file, layout) || layout.info.frames == 0) {
        return out;
    }

    const int channels = layout.info.channels;
    const int bits = layout.info.bitsPerSample;
    const std::size_t width = static_cast<std::size_t>(bits / 8);
    if (width == 0) {
        return out;
    }

    // ReadLayout walks to the end of the chunk list, so the stream is sitting on
    // eof and failbit by the time it returns. seekg does not clear failbit for
    // you - it is a no-op on a failed stream - and without this the read below
    // quietly returns nothing and every file in the pack falls back to its
    // stand-in with only a warn line to say so.
    file.clear();

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(layout.dataBytes));
    file.seekg(static_cast<std::streamoff>(layout.dataOffset), std::ios::beg);
    file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    const std::size_t bytesRead = static_cast<std::size_t>(std::max<std::streamsize>(0, file.gcount()));
    const std::size_t frameBytes = static_cast<std::size_t>(channels) * width;
    const std::size_t readFrames =
        std::min(static_cast<std::size_t>(layout.info.frames), bytesRead / frameBytes);
    if (readFrames == 0) {
        return out;
    }

    // Downmix first, resample second: averaging at the source rate costs one pass
    // over the real samples rather than one over the interpolated ones.
    std::vector<float> mono(readFrames);
    const float scale = 1.0f / static_cast<float>(channels);
    for (std::size_t frame = 0; frame < readFrames; ++frame) {
        float sum = 0.0f;
        const std::uint8_t* base = raw.data() + frame * frameBytes;
        for (int channel = 0; channel < channels; ++channel) {
            sum += DecodeSample(base + static_cast<std::size_t>(channel) * width, bits,
                                layout.info.floatFormat);
        }
        mono[frame] = sum * scale;
    }

    if (layout.info.sampleRate == targetRate) {
        out.samples = std::move(mono);
        return out;
    }

    const double ratio = static_cast<double>(targetRate) / static_cast<double>(layout.info.sampleRate);
    const std::size_t outFrames = static_cast<std::size_t>(static_cast<double>(mono.size()) * ratio);
    out.samples.resize(outFrames);
    for (std::size_t i = 0; i < outFrames; ++i) {
        const double source = static_cast<double>(i) / ratio;
        const std::size_t index = static_cast<std::size_t>(source);
        const float frac = static_cast<float>(source - static_cast<double>(index));
        const float a = index < mono.size() ? mono[index] : 0.0f;
        const float b = index + 1 < mono.size() ? mono[index + 1] : a;
        out.samples[i] = a + (b - a) * frac;
    }
    return out;
}

void EncodeWavPcm16Into(std::span<const float> mono, int sampleRate, std::vector<std::uint8_t>& out) {
    constexpr std::uint32_t kHeaderBytes = 44;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(mono.size() * sizeof(std::int16_t));

    // resize, not assign: a buffer that has already carried one composite keeps
    // its capacity, which is the whole point of the caller owning it.
    out.resize(kHeaderBytes + dataBytes);
    std::uint8_t* p = out.data();

    const auto put32 = [&p](std::uint32_t value) {
        std::memcpy(p, &value, sizeof(value));
        p += sizeof(value);
    };
    const auto put16 = [&p](std::uint16_t value) {
        std::memcpy(p, &value, sizeof(value));
        p += sizeof(value);
    };
    const auto putTag = [&p](const char* tag) {
        std::memcpy(p, tag, 4);
        p += 4;
    };

    putTag("RIFF");
    put32(36 + dataBytes);
    putTag("WAVE");
    putTag("fmt ");
    put32(16);
    put16(kFormatPcm);
    put16(1);  // mono; the engine spatialises, so a stereo image here would fight it
    put32(static_cast<std::uint32_t>(sampleRate));
    put32(static_cast<std::uint32_t>(sampleRate) * 2);  // byte rate
    put16(2);                                           // block align
    put16(16);
    putTag("data");
    put32(dataBytes);

    for (const float sample : mono) {
        const float clamped = std::clamp(sample, -1.0f, 1.0f);
        const auto value = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        std::memcpy(p, &value, sizeof(value));
        p += sizeof(value);
    }
}

std::vector<std::uint8_t> EncodeWavPcm16(std::span<const float> mono, int sampleRate) {
    std::vector<std::uint8_t> out;
    EncodeWavPcm16Into(mono, sampleRate, out);
    return out;
}

// -- PcmCache ---------------------------------------------------------------

std::uint32_t PcmCache::Key(SlotId slot, std::uint8_t variant) {
    return static_cast<std::uint32_t>(slot) << 8 | variant;
}

void PcmCache::SetBank(const SoundBank* bank, int sampleRate) {
    m_bank = bank;
    m_sampleRate = sampleRate > 0 ? sampleRate : 48000;
    m_entries.clear();
}

void PcmCache::Clear() { m_entries.clear(); }

const PcmBuffer& PcmCache::Get(SlotId slot, std::uint8_t variant) {
    const std::uint32_t key = Key(slot, variant);
    const auto it = std::ranges::find_if(m_entries, [key](const auto& e) { return e.first == key; });
    if (it != m_entries.end()) {
        return it->second;
    }

    PcmBuffer buffer;
    buffer.sampleRate = m_sampleRate;

    ResolvedSound sound{};
    if (m_bank != nullptr && m_bank->Get(slot, variant, sound)) {
        if (!sound.procedural && !sound.path.empty()) {
            buffer = ReadWavMono(sound.path, m_sampleRate);
            if (buffer.Empty()) {
                // The bank measured this file at load, so a failure here is the
                // file changing under us, or a format the length walk tolerated
                // and the sample walk did not. Say so once and stand in for it,
                // rather than dropping the layer and leaving a hole in the stack.
                spdlog::warn("pcm: {} could not be decoded; standing in for {} variant {}",
                             sound.path, ToString(slot), variant);
            }
        }
        if (buffer.Empty()) {
            const SynthBuffer synth = Synthesise(sound, static_cast<float>(m_sampleRate));
            buffer.samples = synth.samples;
            buffer.sampleRate = m_sampleRate;
            buffer.procedural = true;
        } else {
            buffer.procedural = false;
        }
    }

    m_entries.emplace_back(key, std::move(buffer));
    return m_entries.back().second;
}

}  // namespace rds
