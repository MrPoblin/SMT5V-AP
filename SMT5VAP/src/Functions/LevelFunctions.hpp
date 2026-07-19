#pragma once
#include <cstdint>

namespace LevelFunctions {

    // E_PLAYER_TYPE::E_PLAYER_NAHOBINO — the protagonist.
    static constexpr uint8_t kProtagonist = 2;

    // Resolves BPL_PartyData::GetPlayerLevel / GetNkmLevel + CDO.
    // Call once in on_unreal_init. Safe to call multiple times.
    void Setup();

    // Protagonist (Nahobino) level. Returns -1 if not resolvable yet.
    int32_t GetProtagonistLevel();

    // Any party Nkm level (demon OR guest) by its Nkm index.
    // Returns 1 if not resolvable yet.
    int32_t GetNkmLevel(int32_t nkmIndex);

}
