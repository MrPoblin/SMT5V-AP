#include "NaviDevilHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace NaviDevilHooks {
    static std::vector<NaviGimmickCollectedCallback> s_Callbacks;
    static std::mutex s_Mutex;

    // Shared trigger flag: set by AddCheckCounter, consumed by SetGimmickExistFiltered
    static std::atomic<bool> s_PendingPickup{false};

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

    void OnNaviGimmickCollected(NaviGimmickCollectedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
