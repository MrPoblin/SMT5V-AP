#pragma once
#include <cstdint>
#include <functional>

namespace GloryHooks {
    // Fired when AddGodParameterPoint is called with a native (blocked) value.
    using GloryCollectCallback = std::function<void(std::int32_t gloryAmount)>;

    void Setup();
    void OnGloryCollected(GloryCollectCallback cb);
    // When true, AddGodParameterPoint value is zeroed (glory blocked for AP replacement)
    void SetBlockGlory(bool block);
    // AP must set this to true around ProcessEvent calls to AddGodParameterPoint
    // so the pre-hook knows to let AP-initiated glory through.
    extern thread_local bool g_APInitiatedGlory;
}
