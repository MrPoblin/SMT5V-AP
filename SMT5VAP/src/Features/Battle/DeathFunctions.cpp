#include "DeathFunctions.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/CallbackIterationData.hpp>
#include <vector>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace DeathFunctions {

// Death-link kill: force SMTVV's real defeat step (E_BTL_STEP_GAMEOVER).
// Work is deferred to the engine-tick PRE callback so it never runs mid
// engine-tick (which would corrupt the battle state machine).
static bool s_PendingKill{false};
static bool s_RetryUntilSuccess{false};

static auto s_LastPollTime = std::chrono::steady_clock::now();
static constexpr auto POLL_INTERVAL = std::chrono::seconds(1);

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

static bool GameOverActuallyStarted() {
    auto* bm = GetBattleMainActor();
    if (!bm) return false;
    // m_Step at 0x260 on ABattleMainWorkBase
    uint8 step = *reinterpret_cast<uint8*>(reinterpret_cast<uint8*>(bm) + 0x260);
    if (step == 29) return true; // E_BTL_STEP_GAMEOVER
    // m_GameOverWidget at 0xE18 on ABattleMain_C
    void* widget = *reinterpret_cast<void**>(reinterpret_cast<uint8*>(bm) + 0xE18);
    return widget != nullptr;
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
    bm->ProcessEvent(fn, params);
    LOG("[Death] SetNextStep(GAMEOVER) called");
}

static void OnEngineTickPre(Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
    auto now = std::chrono::steady_clock::now();
    if (now - s_LastPollTime < POLL_INTERVAL) return;
    s_LastPollTime = now;

    if (s_PendingKill) {
        if (!s_RetryUntilSuccess) {
            s_PendingKill = false;
        }
        else if (GameOverActuallyStarted() || GameState::MapName().contains(L"Title/LV_Title")) {
            s_PendingKill = false;
            s_RetryUntilSuccess = false;
            return;
        }
        TriggerGameOverNow();
    }
}

void Setup() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    Hook::RegisterEngineTickPreCallback(OnEngineTickPre, Hook::FCallbackOptions{});
}

bool KillLocalPlayer(bool retry) {    
    s_PendingKill = true;
    s_RetryUntilSuccess = retry;
    LOG("[Death] game-over requested");
    return true;
}
}
