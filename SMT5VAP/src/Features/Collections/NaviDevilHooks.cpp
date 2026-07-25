#include "NaviDevilHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace NaviDevilHooks {
    static std::vector<NaviGimmickCollectedCallback> s_Callbacks;
    static std::mutex s_Mutex;

    // Navigator change detection
    static std::vector<NaviDevilChangedCallback> s_ChangedCallbacks;
    static std::mutex s_ChangedMutex;
    static UObject* s_NaviCDO = nullptr;
    static UFunction* s_GetNaviIDFn = nullptr;

    static UFunction* FindFunc(std::initializer_list<const wchar_t*> paths) {
        for (auto* p : paths) {
            if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
                return F;
        }
        return nullptr;
    }

    static UObject* FindCDO(std::initializer_list<const wchar_t*> paths) {
        for (auto* p : paths) {
            if (auto* Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, p))
                return Obj;
        }
        return nullptr;
    }

    // Find a CDO by class name, trying multiple naming patterns
    static UObject* FindNaviCDO(const wchar_t* className) {
        StringType path = StringType(STR("/Script/Project.Default__")) + className;
        auto* Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, path.c_str());
        if (Obj) return Obj;
        path = StringType(STR("/Script/Project.Default__")) + className + STR("_C");
        Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, path.c_str());
        if (Obj) return Obj;
        StringType classPath = StringType(STR("/Script/Project.")) + className;
        auto* Cls = UObjectGlobals::FindObject<UClass>(nullptr, classPath.c_str());
        if (!Cls) {
            classPath += STR("_C");
            Cls = UObjectGlobals::FindObject<UClass>(nullptr, classPath.c_str());
        }
        if (Cls) return Cls->CreateDefaultObject();
        return nullptr;
    }

    void SetupNaviDevilChanged() {
        LOG("[NaviDevil] SetupNaviDevilChanged...");

        auto* F = FindFunc({
            STR("/Script/Project.BPL_MapData:SetCurrentNaviDevil"),
            STR("/Script/Project.BPL_MapData_C:SetCurrentNaviDevil"),
        });
        if (!F) {
            WARN("[NaviDevil] SetCurrentNaviDevil NOT FOUND");
            return;
        }

        // Cache CDO for calling GetCurrentNaviDevilID
        s_NaviCDO = FindNaviCDO(STR("BPL_MapData"));
        s_GetNaviIDFn = FindFunc({
            STR("/Script/Project.BPL_MapData:GetCurrentNaviDevilID"),
            STR("/Script/Project.BPL_MapData_C:GetCurrentNaviDevilID"),
        });

        if (!s_NaviCDO) WARN("[NaviDevil] BPL_MapData CDO NOT FOUND");
        if (!s_GetNaviIDFn) WARN("[NaviDevil] GetCurrentNaviDevilID NOT FOUND");

        F->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (!s_NaviCDO || !s_GetNaviIDFn) return;

            struct { int32 ReturnValue; } Params{};
            s_NaviCDO->ProcessEvent(s_GetNaviIDFn, &Params);

            int32 devilID = Params.ReturnValue;
            if (devilID > 0) {
                LOG("[NaviDevil] Navigator changed: DevilID={}", devilID);
                std::lock_guard<std::mutex> lock(s_ChangedMutex);
                for (auto& cb : s_ChangedCallbacks) cb(devilID);
            }
        });

        LOG("[NaviDevil] SetupNaviDevilChanged OK");
    }

    void OnNaviDevilChanged(NaviDevilChangedCallback cb) {
        std::lock_guard<std::mutex> lock(s_ChangedMutex);
        s_ChangedCallbacks.push_back(std::move(cb));
    }

    // Shared trigger flag: set by AddCheckCounter, consumed by SetGimmickExistFiltered
    static std::atomic<bool> s_PendingPickup{false};

    // ── SetNaviDevilGimmickUniqueSaveID (post-hook, fires on unique gimmick save) ──
    void SetupUniqueSaveID() {
        LOG("[NaviDevil] SetupUniqueSaveID...");
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_MapData:SetNaviDevilGimmickUniqueSaveID"),
                STR("/Script/Project.BPL_MapData_C:SetNaviDevilGimmickUniqueSaveID"),
            })) {
            auto* SaveIdProp = F->GetPropertyByName(STR("SaveId"));
            auto* FlagProp = F->GetPropertyByName(STR("flag"));
            F->RegisterPostHook([SaveIdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 saveId = -1;
                bool flag = false;
                if (SaveIdProp)
                    if (auto* P = SaveIdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals())) saveId = *P;
                if (FlagProp)
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals())) flag = *P;
                if (flag && saveId >= 0) {
                    LOG("[NaviDevil] UniqueSaveID: SaveId={}", saveId);
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) cb(saveId);
                }
            });
            LOG("[NaviDevil] SetupUniqueSaveID OK");
        } else {
            WARN("[NaviDevil] SetNaviDevilGimmickUniqueSaveID NOT FOUND");
        }
    }

    // ── AddNaviDevilGimmickCheckCounter (post-hook, sets trigger flag) ──
    void SetupAddCheckCounter() {
        LOG("[NaviDevil] SetupAddCheckCounter...");
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_MapData:AddNaviDevilGimmickCheckCounter"),
                STR("/Script/Project.BPL_MapData_C:AddNaviDevilGimmickCheckCounter"),
            })) {
            F->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
                s_PendingPickup.store(true);
                LOG("[NaviDevil] AddCheckCounter: trigger set");
            });
            LOG("[NaviDevil] SetupAddCheckCounter OK (trigger mode)");
        } else {
            WARN("[NaviDevil] AddNaviDevilGimmickCheckCounter NOT FOUND");
        }
    }

    // ── SetNaviDevilGimmickExist (pre-hook, only fires when trigger is active) ──
    void SetupSetGimmickExistFiltered() {
        LOG("[NaviDevil] SetupSetGimmickExistFiltered...");
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_MapData:SetNaviDevilGimmickExist"),
                STR("/Script/Project.BPL_MapData_C:SetNaviDevilGimmickExist"),
            })) {
            auto* IdProp = F->GetPropertyByName(STR("ID"));
            auto* ExistProp = F->GetPropertyByName(STR("exist"));
            F->RegisterPreHook([IdProp, ExistProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                if (!s_PendingPickup.load()) return;

                int32 id = -1;
                bool exist = false;
                if (IdProp)
                    if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals())) id = *P;
                if (ExistProp)
                    if (auto* P = ExistProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals())) exist = *P;

                // Reset trigger so subsequent SetGimmickExist calls (same tick) get filtered
                s_PendingPickup.store(false);

                LOG("[NaviDevil] GimmickExistFiltered: ID={} exist={}", id, exist);

                std::lock_guard<std::mutex> lock(s_Mutex);
                for (auto& cb : s_Callbacks) cb(id);
            });
            LOG("[NaviDevil] SetupSetGimmickExistFiltered OK");
        } else {
            WARN("[NaviDevil] SetNaviDevilGimmickExist NOT FOUND");
        }
    }

    // ── Item replacement: modify m_ItemInfo from GetNaviDevilGimmickData return values ──
    // FNaviDevilGimmickData layout (size 0x50):
    //   +0x00 int32 m_MapID
    //   +0x04 int32 m_Index
    //   +0x08 int32 m_ExcavateType
    //   ...
    //   +0x40 TArray<FNaviDevilGimmickItemData> m_ItemInfo
    //     TArray: +0x00 Data*, +0x08 int32 Count, +0x0C int32 Max
    // FNaviDevilGimmickItemData layout (size 0x18):
    //   +0x00 bool m_IsItem
    //   +0x01 bool m_IsMakka
    //   +0x04 int32 m_ItemID
    //   +0x08 int32 m_Num
    //   +0x0C int32 m_RandomNum
    //   +0x10 int32 m_Rate
    //   +0x14 int32 m_OnFlag
    static constexpr int32 ITEM_INFO_OFFSET = 0x40;
    static constexpr int32 ARRAY_DATA_OFFSET = 0x00;
    static constexpr int32 ARRAY_COUNT_OFFSET = 0x08;
    static constexpr int32 ITEM_ENTRY_SIZE = 0x18;
    static constexpr int32 M_IS_ITEM_OFFSET = 0x00;
    static constexpr int32 M_IS_MAKKA_OFFSET = 0x01;
    static constexpr int32 M_ITEM_ID_OFFSET = 0x04;
    static constexpr int32 M_NUM_OFFSET = 0x08;

    static std::atomic<bool> s_BlockItems{true};
    static std::atomic<int32> s_ReplaceMaccaAmount{0}; // 0 = suppress, >0 = macca amount

    void SetBlockItems(bool block) {
        s_BlockItems.store(block, std::memory_order_release);
        LOG("[NaviDevil] SetBlockItems({})", block);
    }

    void SetReplaceMacca(int32_t amount) {
        s_ReplaceMaccaAmount.store(amount, std::memory_order_release);
        LOG("[NaviDevil] SetReplaceMacca({})", amount);
    }

    static void registerItemReplaceHook(const wchar_t* FuncPath) {
        auto* Fn = UObjectGlobals::FindObject<UFunction>(nullptr, FuncPath);
        if (!Fn) {
            WARN("[NaviDevil] Failed to find item replace target: {}", FuncPath);
            return;
        }
        LOG("[NaviDevil] Found item replace target: {}", FuncPath);
        Fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            static bool s_Logged = false;
            if (!s_BlockItems.load(std::memory_order_acquire)) return;
            if (auto* Result = static_cast<uint8*>(Ctx.RESULT_DECL)) {
                auto* CountPtr = reinterpret_cast<int32*>(Result + ITEM_INFO_OFFSET + ARRAY_COUNT_OFFSET);
                int32 count = *CountPtr;
                if (count <= 0) return;

                int32 maccaAmount = s_ReplaceMaccaAmount.load(std::memory_order_acquire);
                if (!s_Logged) {
                    if (maccaAmount == 0) {
                        LOG("[NaviDevil] Item replacement active: suppressing all gimmick items");
                    } else {
                        LOG("[NaviDevil] Item replacement active: giving {} macca per gimmick entry", maccaAmount);
                    }
                    s_Logged = true;
                }
                if (maccaAmount == 0) {
                    *CountPtr = 0;
                } else {
                    auto* DataPtr = *reinterpret_cast<void**>(Result + ITEM_INFO_OFFSET + ARRAY_DATA_OFFSET);
                    if (!DataPtr) return;
                    for (int32 i = 0; i < count; ++i) {
                        auto* Entry = static_cast<uint8*>(DataPtr) + i * ITEM_ENTRY_SIZE;
                        *reinterpret_cast<bool*>(Entry + M_IS_ITEM_OFFSET) = false;
                        *reinterpret_cast<bool*>(Entry + M_IS_MAKKA_OFFSET) = true;
                        *reinterpret_cast<int32*>(Entry + M_ITEM_ID_OFFSET) = 0;
                        *reinterpret_cast<int32*>(Entry + M_NUM_OFFSET) = maccaAmount;
                    }
                }
            }
        });
    }

    void SetupBlockItems() {
        LOG("[NaviDevil] SetupBlockItems...");
        registerItemReplaceHook(STR("/Script/Project.BPL_NaviDevilData:GetNaviDevilGimmickData"));
        registerItemReplaceHook(STR("/Script/Project.BPL_NaviDevilData:GetNaviDevilGimmickData_FromID"));
        LOG("[NaviDevil] SetupBlockItems complete");
    }

    void OnNaviGimmickCollected(NaviGimmickCollectedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
