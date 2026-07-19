#pragma once
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace APState {
    namespace Essences {
        // Tracked essence item IDs received from Archipelago
        void AddEssence(int32_t itemId);
        void RemoveEssence(int32_t itemId);
        bool HasEssence(int32_t itemId);
        std::vector<int32_t> GetAllEssences();
        void Clear();
    }

    namespace FusionRaces {
        // Race gating state for Demon Fusion, driven by Archipelago data.
        // Race is an int32 index 0-49 (see RACE_ARRAY in FusionGating).
        void SetRaceGated(int32_t race, bool gated);
        bool IsRaceGated(int32_t race);
        void Clear();
        void Fill();
    }
}
