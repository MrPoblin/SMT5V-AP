#pragma once
#include <cstdint>
#include <functional>
#include <unordered_set>

namespace VendingHooks {
    using VendingCollectCallback = std::function<void(std::int32_t saveId)>;

    void Setup();
    void OnVendingCollected(VendingCollectCallback cb);

    // Set the item/amount/rate that vending machines give.
    // Pass -1 for any field to leave it unchanged.
    void SetReplacementItem(int32_t itemId);
    void SetReplacementAmount(int32_t amount);
    void SetReplacementRate(int32_t rate);
    void SetReplacement(int32_t itemId = -1, int32_t amount = -1, int32_t rate = -1);

    // ── Tracking set (will be replaced by AP infrastructure later) ──
    bool IsVendingCollected(int32_t saveId);
    void MarkVendingCollected(int32_t saveId);
    void ClearVendingCollected();
}
