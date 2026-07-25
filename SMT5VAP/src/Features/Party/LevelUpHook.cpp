#include "LevelUpHook.hpp"
#include "src/HookHelper.hpp"
#include "src/Features/Party/LevelFunctions.hpp"
#include "src/Log/Log.hpp"
#include <mutex>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace LevelUpTick {

// ── Callbacks ──
static std::vector<LevelUpCallback> s_Callbacks;
static std::mutex s_Mutex;

// ── Level tracking / dedup ──
static int32 s_CachedLevel = -1;
static bool s_LevelKnown = false;

// Hook ID
static CallbackId s_PostHookId = -1;

static void Fire(int32 oldLevel, int32 newLevel) {
    LOG("[LevelUpTick] Protagonist levelled up: {} -> {} (gained {})",
        oldLevel, newLevel, newLevel - oldLevel);
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& cb : s_Callbacks) cb(oldLevel, newLevel);
}

// Post-hook for GetPlayerLevel: fires when the returned level is higher
// than our cached value. This catches level-ups from ANY source (battle,
// items, quest rewards) because GetPlayerLevel is called every time the
// UI or game logic needs the level.
static void OnGetPlayerLevel(UnrealScriptFunctionCallableContext& Ctx, void*) {
    int32 newLevel = -1;
    if (auto* Result = static_cast<int32*>(Ctx.RESULT_DECL)) {
        newLevel = *Result;
    }
    if (newLevel < 1) return;

    if (!s_LevelKnown) {
        s_CachedLevel = newLevel;
        s_LevelKnown = true;
        return;
    }

    if (newLevel > s_CachedLevel) {
        Fire(s_CachedLevel, newLevel);
        s_CachedLevel = newLevel;
    } else if (newLevel < s_CachedLevel) {
        // Level dropped (NG+ reset, etc.) — resync, don't fire
        s_CachedLevel = newLevel;
    }
}

void Setup() {
    if (s_PostHookId != -1) return;

    LevelFunctions::Setup();

    auto* Func = HookHelper::FindFunc(STR("/Script/Project.BPL_PartyData:GetPlayerLevel"));
    if (!Func) Func = HookHelper::FindFunc(STR("/Script/Project.BPL_PartyData_C:GetPlayerLevel"));
    if (!Func) {
        WARN(STR("[LevelUpTick] BPL_PartyData:GetPlayerLevel NOT FOUND"));
        return;
    }

    s_PostHookId = HookHelper::HookPost(STR("/Script/Project.BPL_PartyData:GetPlayerLevel"), OnGetPlayerLevel);
    LOG(STR("[LevelUpTick] Hook installed on GetPlayerLevel (id={})"), s_PostHookId);
}

void OnLevelUp(LevelUpCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

void Reset() {
    s_LevelKnown = false;
    s_CachedLevel = -1;
}

}
