#include "src/Archipelago/APState.hpp"
#include "src/Archipelago/ItemSync.hpp"
#include "src/Log/Log.hpp"
#include <mutex>
#include <unordered_set>


namespace APState {

    void ClearState() {
        Locations::Clear();
        Essences::Clear();
        Miracles::Clear();
        FusionRaces::Clear();
        SkillCategories::Clear();

        // Fired on the APCpp websocket thread on every Connected, BEFORE the
        // refire stream. Race-safe resync-window start.
        ItemSync::OnResyncStart();
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
        static bool UnlockedRaces[RACE_COUNT]{};

        void SetRaceUnlocked(int32_t race, bool gated) {
            if (race < 0 || race >= RACE_COUNT) return;
            std::lock_guard lock(GatedRacesMutex);
            UnlockedRaces[race] = gated;
        }

        bool IsRaceGated(int32_t race) {
            if (race < 0 || race >= RACE_COUNT) return false;
            std::lock_guard lock(GatedRacesMutex);
            return !UnlockedRaces[race];
        }

        bool IsRaceUnlocked(int32_t race) {
            return !IsRaceGated(race);
        }

        void Clear() {
            std::lock_guard lock(GatedRacesMutex);
            for (int32_t i = 0; i < RACE_COUNT; i++) {
                UnlockedRaces[i] = false;
            }
        }

        void Fill() {
            std::lock_guard lock(GatedRacesMutex);
            for (int32_t i = 0; i < RACE_COUNT; i++) {
                UnlockedRaces[i] = true;
            }
        }
    }

    namespace SkillCategories {
        static std::mutex SkillCatMutex;
        static bool BlockedCategories[CATEGORY_COUNT]{};
        static Scope CurrentScope{Scope::Both};

        void SetCategoryBlocked(int32_t iconCategory, bool blocked) {
            if (iconCategory < 0 || iconCategory >= CATEGORY_COUNT) return;
            std::lock_guard lock(SkillCatMutex);
            BlockedCategories[iconCategory] = blocked;
            LOG("[SkillCategories] Category {} ({})", iconCategory,
                blocked ? STR("blocked") : STR("unblocked"));
        }

        bool IsCategoryBlocked(int32_t iconCategory) {
            if (iconCategory < 0 || iconCategory >= CATEGORY_COUNT) return false;
            std::lock_guard lock(SkillCatMutex);
            return BlockedCategories[iconCategory];
        }

        void SetScope(Scope scope) {
            std::lock_guard lock(SkillCatMutex);
            CurrentScope = scope;
            LOG("[SkillCategories] Scope set to {}", static_cast<int32_t>(scope));
        }

        Scope GetScope() {
            std::lock_guard lock(SkillCatMutex);
            return CurrentScope;
        }

        void Clear() {
            std::lock_guard lock(SkillCatMutex);
            for (int32_t i = 0; i < CATEGORY_COUNT; i++) {
                BlockedCategories[i] = false;
            }
            CurrentScope = Scope::Both;
        }
    }
}
