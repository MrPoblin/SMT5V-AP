#pragma once
#include <cstdint>
#include <unordered_set>
#include <mutex>
#include <initializer_list>
#include <string>

namespace MissionRewardHook {
    enum class FilterMode {
        Disabled = 0,   // Pass-through: no filtering
        BlockAll = 1,   // Zero macca + exp + item arrays
        KeepExp  = 2,   // Zero macca + item arrays, keep exp
    };

    void Setup();

    void SetMode(FilterMode mode);
    FilterMode GetMode();

    void AddException(int32_t missionId);
    void RemoveException(int32_t missionId);
    void ClearExceptions();
    void SetExceptions(std::initializer_list<int32_t> ids);
    bool IsException(int32_t missionId);

    // ── Custom reward text for mission menu ──
    // Overrides the reward description shown in the quest log/mission menu.
    // Uses ItemTableInjector to register a magic item ID (1000000+missionId)
    // with the game's native item DataTable so the menu shows the custom text.
    void SetCustomRewardText(int32_t missionId, const wchar_t* text);
    void ClearCustomRewardText(int32_t missionId);
    void ClearAllCustomRewardTexts();

    // ── Direct item name override ──
    // Overrides BPL_ItemData:ItemGetName return value for a specific item ID.
    // Use this only for non-menu contexts (popups, etc.).
    void RegisterCustomItemName(int32_t itemId, const wchar_t* name);
    void ClearCustomItemName(int32_t itemId);
    void ClearAllCustomItemNames();
}
