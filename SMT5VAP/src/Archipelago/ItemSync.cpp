#include "ItemSync.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include "src/Helper/Deferred.hpp"
#include "src/Features/Save/SaveHooks.hpp"
#include "src/Archipelago/APManager.hpp"
#include "Archipelago.h"
#include <mutex>
#include <map>
#include <chrono>
#include <algorithm>

namespace ItemSync {

using namespace std::chrono;

namespace {

    std::mutex s_Mutex;

    // Copies received since the last confirmed save, per item id. This is what
    // gets restored after a death. Reset on reconnect so the server's refire
    // stream rebuilds exact counts (including multiple copies of the same id).
    std::map<int64_t, uint32_t> s_PendingGrant;

    // Sub-counter of s_PendingGrant: which pending copies are already physically
    // present in the loaded world. Cleared on save-load (world rebuilt) and on a
    // confirmed save. Survives a reconnect, which is what lets the resync
    // reconcile avoid re-granting copies that were never lost to a death.
    std::map<int64_t, uint32_t> s_GrantedInWorld;

    // Item ids whose grants have been physically delivered to the handler at
    // least once during the current unconfirmed window. Unlike s_GrantedInWorld
    // this survives the return-to-title (world teardown), which is exactly how a
    // death save-load regrant later knows the player already saw the item
    // (seen=true) and can suppress the repeat notification. Cleared alongside
    // the other state on confirm + seed change.
    std::map<int64_t, bool> s_SeenGranted;

    struct GrantRequest {
        int64_t itemId;
        uint32_t count;
        bool seen;
    };
    // Grants queued for the game thread. Drained in Tick().
    std::vector<GrantRequest> s_GrantQueue;

    // True while the server is replaying the unconfirmed item batch after a
    // reconnect. During the window, deliveries only accumulate into
    // s_PendingGrant (no live grant); the difference is granted when the window
    // ends.
    bool s_InResync{ false };
    steady_clock::time_point s_ResyncStart{};
    constexpr auto RESYNC_WINDOW = std::chrono::milliseconds(2000);

