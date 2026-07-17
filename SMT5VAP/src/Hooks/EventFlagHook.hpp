#pragma once
#include <functional>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlagHook {

// Called whenever the game sets an event flag. `flagName` is the flag's name
// (e.g. "mis_m064_em2420_4") and `newValue` is the value it was set to (true or
// false). The consumer decides what to do with it.
// Integer-id writes (from SetEventFlagValue) are reported with the name "[<id>]".
using FlagSetCallback = std::function<void(const RC::StringType& flagName, bool newValue)>;

// Installs the SetEventFlag + SetEventFlagValue hooks. Call once at mod init.
void Setup();

// Subscribe to event-flag-set events (fires on both true and false sets).
void OnFlagSet(FlagSetCallback cb);

// ── Map-event flags (UBPL_MapEventData) ──
// These are a SEPARATE flag store from BPL_EventFlag (the game sets map-event
// Start/End/After flags via BPL_MapEventData, NOT via BPL_EventFlag), so they
// were invisible to the event-flag hooks above. They are intercepted here too
// so "all story flags" are covered by one module.
enum class MapEventFlagKind : uint8_t {
    Start,
    End,
    After,
};

// Called whenever the game sets a map-event flag. `mapEventId` is the MAP EVENT
// id (e.g. 474 for LV_E2640, 587 for LV_E3420), NOT the underlying flag id.
using MapEventFlagCallback = std::function<void(int32_t mapEventId, MapEventFlagKind kind, bool value)>;

// Subscribe to map-event-flag-set events.
void OnMapEventFlagSet(MapEventFlagCallback cb);

// When suppress is enabled, flag-set notifications are not dispatched. Set this
// around programmatic writes (e.g. FlagGating) so they don't echo back as
// game-originated changes.
void SetSuppressNotify(bool suppress);
bool IsSuppressNotify();

// RAII helper: suppresses notifications for the lifetime of the scope and
// ALWAYS restores them on exit (even on exception/early return). Use this
// instead of manually toggling SetSuppressNotify so the guard can never leak
// and accidentally swallow unrelated game flag changes.
struct ScopedSuppressNotify {
    ScopedSuppressNotify() { SetSuppressNotify(true); }
    ~ScopedSuppressNotify() { SetSuppressNotify(false); }
    ScopedSuppressNotify(const ScopedSuppressNotify&) = delete;
    ScopedSuppressNotify& operator=(const ScopedSuppressNotify&) = delete;
};

} // namespace EventFlagHook
