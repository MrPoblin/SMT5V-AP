#pragma once
#include <cstdint>
#include <functional>

namespace UseItemHook {
    // Fired when any item is used from the camp menu.
    using ItemUseCallback = std::function<void(std::int32_t itemId)>;

    void Setup();
    void OnItemUsed(ItemUseCallback cb);
}
