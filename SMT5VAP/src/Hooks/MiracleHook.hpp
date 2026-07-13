#pragma once
#include <cstdint>

namespace MiracleHook {

    // RAII guard used by AP code paths that legitimately learn miracles, so the
    // purchase-block hook ignores those calls.
    struct BypassGuard {
        BypassGuard();
        ~BypassGuard();
        BypassGuard(const BypassGuard&) = delete;
        BypassGuard& operator=(const BypassGuard&) = delete;
    };

    void Setup();

    // When true (default), miracle purchases triggered by the game (abscesses,
    // quests, etc.) are blocked. AP grants miracles via GrantMiracle instead,
    // which bypasses the block using BypassGuard. Once learned, a miracle stays
    // learned — there is no need to track granted ids.
    void SetBlockUnlocks(bool block);

    // Force-learn a miracle through BPL_GodParameter::GodParameterSkillLearning
    // with forced=true, bypassing the purchase block. This is the Archipelago
    // "buy" path: AP directly grants the miracle.
    bool GrantMiracle(int32_t skillId);

} // namespace MiracleHook
