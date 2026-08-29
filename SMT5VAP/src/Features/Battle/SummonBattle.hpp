#pragma once
#include <cstdint>
#include <vector>

namespace SummonBattle {

// Setup hook called once at mod init. The Summon() call goes
// through the BP VM string dispatcher, which doesn't need
// pre-resolved UFunction pointers; Setup() exists for parity with
// the other modules and to log that the module is active.
void Setup();

// Spawns an encounter at the current player position by invoking
// MapCommonCtrl_C::BPI_CallEncountForDebug through the BP VM
// (the same path the in-game debug menu uses). The engine then
// drives the normal EncountActor → BattleMain pipeline, so the
// resulting battle is indistinguishable from a natural one
// (same BGM hooks, same item/XP rewards, same save flow).
//
// Parameters:
//   encountID — row in the EncountData table. Pass a valid row for
//               the current map area; the engine will look up that
//               row's m_EnemyIDArray and spawn its enemies.
//   enemyList — optional devil-ID override. When non-empty, this
//               list is used INSTEAD of the table row's enemies.
//               When empty, the table row is used verbatim. In
//               practice passing an empty vector and a valid
//               encountID is enough — the engine will pull the
//               encounter's enemies from the table on its own.
//
// This function ONLY dispatches the spawn. It does not touch the
// resulting battle in any way. If you want to force a defeat, do
// that through DeathFunctions::KillLocalPlayer after the
// BattleMainWorkBase has come up. SummonBattle deliberately keeps
// the two responsibilities separate so callers can chain them
// however they like (e.g. spawn-only for tests, spawn-then-kill
// for death-link).
//
// Note: `int32_t` is used here instead of the UE typedef `int32`
// because the latter lives inside `RC::Unreal::FGenericPlatformTypes`
// and isn't visible at namespace scope in a header. The .cpp uses
// `int32_t` consistently too since we no longer have a need for
// the type-specific reflection machinery.
__declspec(noinline) void Summon(int32_t encountID, const std::vector<int32_t>& enemyList);

// Diagnostic: true if the last Summon() call successfully dispatched
// the BP VM call. The encounter itself is owned by the engine and
// may or may not come up depending on map state, so this is only
// useful for "did the call go out" sanity checks.
bool IsActive();

} // namespace SummonBattle
