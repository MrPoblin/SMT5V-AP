#pragma once
#include <cstdint>
#include <unordered_set>
#include <mutex>
#include <initializer_list>
#include <string>
#include <functional>

namespace MissionRewardHook {
    enum class FilterMode {
        Disabled = 0,   // Pass-through: no filtering
        BlockAll = 1,   // Zero macca + exp + item arrays
        KeepExp  = 2,   // Zero macca + item arrays, keep exp
    };

    // Callback when a mission reward with custom text is shown in the menu.
    // Args: missionId, customText
    using RewardShownCallback = std::function<void(int32_t missionId, const std::wstring& customText)>;

    void Setup();

    void SetMode(FilterMode mode);
    FilterMode GetMode();

    void AddException(int32_t missionId);
    void RemoveException(int32_t missionId);
    void ClearExceptions();
    void SetExceptions(std::initializer_list<int32_t> ids);
    bool IsException(int32_t missionId);

    // ── Custom reward text for mission menu ──
    // Adds a custom reward line to the quest log/mission menu.
    // The original rewards are preserved; the custom text appears as an additional item.
    // Uses a magic item ID (1000000+missionId) with a direct ItemGetName hook.
    void SetCustomRewardText(int32_t missionId, const wchar_t* text);
    void ClearCustomRewardText(int32_t missionId);
    void ClearAllCustomRewardTexts();

    // ── Direct item name override ──
    // Overrides ItemGetName return value for a specific item ID.
    void RegisterCustomItemName(int32_t itemId, const wchar_t* name);
    void ClearCustomItemName(int32_t itemId);
    void ClearAllCustomItemNames();

    // ── Callback when a mission reward is shown ──
    // Fires when GetMissionRewardMsg is called for a mission with custom text.
    void OnRewardShown(RewardShownCallback callback);
}
