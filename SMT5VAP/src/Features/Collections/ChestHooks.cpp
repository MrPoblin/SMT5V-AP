#include "ChestHooks.hpp"
#include "src/Features/UI/PopupSuppression.hpp"
#include "src/HookHelper.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Windows.h>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

namespace ChestHooks {
    // ── Existing: SetTakaraFlag detection ──
    static CallbackId s_SetTakaraFlagHookId{-1};
    static std::vector<ChestOpenCallback> s_Callbacks;
    static std::mutex s_CallbackMutex;

    // ── Chest content manipulation state ──
    static std::atomic<bool> s_EmptyAllChests{false};
    static std::atomic<int32_t> s_ReplaceItemId{-1};
    static std::atomic<int32_t> s_ReplaceAmount{-1};
    static std::atomic<int32_t> s_ReplaceMacca{-1};
    static std::unordered_set<int32_t> s_ExcludedChests;
    static std::mutex s_ExcludeMutex;

    // ── PolyHook2 native hook on FUN_1474c91a0 ──
    static std::unique_ptr<PLH::x64Detour> s_DataTableHook;
    static uint64_t s_OrigFunc = 0;

    // Runtime FTakaraData field accessors (offsets from project source headers)
    static PropertyField<int32> s_TakaraItemId(0x14);
    static PropertyField<int32> s_TakaraNum(0x18);
    static PropertyField<int32> s_TakaraMacca(0x1C);

    // The hooked function: FUN_1474c91a0 (DataTable linear search)
    // RCX = DataTable* (global+0x13C0), RDX = mapId, R8 = takaraID, R9 = saveIdOut*
    // Returns void* = pointer to row data
    static void* __fastcall HkDataTableLookup(void* dataTable, int mapId, int takaraID, int* saveIdOut) {
        auto* result = reinterpret_cast<decltype(&HkDataTableLookup)>(s_OrigFunc)(dataTable, mapId, takaraID, saveIdOut);

        if (saveIdOut && *saveIdOut >= 0) {
            int32 idx = *saveIdOut;

            bool excluded = false;
            {
                std::lock_guard<std::mutex> lock(s_ExcludeMutex);
                excluded = s_ExcludedChests.count(idx) > 0;
            }

            if (!excluded) {
                int32& itemId = s_TakaraItemId.Get(result);
                int32& num    = s_TakaraNum.Get(result);
                int32& macca  = s_TakaraMacca.Get(result);

                if (s_EmptyAllChests.load(std::memory_order_acquire)) {
                    itemId = 0;
                    num    = 0;
                    macca  = 0;
                }

                int32 replItemId = s_ReplaceItemId.load(std::memory_order_acquire);
                int32 replAmount = s_ReplaceAmount.load(std::memory_order_acquire);
                int32 replMacca  = s_ReplaceMacca.load(std::memory_order_acquire);

                if (replItemId >= 0) itemId = replItemId;
                if (replAmount >= 0) num    = replAmount;
                if (replMacca  >= 0) macca  = replMacca;
            }
        }

        return result;
    }

    // ── Public API ──

    void SetEmptyAllChests(bool empty) {
        s_EmptyAllChests.store(empty, std::memory_order_release);
        LOG("[ChestHooks] SetEmptyAllChests({})", empty);
    }

    void SetReplacementItem(int32_t itemId) {
        s_ReplaceItemId.store(itemId, std::memory_order_release);
    }

    void SetReplacementAmount(int32_t amount) {
        s_ReplaceAmount.store(amount, std::memory_order_release);
    }

    void SetReplacementMacca(int32_t macca) {
        s_ReplaceMacca.store(macca, std::memory_order_release);
    }

    void SetReplacement(int32_t itemId, int32_t amount, int32_t macca) {
        s_ReplaceItemId.store(itemId, std::memory_order_release);
        s_ReplaceAmount.store(amount, std::memory_order_release);
        s_ReplaceMacca.store(macca, std::memory_order_release);
        LOG("[ChestHooks] SetReplacement(itemId={}, amount={}, macca={})", itemId, amount, macca);
    }

