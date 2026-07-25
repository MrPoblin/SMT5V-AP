#include "BattleHook.hpp"
#include "src/HookHelper.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace BattleHook {

static CallbackId s_PreHookId{-1};
static bool s_SuppressItems{false};
static bool s_SuppressMitamaItems{false};
static std::vector<VictoryCallback> s_Callbacks;
static std::mutex s_Mutex;

static CallbackId s_HPHookId{-1};
static std::vector<AllyDownedCallback> s_DownCallbacks;
static std::mutex s_DownMutex;
static UFunction* s_GetHeroIndexFunc{nullptr};

// Resolves the protagonist's current party index via the C++ base class
// UFunction GetHeroIndex(). Returns -1 if it could not be resolved.
static int32 ResolveHeroIndex(UObject* comp) {
    if (!comp) return -1;
    if (!s_GetHeroIndexFunc) {
        s_GetHeroIndexFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BattlePartySystemComponentBase:GetHeroIndex"));
        if (!s_GetHeroIndexFunc) {
            WARN("[BattleHook] Could not find GetHeroIndex");
            return -1;
        }
        LOG("[BattleHook] Found GetHeroIndex");
    }
    struct FGetHeroIndexParams { int32 ReturnValue; };
    FGetHeroIndexParams params{};
    comp->ProcessEvent(s_GetHeroIndexFunc, &params);
    return params.ReturnValue;
}

// Defeated-enemy record (mirrors FBtlKillData from the dumped headers).
struct FBtlKillData { int32 m_EnemyID; int32 m_KillCnt; };

// ── CustomProperty-based field accessors for game structs ──
//
// UBattleResult_C: m_ResultData at +0x0130
//   FBtlResultData:
//     TArray<int32> m_ItemID  at +0x0130
//     TArray<int32> m_ItemCnt at +0x0140
static PropertyArrayAccessor<int32> s_ResultItemIDs(0x0130);
static PropertyArrayAccessor<int32> s_ResultItemCnts(0x0140);

// ABattleMain_C: FBtlEncData m_EncountData at +0x0350 (0x58 bytes):
//   bool    m_IsRoomBoss   at +0x0360
//   int32   m_EncID        at +0x0364
//   int32   m_EvtEncID     at +0x0368
//   TArray<int32> m_EnemyIDArray at +0x0398
static PropertyField<bool>        s_IsRoomBoss(0x0360);
static PropertyField<int32>       s_EncID(0x0364);
static PropertyField<int32>       s_EvtEncID(0x0368);
static PropertyArrayAccessor<int32> s_EnemyIDArray(0x0398);

// AActor* m_BossInstance at +0x0EB0
static PropertyField<UObject*>    s_BossInstance(0x0EB0);

// TArray<FBtlKillData> m_EnemyKillList at +0x0290
static PropertyArrayAccessor<FBtlKillData> s_EnemyKillList(0x0290);

// UObject Outer pointer at +0x20 (from UObjectBase)
static PropertyField<UObject*>    s_UObjectOuter(0x20);

static void suppressItems(UObject* resultComp) {
    int32 count = s_ResultItemIDs.GetCount(resultComp);
    if (count <= 0) return;

    int32* idData = s_ResultItemIDs.GetData(resultComp);
    int32* cntData = s_ResultItemCnts.GetData(resultComp);

    for (int32 i = 0; i < count; i++) {
        idData[i] = -1;
        cntData[i] = 0;
    }
    LOG("[BattleHook] Suppressed {} item(s)", count);
}

// ── Mitama detection ──
// E_GROUP_ID::E_GROUP_ID_MITAMA == 16; FDevilBaseData::m_Group is at offset 8.
static constexpr uint8 kMitamaGroup = 16;

static UFunction* s_GetDevilBaseDataFunc{nullptr};
static UObject* s_DevilDataCDO{nullptr};
static bool s_DevilDataInitFailed{false};

