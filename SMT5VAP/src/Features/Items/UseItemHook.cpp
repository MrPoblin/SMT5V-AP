#include "UseItemHook.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace UseItemHook {
    static std::vector<ItemUseCallback> s_Callbacks;
    static std::mutex s_Mutex;

    static thread_local int32 s_LastItemId = -1;

    static void TryHookCampItemUse() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_CampItem:CampItem_Use"));
        if (!Func) Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_CampItem_C:CampItem_Use"));
        if (!Func) {
            WARN("[UseItemHook] BPL_CampItem::CampItem_Use NOT FOUND");
            return;
        }
        LOG("[UseItemHook] Found BPL_CampItem::CampItem_Use");

        auto* IdProp = Func->GetPropertyByName(STR("ItemId"));
        LOG("[UseItemHook] CampItem_Use ItemId prop={}", IdProp ? 1 : 0);

        Func->RegisterPreHook([IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (IdProp) {
                if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                    s_LastItemId = *P;
            }
        });

        Func->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            int32 itemId = s_LastItemId;
            s_LastItemId = -1;
            if (itemId > 0) {
                LOG("[UseItemHook:CampItem_Use] Item used: id={}", itemId);
                std::lock_guard<std::mutex> lock(s_Mutex);
                for (auto& cb : s_Callbacks) cb(itemId);
            }
        });

        LOG("[UseItemHook] CampItem_Use hooks registered");
    }

    static void TryHookUseItem() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.CampCharaPanelCtrlBase:UseItem"));
        if (!Func) Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.CampCharaPanelCtrlBase_C:UseItem"));
        if (!Func) {
            WARN("[UseItemHook] CampCharaPanelCtrlBase::UseItem NOT FOUND");
            return;
        }
        LOG("[UseItemHook] Found CampCharaPanelCtrlBase::UseItem");

        auto* IdProp = Func->GetPropertyByName(STR("InUseItemId"));
        LOG("[UseItemHook] UseItem InUseItemId prop={}", IdProp ? 1 : 0);

        Func->RegisterPreHook([IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (IdProp) {
                if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                    s_LastItemId = *P;
            }
        });

        Func->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            int32 itemId = s_LastItemId;
            s_LastItemId = -1;
            if (itemId > 0) {
                LOG("[UseItemHook:CtrlBase:UseItem] Item used: id={}", itemId);
                std::lock_guard<std::mutex> lock(s_Mutex);
                for (auto& cb : s_Callbacks) cb(itemId);
            }
        });

        LOG("[UseItemHook] UseItem hooks registered");
    }

    void Setup() {
        LOG("[UseItemHook] Setup...");
        TryHookCampItemUse();
        TryHookUseItem();
        LOG("[UseItemHook] Setup complete");
    }

    void OnItemUsed(ItemUseCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
