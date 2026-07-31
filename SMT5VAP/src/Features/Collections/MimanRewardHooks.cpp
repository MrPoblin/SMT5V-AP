#include "MimanRewardHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/Helper/HookHelper.hpp"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FFrame.hpp>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

// ── State ──────────────────────────────────────────────────────────────
static std::mutex s_TextMutex;
static std::unordered_map<int32_t, std::wstring> s_CustomTexts;
static std::atomic<bool> s_Blocking{false};

static constexpr int32 MIMAN_MAGIC_ID_BASE = 2000000;

// ── Native hook on the name lookup function ────────────────────────────
// Decompilation of GetMimanRewardItemSetName exec:
//   execGetMimanRewardItemSetName(this, frame, result) {
//       itemSetID = read_int_param(frame);
//       ftext_ptr = NameLookup(itemSetID);   // sub_1474D6AF0
//       *result = *ftext_ptr;
//   }
// NameLookup(itemSetID):
//   base = GetManager() + 5648;
//   if (itemSetID < 1 || itemSetID > 19) return base + 0;  // default "NOT USED:MSG_DUMMY"
//   return base + 24 * itemSetID;
// The table is a flat array of FText (24 bytes each) indexed by itemSetID.
// We hook NameLookup to intercept magic IDs and return custom FText.
static std::unique_ptr<PLH::x64Detour> s_NameLookupDetour;
static uint64_t s_OrigNameLookup = 0;
static constexpr uint64_t NAME_LOOKUP_ADDR = 0x1474D6AF0;

static FText s_CustomReturnText;

static void* __fastcall HkNameLookup(int itemSetID) {
    if (itemSetID >= MIMAN_MAGIC_ID_BASE) {
        int32_t tierIndex = itemSetID - MIMAN_MAGIC_ID_BASE;
        std::wstring customText;
        {
            std::lock_guard<std::mutex> lock(s_TextMutex);
            auto it = s_CustomTexts.find(tierIndex);
            if (it != s_CustomTexts.end()) customText = it->second;
        }
        if (!customText.empty()) {
            s_CustomReturnText = FText(customText.c_str());
            return &s_CustomReturnText;
        }
    }
    return reinterpret_cast<decltype(&HkNameLookup)>(s_OrigNameLookup)(itemSetID);
}

// FShopMimanItemTable layout:
//   +0x00: int32 MimanNum
//   +0x04: int32 DispNameID
//   +0x08: TArray<FShopMimanItemData> ItemList
//          +0x00: void* Data  (8 bytes)
//          +0x08: int32 Num
//          +0x0C: int32 Max

namespace MimanRewardHooks {
    static CallbackId s_SetRewardFlagHookId{-1};
    static CallbackId s_GetTablePreHookId{-1};
    static CallbackId s_GetTablePostHookId{-1};
    static std::vector<MimanRewardCallback> s_Callbacks;
    static std::mutex s_Mutex;
    static std::atomic<int32_t> s_PendingTierIndex{-1};

