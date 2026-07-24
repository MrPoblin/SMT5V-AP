#include "src/Archipelago/APState.hpp"
#include "src/Log/Log.hpp"
#include <mutex>
#include <unordered_set>


namespace APState {

    void ClearState() {
        Locations::Clear();
        Essences::Clear();
        Miracles::Clear();
        FusionRaces::Clear();
    }

    namespace Locations {
        static std::mutex CheckedLocationsMutex;
        static std::unordered_set<int64_t> CheckedLocations;

        void OnLocationChecked(int64_t locationId){
            std::lock_guard lock(CheckedLocationsMutex);
            CheckedLocations.insert(locationId);
            LOG("Location in server checked: {}", locationId);
        }

        void Clear() {
            std::lock_guard lock(CheckedLocationsMutex);
            CheckedLocations.clear();
            LOG("Locations cleared");
        }

        bool Contains(int64_t locationId) {
            std::lock_guard lock(CheckedLocationsMutex);
            return CheckedLocations.contains(locationId);
        }
    }

    namespace Essences {
        static std::mutex EssenceListMutex;
        static std::unordered_set<int32_t> EssenceList;

        void AddEssence(int32_t itemId) {
            std::lock_guard lock(EssenceListMutex);
            EssenceList.insert(itemId);
        }

        void RemoveEssence(int32_t itemId) {
            std::lock_guard lock(EssenceListMutex);
            EssenceList.erase(itemId);
        }

        bool HasEssence(int32_t itemId) {
            std::lock_guard lock(EssenceListMutex);
            return EssenceList.contains(itemId);
        }

        std::vector<int32_t> GetAllEssences() {
            std::lock_guard lock(EssenceListMutex);
            std::vector<int32_t> result;
            result.reserve(EssenceList.size());
            for (auto id : EssenceList) {
                result.push_back(id);
            }
            return result;
        }

        void Clear() {
            std::lock_guard lock(EssenceListMutex);
            EssenceList.clear();
        }
    }

    namespace Miracles {
        static std::mutex MiracleListMutex;
        static std::unordered_set<int32_t> MiracleList;

        void Add(int32_t skillId) {
            std::lock_guard lock(MiracleListMutex);
            MiracleList.insert(skillId);
        }

        bool Has(int32_t skillId) {
            std::lock_guard lock(MiracleListMutex);
            return MiracleList.contains(skillId);
        }

        void Clear() {
            std::lock_guard lock(MiracleListMutex);
            MiracleList.clear();
        }
    }

    namespace FusionRaces {
        static std::mutex GatedRacesMutex;
        static constexpr int32_t RACE_COUNT = 50;
        static bool GatedRaces[RACE_COUNT]{};

        void SetRaceGated(int32_t race, bool gated) {
            if (race < 0 || race >= RACE_COUNT) return;
            std::lock_guard lock(GatedRacesMutex);
            GatedRaces[race] = gated;
        }

        bool IsRaceGated(int32_t race) {
            if (race < 0 || race >= RACE_COUNT) return false;
            std::lock_guard lock(GatedRacesMutex);
            return GatedRaces[race];
        }

        void Clear() {
            std::lock_guard lock(GatedRacesMutex);
            for (int32_t i = 0; i < RACE_COUNT; i++) {
                GatedRaces[i] = false;
            }
        }

        void Fill() {
            std::lock_guard lock(GatedRacesMutex);
            for (int32_t i = 0; i < RACE_COUNT; i++) {
                GatedRaces[i] = true;
            }
        }
    }
}
