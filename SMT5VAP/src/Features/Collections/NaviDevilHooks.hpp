#pragma once
#include <cstdint>
#include <functional>

namespace NaviDevilHooks {
    using NaviDevilChangedCallback = std::function<void(std::int32_t devilID)>;

    // Fires when the navigator demon changes (provides the new navigator's DevilID)
    void SetupNaviDevilChanged();
    void OnNaviDevilChanged(NaviDevilChangedCallback cb);

    // Item replacement: post-hooks GetNaviDevilGimmickData/FromID
    // SetBlockItems(false) to disable entirely (passthrough)
    // SetReplaceMacca(0) to suppress (give nothing)
    // SetReplaceMacca(N) to replace with N macca
    void SetupBlockItems();
    void SetBlockItems(bool block);
    void SetReplaceMacca(std::int32_t amount);
}
