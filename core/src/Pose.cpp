#include "rds/Pose.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace rds::pose {
namespace {

/// Everything is read and written through whole structs, which is safe only
/// because every one of them is padding-free - see the asserts in Pose.h. The
/// alternative, field-by-field memcpy, buys portability to a platform neither
/// half of this build has ever run on.
template <typename T>
void Append(std::vector<std::byte>& out, const T& value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] bool Take(const std::byte*& cursor, const std::byte* end, T& value) {
    if (static_cast<std::size_t>(end - cursor) < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

}  // namespace

bool Write(const std::filesystem::path& file, const std::vector<FeedEvent>& events, double originMs,
           double loMs, double hiMs, std::size_t& framesOut, std::string& error) {
    framesOut = 0;

    std::vector<std::byte> body;
    // 516 bytes a frame at sixty hertz. Reserving a second of it costs nothing
    // and saves the first dozen reallocations on every take.
    body.reserve(64 * 1024);

    std::uint32_t frames = 0;
    std::size_t index = 0;
    while (index < events.size()) {
        const FeedEvent& first = events[index];
        if (!IsTickSample(first) || first.timeMs < loMs || first.timeMs > hiMs) {
            ++index;
            continue;
        }

        // One tick publishes one contiguous run per actor, so a frame is the run
        // of samples sharing a timestamp and an actor. Interleaved actors would
        // merely split into more, smaller frames - still correct, just less
        // compact - which is why this groups rather than assuming.
        std::size_t end = index;
        while (end < events.size() && IsTickSample(events[end]) &&
               events[end].timeMs == first.timeMs && events[end].actorId == first.actorId) {
            ++end;
        }

        FrameHeader header{};
        header.timeMs = static_cast<float>(first.timeMs - originMs);
        header.actorId = first.actorId;
        header.limbCount = static_cast<std::uint16_t>(end - index);
        header.phase = static_cast<std::uint16_t>(first.phase);
        Append(body, header);

        for (std::size_t i = index; i < end; ++i) {
            const FeedEvent& event = events[i];
            LimbRecord record{};
            record.limbIndex = event.limbIndex;
            record.pos[0] = event.position.x;
            record.pos[1] = event.position.y;
            record.pos[2] = event.position.z;
            record.vel[0] = event.velocity.x;
            record.vel[1] = event.velocity.y;
            record.vel[2] = event.velocity.z;
            Append(body, record);
        }

        ++frames;
        index = end;
    }

    // No frames leaves no file. A take captured with sampling off should look
    // like a take from before pose existed, not like a corrupt one - and an
    // empty file would have to be special-cased by every reader instead.
    if (frames == 0) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        return true;
    }

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kVersion;
    header.frameCount = frames;
    header.limbStride = kLimbStride;
    header.originMs = originMs;

    std::ofstream out(file, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = std::format("cannot write {}", file.string());
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(body.data()),
              static_cast<std::streamsize>(body.size()));
    if (!out) {
        error = std::format("failed writing {}", file.string());
        return false;
    }

    framesOut = frames;
    return true;
}

bool Read(const std::filesystem::path& file, std::vector<FeedEvent>& out, std::size_t& framesOut,
          std::string& error) {
    framesOut = 0;

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        // Not an error: a take recorded before pose existed. The caller says so
        // as a warning, and the engine falls back to inferring air time from the
        // gaps between contacts.
        return true;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = std::format("cannot open {}", file.string());
        return false;
    }
    // Through a string rather than straight into a byte vector: char does not
    // convert to std::byte implicitly, and the cast belongs here once rather
    // than on every element.
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    const auto* cursor = reinterpret_cast<const std::byte*>(bytes.data());
    const std::byte* const end = cursor + bytes.size();

    Header header{};
    if (!Take(cursor, end, header)) {
        error = std::format("{} is too short to hold a pose header", file.string());
        return false;
    }
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        error = std::format("{} is not a pose file", file.string());
        return false;
    }
    if (header.version != kVersion) {
        error = std::format("{} is pose version {}, this build reads {}", file.string(),
                            header.version, kVersion);
        return false;
    }
    if (header.limbStride < sizeof(LimbRecord)) {
        error = std::format("{} has a {} byte limb record, smaller than the {} this build reads",
                            file.string(), header.limbStride, sizeof(LimbRecord));
        return false;
    }
    // A stride *larger* than ours is a later version's extra fields, which are
    // read past rather than refused - that is what the field is for.
    const std::size_t skipPerLimb = header.limbStride - sizeof(LimbRecord);

    for (std::uint32_t frame = 0; frame < header.frameCount; ++frame) {
        FrameHeader frameHeader{};
        if (!Take(cursor, end, frameHeader)) {
            error = std::format("{} ends inside frame {} of {}", file.string(), frame,
                                header.frameCount);
            return false;
        }
        for (std::uint16_t limb = 0; limb < frameHeader.limbCount; ++limb) {
            LimbRecord record{};
            if (!Take(cursor, end, record)) {
                error = std::format("{} ends inside frame {}", file.string(), frame);
                return false;
            }
            if (static_cast<std::size_t>(end - cursor) < skipPerLimb) {
                error = std::format("{} ends inside frame {}", file.string(), frame);
                return false;
            }
            cursor += skipPerLimb;

            FeedEvent event{};
            event.kind = EventKind::kLimbSample;
            event.timeMs = header.originMs + static_cast<double>(frameHeader.timeMs);
            event.actorId = frameHeader.actorId;
            event.limbIndex = record.limbIndex;
            event.phase = static_cast<ActorPhase>(frameHeader.phase);
            event.position = {record.pos[0], record.pos[1], record.pos[2]};
            event.velocity = {record.vel[0], record.vel[1], record.vel[2]};
            // Derived rather than stored, because it is exactly |velocity| and a
            // stored copy is one more thing that can disagree with it.
            event.bodySpeed = Length(event.velocity);
            out.push_back(event);
        }
        ++framesOut;
    }

    return true;
}

bool Probe(const std::filesystem::path& file, std::size_t& framesOut) {
    framesOut = 0;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    Header header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        return false;
    }
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion) {
        return false;
    }
    framesOut = header.frameCount;
    return header.frameCount > 0;
}

}  // namespace rds::pose
