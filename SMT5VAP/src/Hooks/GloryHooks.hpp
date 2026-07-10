#pragma once
#include <cstdint>
#include <functional>

namespace GloryHooks {
    // Fired when AddGodParameterPoint is called with a blocked value.
    using GloryCollectCallback = std::function<void(std::int32_t gloryAmount)>;
    void Setup();
    void OnGloryCollected(GloryCollectCallback cb);
    // When true, AddGodParameterPoint value is zeroed (glory blocked for AP replacement)
    void SetBlockGlory(bool block);
}
