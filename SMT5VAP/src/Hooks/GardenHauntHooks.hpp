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

// When enabled, item gifts from haunt/garden talks are suppressed (the actual
// grant via BPL_ItemData::ItemGet is blocked; power-ups are unaffected).
void SetSuppressGifts(bool suppress);

// Called by ItemBlocker's BPL_ItemData::ItemGet pre-hook. Returns true while
// suppression is enabled AND a PickItemReward pick is pending (the actual grant
// fires after PickItemReward returns and the context is cleared once it is
// intercepted). No time limit, so a long dialogue can't let the gift through.
// It is purely context-based — never keyed off the item ID — so it cannot
// affect other sources.
bool IsSuppressingGardenGiftNow();

// Disarm the garden-gift context immediately (called by ItemBlocker once it has
// blocked the grant), so the armed flag cannot over-block later grants.
void ClearGardenGiftContext();

} // namespace GardenHauntHooks
