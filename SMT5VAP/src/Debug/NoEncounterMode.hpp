#pragma once
#include <chrono>

class NoEncounterMode {
public:
    static bool IsEnabled() { return s_Enabled; }
    static void SetEnabled(bool enabled);

    static void Setup();
    static void Tick();

private:
    static inline bool s_Enabled{false};
    static inline auto s_LastApplyTime = std::chrono::steady_clock::now();

    static void Apply();
};
