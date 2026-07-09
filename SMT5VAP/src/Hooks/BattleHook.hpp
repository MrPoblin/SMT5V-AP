#pragma once
#include <cstdint>
#include <functional>

namespace BattleHook {

using VictoryCallback = std::function<void(int32_t encounterId, int32_t eventEncounterId, bool isBoss)>;

void Setup();
void OnVictory(VictoryCallback cb);
void SetSuppressItems(bool suppress);

}
