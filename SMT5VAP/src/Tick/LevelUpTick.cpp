#include "LevelUpTick.hpp"
#include "src/Functions/LevelFunctions.hpp"
#include "src/Log/Log.hpp"
#include <mutex>
#include <vector>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace LevelUpTick {

// ── Polling state (3s real-time interval, framerate-independent) ──
static auto s_LastPollTime = std::chrono::steady_clock::now();
static constexpr auto POLL_INTERVAL = std::chrono::seconds(3);

// ── Callbacks ──
static std::vector<LevelUpCallback> s_Callbacks;
static std::mutex s_Mutex;

// ── Level tracking / dedup ──
static int32 s_CachedLevel = -1;
static bool s_LevelKnown = false;

static void Fire(int32 oldLevel, int32 newLevel) {
    LOG("[LevelUpTick] Protagonist levelled up: {} -> {} (gained {})",
        oldLevel, newLevel, newLevel - oldLevel);
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& cb : s_Callbacks) cb(oldLevel, newLevel);
}

// Fires the callback at most once per level increase.
static void DetectLevelChange(int32 level) {
    if (level < 1) return;
    if (!s_LevelKnown) {
        s_CachedLevel = level;
        s_LevelKnown = true;
        return;
    }
    if (level > s_CachedLevel) {
        Fire(s_CachedLevel, level);
        s_CachedLevel = level;
    } else if (level < s_CachedLevel) {
        // Level dropped (e.g. NG+ reset) — resync baseline, don't fire.
        s_CachedLevel = level;
    }
}

// ── Detection ──
// The protagonist's level is applied through a C++ path that bypasses any
// UFunction hook, so we poll GetPlayerLevel each frame (from on_update). This
// reliably catches every level-up (real battles, exp items, etc.) and computes
// the gain from the before/after result.
void Poll() {
    // Real-time throttle (framerate-independent)
    auto now = std::chrono::steady_clock::now();
    if (now - s_LastPollTime < POLL_INTERVAL) return;
    s_LastPollTime = now;

    DetectLevelChange(LevelFunctions::GetProtagonistLevel());
}

void OnLevelUp(LevelUpCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

void Reset() {
    s_LevelKnown = false;
    s_CachedLevel = -1;
}

}
