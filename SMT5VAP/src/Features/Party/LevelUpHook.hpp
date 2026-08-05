#pragma once
#include <cstdint>
#include <functional>

namespace LevelUpTick {

// Called when the protagonist (Nahobino) gains one or more levels.
// oldLevel/newLevel are the protagonist's level immediately before/after the
// gain. levelsGained = newLevel - oldLevel.
using LevelUpCallback = std::function<void(int32_t oldLevel, int32_t newLevel)>;

// Protagonist (Nahobino) level-up detection.
//
// The protagonist's level-up is applied through a native C++ path that bypasses
// UFunction exec hooks, so a native inline hook (PLH::x64Detour) is installed on
// FUN_147248ad0 — the level-up core that every source (battle exp, exp items,
// quest rewards) funnels through. The hook reads the resulting level directly
// from party data (no ProcessEvent) and fires the callback immediately. No
// per-frame tick is involved.
void Setup();

// Subscribe to protagonist level-up events.
void OnLevelUp(LevelUpCallback cb);

// Re-establishes the level baseline (call on save load/unload so a higher-level
// save doesn't register as a spurious level-up).
void Reset();

// Cached protagonist level (-1 if the baseline hasn't been established yet).
int32_t GetCachedLevel();

// Most recent detected level-up bounds (-1 if none yet). For debug display.
int32_t LastLevelUpOld();
int32_t LastLevelUpNew();

}
