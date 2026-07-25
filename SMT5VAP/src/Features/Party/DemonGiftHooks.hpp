#pragma once
#include <cstdint>
#include <functional>

namespace DemonGiftHooks {

    // Fired when a levelled-up demon's gift is received, so Archipelago can
    // track/re-grant it. Argument is the gift item id (0 means no gift and is
    // never reported).
    using DemonGiftCallback = std::function<void(std::int32_t itemId, std::int32_t num)>;

    void Setup();

    // When enabled, demon level-up gifts are observed (logged + reported via the
    // callback). No suppression is performed.
    void SetObserve(bool observe);

    // Register a callback for received demon gifts.
    void OnDemonGift(DemonGiftCallback cb);

    // Fire all registered gift callbacks with a captured item id/amount.
    void FireGiftCallbacks(std::int32_t itemId, std::int32_t num);

} // namespace DemonGiftHooks
