#include "src/Archipelago/APState.hpp"

namespace APState {
    namespace Essences {
        static std::unordered_set<int32_t> EssenceList;

        void AddEssence(int32_t itemId) {
            EssenceList.insert(itemId);
        }

        void RemoveEssence(int32_t itemId) {
            EssenceList.erase(itemId);
        }

        bool HasEssence(int32_t itemId) {
            return EssenceList.contains(itemId);
        }

        std::vector<int32_t> GetAllEssences() {
            std::vector<int32_t> result;
            result.reserve(EssenceList.size());
            for (auto id : EssenceList) {
                result.push_back(id);
            }
            return result;
        }

        void Clear() {
            EssenceList.clear();
        }
    }

    namespace Miracles {
        static std::unordered_set<int32_t> MiracleList;

        void Add(int32_t skillId) {
            MiracleList.insert(skillId);
        }

        bool Has(int32_t skillId) {
            return MiracleList.contains(skillId);
        }

        void Clear() {
            MiracleList.clear();
        }
    }

    namespace FusionRaces {
        static constexpr int32_t RACE_COUNT = 50;
        static bool GatedRaces[RACE_COUNT]{};

        void SetRaceGated(int32_t race, bool gated) {
            if (race < 0 || race >= RACE_COUNT) return;
            GatedRaces[race] = gated;
        }

        bool IsRaceGated(int32_t race) {
            if (race < 0 || race >= RACE_COUNT) return false;
            return GatedRaces[race];
        }

        void Clear() {
            for (int32_t i = 0; i < RACE_COUNT; i++) {
                GatedRaces[i] = false;
            }
        }

        void Fill() {
            for (int32_t i = 0; i < RACE_COUNT; i++) {
                GatedRaces[i] = true;
            }
        }
    }
}
