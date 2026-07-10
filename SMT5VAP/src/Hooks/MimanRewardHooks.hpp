#pragma once
#include <cstdint>
#include <functional>

namespace MimanRewardHooks {
    using MimanRewardCallback = std::function<void(std::int32_t rewardId)>;
    void Setup();
    void OnMimanRewardClaimed(MimanRewardCallback cb);
}
