#pragma once
#include <cstdint>
#include <functional>

namespace LevelUpTick {

// Called when the protagonist (Nahobino) gains one or more levels.
// oldLevel/newLevel are the protagonist's level immediately before/after the
// gain. levelsGained = newLevel - oldLevel.
using LevelUpCallback = std::function<void(int32_t oldLevel, int32_t newLevel)>;

// Install hooks on BPL_PartyData:AddPlayerExp for event-driven level detection.
// Call once from on_unreal_init.
void Setup();

// Subscribe to protagonist level-up events.
void OnLevelUp(LevelUpCallback cb);

// Re-establishes the level baseline (call on save load/unload so a higher-level
// save doesn't register as a spurious level-up).
void Reset();

}
