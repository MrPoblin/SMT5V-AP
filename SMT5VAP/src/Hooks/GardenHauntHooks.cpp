#include "GardenHauntHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/World.hpp>
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

// The demon level captured from the most recent PickItemReward (for reporting).
static int32 s_DevilLevel{0};

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

        LOG("[GardenHauntHooks] PickItemReward props: DevilLevel={}",
            DevilLevelProp ? 1 : 0);

        // Arm the context flag before PickItemReward runs and capture the demon
        // level (the in-param reads fine; the out-params are not reliable for the
        // gift path, so the real item id is captured later from the ItemGet grant
        // itself). The actual item grant (BPL_ItemData::ItemGet) fires after
        // PickItemReward returns, so we keep the flag armed until it is intercepted.
        F->RegisterPreHook([DevilLevelProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            s_GardenGiftActive = true;
            if (DevilLevelProp) {
                s_DevilLevel = *DevilLevelProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            }
            LOG("[GardenHaunt] PickItemReward armed (DevilLevel={})", s_DevilLevel);
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
    bool isSuppressing{ s_SuppressGifts.load(std::memory_order_acquire) && s_GardenGiftActive && IsInGardenLevel() };
    ClearGardenGiftContext();
    return isSuppressing;
}

void CaptureGiftGrant(int32_t itemId, int32_t itemNum) {
    LOG("[GardenHaunt] Gift grant captured - DevilLevel={}, ItemID={}, Num={}",
        s_DevilLevel, itemId, itemNum);
    {
        std::lock_guard<std::mutex> L(s_Mutex);
        for (auto& cb : s_GiftCallbacks) {
            cb(s_DevilLevel, itemId, itemNum);
        }
    }
}

bool IsInGardenLevel() {
    // There can be several UWorld objects alive at once (the main world plus
    // streamed sub-levels such as the haunt itself). FindFirstOf may return a
    // sub-level world whose authority game mode is null, so iterate and use the
    // world that actually owns a game mode.
    AGameModeBase* GM = nullptr;
    {
        std::vector<UObject*> Worlds;
        UObjectGlobals::FindAllOf(STR("World"), Worlds);
        for (auto* Obj : Worlds) {
            auto* W = static_cast<UWorld*>(Obj);
            if (!W) continue;
            AGameModeBase* Candidate = W->GetAuthorityGameMode();
            if (Candidate) { GM = Candidate; break; }
        }
    }
    if (!GM) {
        // Fallback to the original single-world lookup.
        UWorld* World = static_cast<UWorld*>(UObjectGlobals::FindFirstOf(STR("World")));
        if (World) GM = World->GetAuthorityGameMode();
    }
    if (!GM) return false;

    // BPI_GameMode:IsInGardenLevel is a Blueprint Interface function. Resolve it
    // off the GameMode instance; try the class map first, then the full chain
    // (which includes implemented interfaces), mirroring the engine's generated
    // IBPI_GameMode_C::Execute_IsInGardenLevel.
    UFunction* F = GM->GetFunctionByName(STR("IsInGardenLevel"));
    if (!F) F = GM->GetFunctionByNameInChain(STR("IsInGardenLevel"));
    if (!F) {
        // Function unresolvable: PickItemReward only fires during a garden/haunt
        // talk, so its arming is itself a reliable haunt signal; trust it rather
        // than silently disabling suppression.
        return true;
    }
    struct { bool IsInGardenLevel; } params{};
    GM->ProcessEvent(F, &params);
    return params.IsInGardenLevel;
}

void ClearGardenGiftContext() {
    s_GardenGiftActive = false;
}

} // namespace GardenHauntHooks
