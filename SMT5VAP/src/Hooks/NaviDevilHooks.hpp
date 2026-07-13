#pragma once
#include <cstdint>
#include <functional>

namespace NaviDevilHooks {
    using NaviGimmickCollectedCallback = std::function<void(std::int32_t saveId)>;
    using NaviDevilChangedCallback = std::function<void(std::int32_t devilID)>;

    // Fires on some spots collected
    void SetupUniqueSaveID();
    void OnNaviGimmickCollected(NaviGimmickCollectedCallback cb);

    // Fires when the navigator demon changes (provides the new navigator's DevilID)
    void SetupNaviDevilChanged();
    void OnNaviDevilChanged(NaviDevilChangedCallback cb);

    // Hybrid: uses AddCheckCounter as trigger + filtered SetGimmickExist for IDs
    // Only fires when both fire on the same frame (eliminates map-load spam)
    // The Ids are unique per map
    // Total number of locations (source: frontiernav.net)
    // Minato: 225
    // Shinagawa: 265
    // Chiyoda: 274
    // Shinjuku: 455
    // Taito: 303
    void SetupAddCheckCounter();        // post-hook, sets trigger flag
    void SetupSetGimmickExistFiltered(); // pre-hook, only fires when trigger is active

    // Item replacement: post-hooks GetNaviDevilGimmickData/FromID
    // SetBlockItems(false) to disable entirely (passthrough)
    // SetReplaceMacca(0) to suppress (give nothing)
    // SetReplaceMacca(N) to replace with N macca
    void SetupBlockItems();
    void SetBlockItems(bool block);
    void SetReplaceMacca(std::int32_t amount);
}
