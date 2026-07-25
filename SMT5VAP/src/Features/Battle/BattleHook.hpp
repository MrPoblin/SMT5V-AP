#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace BattleHook {

// Reports, for the just-finished battle, the devil IDs of every enemy in the
// encounter and of every enemy actually defeated (from
// ABattleMainWorkBase::m_EnemyKillList). The consumer decides what counts
// (e.g. filter Mitamas via IsMitamaDevil).
using VictoryCallback = std::function<void(int32_t encounterId, int32_t eventEncounterId, bool isBoss, const std::vector<int32_t>& defeatedEnemyIds, const std::vector<int32_t>& encounteredEnemyIds)>;
using AllyDownedCallback = std::function<void(int32_t partyIndex, int32_t newHP, int32_t heroIndex)>;

void Setup();
void OnVictory(VictoryCallback cb);
void OnAllyDowned(AllyDownedCallback cb);
void SetSuppressItems(bool suppress);

// True if the given devil belongs to the Mitama group (E_GROUP_ID_MITAMA).
bool IsMitamaDevil(int32_t devilId);

// Suppress ONLY items granted by Mitama battles (enemy group == MITAMA).
// The grant is handled by the shared result-data suppression, so multiple
// items per battle are covered.
void SetSuppressMitamaItems(bool suppress);

}
