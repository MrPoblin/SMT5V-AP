#pragma once
#include <cstdint>
#include <functional>

namespace AogamiHooks {
    // Fired when an Aogami debris (husk) is collected.
    // Provides the tableIndex of the collected debris.
    using AogamiDebrisCollectedCallback = std::function<void(std::int32_t tableIndex)>;
    void Setup();
    void OnAogamiDebrisCollected(AogamiDebrisCollectedCallback cb);

    // Replace the item ID that Aogami husks give when collected.
    // Set to 0 to give nothing. Set to -1 to disable replacement (give original item).
    void SetReplaceItemId(std::int32_t itemId);
}
