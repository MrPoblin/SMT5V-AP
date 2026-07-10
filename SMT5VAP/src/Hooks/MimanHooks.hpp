#pragma once
#include <cstdint>
#include <functional>

namespace MimanHooks {
    using MimanFoundCallback = std::function<void(std::int32_t mimanId)>;
    void Setup();
    void OnMimanFound(MimanFoundCallback cb);
}
