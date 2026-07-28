#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace MissionScoutManager {

    // Callback when the active mission list is loaded/refreshed.
    // Provides a list of all currently active mission IDs (main + sub).
    using MissionsLoadedCallback = std::function<void(const std::vector<int32_t>& missionIds)>;

    // Callback when a new mission is entered/added.
    using MissionAddedCallback = std::function<void(int32_t missionId)>;

    void Setup();

    // Scan active missions from game data now. Returns true if found.
    bool Rescan();

    // Get the last known list of active mission IDs
    const std::vector<int32_t>& GetActiveMissions();

    // Subscribe to events
    void OnMissionsLoaded(MissionsLoadedCallback callback);
    void OnMissionAdded(MissionAddedCallback callback);
}
