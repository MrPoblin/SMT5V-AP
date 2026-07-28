#pragma once

#include <cstdint>
#include <string>

namespace FusionGating {

    void Setup();

    // Enable/disable the whole feature.
    void SetEnabled(bool enabled);
    bool IsEnabled();
    void SetEssenceGatingEnabled(bool enabled);
    bool IsEssenceGatingEnabled();

    // Build/refresh the race-name table (cached once).
    std::string RaceName(int32_t race);
}
