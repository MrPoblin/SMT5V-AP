#pragma once
#include <cstdint>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlags {

    // Resolves the BPL_EventFlag UFunctions + CDO. Call once at mod init
    // (game thread). Safe to call multiple times.
    void Setup();

    // ── Write ──
    // Sets a flag by its integer id. Notifications from EventFlagHook are
    // suppressed for the duration of the write so it doesn't echo back as a
    // game-originated change.
    void Set(int32_t flagId, bool value = true);
    // Sets a flag by its FName (e.g. "mis_m064_em2420_4").
    void Set(const RC::StringType& flagName, bool value = true);

    // ── Read ──
    // Reads a flag by integer id. Returns the current game state (false if the
    // flag can't be resolved). Use this for gating decisions so you never depend
    // on having caught the callback edge.
    bool Get(int32_t flagId);
    // Reads a flag by FName.
    bool Get(const RC::StringType& flagName);

}
