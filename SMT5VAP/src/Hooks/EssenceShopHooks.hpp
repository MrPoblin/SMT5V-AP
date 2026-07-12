#pragma once
#include <cstdint>
#include <functional>

namespace EssenceShopHooks {
    using EssenceShopCallback = std::function<void(std::int32_t itemId)>;

    // Hooks ShopItemList.Add to block auto-populated essences from the facility shop
    // and provides a function to manually add items to the facility shop
    void Setup();

    // Manually add an item (essence) to the facility shop's ShopItemList
    void AddItemToShop(std::int32_t itemId);

    // Toggle essence blocking on/off (default: on)
    void SetBlockEssences(bool block);

    // Callback when an essence is blocked from auto-population
    void OnEssenceBlocked(EssenceShopCallback cb);
}
