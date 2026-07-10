#include "MissionHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace MissionHooks {
    static std::vector<MissionCallback> s_Callbacks;
    static std::mutex s_Mutex;
    static std::atomic<bool> s_BlockReward{false};

    static UFunction* FindFunc(std::initializer_list<const wchar_t*> paths) {
        for (auto* p : paths) {
            if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
                return F;
        }
        return nullptr;
    }

    void Setup() {
        LOG("[MissionHooks] Setup...");

        // ── CompleteMission (mission reported/completed) ──
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_MissionData:CompleteMission"),
                STR("/Script/Project.BPL_MissionData_C:CompleteMission"),
            })) {
            auto* IdProp = F->GetPropertyByName(STR("MissionId"));
            LOG("[MissionHooks] CompleteMission found, MissionId prop={}", IdProp ? 1 : 0);
            F->RegisterPreHook([IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 id = -1;
                if (IdProp)
                    if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        id = *P;
                LOG("[Mission] CompleteMission MissionId={}", id);
                std::lock_guard<std::mutex> L(s_Mutex);
                for (auto& cb : s_Callbacks) cb(id);
            });
        } else {
            WARN("[MissionHooks] CompleteMission NOT FOUND");
        }

        LOG("[MissionHooks] Setup complete");
    }

    void OnMissionCompleted(MissionCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }

    void SetBlockMissionReward(bool block) {
        s_BlockReward = block;
        LOG("[MissionHooks] SetBlockMissionReward({})", block);
    }
}