    GrantFn s_GrantHandler;

} // namespace

// Forward declarations at ItemSync scope (used before their definitions).
void DoSaveLoadRegrant();
void EndResyncWindow();
void DrainGrantQueue();

void SetGrantHandler(GrantFn fn) {
    s_GrantHandler = std::move(fn);
}

void OnResyncStart() {
    std::lock_guard lock(s_Mutex);
    if (s_InResync) return;
    s_InResync = true;
    s_ResyncStart = steady_clock::now();
    s_PendingGrant.clear();
    LOG("[ItemSync] Resync window started (reconnect)");
}

void OnDisconnected() {
    std::lock_guard lock(s_Mutex);
    if (s_InResync) {
        s_InResync = false;
        LOG("[ItemSync] Disconnected - resync window cancelled");
    }
}

void OnSeedChanged() {
    std::lock_guard lock(s_Mutex);
    s_PendingGrant.clear();
    s_GrantedInWorld.clear();
    s_SeenGranted.clear();
    s_GrantQueue.clear();
    if (!s_InResync) {
        s_InResync = true;
        s_ResyncStart = steady_clock::now();
    }
    LOG("[ItemSync] New seed detected - cleared pending/in-world state");
}

void OnItemReceived(int64_t itemId, bool notify) {
    if (!notify) return;

    std::lock_guard lock(s_Mutex);
    s_PendingGrant[itemId]++;

    if (GameState::IsSaveLoaded() && !s_InResync) {
        s_GrantedInWorld[itemId]++;
        s_GrantQueue.push_back({ itemId, 1, false });
        LOG("[ItemSync] Live grant queued: item {} (pending={}, inworld={})",
            itemId, s_PendingGrant[itemId], s_GrantedInWorld[itemId]);
    } else {
        LOG("[ItemSync] Pending recorded: item {} (pending={}, inresync={})",
            itemId, s_PendingGrant[itemId], s_InResync);
    }
}

void EndResyncWindow() {
    std::lock_guard lock(s_Mutex);
    if (!s_InResync) return;
    s_InResync = false;

    if (GameState::IsSaveLoaded()) {
        size_t granted = 0;
        for (auto& [id, pending] : s_PendingGrant) {
            auto it = s_GrantedInWorld.find(id);
            uint32_t inWorld = (it != s_GrantedInWorld.end()) ? it->second : 0;
            if (pending > inWorld) {
                // Resync difference = copies that were never physically granted
                // in-world -> first-time grants, so seen=false.
                s_GrantQueue.push_back({ id, pending - inWorld, false });
                s_GrantedInWorld[id] = pending;
                granted++;
            }
        }
        if (granted) {
            LOG("[ItemSync] Resync reconcile: {} item kinds queued for grant", granted);
        }
    }
    LOG("[ItemSync] Resync window ended");
}

void DrainGrantQueue() {
    std::vector<GrantRequest> batch;
    {
        std::lock_guard lock(s_Mutex);
        batch.swap(s_GrantQueue);
    }
    for (const auto& r : batch) {
        if (s_GrantHandler) {
            s_GrantHandler(r.itemId, r.count, r.seen);
        } else {
            WARN("[ItemSync] No grant handler set - dropping item {} x{}", r.itemId, r.count);
        }
    }
    // Anything that reached the handler was at least shown once -> mark seen so
    // a later death save-load regrant knows to suppress the repeat notification.
    std::lock_guard lock(s_Mutex);
    for (const auto& r : batch) {
        s_SeenGranted[r.itemId] = true;
    }
}

void Tick() {
    bool shouldEndResync = false;
    {
        std::lock_guard lock(s_Mutex);
        if (s_InResync && steady_clock::now() - s_ResyncStart >= RESYNC_WINDOW) {
            shouldEndResync = true;
        }
    }
    if (shouldEndResync) {
        EndResyncWindow();
    }
    DrainGrantQueue();
}

void OnSaveLoaded() {
    // World is being rebuilt from the save. Regrant all pending copies once the
    // world has settled (Deferred waits for a map load + settling delay).
    Deferred::EnqueueAfterMapChange([]() { DoSaveLoadRegrant(); });
}

void DoSaveLoadRegrant() {
    std::lock_guard lock(s_Mutex);
    s_GrantedInWorld.clear();
    size_t kinds = s_PendingGrant.size();
    for (const auto& [id, count] : s_PendingGrant) {
        // seen=true when the player already physically received this id this
        // session (granted before the death): lets the handler suppress the
        // repeat notification. Ids that were only ever pending (arrived while
        // no world was loaded / during resync) stay unseen -> first-time notif.
        bool seen = s_SeenGranted.find(id) != s_SeenGranted.end();
        s_GrantQueue.push_back({ id, count, seen });
        s_GrantedInWorld[id] = count;
    }
    LOG("[ItemSync] Save-load regrant queued: {} item kinds", kinds);
}

void OnSaveCompleted() {
    if (APManager::getAPConnected()) {
        AP_ConfirmReceivedItems();
        LOG("[ItemSync] Confirmed received items with AP server");
    } else {
        LOG("[ItemSync] Save completed while disconnected - confirm skipped (items stay pending until server resync)");
    }
    std::lock_guard lock(s_Mutex);
    s_PendingGrant.clear();
    s_GrantedInWorld.clear();
    s_SeenGranted.clear();
}

void Setup() {
    GameState::OnSaveLoaded([](bool isLoaded) {
        if (isLoaded) {
            OnSaveLoaded();
        } else {
            // Returned to title: the world is gone. Drop in-world bookkeeping
            // (and any undrained grants) but keep pending copies - they will be
            // regranted on the next load.
            std::lock_guard lock(s_Mutex);
            s_GrantedInWorld.clear();
            s_GrantQueue.clear();
        }
    });

    SaveHooks::OnGameSaved([](int32_t, bool) {
        OnSaveCompleted();
    });

    APManager::OnConnected(OnResyncStart);
    APManager::OnDisconnected(OnDisconnected);
    APManager::OnNewSeed(OnSeedChanged);

    LOG("[ItemSync] Setup complete");
}

// ---- Debug / status ----

bool IsInResync() {
    std::lock_guard lock(s_Mutex);
    return s_InResync;
}

bool IsSaveLoaded() {
    return GameState::IsSaveLoaded();
}

size_t QueuedGrantCount() {
    std::lock_guard lock(s_Mutex);
    return s_GrantQueue.size();
}

std::vector<std::pair<int64_t, uint32_t>> GetPendingList() {
    std::lock_guard lock(s_Mutex);
    return std::vector<std::pair<int64_t, uint32_t>>(s_PendingGrant.begin(), s_PendingGrant.end());
}

std::vector<std::pair<int64_t, uint32_t>> GetInWorldList() {
    std::lock_guard lock(s_Mutex);
    return std::vector<std::pair<int64_t, uint32_t>>(s_GrantedInWorld.begin(), s_GrantedInWorld.end());
}

uint32_t PendingCount(int64_t itemId) {
    std::lock_guard lock(s_Mutex);
    auto it = s_PendingGrant.find(itemId);
    return it != s_PendingGrant.end() ? it->second : 0;
}

uint32_t InWorldCount(int64_t itemId) {
    std::lock_guard lock(s_Mutex);
    auto it = s_GrantedInWorld.find(itemId);
    return it != s_GrantedInWorld.end() ? it->second : 0;
}

void DebugSimulateReceive(int64_t itemId, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        OnItemReceived(itemId, true);
    }
    LOG("[ItemSync] (debug) simulated live receive: item {} x{}", itemId, count);
}

void DebugSimulateRefire(int64_t itemId, uint32_t count) {
    {
        std::lock_guard lock(s_Mutex);
        s_PendingGrant[itemId] += count;
    }
    LOG("[ItemSync] (debug) simulated refire: item {} x{}", itemId, count);
}

void DebugStartResync() {
    OnResyncStart();
    LOG("[ItemSync] (debug) started resync window");
}

void DebugEndResync() {
    EndResyncWindow();
    DrainGrantQueue();
    LOG("[ItemSync] (debug) ended resync window");
}

void DebugSimulateDeath() {
    DoSaveLoadRegrant();
    DrainGrantQueue();
    LOG("[ItemSync] (debug) simulated death save-load regrant");
}

void DebugForceConfirm() {
    OnSaveCompleted();
    LOG("[ItemSync] (debug) forced save confirm");
}

} // namespace ItemSync
