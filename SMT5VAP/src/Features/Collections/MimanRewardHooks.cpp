#include "MimanRewardHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace MimanRewardHooks {
    static CallbackId s_SetRewardFlagHookId{-1};
    static std::vector<MimanRewardCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        // Gustave marks a Miman-count reward as claimed here.
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_FacilityShop:SetShopMimanRewardFlag"));
        if (!Func) {
            WARN("[MimanRewardHooks] Failed to find SetShopMimanRewardFlag");
            return;
        }
        LOG("[MimanRewardHooks] Found SetShopMimanRewardFlag");

        FProperty* IdProp = Func->GetPropertyByName(STR("ID"));
        FProperty* SwProp = Func->GetPropertyByName(STR("sw"));

        s_SetRewardFlagHookId = Func->RegisterPostHook(
            [IdProp, SwProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 rewardId = -1;
                bool sw = false;
                if (IdProp) {
                    if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        rewardId = *P;
                }
                if (SwProp) {
                    if (auto* P = SwProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        sw = *P;
                }

                if (sw && rewardId >= 0) {
                    LOG("[MimanReward] Claimed: reward ID={}", rewardId);
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) {
                        cb(rewardId);
                    }
                }
            }
        );
        LOG("[MimanRewardHooks] SetShopMimanRewardFlag post-hook registered (id={})", s_SetRewardFlagHookId);
    }

    void OnMimanRewardClaimed(MimanRewardCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
