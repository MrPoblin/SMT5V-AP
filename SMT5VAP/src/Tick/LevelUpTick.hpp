#pragma once
#include <cstdint>
#include <functional>

namespace LevelUpTick {

// Called when the protagonist (Nahobino) gains one or more levels.
// oldLevel/newLevel are the protagonist's level immediately before/after the
// gain. levelsGained = newLevel - oldLevel.
using LevelUpCallback = std::function<void(int32_t oldLevel, int32_t newLevel)>;

// Per-frame level check. Call from dllmain::on_update; internally throttled
// to run at most once per PollInterval seconds of real time (framerate
// independent).
void Poll();

// Set the poll interval in seconds (framerate independent throttle). Default 1.0.
void SetPollInterval(double seconds);

// Subscribe to protagonist level-up events.
void OnLevelUp(LevelUpCallback cb);

// Re-establishes the level baseline (call on save load/unload so a higher-level
// save doesn't register as a spurious level-up).
void Reset();

}
