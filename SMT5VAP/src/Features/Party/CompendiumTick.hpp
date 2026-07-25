#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace CompendiumTick {

using DemonAcquiredCallback = std::function<void(int32_t devilID)>;

void Poll();  // Call from on_update (only when save is loaded)
void OnDemonAcquired(DemonAcquiredCallback cb);

} // namespace CompendiumTick