    void Setup() {
        LOG("[MimanRewardHooks] Setup...");

        // ── PolyHook2 detour on native name lookup ────────────────────
        {
            uint64_t origAddr = 0;
            auto det = std::make_unique<PLH::x64Detour>(
                NAME_LOOKUP_ADDR,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkNameLookup, &s_OrigNameLookup)),
                &origAddr);
            if (det->hook()) {
                s_OrigNameLookup = PLH::FnCast(origAddr, s_OrigNameLookup);
                s_NameLookupDetour = std::move(det);
                LOG("[MimanRewardHooks] Name lookup hook installed at {:p}", (void*)NAME_LOOKUP_ADDR);
            } else {
                WARN("[MimanRewardHooks] Name lookup hook FAILED at {:p}", (void*)NAME_LOOKUP_ADDR);
            }
        }

        // ── Hook GetShopMimanItemTable: pre captures tier, post modifies result ──
        auto* tableFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_ShopTableFunction:GetShopMimanItemTable"));
        if (!tableFunc)
            tableFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_ShopTableFunction_C:GetShopMimanItemTable"));

        if (tableFunc) {
            auto* IdProp = tableFunc->GetPropertyByName(STR("Index"));

            s_GetTablePreHookId = tableFunc->RegisterPreHook(
                [IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    if (IdProp) {
                        auto* ptr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                        if (ptr) s_PendingTierIndex.store(*ptr, std::memory_order_relaxed);
                    }
                }
            );

            s_GetTablePostHookId = tableFunc->RegisterPostHook(
                [](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    auto* result = static_cast<uint8_t*>(Ctx.RESULT_DECL);
                    if (!result) return;

                    if (s_Blocking.load(std::memory_order_relaxed)) {
                        *reinterpret_cast<int32*>(result + 0x10) = 0; // ItemList.Num = 0
                    }

                    int32_t tierIndex = s_PendingTierIndex.load(std::memory_order_relaxed);
                    if (tierIndex >= 0) {
                        std::lock_guard<std::mutex> lock(s_TextMutex);
                        auto it = s_CustomTexts.find(tierIndex);
                        if (it != s_CustomTexts.end() && !it->second.empty()) {
                            *reinterpret_cast<int32*>(result + 0x04) = MIMAN_MAGIC_ID_BASE + tierIndex;
                        }
                    }
                }
            );
            LOG("[MimanRewardHooks] GetShopMimanItemTable hooks registered");
        } else {
            WARN("[MimanRewardHooks] GetShopMimanItemTable UFunction NOT FOUND");
        }

        // ── Post-hook on SetShopMimanRewardFlag ───────────────────────
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_FacilityShop:SetShopMimanRewardFlag"));
        if (!Func) {
            WARN("[MimanRewardHooks] Failed to find SetShopMimanRewardFlag");
        } else {
            FProperty* IdProp = Func->GetPropertyByName(STR("ID"));
            FProperty* SwProp = Func->GetPropertyByName(STR("sw"));

            s_SetRewardFlagHookId = Func->RegisterPostHook(
                [IdProp, SwProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 rewardId = -1;
                    bool sw = false;
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            rewardId = *P;
                    if (SwProp)
                        if (auto* P = SwProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                            sw = *P;

                    if (sw && rewardId >= 0) {
                        LOG("[MimanReward] Claimed: reward ID={}", rewardId);
                        std::lock_guard<std::mutex> lock(s_Mutex);
                        for (auto& cb : s_Callbacks) cb(rewardId);
                    }
                }
            );
            LOG("[MimanRewardHooks] SetShopMimanRewardFlag post-hook registered");
        }

        LOG("[MimanRewardHooks] Setup complete");
    }

    // ── Public API ─────────────────────────────────────────────────────

    void SetBlocking(bool block) {
        s_Blocking.store(block, std::memory_order_relaxed);
        LOG("[MimanRewardHooks] Blocking set to {}", block);
    }

    bool GetBlocking() {
        return s_Blocking.load(std::memory_order_relaxed);
    }

    void SetCustomText(int32_t tierIndex, const wchar_t* text) {
        {
            std::lock_guard<std::mutex> lock(s_TextMutex);
            if (text && text[0]) {
                s_CustomTexts[tierIndex] = text;
            } else {
                s_CustomTexts.erase(tierIndex);
            }
        }
        LOG("[MimanRewardHooks] Custom text for tier {}: {}", tierIndex, text);
    }

    void ClearCustomText(int32_t tierIndex) {
        std::lock_guard<std::mutex> lock(s_TextMutex);
        s_CustomTexts.erase(tierIndex);
    }

    void ClearAllCustomTexts() {
        std::lock_guard<std::mutex> lock(s_TextMutex);
        s_CustomTexts.clear();
    }

    void OnMimanRewardClaimed(MimanRewardCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
