#include "GardenHauntHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace GardenHauntHooks {

static std::vector<GardenGiftCallback> s_GiftCallbacks;
static std::vector<GardenPowerUpCallback> s_PowerUpCallbacks;
static std::mutex s_Mutex;

// When true, haunt/garden item gifts are suppressed.
static std::atomic<bool> s_SuppressGifts{false};

// Context flag for the actual grant (BPL_ItemData::ItemGet). The grant fires
// AFTER PickItemReward returns, so we arm the flag in the pre-hook and keep it
// armed until the gift's ItemGet is intercepted (then ItemBlocker clears it).
// No time limit. Never keyed off the item ID.
static thread_local bool s_GardenGiftActive{false};

// ── PickItemReward ──
// Function Project.GardenTalk.PickItemReward
// Params:
//   [0x00] IntProperty DevilLevel    (in)
//   [0x04] IntProperty ChosenItemID  (out)
//   [0x08] IntProperty ChosenItemNum (out)
//   [0x0C] BoolProperty ReturnValue  (out)
static UFunction* FindPickItemReward() {
    for (auto* p : {
        STR("/Script/Project.GardenTalk:PickItemReward"),
        STR("/Script/Project.GardenTalk_C:PickItemReward"),
    }) {
        if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
            return F;
    }
    return nullptr;
}

// ── BuildGardenResultData ──
// Function Project.GardenTalk.BuildGardenResultData
// Called after a garden talk to build the result data (for both gifts and power-ups).
// From dumped header:
//   void BuildGardenResultData(int32 NkmIndex, bool IsPowerUpMessage, FGardenResultData& GardenResultData);
// Params (from GObjects dump):
//   [0x00] IntProperty NkmIndex           (in)
//   [0x04] BoolProperty IsPowerUpMessage   (in)
//   [0x08] StructProperty GardenResultData (out)
static UFunction* FindBuildGardenResultData() {
    for (auto* p : {
        STR("/Script/Project.GardenTalk:BuildGardenResultData"),
        STR("/Script/Project.GardenTalk_C:BuildGardenResultData"),
    }) {
        if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
            return F;
    }
    return nullptr;
}

void Setup() {
    LOG("[GardenHauntHooks] Setup...");

    // ── Hook PickItemReward ──
    if (auto* F = FindPickItemReward()) {
        LOG("[GardenHauntHooks] GardenTalk.PickItemReward found");

        auto* DevilLevelProp   = F->GetPropertyByName(STR("DevilLevel"));
        auto* ChosenItemIDProp = F->GetPropertyByName(STR("ChosenItemID"));
        auto* ChosenItemNumProp = F->GetPropertyByName(STR("ChosenItemNum"));

        LOG("[GardenHauntHooks] PickItemReward props: DevilLevel={}, ChosenItemID={}, ChosenItemNum={}",
            DevilLevelProp ? 1 : 0,
            ChosenItemIDProp ? 1 : 0,
            ChosenItemNumProp ? 1 : 0);

        // Arm the context flag before PickItemReward runs. The actual item grant
        // (BPL_ItemData::ItemGet) fires after PickItemReward returns, so we keep
        // the flag armed until it is intercepted. Never keyed off the item ID.
        F->RegisterPreHook([](UnrealScriptFunctionCallableContext&, void*) {
            s_GardenGiftActive = true;
        });

        F->RegisterPostHook([DevilLevelProp, ChosenItemIDProp, ChosenItemNumProp](
            UnrealScriptFunctionCallableContext& Ctx, void*) {

            int32 devilLevel    = 0;
            int32 chosenItemId  = 0;
            int32 chosenItemNum = 0;

            if (DevilLevelProp) {
                devilLevel = *DevilLevelProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            }
            if (ChosenItemIDProp) {
                chosenItemId = *ChosenItemIDProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            }
            if (ChosenItemNumProp) {
                chosenItemNum = *ChosenItemNumProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            }

            LOG("[GardenHaunt] Item Gift - DevilLevel={}, ItemID={}, Num={}",
                devilLevel, chosenItemId, chosenItemNum);

            // Report the pick (always, suppressed or not) so Archipelago can
            // re-grant it.
            std::lock_guard<std::mutex> L(s_Mutex);
            for (auto& cb : s_GiftCallbacks) {
                cb(devilLevel, chosenItemId, chosenItemNum);
            }

            // Do NOT disarm here: the grant (BPL_ItemData::ItemGet) happens after
            // PickItemReward returns. ItemBlocker clears the context once it
            // blocks the grant, otherwise it auto-expires via the time window.
        });
    } else {
        WARN("[GardenHauntHooks] GardenTalk.PickItemReward NOT FOUND");
    }

    // ── Hook BuildGardenResultData ──
    // Called after a garden talk to build the result (fires for both gifts and power-ups).
    // Same class as PickItemReward (GardenTalk), so it should be found.
    if (auto* F = FindBuildGardenResultData()) {
        LOG("[GardenHauntHooks] GardenTalk.BuildGardenResultData found");

        auto* NkmIndexProp         = F->GetPropertyByName(STR("NkmIndex"));
        auto* IsPowerUpMessageProp  = F->GetPropertyByName(STR("IsPowerUpMessage"));

        LOG("[GardenHauntHooks] BuildGardenResultData props: NkmIndex={}, IsPowerUpMessage={}",
            NkmIndexProp ? 1 : 0,
            IsPowerUpMessageProp ? 1 : 0);

        F->RegisterPostHook([NkmIndexProp, IsPowerUpMessageProp](
            UnrealScriptFunctionCallableContext& Ctx, void*) {

            int32 nkmIndex = -1;
            bool isPowerUp = false;

            if (NkmIndexProp) {
                nkmIndex = *NkmIndexProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            }
            if (IsPowerUpMessageProp) {
                isPowerUp = *IsPowerUpMessageProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
            }

            LOG("[GardenHaunt] BuildGardenResultData - NkmIndex={}, IsPowerUp={}",
                nkmIndex, isPowerUp);

            if (nkmIndex >= 0 && isPowerUp) {
                std::lock_guard<std::mutex> L(s_Mutex);
                for (auto& cb : s_PowerUpCallbacks) {
                    cb(nkmIndex);
                }
            }
        });
    } else {
        WARN("[GardenHauntHooks] GardenTalk.BuildGardenResultData NOT FOUND");
    }

    LOG("[GardenHauntHooks] Setup complete");
}

void OnGardenGift(GardenGiftCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_GiftCallbacks.push_back(std::move(cb));
}

void OnGardenPowerUp(GardenPowerUpCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_PowerUpCallbacks.push_back(std::move(cb));
}

void SetSuppressGifts(bool suppress) {
    s_SuppressGifts.store(suppress, std::memory_order_release);
    LOG("[GardenHauntHooks] SetSuppressGifts({})", suppress);
}

bool IsSuppressingGardenGiftNow() {
    return s_SuppressGifts.load(std::memory_order_acquire) && s_GardenGiftActive;
}

void ClearGardenGiftContext() {
    s_GardenGiftActive = false;
}

} // namespace GardenHauntHooks
