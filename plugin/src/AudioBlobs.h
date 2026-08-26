#pragma once

// Handing the engine audio we mixed ourselves, with no file on disk.
//
// The design assumed a voice per layer played off disk, and that cannot produce
// the composite it is built on: at 60 fps a frame is 16.7 ms and the layer stack
// is transient +0 / surface +8 / body +20 / sub +65, so the first two land on the
// same frame. Mixing the stack ourselves fixes the timing, but then there is no
// file to point `GetSoundHandleByFile` at.
//
// The way through is vanilla's own: `BSAudioInit` carries an
// `BSExternalAudioIO::ExternalIOInterface*`, and flag 0x8000 on `GetSoundHandle`
// resolves a resource id through that interface instead of through the file
// system. Vanilla installs one for lip-synced FUZ voices; we chain onto it under
// our own directory hash and serve a buffer. Everything downstream is unchanged -
// the output model, the reverb send, distance falloff, VR spatialisation and the
// category slider all still apply, because as far as the engine is concerned this
// is an ordinary BSGameSound.
//
// Worked out and proven in SkyrimNet: skse/SkyrimNet/src/audio/EngineSoundPlayer.cpp
// and its docs/Engine Audio RE Notes/. This is the same mechanism with the parts
// we do not need - three sound categories, bard buses, the output-model
// correction loop - left out.
//
// Two hazards, both load-bearing:
//
//   - The engine keeps a raw pointer to the blob and reads the bytes IN PLACE for
//     the whole playback. The Registration must never move and the bytes must not
//     be freed until the engine has let go, which is a refcount plus a grace
//     window, not just a refcount.
//   - The interface slot is shared. Vanilla is in it, SkyrimNet may already have
//     chained onto vanilla, and we may be either side of that. Delegate anything
//     that is not ours, never decline it, and hold the install lock across the
//     whole read-modify-write.

#include <cstdint>
#include <span>

namespace rds::game {

/// Layout the engine expects back from `ExternalIOInterface::Open`. Mirrored from
/// SkyrimNet's RE of `BSXAudio2DataSrc::OpenFromExternalBlob` (AE 0x140CC1E70,
/// SE 0x140BFFDA0, VR 0x140C3AC20).
///
/// `data` must stay alive and unmoved for the whole playback: XAudio2 reads it
/// where it lies.
struct ExternalAudioBlob {
    volatile std::int32_t refCount;  // 00 - producer pre-increments, engine decrements on close
    std::uint32_t size;              // 04
    std::uint64_t reserved;          // 08 - unread by the audio path
    const std::uint8_t* data;        // 10 - complete RIFF/WAVE PCM container
};
static_assert(sizeof(ExternalAudioBlob) == 0x18);

/// In-memory audio the engine can open. One instance, installed once after data
/// load and never removed - the engine holds the interface pointer for the
/// process lifetime, exactly as vanilla's own static does.
class BlobRegistry {
public:
    static BlobRegistry& Get();

    /// Chain our interface into `BSAudioManager::initSettings.externalAudioIO`.
    /// Safe to call more than once; re-checks the slot rather than trusting a
    /// flag, because a rebuilt `BSAudioInit` would silently restore vanilla's.
    bool Install();
    [[nodiscard]] bool Installed() const;

    /// Copy `wav` into a registration and hand back the token plus the resource
    /// id to open it with. 0 means it was refused.
    ///
    /// A copy rather than a move, so the slot's buffer keeps its capacity across
    /// registrations: a knockdown registers a composite every few frames and the
    /// steady path must not allocate.
    std::uint64_t Register(std::span<const std::uint8_t> wav, std::uint32_t& fileIdOut,
                           std::uint32_t& dirIdOut);

    /// We are done with this blob. The bytes are freed later, once the engine has
    /// dropped its reference and the grace window has passed.
    void Retire(std::uint64_t token);

    /// Reclaim what the engine has let go. Called from the frame hook; retiring
    /// also collects, so this only matters when nothing is being retired.
    void Collect();

    /// Serve a registered blob. Runs on the audio manager thread. Public only
    /// because the interface implementation lives in the .cpp's anonymous
    /// namespace.
    std::uint32_t Resolve(std::uint32_t fileId, std::uint32_t dirId, ExternalAudioBlob** out);

    /// The directory hash every resource id of ours carries, so a foreign id can
    /// be delegated rather than swallowed. 'RDSX', for the same reason SkyrimNet
    /// uses 'SNET'.
    static constexpr std::uint32_t kDirId = 0x52445358;

    /// How many registrations are live, and how many bytes they hold. For the
    /// one-line status the log prints when a knockdown ends.
    void Stats(std::size_t& liveOut, std::size_t& bytesOut) const;

private:
    BlobRegistry() = default;

    struct Impl;
    [[nodiscard]] Impl& Storage();
    [[nodiscard]] const Impl& Storage() const;
};

/// The output model every voice of ours is given.
///
/// LOAD-BEARING. A sound opened without one is a flat 2D voice that follows the
/// listener round the room: no distance falloff, no reverb send, no VR
/// spatialisation. A null model is worse still than a dry one, because with
/// nothing to consult the engine loses the mono channel handling a model
/// declares. Resolved once and cached.
///
/// This is also why the engine applies no rolloff of its own and `DistanceConfig`
/// has no gain term: falloff is the game's, through this model. Doing it in both
/// places attenuated every cue twice.
[[nodiscard]] const RE::BSISoundOutputModel* DefaultOutputModel();

/// Which volume slider a voice plays under.
///
/// Two, because "turn the ragdoll sounds down" and "turn the wet ones down" are
/// different asks and a single slider can only answer the first. They are not
/// independent: `kGore` is a child category of `kMain`, so the ragdoll slider
/// still governs gore and the gore slider only trims within it.
enum class SoundBus : std::uint8_t {
    /// Everything the mod plays: impacts, taps, head accents, scrapes, rustle.
    kMain,
    /// The crunch and gore layers alone, so they can be taken out without taking
    /// the impacts with them.
    kGore,
};

/// The sound category a bus plays on, or null if `RagdollSounds.esp` is not in
/// the load order.
///
/// The category is the volume bus, and it is the ONLY level control on a sound
/// the engine applies continuously - everything else is a message that can be
/// dropped before the source voice exists. Assigning one is what puts a slider
/// on the Audio settings page in front of our audio; see
/// tools/make_categories_esp.py for the records and why they nest.
///
/// Null is survivable and is the pre-plugin behaviour: an uncategorised sound
/// plays at the level we mixed it at, with no slider over it. So a missing esp
/// costs the sliders and nothing else.
///
/// Retried rather than cached-on-first-call, and that distinction is load
/// bearing: a magic static that runs before the load order is up pins null for
/// the whole session. SkyrimNet shipped that bug and the note in its
/// EngineSoundPlayer.cpp is where this pattern comes from.
[[nodiscard]] const RE::BSISoundCategory* CategoryFor(SoundBus bus);

/// The model a tap composite is given instead, when `iTapOutputModelFormID` names
/// one.
///
/// Falls back to `DefaultOutputModel()` when the key is 0 or names something this
/// load order does not have, so the split can never be the reason a voice opens
/// with no model at all. Resolved once and cached the same way.
[[nodiscard]] const RE::BSISoundOutputModel* TapOutputModel();

}  // namespace rds::game
