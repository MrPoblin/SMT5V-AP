#include "MissionRewardHook.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

// ── Game binary function signatures ────────────────────────────────────
static constexpr const char* SIG_REWARD_EVAL   = "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 20 4C 89 C7";
static constexpr const char* SIG_EXP_EVAL      = "40 53 48 83 EC 20 8B D9 E8 ? ? ? ? 8B D3 48 8D 88 ? ? ? ? E8 ? ? ? ? 48 8B D8 E8 ? ? ? ? 84 C0";
static constexpr const char* SIG_MANAGER_GET   = "48 83 EC 28 4C 8B 0D ? ? ? ? 33 C0 48 89 5C 24 20 85 C0 78 ? 41 3B 81 ? ? ? ? 7D ?";
static constexpr const char* SIG_CACHE_LOOKUP  = "48 83 EC 28 85 D2 78 ? 3B 51 ? 7D ? 48 63 C2 48 69 C0 B8 00 00 00";

// ── Cache entry offsets (184 bytes per mission) ────────────────────────
static constexpr ptrdiff_t CACHE_ITEM_ARRAY  = 0x10;
static constexpr ptrdiff_t CACHE_ITEM_COUNT  = 0x18;
static constexpr ptrdiff_t CACHE_MACCA       = 0x20;
static constexpr ptrdiff_t CACHE_EXP         = 0x24;

// ── Native function pointers ──────────────────────────────────────────
using FnGetManager   = uint64_t (__fastcall*)();
using FnCacheLookup  = uint64_t (__fastcall*)(uint64_t cacheBase, int missionId);

static FnGetManager   s_GetManager   = nullptr;
static FnCacheLookup  s_CacheLookup  = nullptr;
static constexpr uint64_t CACHE_BASE_OFFSET = 0x13F0;

// ── PolyHook2 detours ──────────────────────────────────────────────────
static std::unique_ptr<PLH::x64Detour> s_RewardEvalDetour;
static std::unique_ptr<PLH::x64Detour> s_ExpEvalDetour;
static uint64_t s_OrigRewardEval = 0;
static uint64_t s_OrigExpEval    = 0;

// ── State ──────────────────────────────────────────────────────────────
static std::atomic<MissionRewardHook::FilterMode> s_Mode{MissionRewardHook::FilterMode::Disabled};
static std::mutex s_ExceptionMutex;
static std::unordered_set<int32_t> s_Exceptions;
static bool s_Resolved = false;
static CallbackId s_CompleteMissionHookId{-1};

// ── Helpers ────────────────────────────────────────────────────────────
static bool IsException_Unlocked(int32_t id) {
    std::lock_guard<std::mutex> L(s_ExceptionMutex);
    return s_Exceptions.count(id) > 0;
}

static bool ShouldBlock(int32_t missionId) {
    auto mode = s_Mode.load(std::memory_order_relaxed);
    if (mode == MissionRewardHook::FilterMode::Disabled)
        return false;
    return !IsException_Unlocked(missionId);
}

static uint8_t* GetCacheEntry(int32_t missionId) {
    if (!s_GetManager || !s_CacheLookup) return nullptr;

    uint64_t manager = s_GetManager();
    if (!manager) return nullptr;

    uint64_t cacheBase = manager + CACHE_BASE_OFFSET;
    uint64_t rawEntry = s_CacheLookup(cacheBase, missionId);

    uint8_t* entry = reinterpret_cast<uint8_t*>(rawEntry);
    if (!entry || reinterpret_cast<uint64_t>(entry) < 0x10000) return nullptr;

    return entry;
}

