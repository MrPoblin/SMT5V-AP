#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace ItemWindow {

    void Setup();
    void Update();
    void OnMapChanged();

    void ShowItemPopup(std::int32_t itemId, std::int32_t amount = 1);
    void ShowItemPopupCustom(std::int32_t itemId, const wchar_t* customText);

    using ShownCallback = std::function<void(std::int32_t itemId, std::int32_t itemNum, std::int32_t itemAdd)>;
    void OnShown(ShownCallback callback);

} // namespace ItemWindow
