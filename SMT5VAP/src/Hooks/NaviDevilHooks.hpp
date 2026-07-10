#pragma once
#include <cstdint>
#include <functional>

namespace NaviDevilHooks {
    using NaviGimmickCollectedCallback = std::function<void(std::int32_t saveId)>;
    void Setup();
    void OnNaviGimmickCollected(NaviGimmickCollectedCallback cb);
}
