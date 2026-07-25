#include "EssenceShopHooks.hpp"
#include "src/Archipelago/APState.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <vector>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace EssenceShopHooks {

    // ── FShopItemData POD matching the game's struct ──
    // From SDK: struct FShopItemData { int32 ItemId; bool bNewItem; uint8 Pad_5[0x3]; };
    struct ShopItemData {
        int32 ItemId;
        bool bNewItem;
    };
    static_assert(sizeof(ShopItemData) == 8, "ShopItemData should be 8 bytes");

    // ── Essence detection ──
    // Demon essences in SMT5V have ItemIds in range 311-611
    static bool IsEssence(int32 itemId) {
        return itemId >= 311 && itemId <= 611;
    }

    // ── State ──
    static std::atomic<bool> s_BlockEssences{true};
    static UObject* s_FacilityShopList = nullptr;

    // Cached UFunction pointer for RemoveAt
    static UFunction* s_RemoveAtFn = nullptr;
    static UFunction* s_AddFn = nullptr;

    // Flag to bypass the blocking when doing manual additions
    static std::atomic<bool> s_IsManualAdd{false};

    // ── Cache UFunction pointers ──
    static void CacheFunctions() {
        if (s_RemoveAtFn) return;

        s_RemoveAtFn = UObjectGlobals::FindObject<UFunction>(
            nullptr, STR("/Script/Project.ShopItemList:RemoveAt"));
        if (!s_RemoveAtFn) {
            s_RemoveAtFn = UObjectGlobals::FindObject<UFunction>(
                nullptr, STR("/Script/Project.ShopItemList_C:RemoveAt"));
        }

        s_AddFn = UObjectGlobals::FindObject<UFunction>(
            nullptr, STR("/Script/Project.ShopItemList:Add"));
        if (!s_AddFn) {
            s_AddFn = UObjectGlobals::FindObject<UFunction>(
                nullptr, STR("/Script/Project.ShopItemList_C:Add"));
        }

        if (s_RemoveAtFn) LOG("[EssenceShop] Found ShopItemList.RemoveAt");
        else WARN("[EssenceShop] ShopItemList.RemoveAt NOT FOUND");

        if (s_AddFn) LOG("[EssenceShop] Found ShopItemList.Add");
        else WARN("[EssenceShop] ShopItemList.Add NOT FOUND");
    }

    // ── Setup: hook ShopItemList.Add post-hook ──
    void Setup() {
        LOG("[EssenceShop] Setup...");

        CacheFunctions();

        if (!s_AddFn) {
            WARN("[EssenceShop] ShopItemList.Add NOT FOUND - cannot block essences");
            return;
        }
        if (!s_RemoveAtFn) {
            WARN("[EssenceShop] ShopItemList.RemoveAt NOT FOUND - cannot remove essences");
            return;
        }

        s_AddFn->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (!s_BlockEssences.load()) return;
            // Skip blocking if this is a manual addition from AddItemToShop
            if (s_IsManualAdd.load()) return;

            auto* shopList = Ctx.Context;
            if (!shopList) return;

            // Cache this ShopItemList for later manual additions
            s_FacilityShopList = shopList;

            // ── Read the last item from the TArray ──
            // In this post-hook of Add(), the item has already been appended to the end.
            // TArray<FShopItemData> m_ItemList at offset 0x28:
            //   +0x00: Data*, +0x08: Count(int32), +0x0C: Max(int32)
            static constexpr int32 M_ITEMLIST_OFFSET = 0x28;
            auto* objBytes = static_cast<uint8*>(static_cast<void*>(shopList));
            void* dataPtr = *reinterpret_cast<void**>(objBytes + M_ITEMLIST_OFFSET);
            int32 count = *reinterpret_cast<int32*>(objBytes + M_ITEMLIST_OFFSET + 0x08);

            if (!dataPtr || count <= 0) return;

            int32 itemId = reinterpret_cast<ShopItemData*>(dataPtr)[count - 1].ItemId;

            if (!IsEssence(itemId)) return; // Not an essence, let it through

            LOG("[EssenceShop] Blocking essence ItemId={} from shop list (index {})", itemId, count - 1);

            // Remove the last item (which is the essence we just detected)
            struct { int32 Index; } removeParams;
            removeParams.Index = count - 1;
            shopList->ProcessEvent(s_RemoveAtFn, &removeParams);

            // ── Check if all essences are cleared; if so, batch-add APState ones ── buggy, but it works
            // Re-read TArray after RemoveAt
            void* newDataPtr = *reinterpret_cast<void**>(objBytes + M_ITEMLIST_OFFSET);
            int32 newCount = *reinterpret_cast<int32*>(objBytes + M_ITEMLIST_OFFSET + 0x08);

            bool anyEssencesRemain = false;
            if (newDataPtr && newCount > 0) {
                for (int32 i = 0; i < newCount; ++i) {
                    if (IsEssence(reinterpret_cast<ShopItemData*>(newDataPtr)[i].ItemId)) {
                        anyEssencesRemain = true;
                        break;
                    }
                }
            }

            if (!anyEssencesRemain) {
                auto apEssences = APState::Essences::GetAllEssences();
                if (!apEssences.empty()) {
                    LOG("[EssenceShop] All essences cleared. Re-adding {} earned essence(s) from APState", apEssences.size());
                    for (auto id : apEssences) {
                        AddItemToShop(id);
                    }
                }
            }
        });

        LOG("[EssenceShop] Setup complete");
    }

    // ── Manually add an item to the facility shop ──
    void AddItemToShop(int32_t itemId) {
        if (!s_FacilityShopList || !s_AddFn) {
            WARN("[EssenceShop] Cannot add item {}: facility shop not ready", itemId);
            return;
        }

        struct AddParams {
            ShopItemData Data;
        };
        AddParams params;
        params.Data.ItemId = itemId;
        params.Data.bNewItem = true;

        // Set bypass flag so the post-hook doesn't remove our manual add
        s_IsManualAdd.store(true);
        s_FacilityShopList->ProcessEvent(s_AddFn, &params);
        s_IsManualAdd.store(false);

        LOG("[EssenceShop] Added item {} to facility shop", itemId);
    }

    void SetBlockEssences(bool block) {
        s_BlockEssences.store(block);
        LOG("[EssenceShop] SetBlockEssences({})", block);
    }
}
