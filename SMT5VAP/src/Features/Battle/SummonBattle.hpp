#pragma once
#include <cstdint>
#include <vector>

namespace SummonBattle {

// Setup hook called once at mod init. Resolves the MapEventData library
// functions/CDO used for row discovery, plus the interface UFunction for
// BPI_CallEventEncount (lazily, on first use). Safe to call multiple times.
void Setup();

// Spawns a battle through the game's NATURAL encounter pipeline (the
// MapEvent / scripted-encounter path), not the debug spawn entry. This is
// the "full-fidelity" summon: the engine runs the same event-encounter
// setup it uses for scripted fights, so models, BGM, camera and the
// MapEvent context are all driven correctly.
//
// eventEncountId — row id in the MapEventData table (NOT an EncountData
// row). The MapEvent row carries the encounter's enemy composition,
// flags, and battle setup. If 0 / auto, the module discovers the best
// usable MapEvent row for the current map on its own and uses that.
//
// This function ONLY dispatches the encounter. It does not touch the
// resulting battle in any way. If you want to force a defeat, do that
// through DeathFunctions::KillLocalPlayer after the battle comes up.
__declspec(noinline) void SummonEvent(int32_t eventEncountId = 0);

// Convenience entry that routes through the full MapEvent pipeline. The
// encountID is used only as a hint: when it also identifies a usable
// MapEvent row (and no explicit mapEventId override is given), that row is
// used; otherwise an auto-discovered row is used.
//
// Kept for source compatibility with the old debug-path callers.
__declspec(noinline) void Summon(int32_t encountID, const std::vector<int32_t>& enemyList = {});

// Diagnostic: true if the last Summon/SummonEvent call successfully
// dispatched the encounter request. The battle itself is owned by the
// engine and may or may not come up depending on map state.
bool IsActive();

} // namespace SummonBattle
