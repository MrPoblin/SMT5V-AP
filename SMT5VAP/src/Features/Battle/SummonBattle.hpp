#pragma once
#include <cstdint>

namespace SummonBattle {

// Setup hook called once at mod init. Resolves the MapEventData library
// functions/CDO used for row discovery, plus the interface UFunction for
// the event encounter (lazily, on first use). Safe to call multiple times.
void Setup();

// Spawns a battle with Event Encounter ID.
// (BPI_CallEventEncount_ForUniqueSymbol, keyed by a MapEventData row).
// The engine runs the same event-encounter setup it uses for scripted
// fights, so models, BGM, camera and the MapEvent context are all driven
// correctly — a fully normal battle.
__declspec(noinline) void Summon(int32_t mapEventId = 0);

// Diagnostic: true if the last Summon call successfully dispatched the
// encounter request. The battle itself is owned by the engine and may or
// may not come up depending on map state.
bool IsActive();

} // namespace SummonBattle
