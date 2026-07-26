#pragma once
#include <cstdint>
#include <unordered_set>
#include <mutex>
#include <initializer_list>

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
}
