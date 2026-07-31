#include "MimanHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/Helper/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace MimanHooks {
    static CallbackId s_SetMimanFlagHookId{-1};
    static std::vector<MimanFoundCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetMimanFlag"));
        if (!Func) {
            WARN("[MimanHooks] Failed to find SetMimanFlag");
            return;
        }
        LOG("[MimanHooks] Found SetMimanFlag");

        FProperty* IdProp = Func->GetPropertyByName(STR("ID"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

        s_SetMimanFlagHookId = Func->RegisterPostHook(
            [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 mimanId = -1;
                bool flag = false;
                if (IdProp) {
                    if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        mimanId = *P;
                }
                if (FlagProp) {
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        flag = *P;
                }

                if (flag && mimanId >= 0) {
                    LOG("[Miman] Found: ID={}", mimanId);
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) {
                        cb(mimanId);
                    }
                }
            }
        );
        LOG("[MimanHooks] SetMimanFlag post-hook registered (id={})", s_SetMimanFlagHookId);
    }

    void OnMimanFound(MimanFoundCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
