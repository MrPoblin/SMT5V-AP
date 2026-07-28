#include "MissionScoutManager.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <mutex>
#include <vector>
#include <functional>

using namespace RC;
using namespace RC::Unreal;

static std::mutex s_Mutex;
static std::vector<int32_t> s_ActiveMissions;
static bool s_Resolved = false;

static std::mutex s_CallbackMutex;
static std::vector<MissionScoutManager::MissionsLoadedCallback> s_LoadedCallbacks;
static std::vector<MissionScoutManager::MissionAddedCallback> s_AddedCallbacks;

// E_MISSION_STATE values from CXX dump
static constexpr int32 MISSION_STATE_NOW_ENTRY    = 3;
static constexpr int32 MISSION_STATE_CAN_COMPLETE = 4;

// Brute-force scan: query GetMissionState for IDs 1-300.
// Works regardless of quest controller timing.
static bool TryScanActiveMissions() {
    auto* func = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_MissionData:GetMissionState"));
    if (!func)
        func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData_C:GetMissionState"));

    auto* cdo = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
        STR("/Script/Project.Default__BPL_MissionData"));
    if (!cdo)
        cdo = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_MissionData_C"));

    if (!func) {
        WARN("[MissionScoutManager] GetMissionState UFunction not found");
        return false;
    }
    if (!cdo) {
        WARN("[MissionScoutManager] BPL_MissionData CDO not found (tried Default__BPL_MissionData and _C)");
        return false;
    }

    std::vector<int32_t> missionIds;

    for (int32 id = 1; id <= 300; id++) {
        struct { int32 MissionId; int32 ReturnValue; } params{};
        params.MissionId = id;
        cdo->ProcessEvent(func, &params);

        int32 state = params.ReturnValue;
        if (state == MISSION_STATE_NOW_ENTRY || state == MISSION_STATE_CAN_COMPLETE) {
            missionIds.push_back(id);
        }
    }

    if (missionIds.empty()) {
        LOG("[MissionScoutManager] No active missions found via GetMissionState");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_ActiveMissions = missionIds;
    }

    LOG("[MissionScoutManager] Found {} active missions", missionIds.size());

    std::lock_guard<std::mutex> lock(s_CallbackMutex);
    for (auto& cb : s_LoadedCallbacks) cb(missionIds);
    return true;
}

static CallbackId s_EntryMissionHookId{-1};
static CallbackId s_MakeUpdateInfoHookId{-1};

namespace MissionScoutManager {

    void Setup() {
        if (s_Resolved) return;
        LOG("[MissionScoutManager] Setup...");

        // Hook MakeUpdateInfoList — fires after missions are loaded
        auto* updateFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:MakeUpdateInfoList"));
        if (!updateFunc)
            updateFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:MakeUpdateInfoList"));

        if (updateFunc) {
            s_MakeUpdateInfoHookId = updateFunc->RegisterPostHook(
                [](UnrealScriptFunctionCallableContext&, void*) {
                    LOG("[MissionScoutManager] MakeUpdateInfoList fired");
                    TryScanActiveMissions();
                }
            );
            LOG("[MissionScoutManager] MakeUpdateInfoList hook registered");
        }

        // Hook EntryMission to detect new missions
        auto* entryFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:EntryMission"));
        if (!entryFunc)
            entryFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:EntryMission"));

        if (entryFunc) {
            s_EntryMissionHookId = entryFunc->RegisterPostHook(
                [](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    auto* IdProp = Ctx.TheStack.Node()->GetPropertyByName(STR("MissionId"));
                    if (!IdProp) return;
                    auto* ptr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                    if (!ptr || *ptr <= 0) return;

                    int32_t newId = *ptr;
                    LOG("[MissionScoutManager] New mission: {}", newId);

                    {
                        std::lock_guard<std::mutex> lock(s_Mutex);
                        for (auto id : s_ActiveMissions)
                            if (id == newId) return;
                        s_ActiveMissions.push_back(newId);
                    }

                    std::lock_guard<std::mutex> lock(s_CallbackMutex);
                    for (auto& cb : s_AddedCallbacks) cb(newId);
                }
            );
            LOG("[MissionScoutManager] EntryMission hook registered");
        }

        s_Resolved = true;
        LOG("[MissionScoutManager] Setup complete");
    }

    bool Rescan() { return TryScanActiveMissions(); }

    const std::vector<int32_t>& GetActiveMissions() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        return s_ActiveMissions;
    }

    void OnMissionsLoaded(MissionsLoadedCallback cb) {
        std::lock_guard<std::mutex> lock(s_CallbackMutex);
        s_LoadedCallbacks.push_back(std::move(cb));
    }

    void OnMissionAdded(MissionAddedCallback cb) {
        std::lock_guard<std::mutex> lock(s_CallbackMutex);
        s_AddedCallbacks.push_back(std::move(cb));
    }
}
