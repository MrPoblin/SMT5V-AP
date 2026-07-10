#include "AogamiHooks.hpp"
#include "PopupSuppression.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace AogamiHooks {
    static CallbackId s_SetAogamiDebrisFlagHookId{-1};
    static CallbackId s_GetAogamiDebrisDataHookId{-1};
    static std::vector<AogamiDebrisCollectedCallback> s_Callbacks;
    static std::mutex s_Mutex;

    static std::atomic<int32> s_ReplaceItemId{-1};  // -1 = disabled

    void SetReplaceItemId(int32_t itemId) {
        s_ReplaceItemId.store(itemId, std::memory_order_release);
        if (itemId >= 0) {
            LOG("[AogamiHooks] Item replacement enabled: all husks will give item ID={}", itemId);
        } else {
            LOG("[AogamiHooks] Item replacement disabled");
        }
    }

    // FAogamiDebrisData layout (0x14 bytes):
    //   +0x00 int32 m_MapID
    //   +0x04 int32 m_Index
    //   +0x08 int32 m_ItemID   <-- we replace this
    //   +0x0C int32 m_RebornTime
    //   +0x10 int32 m_RebornRate
    static constexpr int32 ITEM_ID_OFFSET = 0x08;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetAogamiDebrisFlag"));
        if (!Func) {
            WARN("[AogamiHooks] Failed to find SetAogamiDebrisFlag");
            return;
        }
        LOG("[AogamiHooks] Found SetAogamiDebrisFlag");

        FProperty* TableIndexProp = Func->GetPropertyByName(STR("tableIndex"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

        s_SetAogamiDebrisFlagHookId = Func->RegisterPostHook(
            [TableIndexProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 tableIndex = -1;
                bool flag = false;
                if (TableIndexProp) {
                    if (auto* P = TableIndexProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        tableIndex = *P;
                }
                if (FlagProp) {
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        flag = *P;
                }

                if (flag && tableIndex >= 0) {
                    LOG("[Aogami] Debris collected: tableIndex={}", tableIndex);

                    if (PopupSuppression::IsBlockingAogamiDebris()) {
                        PopupSuppression::SetBlockNextSpawn(true);
                    }

                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) {
                        cb(tableIndex);
                    }
                }
            }
        );
        LOG("[AogamiHooks] SetAogamiDebrisFlag post-hook registered (id={})", s_SetAogamiDebrisFlagHookId);

        // ── Helper to register item replacement on a GetAogamiDebrisData-like function ──
        auto registerItemReplaceHook = [](const wchar_t* FuncPath) {
            auto* Fn = UObjectGlobals::FindObject<UFunction>(nullptr, FuncPath);
            if (!Fn) {
                WARN("[AogamiHooks] Failed to find {}", FuncPath);
                return CallbackId{-1};
            }
            LOG("[AogamiHooks] Found {}", FuncPath);
            return Fn->RegisterPostHook(
                [](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 replaceWith = s_ReplaceItemId.load(std::memory_order_acquire);
                    if (replaceWith < 0) return;  // replacement disabled

                    if (auto* Result = static_cast<uint8*>(Ctx.RESULT_DECL)) {
                        int32 oldId = *reinterpret_cast<int32*>(Result + ITEM_ID_OFFSET);
                        if (oldId != replaceWith) {
                            *reinterpret_cast<int32*>(Result + ITEM_ID_OFFSET) = replaceWith;
                            LOG("[Aogami] Replaced debris item ID: {} -> {}", oldId, replaceWith);
                        }
                    }
                }
            );
        };

        // Hook both getters so whichever the game calls gets intercepted.
        s_GetAogamiDebrisDataHookId = registerItemReplaceHook(
            STR("/Script/Project.BPL_AogamiDebrisData:GetAogamiDebrisData"));
        LOG("[AogamiHooks] GetAogamiDebrisData post-hook registered (id={})", s_GetAogamiDebrisDataHookId);

        auto Id2 = registerItemReplaceHook(
            STR("/Script/Project.BPL_AogamiDebrisData:GetAogamiDebrisData_FromTableIndex"));
        LOG("[AogamiHooks] GetAogamiDebrisData_FromTableIndex post-hook registered (id={})", Id2);
    }

    void OnAogamiDebrisCollected(AogamiDebrisCollectedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
