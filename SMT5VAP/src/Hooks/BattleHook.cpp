#include "BattleHook.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace BattleHook {

static CallbackId s_PreHookId{-1};
static bool s_SuppressItems{false};
static std::vector<VictoryCallback> s_Callbacks;
static std::mutex s_Mutex;

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

            // Print demon IDs from the encounter
            FScriptArray* enemyIDs = reinterpret_cast<FScriptArray*>(bm + kEncountDataOffset + kEnemyIDArrayOffset);
            if (enemyIDs && enemyIDs->Num() > 0) {
                int32* ids = static_cast<int32*>(enemyIDs->GetData());
                for (int32 i = 0; i < enemyIDs->Num(); i++) {
                    if (ids[i] != 0) {
                        LOG("[BattleHook] Encounter demon ID[{}] = {}", i, ids[i]);
                    }
                }
            }

            // Fire callbacks
            {
                std::lock_guard<std::mutex> lock(s_Mutex);
                for (auto& cb : s_Callbacks) cb(encId, evtEncId, isBoss);
            }

            // Suppress items BEFORE SetResultData processes the data
            if (s_SuppressItems) {
                suppressItems(resultComp);
            }
        }
    );
    LOG("[BattleHook] SetResultData pre-hook registered (id={})", s_PreHookId);
}

void OnVictory(VictoryCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

void SetSuppressItems(bool suppress) {
    s_SuppressItems = suppress;
    if (suppress) {
        LOG("[BattleHook] Item suppression ENABLED");
    } else {
        LOG("[BattleHook] Item suppression DISABLED");
    }
}

}
