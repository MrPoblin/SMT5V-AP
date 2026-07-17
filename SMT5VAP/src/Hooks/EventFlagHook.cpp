#include "EventFlagHook.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <mutex>
#include <vector>
#include <atomic>
#include <string>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlagHook {

static CallbackId s_SetEventFlagHookId{-1};
static CallbackId s_SetEventFlagValueHookId{-1};

static std::vector<FlagSetCallback> s_Callbacks;
static std::mutex s_Mutex;

// Guard so external writers (e.g. FlagGating) can suppress echo of their own writes.
static std::atomic<bool> s_SuppressNotify{false};
void SetSuppressNotify(bool suppress) { s_SuppressNotify.store(suppress, std::memory_order_release); }
bool IsSuppressNotify() { return s_SuppressNotify.load(std::memory_order_acquire); }

// Forward to all subscribers. Skipped when an external writer set the suppress guard.
static void Dispatch(const StringType& flagName, bool val) {
    if (IsSuppressNotify()) return;
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& cb : s_Callbacks) cb(flagName, val);
}

void Setup() {
    // ── SetEventFlag(FName Name, bool sw) ──
    {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_EventFlag:SetEventFlag"));
        if (!Func) {
            WARN("[EventFlagHook] BPL_EventFlag.SetEventFlag NOT FOUND");
        } else {
            FProperty* NameProp = Func->GetPropertyByName(STR("Name"));
            FProperty* FlagProp = Func->GetPropertyByName(STR("sw"));
            if (!NameProp || !FlagProp) {
                WARN("[EventFlagHook] SetEventFlag param resolution failed");
            } else {
                s_SetEventFlagHookId = Func->RegisterPreHook(
                    [NameProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                        auto* locals = Ctx.TheStack.Locals();
                        if (!locals) return;
                        bool val = false;
                        FName name;
                        if (auto* p = FlagProp->ContainerPtrToValuePtr<bool>(locals)) val = *p;
                        if (auto* p = NameProp->ContainerPtrToValuePtr<FName>(locals)) name = *p;
                        Dispatch(name.ToString(), val);
                    }
                );
                LOG("[EventFlagHook] SetEventFlag hook registered (id={})", s_SetEventFlagHookId);
            }
        }
    }

    // ── SetEventFlagValue(int32 flag, bool sw) ──
    // Same flag store, addressed by integer id. Merged into the same callback so
    // detection catches both the FName and int32 write paths.
    {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_EventFlag:SetEventFlagValue"));
        if (!Func) {
            WARN("[EventFlagHook] BPL_EventFlag.SetEventFlagValue NOT FOUND");
        } else {
            FProperty* FlagIdProp = Func->GetPropertyByName(STR("flag"));
            FProperty* SwProp = Func->GetPropertyByName(STR("sw"));
            if (!FlagIdProp || !SwProp) {
                WARN("[EventFlagHook] SetEventFlagValue param resolution failed");
            } else {
                s_SetEventFlagValueHookId = Func->RegisterPreHook(
                    [FlagIdProp, SwProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                        auto* locals = Ctx.TheStack.Locals();
                        if (!locals) return;
                        int32 flagId = -1;
                        bool val = false;
                        if (auto* p = FlagIdProp->ContainerPtrToValuePtr<int32>(locals)) flagId = *p;
                        if (auto* p = SwProp->ContainerPtrToValuePtr<bool>(locals)) val = *p;
                        // No FName available here; surface the integer id so it's still traceable.
                        StringType flagName = StringType(STR("[")) + std::to_wstring(flagId) + StringType(STR("]"));
                        Dispatch(flagName, val);
                    }
                );
                LOG("[EventFlagHook] SetEventFlagValue hook registered (id={})", s_SetEventFlagValueHookId);
            }
        }
    }
}

void OnFlagSet(FlagSetCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

} // namespace EventFlagHook
