#include "VendingHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Windows.h>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <cstdint>

using namespace RC;
using namespace RC::Unreal;

namespace VendingHooks {
    static std::atomic<int32_t> s_ReplaceItemId{-1};
    static std::atomic<int32_t> s_ReplaceAmount{-1};
    static std::atomic<int32_t> s_ReplaceRate{-1};

    static std::unordered_set<int32_t> s_CollectedSaveIds;
    static std::mutex s_SaveIdMutex;

    static std::vector<VendingCollectCallback> s_Callbacks;
    static std::mutex s_CallbackMutex;

    static CallbackId s_SetRelicFlagHookId{-1};
    static CallbackId s_GetRelicDataHookId{-1};
    static CallbackId s_GetRelicDataFromTableIdxHookId{-1};

    static void HandleGetRelicData(UnrealScriptFunctionCallableContext& Ctx) {
        if (!Ctx.RESULT_DECL) return;

        auto* relicData = reinterpret_cast<uint8*>(Ctx.RESULT_DECL);
        auto* groupsPtr = *reinterpret_cast<int32**>(relicData + 0x10);
        auto  groupCount = *reinterpret_cast<int32*>(relicData + 0x18);
        if (!groupsPtr || groupCount <= 0) return;

        int32 replItemId = s_ReplaceItemId.load(std::memory_order_acquire);
        int32 replAmount = s_ReplaceAmount.load(std::memory_order_acquire);
        int32 replRate   = s_ReplaceRate.load(std::memory_order_acquire);
        if (replItemId < 0 && replAmount < 0 && replRate < 0) return;

        int32 count = (groupCount > 3) ? 3 : groupCount;
        for (int32 i = 0; i < count; i++) {
            if (replItemId >= 0) groupsPtr[i * 3 + 0] = replItemId;
            if (replAmount >= 0) groupsPtr[i * 3 + 1] = replAmount;
            if (replRate   >= 0) groupsPtr[i * 3 + 2] = replRate;
        }
    }

    void SetReplacementItem(int32_t itemId) {
        s_ReplaceItemId.store(itemId, std::memory_order_release);
    }

    void SetReplacementAmount(int32_t amount) {
        s_ReplaceAmount.store(amount, std::memory_order_release);
    }

    void SetReplacementRate(int32_t rate) {
        s_ReplaceRate.store(rate, std::memory_order_release);
    }

    void SetReplacement(int32_t itemId, int32_t amount, int32_t rate) {
        s_ReplaceItemId.store(itemId, std::memory_order_release);
        s_ReplaceAmount.store(amount, std::memory_order_release);
        s_ReplaceRate.store(rate, std::memory_order_release);
        LOG("[VendingHooks] SetReplacement(itemId={}, amount={}, rate={})", itemId, amount, rate);
    }

    bool IsVendingCollected(int32_t saveId) {
        std::lock_guard<std::mutex> lock(s_SaveIdMutex);
        return s_CollectedSaveIds.count(saveId) > 0;
    }

    void MarkVendingCollected(int32_t saveId) {
        std::lock_guard<std::mutex> lock(s_SaveIdMutex);
        s_CollectedSaveIds.insert(saveId);
    }

    void ClearVendingCollected() {
        std::lock_guard<std::mutex> lock(s_SaveIdMutex);
        s_CollectedSaveIds.clear();
    }

    void OnVendingCollected(VendingCollectCallback cb) {
        std::lock_guard<std::mutex> lock(s_CallbackMutex);
        s_Callbacks.push_back(std::move(cb));
    }

    void Setup() {
        // ── BP post-hook on GetRelicData ──
        {
            auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_RelicData:GetRelicData"));
            if (!Func) {
                WARN("[VendingHooks] Failed to find GetRelicData");
            } else {
                s_GetRelicDataHookId = Func->RegisterPostHook(
                    [](UnrealScriptFunctionCallableContext& Ctx, void*) {
                        HandleGetRelicData(Ctx);
                    }
                );
                LOG("[VendingHooks] GetRelicData post-hook registered (id={})", s_GetRelicDataHookId);
            }
        }

        // ── BP post-hook on GetRelicData_FromTableIndex ──
        {
            auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_RelicData:GetRelicData_FromTableIndex"));
            if (!Func) {
                WARN("[VendingHooks] Failed to find GetRelicData_FromTableIndex");
            } else {
                s_GetRelicDataFromTableIdxHookId = Func->RegisterPostHook(
                    [](UnrealScriptFunctionCallableContext& Ctx, void*) {
                        HandleGetRelicData(Ctx);
                    }
                );
                LOG("[VendingHooks] GetRelicData_FromTableIndex post-hook registered (id={})", s_GetRelicDataFromTableIdxHookId);
            }
        }

        // ── SetRelicFlag post-hook ──
        auto* FlagFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetRelicFlag"));
        if (!FlagFunc) {
            WARN("[VendingHooks] Failed to find SetRelicFlag");
        } else {
            FProperty* IdProp = FlagFunc->GetPropertyByName(STR("ID"));
            FProperty* FlagProp = FlagFunc->GetPropertyByName(STR("flag"));

            s_SetRelicFlagHookId = FlagFunc->RegisterPostHook(
                [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 relicId = -1;
                    bool flag = false;
                    if (IdProp) {
                        auto* ptr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                        if (ptr) relicId = *ptr;
                    }
                    if (FlagProp) {
                        auto* ptr = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                        if (ptr) flag = *ptr;
                    }
                    if (!flag || relicId < 0) return;

                    LOG("[VendingHooks] Relic collected: save ID={}", relicId);

                    {
                        std::lock_guard<std::mutex> lock(s_SaveIdMutex);
                        s_CollectedSaveIds.insert(relicId);
                    }

                    {
                        std::lock_guard<std::mutex> lock(s_CallbackMutex);
                        for (auto& cb : s_Callbacks) {
                            cb(relicId);
                        }
                    }
                }
            );
            LOG("[VendingHooks] SetRelicFlag post-hook registered (id={})", s_SetRelicFlagHookId);
        }

        LOG("[VendingHooks] Setup complete");
    }
}
