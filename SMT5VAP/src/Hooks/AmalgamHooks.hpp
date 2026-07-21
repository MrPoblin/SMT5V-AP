#pragma once
#include <cstdint>
#include <functional>

namespace AmalgamHooks {
    // Fired when SetShinseiFlag is called with flag=true (Amalgam/Itsuzuka collected)
    using ShinseiCollectCallback = std::function<void(std::int32_t shinseiId)>;
    void Setup();
    void OnShinseiCollected(ShinseiCollectCallback cb);
}
