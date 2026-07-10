#pragma once
#include <cstdint>
#include <functional>

namespace AogamiHooks {
    // Fired when an Aogami debris (husk) is collected.
    // Provides the tableIndex of the collected debris.
    using AogamiDebrisCollectedCallback = std::function<void(std::int32_t tableIndex)>;
    void Setup();
    void OnAogamiDebrisCollected(AogamiDebrisCollectedCallback cb);
}