    void AddExcludedChest(int32_t tableId) {
        std::lock_guard<std::mutex> lock(s_ExcludeMutex);
        s_ExcludedChests.insert(tableId);
        LOG("[ChestHooks] AddExcludedChest({})", tableId);
    }

    void RemoveExcludedChest(int32_t tableId) {
        std::lock_guard<std::mutex> lock(s_ExcludeMutex);
        s_ExcludedChests.erase(tableId);
    }

    void ClearExcludedChests() {
        std::lock_guard<std::mutex> lock(s_ExcludeMutex);
        s_ExcludedChests.clear();
    }

    void SetExcludedChests(std::initializer_list<int32_t> ids) {
        std::lock_guard<std::mutex> lock(s_ExcludeMutex);
        s_ExcludedChests.clear();
        s_ExcludedChests.insert(ids.begin(), ids.end());
        LOG("[ChestHooks] SetExcludedChests({} ids)", ids.size());
    }

    bool IsChestExcluded(int32_t tableId) {
        std::lock_guard<std::mutex> lock(s_ExcludeMutex);
        return s_ExcludedChests.count(tableId) > 0;
    }

    void OnChestOpened(ChestOpenCallback cb) {
        std::lock_guard<std::mutex> lock(s_CallbackMutex);
        s_Callbacks.push_back(std::move(cb));
    }

    void Setup() {
        // ── PolyHook2 native hook on FUN_1474c91a0 (DataTable linear search) ──
        uint64_t targetAddr = SignatureScanner::FindPattern("48 89 5C 24 ? 48 89 7C 24 ? 4C 63 51");
        if (!targetAddr) {
            WARN("[ChestHooks] DataTable lookup signature NOT FOUND");
        } else {
            uint64_t origAddr = 0;
            auto det = std::make_unique<PLH::x64Detour>(
                targetAddr,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkDataTableLookup, &s_OrigFunc)),
                &origAddr
            );

            if (det->hook()) {
                s_OrigFunc = PLH::FnCast(origAddr, s_OrigFunc);
                s_DataTableHook = std::move(det);
                LOG("[ChestHooks] Native DataTable hook installed at {:p}", (void*)targetAddr);
            } else {
                WARN("[ChestHooks] Native x64Detour FAILED at {:p}", (void*)targetAddr);
            }
        }

        // ── SetTakaraFlag post-hook (chest open detection) ──
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetTakaraFlag"));
        if (!Func) {
            WARN("[ChestHooks] Failed to find SetTakaraFlag");
        } else {
            LOG("[ChestHooks] Found SetTakaraFlag");

            FProperty* IdProp   = Func->GetPropertyByName(STR("ID"));
            FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

            s_SetTakaraFlagHookId = Func->RegisterPostHook(
                [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 chestId = -1;
                    bool flag = false;
                    if (IdProp) {
                        auto* IdPtr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                        if (IdPtr) chestId = *IdPtr;
                    }
                    if (FlagProp) {
                        auto* FlagPtr = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                        if (FlagPtr) flag = *FlagPtr;
                    }

                    if (flag && chestId >= 0) {
                        LOG("[Chest] Flag set: chest save ID={}", chestId);

                        if (PopupSuppression::IsBlockingChests()) {
                            PopupSuppression::SetBlockNextSpawn(true);
                        }

                        std::lock_guard<std::mutex> lock(s_CallbackMutex);
                        for (auto& cb : s_Callbacks) {
                            cb(chestId);
                        }
                    }
                }
            );
            LOG("[ChestHooks] SetTakaraFlag post-hook registered (id={})", s_SetTakaraFlagHookId);
        }

        LOG("[ChestHooks] Setup complete");
    }
}
