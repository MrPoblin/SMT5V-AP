#pragma once
#include <functional>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlagHook {

// Called whenever the game sets an event flag to true. `flagName` is the flag's
// name (e.g. "mis_m064_em2420_4"). The consumer decides what to do with it.
using FlagSetCallback = std::function<void(const RC::StringType& flagName)>;

// Installs the SetEventFlag post-hook. Call once at mod init.
void Setup();

// Subscribe to event-flag-set events.
void OnFlagSet(FlagSetCallback cb);

} // namespace EventFlagHook