static void ZeroCacheEntry(int32_t missionId, bool keepExp) {
    uint8_t* entry = GetCacheEntry(missionId);
    if (!entry) return;

    int32_t oldMacca = *reinterpret_cast<int32_t*>(entry + CACHE_MACCA);
    int32_t oldExp   = *reinterpret_cast<int32_t*>(entry + CACHE_EXP);
    int32_t oldCount = *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT);

    *reinterpret_cast<uint64_t*>(entry + CACHE_ITEM_ARRAY) = 0;
    *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT)  = 0;
    *reinterpret_cast<int32_t*>(entry + CACHE_MACCA)       = 0;
    if (!keepExp)
        *reinterpret_cast<int32_t*>(entry + CACHE_EXP)     = 0;

    if (keepExp)
        LOG("[MissionRewardHook] Zeroed cache for mission {} (was: macca={}, exp={}, items={}) [exp KEPT]",
            missionId, oldMacca, oldExp, oldCount);
    else
        LOG("[MissionRewardHook] Zeroed cache for mission {} (was: macca={}, exp={}, items={})",
            missionId, oldMacca, oldExp, oldCount);
}

static void ZeroDescriptorStructs() {
    if (!s_GetManager) return;
    uint64_t manager = s_GetManager();
    if (!manager) return;

    uint8_t* cacheBase = reinterpret_cast<uint8_t*>(manager + CACHE_BASE_OFFSET);
    uint8_t* descArray = *reinterpret_cast<uint8_t**>(cacheBase + 0x10);
    if (!descArray || reinterpret_cast<uint64_t>(descArray) < 0x10000) return;

    memset(descArray + (1 * 64), 0, 64);
    memset(descArray + (2 * 64), 0, 64);
    LOG("[MissionRewardHook] Zeroed descriptor structs (type 1+2)");
}

static void BlockRewards(int32_t missionId) {
    bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
    ZeroCacheEntry(missionId, keepExp);
    ZeroDescriptorStructs();
}

// ── Hook: items/macca evaluator (sub_1474F1B90) ────────────────────────
static __int64 __fastcall HkRewardEval(int a1, int* a2, int* a3) {
    if (ShouldBlock(a1)) {
        BlockRewards(a1);
    }

    return reinterpret_cast<decltype(&HkRewardEval)>(s_OrigRewardEval)(a1, a2, a3);
}

// ── Hook: exp evaluator (sub_140AEA250) ────────────────────────────────
static __int64 __fastcall HkExpEval(int a1) {
    if (ShouldBlock(a1)) {
        bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
        ZeroCacheEntry(a1, keepExp);
    }

    return reinterpret_cast<decltype(&HkExpEval)>(s_OrigExpEval)(a1);
}

// ── Public API ─────────────────────────────────────────────────────────
namespace MissionRewardHook {

    void SetMode(FilterMode mode) {
        s_Mode.store(mode, std::memory_order_relaxed);
        LOG("[MissionRewardHook] Mode set to {}", static_cast<int>(mode));
    }

    FilterMode GetMode() {
        return s_Mode.load(std::memory_order_relaxed);
    }

    void AddException(int32_t missionId) {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions.insert(missionId);
        LOG("[MissionRewardHook] Added exception for mission {}", missionId);
    }

    void RemoveException(int32_t missionId) {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions.erase(missionId);
        LOG("[MissionRewardHook] Removed exception for mission {}", missionId);
    }

    void ClearExceptions() {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions.clear();
        LOG("[MissionRewardHook] Cleared all exceptions");
    }

    void SetExceptions(std::initializer_list<int32_t> ids) {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions = std::unordered_set<int32_t>(ids);
        LOG("[MissionRewardHook] Set {} exceptions", s_Exceptions.size());
    }

    bool IsException(int32_t missionId) {
        return IsException_Unlocked(missionId);
    }

