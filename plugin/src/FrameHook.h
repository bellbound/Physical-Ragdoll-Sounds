#pragma once

// The once-per-frame call on the game thread, and the engine's own frame delta.
// Everything in the engine is driven by Tick and every window scales against the
// real frame time (07 §4), so without this the mod is inert.
//
// A trampoline hook and NOT SKSE::GetTaskInterface: SKSE's drain loop keeps
// popping until the queue is empty, so a task added from inside a task runs in the
// same frame - a tick that queued the next tick would never give the frame back.
//
// The call site is the same main update VR Editor, VR-Skin-Overlay-Menu and the
// Leash framework hang their frame work off. Addresses are REL::VariantID with the
// VR offset spelled out, because RELOCATION_ID report_and_fails on a VR miss.

#include <cstdint>

namespace rds::game {

class FrameHook {
public:
    /// Write the hook. Safe to call twice. False if the call site could not be
    /// verified, in which case nothing is patched and the caller should say so.
    ///
    /// The caller must have allocated the trampoline already - allocating it here
    /// would release the block VanillaImpactHook is using.
    static bool Install(void (*onFrame)());

    [[nodiscard]] static bool Installed();

    /// The engine's own frame delta in seconds, or 0 before the first frame. Read
    /// from the game's global rather than differenced from a steady_clock: it is
    /// what the engine stepped physics with, so a window scaled against it lines up
    /// with the frame the contacts came from - and it stays honest when paused.
    [[nodiscard]] static float DeltaSeconds();

private:
    static void OnMainThreadUpdate();

    static inline REL::Relocation<decltype(OnMainThreadUpdate)> s_original;
    static inline void (*s_onFrame)() = nullptr;
    static inline bool s_installed = false;
};

}  // namespace rds::game
