#pragma once
#include <cstdint>
#include <functional>

namespace GardenHauntHooks {

// Fired when a demon in the Haunt gives an item gift
// devilLevel: the demon's level
// chosenItemId: the item ID of the gift
// chosenItemNum: the quantity
using GardenGiftCallback = std::function<void(std::int32_t devilLevel, std::int32_t chosenItemId, std::int32_t chosenItemNum)>;

// Fired when a demon in the Haunt gives a stat power-up
// nkmIndex: the demon's NKM index (compendium ID)
using GardenPowerUpCallback = std::function<void(std::int32_t nkmIndex)>;

void Setup();
void OnGardenGift(GardenGiftCallback cb);
void OnGardenPowerUp(GardenPowerUpCallback cb);

} // namespace GardenHauntHooks