    void Setup() {
        LOG("[MissionRewardHook] Setup...");

        if (s_Resolved) {
            LOG("[MissionRewardHook] Already resolved, skipping");
            return;
        }

        // ── Resolve helper native functions ──
        uint64_t mgrAddr = SignatureScanner::FindPattern(SIG_MANAGER_GET);
        if (mgrAddr) {
            s_GetManager = reinterpret_cast<FnGetManager>(mgrAddr);
            LOG("[MissionRewardHook] Manager getter resolved at 0x{:x}", mgrAddr);
        } else {
            LOG("[MissionRewardHook] ERROR: Manager getter signature NOT FOUND");
        }

        uint64_t cacheAddr = SignatureScanner::FindPattern(SIG_CACHE_LOOKUP);
        if (cacheAddr) {
            s_CacheLookup = reinterpret_cast<FnCacheLookup>(cacheAddr);
        LOG("[MissionRewardHook] Cache lookup resolved at 0x{:x}", cacheAddr);
        } else {
        LOG("[MissionRewardHook] ERROR: Cache lookup signature NOT FOUND");
        }

        if (!s_GetManager || !s_CacheLookup) {
        LOG("[MissionRewardHook] Cannot hook — missing native function pointers");
        return;
        }

        // ── PolyHook2 native hooks on evaluators ──
        uint64_t rewardEvalAddr = SignatureScanner::FindPattern(SIG_REWARD_EVAL);
        if (rewardEvalAddr) {
        uint64_t origAddr = 0;
        auto det = std::make_unique<PLH::x64Detour>(
        rewardEvalAddr,
        reinterpret_cast<uint64_t>(PLH::FnCast(&HkRewardEval, &s_OrigRewardEval)),
        &origAddr
        );
        if (det->hook()) {
        s_OrigRewardEval = PLH::FnCast(origAddr, s_OrigRewardEval);
        s_RewardEvalDetour = std::move(det);
        LOG("[MissionRewardHook] Reward eval hook installed at 0x{:x}", rewardEvalAddr);
        } else {
        LOG("[MissionRewardHook] ERROR: Reward eval hook FAILED at 0x{:x}", rewardEvalAddr);
        }
        } else {
        LOG("[MissionRewardHook] WARN: Reward eval signature NOT FOUND");
        }

        uint64_t expEvalAddr = SignatureScanner::FindPattern(SIG_EXP_EVAL);
        if (expEvalAddr) {
        uint64_t origAddr = 0;
        auto det = std::make_unique<PLH::x64Detour>(
        expEvalAddr,
        reinterpret_cast<uint64_t>(PLH::FnCast(&HkExpEval, &s_OrigExpEval)),
        &origAddr
        );
        if (det->hook()) {
        s_OrigExpEval = PLH::FnCast(origAddr, s_OrigExpEval);
        s_ExpEvalDetour = std::move(det);
        LOG("[MissionRewardHook] Exp eval hook installed at 0x{:x}", expEvalAddr);
        } else {
        LOG("[MissionRewardHook] ERROR: Exp eval hook FAILED at 0x{:x}", expEvalAddr);
            }
        } else {
            LOG("[MissionRewardHook] WARN: Exp eval signature NOT FOUND");
        }

        // ── UFunction pre-hook on CompleteMission ──
        auto* compFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:CompleteMission"));
        if (!compFunc)
            compFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:CompleteMission"));

        if (compFunc) {
            auto* IdProp = compFunc->GetPropertyByName(STR("MissionId"));
            LOG("[MissionRewardHook] CompleteMission UFunction found, MissionId prop={}", IdProp ? 1 : 0);

            s_CompleteMissionHookId = compFunc->RegisterPreHook(
                [IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 id = -1;
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            id = *P;

                    LOG("[MissionRewardHook] CompleteMission PRE-hook mission={}", id);

                    if (id >= 0 && ShouldBlock(id)) {
                        BlockRewards(id);
                    }
                }
            );
            LOG("[MissionRewardHook] CompleteMission pre-hook registered (id={})", s_CompleteMissionHookId);
        } else {
            LOG("[MissionRewardHook] ERROR: CompleteMission UFunction NOT FOUND");
        }

        s_Resolved = true;
        LOG("[MissionRewardHook] Setup complete");
    }
}
