#include "DevilStatueHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/Helper/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <cwchar>

using namespace RC;
using namespace RC::Unreal;

namespace DevilStatueHooks {
    static CallbackId s_SetEventFlagHookId{-1};
    static std::vector<DevilStatueCollectCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_EventFlag:SetEventFlag"));
        if (!Func) {
            WARN("[DevilStatueHooks] Failed to find SetEventFlag");
            return;
        }
        LOG("[DevilStatueHooks] Found SetEventFlag");

        FProperty* NameProp = Func->GetPropertyByName(STR("Name"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("sw"));

        s_SetEventFlagHookId = Func->RegisterPostHook(
            [NameProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                FName flagName;
                bool flagValue = false;

                if (NameProp) {
                    if (auto* P = NameProp->ContainerPtrToValuePtr<FName>(Ctx.TheStack.Locals()))
                        flagName = *P;
                }
                if (FlagProp) {
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        flagValue = *P;
                }

                // Only care about statue flags being set to true
                if (flagValue) {
                    StringType nameStr = flagName.ToString();
                    const TCHAR* buf = nameStr.c_str();
                    if (buf && nameStr.length() >= 7 && wcsncmp(buf, STR("Statue_"), 7) == 0) {
                        LOG("[DevilStatue] Collected flag: {}", nameStr);
                        std::lock_guard<std::mutex> lock(s_Mutex);
                        for (auto& cb : s_Callbacks) {
                            cb(flagName);
                        }
                    }
                }
            }
        );
        LOG("[DevilStatueHooks] SetEventFlag post-hook registered (id={})", s_SetEventFlagHookId);
    }

    void OnDevilStatueCollected(DevilStatueCollectCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
