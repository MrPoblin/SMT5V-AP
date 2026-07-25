#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>
#include <initializer_list>

namespace ChestHooks {
    using ChestOpenCallback = std::function<void(std::int32_t takaraSaveId)>;

    void Setup();
    void OnChestOpened(ChestOpenCallback cb);

    // Master switch: zero out item/num/macca for non-excluded chests
    void SetEmptyAllChests(bool empty);

    // Replacement values applied to non-excluded chests.
    // Set to -1 to leave that field unchanged; 0+ overrides the field.
    void SetReplacementItem(int32_t itemId);
    void SetReplacementAmount(int32_t amount);
    void SetReplacementMacca(int32_t macca);
    // Convenience: set all three at once (use -1 to leave a field unchanged)
    void SetReplacement(int32_t itemId = -1, int32_t amount = -1, int32_t macca = -1);

    // Exclusion list: chest table IDs to leave completely untouched
    void AddExcludedChest(int32_t tableId);
    void RemoveExcludedChest(int32_t tableId);
    void ClearExcludedChests();
    void SetExcludedChests(std::initializer_list<int32_t> ids);
    bool IsChestExcluded(int32_t tableId);
}
