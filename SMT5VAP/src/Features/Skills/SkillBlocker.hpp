#pragma once
#include <cstdint>

namespace SkillBlocker {

    // E_SKILL_ICON values matching the game's enum
    enum class SkillIcon : uint8_t {
        Physical = 0,
        Fire = 1,
        Ice = 2,
        Electric = 3,
        Force = 4,
        Light = 5,
        Dark = 6,
        Almighty = 7,
        Ailment = 8,
        Support = 9,
        Recovery = 10,
        Special = 11,
        Two = 12,
    };

    // Get the icon category for a skill ID. Returns -1 on failure.
    int32_t GetSkillIcon(int32_t skillId);

    // Check if a skill is blocked in battle context.
    // Respects scope (protag only / demon only / both).
    // partyIndex: the party member index, -1 if unknown
    bool IsSkillBlockedInBattle(int32_t skillId, int32_t partyIndex = -1);

    // Check if a skill's category is blocked (ignores scope).
    bool IsSkillCategoryBlocked(int32_t skillId);

    // Register hooks. Call at mod init.
    void Setup();

    // Build skill icon cache. Call after save load (not at init).
    void BuildCache();

    // Allow the mod's own skill-grant calls to bypass blocking.
    // Call before granting a skill and unset after.
    void SetGrantBypass(bool granting);

    // Enable/disable discovery logging of all ProcessEvent functions.
    // Useful for finding what actually fires during battle.
    void SetDiscoveryLogging(bool enabled);
    bool IsDiscoveryLoggingEnabled();

} // namespace SkillBlocker