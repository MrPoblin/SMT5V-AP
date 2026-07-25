#pragma once
#include <cstdint>
#include <functional>
#include <Unreal/NameTypes.hpp>

namespace DevilStatueHooks {
    using DevilStatueCollectCallback = std::function<void(const RC::Unreal::FName& flagName)>;
    void Setup();
    void OnDevilStatueCollected(DevilStatueCollectCallback cb);
}
