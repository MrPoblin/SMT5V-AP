#pragma once

#include <string>
#include <format>

namespace CustomPopups {
    void Setup();
    void Update();                                // pump the state machine (call from on_update)
    void OnMapChanged();                          // cleanup + flush deferred (call from GameState::OnMapChanged)

    void ShowNotification(const wchar_t* msg);    // wide (core)
    void ShowNotification(const std::string& msg);            // narrow (UTF-8) -> wide
    template<typename... Args>
    void ShowNotificationF(const std::string& fmt, Args&&... args) {
        ShowNotification(std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...)));
    }

    // Priority: shown immediately after the currently-active message (jumps the queue).
    void ShowNotificationPriority(const wchar_t* msg);
    void ShowNotificationPriority(const std::string& msg);
    template<typename... Args>
    void ShowNotificationPriorityF(const std::string& fmt, Args&&... args) {
        ShowNotificationPriority(std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...)));
    }

    // Drop all pending (queued) messages. The currently-active widget is left alone
    // unless clearActive is true.
    void ClearQueue(bool clearActive = false);
}
