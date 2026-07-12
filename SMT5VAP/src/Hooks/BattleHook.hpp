#pragma once
#include <cstdint>
#include <functional>

namespace BattleHook {

using VictoryCallback = std::function<void(int32_t encounterId, int32_t eventEncounterId, bool isBoss)>;
using AllyDownedCallback = std::function<void(int32_t partyIndex, int32_t newHP, int32_t heroIndex)>;

void Setup();
void OnVictory(VictoryCallback cb);
void OnAllyDowned(AllyDownedCallback cb);
void SetSuppressItems(bool suppress);

}
