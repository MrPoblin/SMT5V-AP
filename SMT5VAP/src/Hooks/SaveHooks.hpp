#pragma once
#include <cstdint>
#include <functional>

namespace SaveHooks {

// Fired when the player saves the game
// Parameters: saveSlotIndex, isInheritSave
using SaveCallback = std::function<void(int32_t saveSlotIndex, bool isInheritSave)>;

void Setup();
void OnGameSaved(SaveCallback cb);

} // namespace SaveHooks
