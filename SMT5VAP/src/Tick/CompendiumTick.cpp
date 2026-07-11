#include "CompendiumTick.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <vector>
#include <mutex>
#include <set>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace CompendiumTick {

static std::vector<DemonAcquiredCallback> s_Callbacks;
static std::mutex s_Mutex;

// ── Polling state (5s real-time interval, framerate-independent) ──
static auto s_LastPollTime = std::chrono::steady_clock::now();
static constexpr auto POLL_INTERVAL = std::chrono::seconds(5);

// Cached state
static std::set<int32_t> s_KnownDevilIDs;
static bool s_HasInitialCount{false};

// ── Cached resources (lazy init) ──
static UFunction* s_CheckBibleEntry{nullptr};
static UObject* s_CDO{nullptr};
static bool s_InitFailed{false};

// Upper bound for DevilID scanning
static constexpr int32_t MAX_DEVIL_ID = 1200;

// ── Helpers ──

static UFunction* findFn(const wchar_t* path) {
    auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, path);
    if (F) return F;
    StringType s(path);
    auto pos = s.find(STR(':'));
    if (pos != StringType::npos) {
        auto c = s.substr(0, pos) + STR("_C") + s.substr(pos);
        return UObjectGlobals::FindObject<UFunction>(nullptr, c.c_str());
    }
    return nullptr;
}

static UObject* findCDO(const wchar_t* className) {
    StringType path = StringType(STR("/Script/Project.Default__")) + className;
    auto* Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, path.c_str());
    if (Obj) return Obj;
    path = StringType(STR("/Script/Project.Default__")) + className + STR("_C");
    Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, path.c_str());
    if (Obj) return Obj;
    StringType classPath = StringType(STR("/Script/Project.")) + className;
    auto* Cls = UObjectGlobals::FindObject<UClass>(nullptr, classPath.c_str());
    if (!Cls) {
        classPath += STR("_C");
        Cls = UObjectGlobals::FindObject<UClass>(nullptr, classPath.c_str());
    }
    if (Cls) return Cls->CreateDefaultObject();
    return nullptr;
}

/// Scan the compendium (Bible) using CheckBibleEntry(DevilID) -> bool.
/// Returns the set of DevilIDs that are actually registered in the compendium.
static std::set<int32_t> scanCompendium() {
    std::set<int32_t> registered;
    if (!s_CheckBibleEntry || !s_CDO) return registered;

    for (int32_t devilID = 1; devilID <= MAX_DEVIL_ID; devilID++) {
        // CheckBibleEntry param layout: int32 DevilID @0, bool ReturnValue @4, pad[3] @5 = 8 bytes total
        struct { int32_t DevilID; bool ReturnValue; } params{devilID, false};
        s_CDO->ProcessEvent(s_CheckBibleEntry, &params);
        if (params.ReturnValue) {
            registered.insert(devilID);
        }
    }
    return registered;
}

static void ensureInit() {
    if (s_CDO || s_InitFailed) return;

    s_CheckBibleEntry = findFn(STR("/Script/Project.BPL_BibleData:CheckBibleEntry"));

    if (!s_CheckBibleEntry) {
        LOG("[CompendiumTick] BPL_BibleData.CheckBibleEntry NOT FOUND");
        s_InitFailed = true;
        return;
    }

    s_CDO = findCDO(STR("BPL_BibleData"));
    if (!s_CDO) {
        LOG("[CompendiumTick] BPL_BibleData CDO NOT FOUND");
        s_InitFailed = true;
        return;
    }

    LOG("[CompendiumTick] Resources ready (fn={}, cdo={})",
        s_CheckBibleEntry ? 1 : 0, s_CDO ? 1 : 0);
}

// ── Public API ──

void Poll() {
    ensureInit();
    if (!s_CheckBibleEntry || !s_CDO) return;

    // Real-time throttle (framerate-independent)
    auto now = std::chrono::steady_clock::now();
    if (now - s_LastPollTime < POLL_INTERVAL) return;
    s_LastPollTime = now;

    // ─── Scan compendium ───
    auto currentDevilIDs = scanCompendium();

    if (!s_HasInitialCount) {
        // ─── First scan ───
        s_KnownDevilIDs = std::move(currentDevilIDs);
        s_HasInitialCount = true;
        LOG("[CompendiumTick] Compendium has {} registered species", s_KnownDevilIDs.size());
        LOG("[Demon] Registered DevilIDs:");
        {
            int32_t c = 0;
            for (int32_t id : s_KnownDevilIDs) {
                LOG("  [{}/{}] DevilID={}", ++c, s_KnownDevilIDs.size(), id);
            }
        }
        return;
    }

    // ─── Detect new registrations ───
    if (currentDevilIDs == s_KnownDevilIDs) {
        return; // No change
    }

    LOG("[CompendiumTick] Compendium changed: {} -> {} species",
        s_KnownDevilIDs.size(), currentDevilIDs.size());

    // Find newly registered DevilIDs
    std::vector<int32_t> acquired;
    for (int32_t id : currentDevilIDs) {
        if (s_KnownDevilIDs.find(id) == s_KnownDevilIDs.end()) {
            acquired.push_back(id);
        }
    }

    // Update cache
    s_KnownDevilIDs = std::move(currentDevilIDs);

    // Log changes
    for (int32_t devilID : acquired) {
        LOG("[Demon] NEW DEMON REGISTERED: DevilID={}", devilID);
    }

    // Fire callbacks
    if (!acquired.empty()) {
        std::lock_guard<std::mutex> L(s_Mutex);
        for (auto& cb : s_Callbacks) {
            for (int32_t devilID : acquired) cb(devilID);
        }
    }
}

void OnDemonAcquired(DemonAcquiredCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

} // namespace CompendiumTick
