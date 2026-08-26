#pragma once

// The once-per-frame call on the game thread, and the engine's own frame delta.
//
// Everything in the engine is driven by Tick, and every window in it scales
// against the real frame time (07 section 4), so this is the piece without which
// the mod is inert - which is exactly what it was until now.
//
// It is a trampoline hook and NOT SKSE::GetTaskInterface, for the reason
// VR-Skin-Overlay-Menu's FrameHook.h sets out at length: SKSE's drain loop keeps
// popping until the queue is empty, so a task added from inside a task runs in
// the same frame. A tick that queues the next tick would never give the frame
// back, and the game would stop.
//
// The call site is the same main update VR Editor, VR-Skin-Overlay-Menu and the
// Leash framework all hang their frame work off. Addresses are in REL::VariantID
// form with the VR offset spelled out, because RELOCATION_ID report_and_fails on
// a VR miss rather than degrading.

#include <cstdint>

namespace rds::game {

class FrameHook {
public:
    /// Write the hook. Safe to call twice; the second call is a no-op. False if
    /// the call site could not be verified, in which case nothing is patched and
    /// the caller should say so rather than pretending to run.
    ///
    /// The caller must have allocated the trampoline already - see plugin.cpp.
    /// Allocating it here would release the block VanillaImpactHook is using.
    static bool Install(void (*onFrame)());

    [[nodiscard]] static bool Installed();

    /// The engine's own frame delta in seconds, or 0 before the first frame.
    ///
    /// Read from the game's global rather than differenced from a steady_clock:
    /// this is the number the engine itself stepped physics with, so a window
    /// scaled against it lines up with the frame the contacts came from. It also
    /// stays honest when the game is paused, which a wall clock does not.
    [[nodiscard]] static float DeltaSeconds();

private:
    static void OnMainThreadUpdate();

    static inline REL::Relocation<decltype(OnMainThreadUpdate)> s_original;
    static inline void (*s_onFrame)() = nullptr;
    static inline bool s_installed = false;
};

}  // namespace rds::game
