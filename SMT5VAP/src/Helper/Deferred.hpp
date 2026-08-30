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
//   - the game is NOT in a map transition, and
//   - a map is actually loaded.
//
// All enqueues and ticks happen on the game thread, so no locking is needed.
namespace Deferred {

    // Schedule a task to run on the game thread as soon as the world is
    // settled (save loaded, on a map, not transitioning). Enqueue anytime —
    // no new-map-load requirement. Safe to call from within hook callbacks
    // (e.g. save-loaded).
    void Enqueue(std::function<void()> task);

    // Schedule a task to run on the game thread once the world is settled, but
    // not before `delaySeconds` real seconds have elapsed (steady clock).
    void DelayedEnqueue(std::function<void()> task, double delaySeconds);

    // Schedule a task that additionally requires a completed map load AFTER the
    // enqueue, plus a short settle window, before it may run. For setup work
    // that must run on a freshly-streamed-in map (e.g. the save-load burst).
    void EnqueueAfterMapChange(std::function<void()> task);

    // Same as EnqueueAfterMapChange, plus `delaySeconds` real seconds before
    // the task may run.
    void DelayedEnqueueAfterMapChange(std::function<void()> task, double delaySeconds);

    // Call every on_update. Runs at most one queued task per tick.
    void Tick();

    // Drop all queued tasks (e.g. returning to title).
    void Clear();

} // namespace Deferred
