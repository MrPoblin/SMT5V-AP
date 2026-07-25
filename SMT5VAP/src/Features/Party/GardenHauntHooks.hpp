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

// Called by ItemBlocker's BPL_ItemData::ItemGet pre-hook. Returns true only when
// suppression is enabled, a PickItemReward pick is armed, AND the player is
// currently in a Demon Haunt / Garden level (BPI_GameMode::IsInGardenLevel).
// The in-haunt gate is extra safety so a stuck context can never block items
// outside the haunt.
bool IsSuppressingGardenGiftNow();

// True while the player is in a Demon Haunt / Garden level.
bool IsInGardenLevel();

// Called by ItemBlocker when it actually blocks a haunt gift grant. Reports the
// REAL item id/num (captured from the BPL_ItemData::ItemGet call itself) to the
// Archipelago callback together with the demon level captured at PickItemReward,
// then disarms the context so it cannot over-block later grants.
void CaptureGiftGrant(int32_t itemId, int32_t itemNum);

// Disarm the garden-gift context immediately (called by CaptureGiftGrant once it
// has reported the grant), so the armed flag cannot over-block later grants.
void ClearGardenGiftContext();

} // namespace GardenHauntHooks
