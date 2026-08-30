#include "BattleState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace BattleState {

// m_Step is E_BTL_STEP at 0x260 on ABattleMainWorkBase (Dumped Headers
// Project.hpp line 5317).
static constexpr int32_t kStepOffset = 0x260;

UObject* GetBattleMain() {
    std::vector<UObject*> mains;
    UObjectGlobals::FindAllOf(STR("BattleMainWorkBase"), mains);
    for (auto* m : mains) {
        if (m && m->GetWorld()) return m;
    }
    return nullptr;
}

uint8_t GetStep() {
    auto* bm = GetBattleMain();
    if (!bm) return kStepNone;
    return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(bm) + kStepOffset);
}

bool IsBattleRunning() {
    uint8_t step = GetStep();
    // A battle is "running" from its READY step up to (but not including)
    // END. Steps >= END (RESULT/GAMEOVER/END/FINISH_WAIT) mean the battle
    // is finishing or has finished — a new summon must be allowed then.
    return step >= kStepReady && step < kStepEnd;
}

} // namespace BattleState
