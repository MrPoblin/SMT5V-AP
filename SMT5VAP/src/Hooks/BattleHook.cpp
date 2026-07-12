#include "BattleHook.hpp"
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

// UBattleResult_C: m_ResultData at +0x0130
//   FBtlResultData:
//     TArray<int32> m_ItemID at +0x00 (absolute 0x0130)
//     TArray<int32> m_ItemCnt at +0x10 (absolute 0x0140)
static constexpr int32 kResultDataOffset  = 0x0130;

// ABattleMain_C: m_EncountData at +0x0350 (inherited from ABattleMainWorkBase)
// FBtlEncData layout (0x58 bytes):
//   bool m_IsRoomBoss     at +0x10
//   int32 m_EncID         at +0x14
//   int32 m_EvtEncID      at +0x18
// AActor* m_BossInstance  at +0x0EB0 (from ABattleMain_C)
// FBtlEncData m_EncountData at +0x0350 (0x58 bytes):
//   TArray<int32> m_EnemyIDArray at +0x48 (absolute 0x0398)
static constexpr int32 kEncountDataOffset  = 0x0350;
static constexpr int32 kEncIDOffset        = 0x14;
static constexpr int32 kEvtEncIDOffset     = 0x18;
static constexpr int32 kIsRoomBossOffset   = 0x10;
static constexpr int32 kEnemyIDArrayOffset = 0x48;
static constexpr int32 kOuterOffset       = 0x20;
static constexpr int32 kBossInstanceOffset = 0x0EB0;

// ABattleMainWorkBase:
//   TArray<FBtlKillData> m_EnemyKillList at +0x0290
//     FBtlKillData { int32 m_EnemyID; int32 m_KillCnt; } (0x8 bytes)
static constexpr int32 kEnemyKillListOffset = 0x0290;

// Defeated-enemy record (mirrors FBtlKillData from the dumped headers).
struct FBtlKillData { int32 m_EnemyID; int32 m_KillCnt; };

static void suppressItems(UObject* resultComp) {
    uint8* base = reinterpret_cast<uint8*>(resultComp) + kResultDataOffset;

    FScriptArray* itemIDs = reinterpret_cast<FScriptArray*>(base);
    FScriptArray* itemCnts = reinterpret_cast<FScriptArray*>(base + 0x10);

    int32 count = itemIDs->Num();
    if (count <= 0) return;

    int32* idData = static_cast<int32*>(itemIDs->GetData());
    int32* cntData = static_cast<int32*>(itemCnts->GetData());

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
    uint8* bm = reinterpret_cast<uint8*>(outer);
    FScriptArray* enemyIDs = reinterpret_cast<FScriptArray*>(bm + kEncountDataOffset + kEnemyIDArrayOffset);
    if (!enemyIDs || enemyIDs->Num() <= 0) return false;
    int32* ids = static_cast<int32*>(enemyIDs->GetData());
    for (int32 i = 0; i < enemyIDs->Num(); i++) {
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

            // Get the Outer (should be ABattleMain_C) via direct offset
            UObject* outer = *reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(resultComp) + kOuterOffset);

            // Read encounter data from BattleMain
            uint8* bm = reinterpret_cast<uint8*>(outer);
            int32 encId     = *reinterpret_cast<int32*>(bm + kEncountDataOffset + kEncIDOffset);
            int32 evtEncId  = *reinterpret_cast<int32*>(bm + kEncountDataOffset + kEvtEncIDOffset);
            bool isRoomBoss = *reinterpret_cast<bool*>(bm + kEncountDataOffset + kIsRoomBossOffset);
            UObject* bossInst = *reinterpret_cast<UObject**>(bm + kBossInstanceOffset);
            bool isBoss = (bossInst != nullptr || isRoomBoss);

            LOG("[BattleHook] Victory - EncID={}, EvtEncID={}, Boss={}", encId, evtEncId, isBoss);

            // Devil IDs of every enemy in the encounter (not just defeated).
            std::vector<int32_t> encounteredEnemyIds;
            FScriptArray* enemyIDs = reinterpret_cast<FScriptArray*>(bm + kEncountDataOffset + kEnemyIDArrayOffset);
            if (enemyIDs && enemyIDs->Num() > 0) {
                int32* ids = static_cast<int32*>(enemyIDs->GetData());
                for (int32 i = 0; i < enemyIDs->Num(); i++) {
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
                FScriptArray* killList = reinterpret_cast<FScriptArray*>(bm + kEnemyKillListOffset);
                int32 killCount = killList ? killList->Num() : 0;
                if (killCount > 0) {
                    FBtlKillData* data = static_cast<FBtlKillData*>(killList->GetData());
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
