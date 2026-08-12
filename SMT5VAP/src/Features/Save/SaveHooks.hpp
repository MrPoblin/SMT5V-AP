#pragma once
#include <cstdint>
#include <functional>

namespace SaveHooks {

// Fired when a save has FULLY completed writing to disk (polled via
// Project.SaveLoadBase.IsEndDataSave). Parameters: saveSlotIndex, isInheritSave
using SaveCallback = std::function<void(int32_t saveSlotIndex, bool isInheritSave)>;

void Setup();
// Safety fallback: clears a stuck pending-save flag if completion is never
// detected. Call every on_update.
void Tick();
void OnGameSaved(SaveCallback cb);

// True while a save is in progress and completion has not been detected yet.
bool IsSavePending();

} // namespace SaveHooks
