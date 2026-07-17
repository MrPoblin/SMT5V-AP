#pragma once
#include <functional>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlagHook {

// Called whenever the game sets an event flag. `flagName` is the flag's name
// (e.g. "mis_m064_em2420_4") and `newValue` is the value it was set to (true or
// false). The consumer decides what to do with it.
using FlagSetCallback = std::function<void(const RC::StringType& flagName, bool newValue)>;

// Installs the SetEventFlag hook. Call once at mod init.
void Setup();

// Subscribe to event-flag-set events (fires on both true and false sets).
void OnFlagSet(FlagSetCallback cb);

} // namespace EventFlagHook
