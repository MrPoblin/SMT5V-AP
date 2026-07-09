#pragma once
#include <cstdint>
#include <functional>

namespace RelicHooks {
    using RelicCollectCallback = std::function<void(std::int32_t relicId)>;
    void Setup();
    void OnRelicCollected(RelicCollectCallback cb);
}
