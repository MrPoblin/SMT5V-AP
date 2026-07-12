#pragma once
#include <cstdint>
#include <functional>

// Does NOT block macca from battles
namespace MaccaBlocker {
    using MaccaBlockedCallback = std::function<void(int32_t amount)>;

    // Guard: place on the stack before your own ProcessEvent calls to bypass blocking
    struct BypassGuard {
        BypassGuard();
        ~BypassGuard();
        BypassGuard(const BypassGuard&) = delete;
        BypassGuard& operator=(const BypassGuard&) = delete;
    };

    void Setup();

    // Toggle macca blocking on/off (default: off)
    void SetBlockMacca(bool block);

    // Callback fires when macca is intercepted
    void OnMaccaBlocked(MaccaBlockedCallback cb);
}
