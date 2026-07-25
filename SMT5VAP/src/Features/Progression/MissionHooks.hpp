#pragma once
#include <cstdint>
#include <functional>

namespace MissionHooks {
    using MissionCallback = std::function<void(std::int32_t missionId)>;
    void Setup();
    void OnMissionCompleted(MissionCallback cb);
}