static void InitDevilData() {
    if (s_GetDevilBaseDataFunc || s_DevilDataInitFailed) return;
    s_GetDevilBaseDataFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_DevilData:GetDevilBaseData"));
    if (!s_GetDevilBaseDataFunc)
        s_GetDevilBaseDataFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_DevilData_C:GetDevilBaseData"));
    if (!s_GetDevilBaseDataFunc) {
        WARN("[BattleHook] BPL_DevilData::GetDevilBaseData not found");
        s_DevilDataInitFailed = true;
        return;
    }
    s_DevilDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
        STR("/Script/Project.Default__BPL_DevilData"));
    if (!s_DevilDataCDO)
        s_DevilDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_DevilData_C"));
    if (!s_DevilDataCDO) {
        auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_DevilData"));
        if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_DevilData_C"));
        if (cls) s_DevilDataCDO = cls->CreateDefaultObject();
    }
    if (!s_DevilDataCDO) {
        WARN("[BattleHook] BPL_DevilData CDO not found");
        s_DevilDataInitFailed = true;
    }
}

// Returns the enemy's group ID via BPL_DevilData::GetDevilBaseData(devilID).
static uint8 GetDevilGroup(int32 devilId) {
    InitDevilData();
    if (!s_GetDevilBaseDataFunc || !s_DevilDataCDO) return 0;
    auto* retProp = s_GetDevilBaseDataFunc->GetPropertyByName(STR("ReturnValue"));
    if (!retProp) return 0;
    // ID (int32) at 0, then FDevilBaseData return value. Buffer sized generously.
    std::vector<uint8> params(0x800, 0);
    *reinterpret_cast<int32*>(params.data()) = devilId;
    s_DevilDataCDO->ProcessEvent(s_GetDevilBaseDataFunc, params.data());
    uint8* structPtr = retProp->ContainerPtrToValuePtr<uint8>(params.data());
    return structPtr[8]; // m_Group at offset 8 within FDevilBaseData
}

// True if the given defeated enemy belongs to the Mitama group.
bool IsMitamaDevil(int32_t devilId) {
    return GetDevilGroup(devilId) == kMitamaGroup;
}

// True if any enemy in the encounter belongs to the Mitama group.
static bool IsMitamaBattle(UObject* outer) {
    if (!outer) return false;
    int32 count = s_EnemyIDArray.GetCount(outer);
    if (count <= 0) return false;
    int32* ids = s_EnemyIDArray.GetData(outer);
    for (int32 i = 0; i < count; i++) {
        if (ids[i] != 0 && GetDevilGroup(ids[i]) == kMitamaGroup) {
            LOG("[BattleHook] Mitama battle detected (demon ID={})", ids[i]);
            return true;
        }
    }
    return false;
}

