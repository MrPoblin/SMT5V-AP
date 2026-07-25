#pragma once
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <initializer_list>

namespace ItemBlocker {
    using ItemBlockedCallback = std::function<void(int32_t itemId, int32_t num)>;

    // Guard: place on the stack before your own ProcessEvent calls to bypass blocking
    struct BypassGuard {
        BypassGuard();
        ~BypassGuard();
        BypassGuard(const BypassGuard&) = delete;
        BypassGuard& operator=(const BypassGuard&) = delete;
    };

    void Setup();

    // Mode: block all items regardless of ID
    void SetBlockAll(bool block);

    // Selective blocking by item ID
    void BlockItemId(int32_t id);
    void UnblockItemId(int32_t id);
    void SetBlockedIds(std::initializer_list<int32_t> ids);
    bool IsItemBlocked(int32_t id);

    // Callback fires when a blocked item grant is intercepted
    void OnItemBlocked(ItemBlockedCallback cb);
}
