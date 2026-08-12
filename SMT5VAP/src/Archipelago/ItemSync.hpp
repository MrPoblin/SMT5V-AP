#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// AP item confirm / regrant system.
//
// Handles the fact that SMT5V has no auto-save: if the player dies or the game
// crashes, items that were received from the Archipelago server but not yet
// written to a save file would be lost. This module:
//
//   - tracks which received items are still unconfirmed by the server,
//   - regrants them when a save is loaded (death / manual reload / crash),
//   - confirms them to the server once a save has FULLY completed writing
//     (via SaveHooks, which polls IsEndDataSave),
//   - survives disconnects/reconnects by rebuilding the pending counts from
//     the server's refire stream and only granting the difference against
//     what is already present in the world.
//
// The actual item->game granting logic is NOT implemented here. Set a handler
// with SetGrantHandler(); it is invoked on the game thread only.
namespace ItemSync {

// Called on the game thread for every item grant. Implement per-item granting
// (incl. any custom AP items) here.
using GrantFn = std::function<void(int64_t itemId, uint32_t count)>;

void Setup();
void SetGrantHandler(GrantFn fn);

// APCpp websocket thread: called for every delivered item.
void OnItemReceived(int64_t itemId, bool notify);

// Game thread: drain the grant queue + end the reconnect resync window.
// Call every on_update.
void Tick();

// Reconnect window start. Idempotent. May be called from the APCpp item-clear
// callback (websocket thread, race-safe) or the APManager connected callback
// (game thread).
void OnResyncStart();

// Game thread (APManager disconnected callback).
void OnDisconnected();

// Game thread (APManager new-seed callback).
void OnSeedChanged();

// Game thread (GameState::OnSaveLoaded). Defers the regrant until the world
// has settled.
void OnSaveLoaded();

// Game thread (SaveHooks::OnGameSaved, fires AFTER the save fully completed).
void OnSaveCompleted();

// ---- Debug / status ----
bool IsInResync();
bool IsSaveLoaded();
size_t QueuedGrantCount();
std::vector<std::pair<int64_t, uint32_t>> GetPendingList();
std::vector<std::pair<int64_t, uint32_t>> GetInWorldList();
uint32_t PendingCount(int64_t itemId);
uint32_t InWorldCount(int64_t itemId);

void DebugSimulateReceive(int64_t itemId, uint32_t count);
void DebugSimulateRefire(int64_t itemId, uint32_t count);
void DebugStartResync();
void DebugEndResync();
void DebugSimulateDeath();
void DebugForceConfirm();

} // namespace ItemSync
