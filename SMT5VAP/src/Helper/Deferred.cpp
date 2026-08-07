#include "Deferred.hpp"

#include "src/GameState.hpp"
#include "src/Log/Log.hpp"

#include <vector>

namespace {

    // Frames the world must remain save-loaded and on a settled map before
    // queued tasks start running. ~2s at 60fps; gives the map bootstrap time to
    // finish so caches are built against settled state.
    constexpr int STABLE_FRAME_THRESHOLD = 120;

    std::vector<std::function<void()>> g_queue;
    int g_stableFrames = 0;

    // The save-resume path loads its map WITHOUT going through UEngine::LoadMap,
    // so GameState::IsTransitioning() never flips during that load (confirmed in
    // crash logs: CompendiumTick polls with !IsTransitioning() while the target
    // map is still streaming in). "Not transitioning" therefore CANNOT be used
    // to detect that a map is ready. Instead we require a completed map load
    // AFTER the tasks were enqueued: Enqueue() clears this flag and
    // OnMapLoaded() sets it, so queued work only ever runs on a settled map.
    bool g_mapChangedSinceEnqueue = true;
    bool g_mapHookRegistered = false;

    void OnMapLoaded(const std::wstring& /*mapName*/) {
        g_mapChangedSinceEnqueue = true;
        if (!g_queue.empty()) {
            LOG("[Deferred] Map load completed, {} task(s) may now run", g_queue.size());
        }
    }

    void EnsureMapHookRegistered() {
        if (!g_mapHookRegistered) {
            g_mapHookRegistered = true;
            GameState::OnMapChanged(OnMapLoaded);
        }
    }

    bool WorldIsStable() {
        return GameState::IsSaveLoaded()
            && g_mapChangedSinceEnqueue
            && !GameState::IsTransitioning()
            && !GameState::MapName().empty();
    }

} // namespace

namespace Deferred {

    void Enqueue(std::function<void()> task) {
        if (task) {
            EnsureMapHookRegistered();
            // Require the next completed map load before running. Cleared on the
            // first enqueue of a save-load burst so tasks do not run while the
            // target map is still streaming in.
            g_mapChangedSinceEnqueue = false;
            g_queue.emplace_back(std::move(task));
        }
    }

    void Tick() {
        if (g_queue.empty()) {
            return;
        }

        if (!WorldIsStable()) {
            g_stableFrames = 0;
            return;
        }

        if (g_stableFrames < STABLE_FRAME_THRESHOLD) {
            ++g_stableFrames;
            return;
        }

        // Run exactly one task per tick so the game stays responsive and no
        // single frame pays the whole (multi-thousand ProcessEvent) cost. The
        // stable-frame counter is NOT reset between tasks so the queued burst
        // drains over consecutive frames once the world has settled.
        auto task = std::move(g_queue.front());
        g_queue.erase(g_queue.begin());

        LOG("[Deferred] Running deferred task ({} remaining)", g_queue.size());
        task();
    }

    void Clear() {
        g_queue.clear();
        g_stableFrames = 0;
    }

} // namespace Deferred
