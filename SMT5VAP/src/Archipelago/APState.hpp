#pragma once
#include <cstdint>
#include <unordered_set>
#include <mutex>
#include <vector>

namespace APState {
    // ── Track essence item IDs received from Archipelago ──
    // These essences should be re-added to the facility shop when it's re-populated.
    // The shop blocker removes all auto-populated essences, then re-adds only the ones
    // that the player has actually received through Archipelago.
    //
    // Uses C++17 inline variables so the state is shared across all translation units.

    inline std::mutex s_Mutex;
    inline std::unordered_set<int32_t> s_Essences;

    // Track an essence received from Archipelago
    inline void AddEssence(int32_t itemId) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Essences.insert(itemId);
    }

    // Remove an essence from tracking (if it was consumed or removed)
    inline void RemoveEssence(int32_t itemId) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Essences.erase(itemId);
    }

    // Check if the player has received this essence from Archipelago
    inline bool HasEssence(int32_t itemId) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        return s_Essences.contains(itemId);
    }

    // Get all tracked essence IDs (for re-adding to shop on repopulation)
    inline std::vector<int32_t> GetAllEssences() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        std::vector<int32_t> result;
        result.reserve(s_Essences.size());
        for (auto id : s_Essences) {
            result.push_back(id);
        }
        return result;
    }

    // Clear all tracked essences (on reconnect or save reload)
    inline void Clear() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Essences.clear();
    }
}
