#pragma once
#include <cstdint>
#include <functional>

namespace NaviDevilHooks {
    using NaviGimmickCollectedCallback = std::function<void(std::int32_t saveId)>;

    // Fires on some spots collected
    void SetupUniqueSaveID();
    void OnNaviGimmickCollected(NaviGimmickCollectedCallback cb);

    // Hybrid: uses AddCheckCounter as trigger + filtered SetGimmickExist for IDs
    // Only fires when both fire on the same frame (eliminates map-load spam)
    // The Ids are unique per map so we need to find the highest value per map to know how many there are (if there are no gaps)
    // Minato:
    // Shinagawa:
    // Chiyoda: 
    // Shinjuku: 448 (near government building, so assumed last)
    // Taito:
    void SetupAddCheckCounter();        // post-hook, sets trigger flag
    void SetupSetGimmickExistFiltered(); // pre-hook, only fires when trigger is active

}
