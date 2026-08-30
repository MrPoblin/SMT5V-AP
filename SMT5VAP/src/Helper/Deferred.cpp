#include "Deferred.hpp"

#include "src/GameState.hpp"
#include "src/Log/Log.hpp"

#include <chrono>
#include <queue>
#include <vector>

namespace {

    // Frames the world must remain save-loaded and on a settled map before
    // queued tasks start running. ~2s at 60fps; gives the map bootstrap time to
    // finish so caches are built against settled state.
    constexpr int STABLE_FRAME_THRESHOLD = 120;

    struct DelayedTask {
        std::function<void()> task;
        std::chrono::steady_clock::time_point readyAt;

        // priority_queue is a max-heap by default; invert the comparison so
        // the earliest readyAt sits at the top.
        bool operator<(const DelayedTask& other) const {
            return readyAt > other.readyAt;
        }
    };

    std::vector<std::function<void()>> g_queue;
    std::priority_queue<DelayedTask> g_delayedQueue;
    std::vector<std::function<void()>> g_immediateQueue;
    std::priority_queue<DelayedTask> g_immediateDelayedQueue;
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

    bool WorldIsSettled() {
        return GameState::IsSaveLoaded()
            && !GameState::IsTransitioning()
            && !GameState::MapName().empty();
    }

    bool WorldIsStable() {
        // The save-resume path loads its map WITHOUT going through
        // UEngine::LoadMap, so IsTransitioning() never flips during that load.
        // Requiring a completed map load AFTER the enqueue is what guarantees
        // the map has finished streaming in.
        return WorldIsSettled() && g_mapChangedSinceEnqueue;
    }

} // namespace

namespace Deferred {

    void Enqueue(std::function<void()> task) {
        if (task) {
            g_immediateQueue.emplace_back(std::move(task));
        }
    }

    void DelayedEnqueue(std::function<void()> task, double delaySeconds) {
        if (task) {
            auto readyAt = std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(delaySeconds));
            g_immediateDelayedQueue.push({std::move(task), readyAt});
        }
    }

    void EnqueueAfterMapChange(std::function<void()> task) {
        if (task) {
            EnsureMapHookRegistered();
            // Require the next completed map load before running. Cleared on the
            // first enqueue of a save-load burst so tasks do not run while the
            // target map is still streaming in.
            g_mapChangedSinceEnqueue = false;
            g_queue.emplace_back(std::move(task));
        }
    }

    void DelayedEnqueueAfterMapChange(std::function<void()> task, double delaySeconds) {
        if (task) {
            EnsureMapHookRegistered();
            // Same map-load requirement as EnqueueAfterMapChange: the task only
            // ever runs on a settled map.
            g_mapChangedSinceEnqueue = false;
            auto readyAt = std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(delaySeconds));
            g_delayedQueue.push({std::move(task), readyAt});
        }
    }

    void Tick() {
        // --- Immediate mechanism: no map-change requirement, no frame window.
        // Run as soon as the world is settled, one task per tick.

        // Promote the oldest delayed task (if any) whose delay has elapsed.
        if (!g_immediateDelayedQueue.empty()
            && g_immediateDelayedQueue.top().readyAt <= std::chrono::steady_clock::now()) {
            auto task = std::move(g_immediateDelayedQueue.top().task);
            g_immediateDelayedQueue.pop();
            if (task) {
                g_immediateQueue.emplace_back(std::move(task));
            }
        }

        if (!g_immediateQueue.empty() && WorldIsSettled()) {
            auto task = std::move(g_immediateQueue.front());
            g_immediateQueue.erase(g_immediateQueue.begin());

            LOG("[Deferred] Running immediate task ({} remaining)", g_immediateQueue.size());
            task();
        }

        // --- Map-change mechanism: requires a new map load + settle window.
        if (!g_delayedQueue.empty()
            && g_delayedQueue.top().readyAt <= std::chrono::steady_clock::now()) {
            auto task = std::move(g_delayedQueue.top().task);
            g_delayedQueue.pop();
            if (task) {
                g_queue.emplace_back(std::move(task));
            }
        }

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
        g_delayedQueue = {};
        g_immediateQueue.clear();
        g_immediateDelayedQueue = {};
        g_stableFrames = 0;
    }

} // namespace Deferred
