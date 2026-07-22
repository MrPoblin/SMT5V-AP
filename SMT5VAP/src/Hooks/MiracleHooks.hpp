#pragma once
#include <cstdint>

namespace MiracleHooks {

    struct BypassGuard {
        BypassGuard();
        ~BypassGuard();
        BypassGuard(const BypassGuard&) = delete;
        BypassGuard& operator=(const BypassGuard&) = delete;
    };

    void Setup();

    void SetBlockUnlocks(bool block);

    bool GrantMiracle(int32_t skillId);

    void UnlockForPurchase(int32_t skillId);

    void ResetUnlocks();

} // namespace MiracleHooks
