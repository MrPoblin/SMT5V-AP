#pragma once
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace APState {

    void ClearState();

    namespace Locations {
        void OnLocationChecked(int64_t locationId);
        void Clear();
        bool Contains(int64_t locationId);
    }

    namespace Essences {
        // Tracked essence item IDs received from Archipelago
        void AddEssence(int32_t itemId);
        void RemoveEssence(int32_t itemId);
        bool HasEssence(int32_t itemId);
        std::vector<int32_t> GetAllEssences();
        void Clear();
    }

    namespace Miracles {
        // AP-unlocked miracle skill IDs available for purchase
        void Add(int32_t skillId);
        bool Has(int32_t skillId);
        void Clear();
    }

    namespace FusionRaces {
        // Race gating state for Demon Fusion, driven by Archipelago data.
        // Race is an int32 index 0-49 (see RACE_ARRAY in FusionGating).
        void SetRaceUnlocked(int32_t race, bool gated);
        bool IsRaceGated(int32_t race);
        bool IsRaceUnlocked(int32_t race);
        void Clear();
        void Fill();
    }

    namespace SkillCategories {
        // Skill icon categories matching E_SKILL_ICON enum values.
        // 0=Physical, 1=Fire, 2=Ice, 3=Electric, 4=Force,
        // 5=Light, 6=Dark, 7=Almighty, 8=Ailment, 9=Support, 10=Recovery
        static constexpr int32_t CATEGORY_COUNT = 11;

        // Scope: which party members are affected by blocked categories
        enum class Scope : int32_t { Both = 0, ProtagOnly = 1, DemonOnly = 2 };

        void SetCategoryBlocked(int32_t iconCategory, bool blocked);
        bool IsCategoryBlocked(int32_t iconCategory);
        void SetScope(Scope scope);
        Scope GetScope();
        void Clear();
    }
}
