#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <format>

namespace InfoWindow {

    void Setup();
    void Update();

    void ShowNotification(const wchar_t* msg);
    void ShowNotification(const std::string& msg);
    void ShowNotificationPriority(const wchar_t* msg);
    void ShowNotificationPriority(const std::string& msg);
    void ClearQueue(bool clearActive = false);

    template<typename... Args>
    void ShowNotificationF(const std::string& fmt, Args&&... args) {
        ShowNotification(std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...)));
    }
    template<typename... Args>
    void ShowNotificationPriorityF(const std::string& fmt, Args&&... args) {
        ShowNotificationPriority(std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...)));
    }

    void OnMapChanged();

    using ShownCallback = std::function<void()>;
    void OnShown(ShownCallback callback);

} // namespace InfoWindow
