#pragma once
#include <cstdint>
#include <functional>

namespace GloryHooks {
    // Fired for both glory (gloryAmount != 0, pieceId == -1) and piece pickups.
    using GloryCollectCallback = std::function<void(std::int32_t pieceId, std::int32_t gloryAmount)>;
    void Setup();
    void OnGloryCollected(GloryCollectCallback cb);
}
