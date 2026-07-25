#include "DeathFunctions.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/CallbackIterationData.hpp>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace DeathFunctions {

// Death-link kill: force SMTVV's real defeat step (E_BTL_STEP_GAMEOVER).
// Work is deferred to the engine-tick PRE callback so it never runs mid
// engine-tick (which would corrupt the battle state machine).
static bool s_PendingKill{false};

static UObject* GetActivePartyComponent() {
    std::vector<UObject*> objs;
    UObjectGlobals::FindAllOf(STR("BattlePartySystemComponentBase"), objs);
    for (int32 i = static_cast<int32>(objs.size()) - 1; i >= 0; --i) {
        UObject* o = objs[i];
        if (o && o->GetWorld()) return o;
    }
    return nullptr;
}

static UObject* GetBattleMainActor() {
    std::vector<UObject*> mains;
    UObjectGlobals::FindAllOf(STR("BattleMainWorkBase"), mains);
    for (auto* m : mains) {
        if (m && m->GetWorld()) return m;
    }
    return nullptr;
}

static void TriggerGameOverNow() {
    auto* comp = GetActivePartyComponent();
    if (!comp) { LOG("[Death] no active party component"); return; }
    auto* bm = GetBattleMainActor();
    if (!bm) { LOG("[Death] no battle main"); return; }

    // Transition the battle into the game-over step. E_BTL_STEP_GAMEOVER = 29.
    auto* fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BattleMainWorkBase:SetNextStep"));
    if (!fn) { LOG("[Death] SetNextStep not found"); return; }
    uint8 params[64]{};
    params[0] = 29; // E_BTL_STEP_GAMEOVER
    LOG("[Death] death-link: forcing defeat (E_BTL_STEP_GAMEOVER)");
    bm->ProcessEvent(fn, params);
    LOG("[Death] SetNextStep(GAMEOVER) called");
}

static void OnEngineTickPre(Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
    if (s_PendingKill) {
        s_PendingKill = false;
        TriggerGameOverNow();
    }
}

void Setup() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    Hook::RegisterEngineTickPreCallback(OnEngineTickPre, Hook::FCallbackOptions{});
}

bool KillLocalPlayer() {    
    s_PendingKill = true;
    LOG("[Death] game-over requested");
    return true;
}
}
