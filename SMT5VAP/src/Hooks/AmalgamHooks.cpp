#include "AmalgamHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace AmalgamHooks {
    static std::vector<ShinseiCollectCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetShinseiFlag"));
        if (!Func) {
            WARN("[AmalgamHooks] Failed to find SetShinseiFlag");
            return;
        }
        LOG("[AmalgamHooks] Found SetShinseiFlag");

        FProperty* IdProp = Func->GetPropertyByName(STR("ID"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

        Func->RegisterPostHook(
            [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 shinseiId = -1;
                bool flag = false;
                if (IdProp) {
                    if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        shinseiId = *P;
                }
                if (FlagProp) {
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        flag = *P;
                }
                if (!flag || shinseiId < 0) return;

                LOG("[AmalgamHooks] Shinsei collected: ID={}", shinseiId);
                std::lock_guard<std::mutex> lock(s_Mutex);
                for (auto& cb : s_Callbacks) {
                    cb(shinseiId);
                }
            }
        );
        LOG("[AmalgamHooks] SetShinseiFlag post-hook registered");
    }

    void OnShinseiCollected(ShinseiCollectCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
