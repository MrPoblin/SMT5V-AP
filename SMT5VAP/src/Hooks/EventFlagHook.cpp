#include "EventFlagHook.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <mutex>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlagHook {

static CallbackId s_SetEventFlagHookId{-1};

static std::vector<FlagSetCallback> s_Callbacks;
static std::mutex s_Mutex;

void Setup() {
    if (s_SetEventFlagHookId >= 0) return;

    auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_EventFlag:SetEventFlag"));
    if (!Func) {
        WARN("[EventFlagHook] BPL_EventFlag.SetEventFlag NOT FOUND");
        return;
    }

    FProperty* NameProp = Func->GetPropertyByName(STR("Name"));
    FProperty* FlagProp = Func->GetPropertyByName(STR("sw"));
    if (!NameProp || !FlagProp) {
        WARN("[EventFlagHook] SetEventFlag param resolution failed");
        return;
    }

    s_SetEventFlagHookId = Func->RegisterPreHook(
        [NameProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            // Pre-hook: input params are valid before the function executes.
            // (A post-hook would read torn-down locals off the frame stack.)
            auto* locals = Ctx.TheStack.Locals();
            if (!locals) return;

            bool val = false;
            FName name;
            if (auto* p = FlagProp->ContainerPtrToValuePtr<bool>(locals)) val = *p;
            if (auto* p = NameProp->ContainerPtrToValuePtr<FName>(locals)) name = *p;

            StringType flagName = name.ToString();
            std::lock_guard<std::mutex> lock(s_Mutex);
            for (auto& cb : s_Callbacks) cb(flagName, val);
        }
    );
    LOG("[EventFlagHook] SetEventFlag hook registered (id={})", s_SetEventFlagHookId);
}

void OnFlagSet(FlagSetCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

} // namespace EventFlagHook