void Setup() {
    if (s_PreHookId != -1) return;

    auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BattleResultActorComponentBase:SetResultData"));
    if (!Func) {
        WARN("[BattleHook] Could not find SetResultData");
        return;
    }
    LOG("[BattleHook] Found SetResultData");

    // Use pre-hook so suppression runs BEFORE the result data is processed
    s_PreHookId = Func->RegisterPreHook(
        [](UnrealScriptFunctionCallableContext& Ctx, void*) {
            auto* resultComp = Ctx.Context;
            if (!resultComp) return;

            // Get the Outer (should be ABattleMain_C) via UObjectBase field
            UObject* outer = s_UObjectOuter(resultComp);
            if (!outer) return;

            // Read encounter data from BattleMain using named field accessors
            int32 encId     = s_EncID(outer);
            int32 evtEncId  = s_EvtEncID(outer);
            bool isRoomBoss = s_IsRoomBoss(outer);
            UObject* bossInst = s_BossInstance(outer);
            bool isBoss = (bossInst != nullptr || isRoomBoss);

            LOG("[BattleHook] Victory - EncID={}, EvtEncID={}, Boss={}", encId, evtEncId, isBoss);

            // Devil IDs of every enemy in the encounter (not just defeated).
            std::vector<int32_t> encounteredEnemyIds;
            int32 enemyCount = s_EnemyIDArray.GetCount(outer);
            if (enemyCount > 0) {
                int32* ids = s_EnemyIDArray.GetData(outer);
                for (int32 i = 0; i < enemyCount; i++) {
                    if (ids[i] != 0) {
                        encounteredEnemyIds.push_back(ids[i]);
                        LOG("[BattleHook] Encounter demon ID[{}] = {}", i, ids[i]);
                    }
                }
            }

            // Devil IDs of every enemy actually defeated this battle.
            // m_EnemyKillList is de-duplicated by enemy type and carries
            // m_KillCnt (how many of that type died), so expand by count to
            // report the true number of defeated enemies (handles multiple
            // copies of the same foe, e.g. two 290s). A fled/surviving enemy
            // is simply absent here.
            std::vector<int32_t> defeatedEnemyIds;
            {
                int32 killCount = s_EnemyKillList.GetCount(outer);
                if (killCount > 0) {
                    FBtlKillData* data = s_EnemyKillList.GetData(outer);
                    for (int32 i = 0; i < killCount; i++) {
                        int32 id = data[i].m_EnemyID;
                        if (id == 0) continue;
                        int32 cnt = data[i].m_KillCnt > 0 ? data[i].m_KillCnt : 1;
                        for (int32 k = 0; k < cnt; k++) {
                            defeatedEnemyIds.push_back(id);
                            LOG("[BattleHook] Defeated enemy ID = {}", id);
                        }
                    }
                }
            }
            LOG("[BattleHook] defeated enemies={}, encountered enemies={}", defeatedEnemyIds.size(), encounteredEnemyIds.size());

            // Fire callbacks
            {
                std::lock_guard<std::mutex> lock(s_Mutex);
                for (auto& cb : s_Callbacks) cb(encId, evtEncId, isBoss, defeatedEnemyIds, encounteredEnemyIds);
            }

            // Suppress items BEFORE SetResultData processes the data
            if (s_SuppressItems) {
                suppressItems(resultComp);
            }

            // Mitama-only suppression
            if (s_SuppressMitamaItems && IsMitamaBattle(outer)) {
                suppressItems(resultComp);
            }
        }
    );
    LOG("[BattleHook] SetResultData pre-hook registered (id={})", s_PreHookId);

    // ── Player-down detection ──
    // Hook SetHPMP on the C++ base class BattlePartySystemComponentBase.
    // Fires when a party member's HP is set to 0 (downed/dead). The caller
    // is responsible for deciding what counts as a death (e.g. hero only).
    auto* HPFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BattlePartySystemComponentBase:SetHPMP"));
    if (!HPFunc) {
        WARN("[BattleHook] Could not find SetHPMP");
    } else {
        LOG("[BattleHook] Found SetHPMP");
        FProperty* PartyIndexProp = HPFunc->GetPropertyByName(STR("partyIndex"));
        FProperty* InValueProp = HPFunc->GetPropertyByName(STR("InValue"));
        FProperty* IsHPProp = HPFunc->GetPropertyByName(STR("isHP"));

        s_HPHookId = HPFunc->RegisterPostHook(
            [PartyIndexProp, InValueProp, IsHPProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 partyIndex = -1;
                int32 inValue = 0;
                bool isHP = false;
                if (PartyIndexProp) {
                    if (auto* P = PartyIndexProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        partyIndex = *P;
                }
                if (InValueProp) {
                    if (auto* P = InValueProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        inValue = *P;
                }
                if (IsHPProp) {
                    if (auto* P = IsHPProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        isHP = *P;
                }

                // Only react to HP changes, and only when HP reaches 0.
                if (isHP && inValue <= 0 && partyIndex >= 0) {
                    int32 heroIndex = ResolveHeroIndex(Ctx.Context);
                    LOG("[BattleHook] Party member downed - partyIndex={}, HP={}, heroIndex={}", partyIndex, inValue, heroIndex);
                    std::lock_guard<std::mutex> lock(s_DownMutex);
                    for (auto& cb : s_DownCallbacks) cb(partyIndex, inValue, heroIndex);
                }
            }
        );
        LOG("[BattleHook] SetHPMP post-hook registered (id={})", s_HPHookId);
    }
}

void OnVictory(VictoryCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

void OnAllyDowned(AllyDownedCallback cb) {
    std::lock_guard<std::mutex> lock(s_DownMutex);
    s_DownCallbacks.push_back(std::move(cb));
}

void SetSuppressItems(bool suppress) {
    s_SuppressItems = suppress;
    if (suppress) {
        LOG("[BattleHook] Item suppression ENABLED");
    } else {
        LOG("[BattleHook] Item suppression DISABLED");
    }
}

void SetSuppressMitamaItems(bool suppress) {
    s_SuppressMitamaItems = suppress;
    if (suppress) {
        LOG("[BattleHook] Mitama item suppression ENABLED");
    } else {
        LOG("[BattleHook] Mitama item suppression DISABLED");
    }
}

}
