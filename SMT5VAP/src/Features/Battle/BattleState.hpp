#pragma once
#include <cstdint>

namespace RC::Unreal {
class UObject;
}

// Shared battle-state helpers for the battle feature modules
// (SummonBattle, DeathFunctions). Centralizes the BattleMainWorkBase
// lookup and the "is a battle genuinely running right now" decision so
// both modules agree.
namespace BattleState {

// E_BTL_STEP values (Dumped Headers Project_enums.hpp line 2002).
enum : uint8_t {
    kStepNone = 0,        // E_BTL_STEP_NONE
    kStepReady = 2,       // E_BTL_STEP_READY
    kStepPre = 3,         // E_BTL_STEP_PRE
    kStepEnd = 30,        // E_BTL_STEP_END
    kStepFinishWait = 31, // E_BTL_STEP_FINISH_WAIT
    kStepGameOver = 29,   // E_BTL_STEP_GAMEOVER
};

// Live BattleMainWorkBase (world-filtered), or nullptr.
RC::Unreal::UObject* GetBattleMain();

// Current m_Step of the live BattleMainWorkBase, or kStepNone if none.
uint8_t GetStep();

// True while a battle is genuinely running right now: a live
// BattleMainWorkBase whose step is between READY (2) and END (30)
// exclusive. A battle that is tearing down (GAMEOVER / END / FINISH_WAIT)
// or in its pre-READY init does NOT count as running, so a summon can
// take over afterwards.
bool IsBattleRunning();

} // namespace BattleState
