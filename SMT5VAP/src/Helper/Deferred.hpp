#pragma once

#include <functional>

// Run expensive one-time setup work OUTSIDE of game hook stacks.
//
// The old save-load path executed thousands of ProcessEvent calls and native
// detour installs synchronously inside the StartDataLoad post-hook, while save
// data was only partially applied and before the map had loaded. If any of that
// touched a not-yet-initialized game object the process crashed (intermittently,
// depending on timing).
//
// Deferred::Enqueue + Deferred::Tick move that work to the game thread's
// on_update, running tasks one per tick only after:
//   - a save is loaded,
//   - the game is NOT in a map transition,
//   - a map is actually loaded, and
//   - a short settling delay has passed since those conditions became true.
//
// All enqueues and ticks happen on the game thread, so no locking is needed.
namespace Deferred {

    // Schedule a task to run later, once, on the game thread.
    // Safe to call from within hook callbacks (e.g. save-loaded).
    void Enqueue(std::function<void()> task);

    // Call every on_update. Runs at most one queued task per tick.
    void Tick();

    // Drop all queued tasks (e.g. returning to title).
    void Clear();

} // namespace Deferred
