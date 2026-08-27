#pragma once

// One canned impact, on a key.
//
// The renderer has to be provable on its own: without this the first thing that
// can make a sound is the whole contact pipeline, so a silent mod would have half
// a dozen candidate causes instead of one.
//
// Off by default: iTestCueKey = 0 in RagdollSounds.ini.

#include <cstdint>

namespace rds { class Engine; }

namespace rds::game {

class GameRenderer;

/// Listen for the configured scancode. Does nothing when the key is 0.
void InstallTestCue(GameRenderer* renderer, std::int32_t scanCode);

/// Feed one composite - transient, surface, body, sub, at the design's own offsets
/// - straight into the renderer at the player's feet. Public so it can be called
/// from anywhere while working on the renderer.
void FireTestCue(GameRenderer& renderer, double nowMs);

/// True once per keypress. The sink only raises a flag: it does not run on the
/// game thread's frame, and everything the cue touches wants reading from the tick.
[[nodiscard]] bool TakeTestCueRequest();

}  // namespace rds::game
