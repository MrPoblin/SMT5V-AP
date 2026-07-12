#include "LevelUpTick.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <mutex>
#include <vector>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace LevelUpTick {

// E_PLAYER_TYPE::E_PLAYER_NAHOBINO — the protagonist.
static constexpr uint8 kProtagonist = 2;

// ── Protagonist level accessor (BPL_PartyData::GetPlayerLevel) ──
static UFunction* s_GetPlayerLevelFunc = nullptr;
static UObject* s_PartyDataCDO = nullptr;
static bool s_PartyDataInitFailed = false;

// ── Polling state (3s real-time interval, framerate-independent) ──
static auto s_LastPollTime = std::chrono::steady_clock::now();
static constexpr auto POLL_INTERVAL = std::chrono::seconds(3);

static void InitPartyData() {
    if (s_GetPlayerLevelFunc || s_PartyDataInitFailed) return;
    s_GetPlayerLevelFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_PartyData:GetPlayerLevel"));
    if (!s_GetPlayerLevelFunc)
        s_GetPlayerLevelFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData_C:GetPlayerLevel"));
    if (!s_GetPlayerLevelFunc) {
        WARN("[LevelUpTick] GetPlayerLevel not found");
        s_PartyDataInitFailed = true;
        return;
    }
    s_PartyDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
        STR("/Script/Project.Default__BPL_PartyData"));
    if (!s_PartyDataCDO)
        s_PartyDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_PartyData_C"));
    if (!s_PartyDataCDO) {
        auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData"));
        if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData_C"));
        if (cls) s_PartyDataCDO = cls->CreateDefaultObject();
    }
    if (!s_PartyDataCDO) {
        WARN("[LevelUpTick] BPL_PartyData CDO not found");
        s_PartyDataInitFailed = true;
    }
}

static int32 GetProtagonistLevel() {
    InitPartyData();
    if (!s_GetPlayerLevelFunc || !s_PartyDataCDO) return -1;
    struct FParams { uint8 playerType; int32 ReturnValue; };
    FParams params{};
    params.playerType = kProtagonist;
    s_PartyDataCDO->ProcessEvent(s_GetPlayerLevelFunc, &params);
    return params.ReturnValue;
}

// ── Callbacks ──
static std::vector<LevelUpCallback> s_Callbacks;
static std::mutex s_Mutex;

// ── Level tracking / dedup ──
static int32 s_CachedLevel = -1;
static bool s_LevelKnown = false;

static void Fire(int32 oldLevel, int32 newLevel) {
    LOG("[LevelUpTick] Protagonist levelled up: {} -> {} (gained {})",
        oldLevel, newLevel, newLevel - oldLevel);
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& cb : s_Callbacks) cb(oldLevel, newLevel);
}

// Fires the callback at most once per level increase.
static void DetectLevelChange(int32 level) {
    if (level < 1) return;
    if (!s_LevelKnown) {
        s_CachedLevel = level;
        s_LevelKnown = true;
        return;
    }
    if (level > s_CachedLevel) {
        Fire(s_CachedLevel, level);
        s_CachedLevel = level;
    } else if (level < s_CachedLevel) {
        // Level dropped (e.g. NG+ reset) — resync baseline, don't fire.
        s_CachedLevel = level;
    }
}

// ── Detection ──
// The protagonist's level is applied through a C++ path that bypasses any
// UFunction hook, so we poll GetPlayerLevel each frame (from on_update). This
// reliably catches every level-up (real battles, exp items, etc.) and computes
// the gain from the before/after result.
void Poll() {
    // Real-time throttle (framerate-independent)
    auto now = std::chrono::steady_clock::now();
    if (now - s_LastPollTime < POLL_INTERVAL) return;
    s_LastPollTime = now;

    DetectLevelChange(GetProtagonistLevel());
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
