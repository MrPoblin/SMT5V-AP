#pragma once

#include <cstdint>
#include <string>

namespace FusionGating {
    // Race gating for Demon Fusion ("Unite"), driven by APState::FusionRaces.
    // Two layers:
    //   A) Filter the search-result list so gated-race demons don't appear.
    //   B) Hard-block at the commit (AddUnitePartyMember) as a safety net.

    void Setup();

    // Enable/disable the whole feature (used by AP slot data later).
    void SetEnabled(bool enabled);
    bool IsEnabled();
    void SetEssenceGatingEnabled(bool enabled);
    bool IsEssenceGatingEnabled();

    // Build/refresh the race-name table (cached once).
    std::string RaceName(int32_t race);
}
