#pragma once
#include <cstdint>
#include <functional>

namespace ChestHooks {
    using ChestOpenCallback = std::function<void(std::int32_t takaraSaveId)>;
    void Setup();
    void OnChestOpened(ChestOpenCallback cb);
}
