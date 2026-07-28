#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace MimanRewardHooks {
    using MimanRewardCallback = std::function<void(std::int32_t rewardId)>;

    void Setup();

    // Block/allow rewards (items are zeroed when the shop reads them)
    void SetBlocking(bool block);
    bool GetBlocking();

    // Custom display text for each reward tier (0-39).
    // The shop calls GetMimanRewardItemSetName(tierIndex) to get tier name.
    void SetCustomText(int32_t tierIndex, const wchar_t* text);
    void ClearCustomText(int32_t tierIndex);
    void ClearAllCustomTexts();

    // Fires when a reward is claimed (SetShopMimanRewardFlag called with sw=true)
    void OnMimanRewardClaimed(MimanRewardCallback cb);
}
