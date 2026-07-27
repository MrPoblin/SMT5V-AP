#pragma once
#include <cstdint>
#include <string>

namespace ItemTableInjector {
    void Setup();

    void RegisterCustomItem(int32_t id, const wchar_t* name);
    void ClearCustomItem(int32_t id);
    void ClearAllCustomItems();
}
