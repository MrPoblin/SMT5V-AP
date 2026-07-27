#include "MissionRewardHook.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FFrame.hpp>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

// ── Game binary function signatures ────────────────────────────────────
static constexpr const char* SIG_REWARD_EVAL = "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 20 4C 89 C7";
static constexpr const char* SIG_EXP_EVAL    = "40 53 48 83 EC 20 8B D9 E8 ? ? ? ? 8B D3 48 8D 88 ? ? ? ? E8 ? ? ? ? 48 8B D8 E8 ? ? ? ? 84 C0";
static constexpr const char* SIG_MANAGER_GET = "48 83 EC 28 4C 8B 0D ? ? ? ? 33 C0 48 89 5C 24 20 85 C0 78 ? 41 3B 81 ? ? ? ? 7D ?";
static constexpr const char* SIG_CACHE_LOOKUP = "48 83 EC 28 85 D2 78 ? 3B 51 ? 7D ? 48 63 C2 48 69 C0 B8 00 00 00";

// ── Cache entry offsets ────────────────────────────────────────────────
static constexpr ptrdiff_t CACHE_ITEM_ARRAY = 0x10;
static constexpr ptrdiff_t CACHE_ITEM_COUNT = 0x18;
static constexpr ptrdiff_t CACHE_MACCA      = 0x20;
static constexpr ptrdiff_t CACHE_EXP        = 0x24;

// ── Native function pointers ──────────────────────────────────────────
using FnGetManager  = uint64_t (__fastcall*)();
using FnCacheLookup = uint64_t (__fastcall*)(uint64_t cacheBase, int missionId);

static FnGetManager  s_GetManager  = nullptr;
static FnCacheLookup s_CacheLookup = nullptr;
static constexpr uint64_t CACHE_BASE_OFFSET = 0x13F0;

// ── ItemGetName implementation address (FUN_1473a9130) ────────────────
// This is the native function that resolves item ID to name text.
// The UFunction wrapper has a range check (IDs 1-895 only), so we hook
// the implementation directly to handle magic IDs (1000000+).
static constexpr uint64_t ITEM_GET_NAME_IMPL = 0x1473a9130;

// ── PolyHook2 detours ──────────────────────────────────────────────────
static std::unique_ptr<PLH::x64Detour> s_RewardEvalDetour;
static std::unique_ptr<PLH::x64Detour> s_ExpEvalDetour;
static std::unique_ptr<PLH::x64Detour> s_RewardMsgDetour;
static std::unique_ptr<PLH::x64Detour> s_InfoTagDetour;
static std::unique_ptr<PLH::x64Detour> s_ItemGetNameDetour;

static uint64_t s_OrigRewardEval = 0;
static uint64_t s_OrigExpEval    = 0;
static uint64_t s_OrigRewardMsg  = 0;
static uint64_t s_OrigInfoTag    = 0;
static uint64_t s_OrigItemGetName = 0;

// ── UFunction hook IDs ─────────────────────────────────────────────────
static CallbackId s_CompleteMissionHookId{-1};
static CallbackId s_InfoTagHookId{-1};
static CallbackId s_RewardMsgPreHookId{-1};

// ── State ──────────────────────────────────────────────────────────────
static std::atomic<MissionRewardHook::FilterMode> s_Mode{MissionRewardHook::FilterMode::Disabled};
static std::mutex s_ExceptionMutex;
static std::unordered_set<int32_t> s_Exceptions;
static bool s_Resolved = false;

// ── Custom reward text state ───────────────────────────────────────────
static std::mutex s_CustomTextMutex;
static std::unordered_map<int32_t, std::wstring> s_CustomTexts;

// ── Custom item name lookup (used by ItemGetName hook) ─────────────────
static std::mutex s_CustomItemMutex;
static std::unordered_map<int32_t, std::wstring> s_CustomItemNames;

// ── Mission reward shown callback ──────────────────────────────────────
static std::mutex s_CallbackMutex;
static std::vector<MissionRewardHook::RewardShownCallback> s_RewardShownCallbacks;

// ── Original cache values (saved before patching, restored on complete) ─
struct OriginalCacheValues {
    int32_t itemIds[8];
    int32_t itemNums[8];
    int32_t count;
    int32_t macca;
    int32_t exp;
};
static std::mutex s_OriginalCacheMutex;
static std::unordered_map<int32_t, OriginalCacheValues> s_OriginalCache;

// ── InfoTag hook state ─────────────────────────────────────────────────
static std::atomic<int32_t> s_InfoTagMissionId{-1};
static std::atomic<uint8_t> s_InfoTagTextType{0};

// ── RewardMsg hook state ───────────────────────────────────────────────
static std::atomic<int32_t> s_RewardMsgMissionId{-1};

// ── Forward declarations ───────────────────────────────────────────────
static void ZeroDescriptorStructs();
static void ZeroCacheEntry(int32_t missionId, bool keepExp);

// ── Helpers ────────────────────────────────────────────────────────────
static bool IsException_Unlocked(int32_t id) {
    std::lock_guard<std::mutex> L(s_ExceptionMutex);
    return s_Exceptions.count(id) > 0;
}

static bool ShouldBlock(int32_t missionId) {
    auto mode = s_Mode.load(std::memory_order_relaxed);
    if (mode == MissionRewardHook::FilterMode::Disabled) return false;
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

static void SaveOriginals(int32_t missionId, uint8_t* entry) {
    auto** pairArrayPtr = reinterpret_cast<int32_t**>(entry + CACHE_ITEM_ARRAY);
    auto* pairArray = *pairArrayPtr;
    if (!pairArray) return;

    int32_t count = *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT);
    int32_t macca = *reinterpret_cast<int32_t*>(entry + CACHE_MACCA);
    int32_t exp   = *reinterpret_cast<int32_t*>(entry + CACHE_EXP);

    std::lock_guard<std::mutex> lock(s_OriginalCacheMutex);
    if (s_OriginalCache.find(missionId) != s_OriginalCache.end()) return;

    OriginalCacheValues orig{};
    int32_t saveCount = (count > 8) ? 8 : count;
    for (int32_t i = 0; i < saveCount; i++) {
        orig.itemIds[i]  = pairArray[i * 2];
        orig.itemNums[i] = pairArray[i * 2 + 1];
    }
    orig.count = count;
    orig.macca = macca;
    orig.exp   = exp;
    s_OriginalCache[missionId] = orig;
}

static void ZeroCacheEntry(int32_t missionId, bool keepExp) {
    uint8_t* entry = GetCacheEntry(missionId);
    if (!entry) return;
    SaveOriginals(missionId, entry);
    *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT) = 0;
    *reinterpret_cast<int32_t*>(entry + CACHE_MACCA)      = 0;
    if (!keepExp)
        *reinterpret_cast<int32_t*>(entry + CACHE_EXP)    = 0;
}

static void PatchCacheForDisplay(int32_t missionId) {
    {
        std::lock_guard<std::mutex> lock(s_CustomTextMutex);
        if (s_CustomTexts.find(missionId) == s_CustomTexts.end()) return;
    }

    uint8_t* entry = GetCacheEntry(missionId);
    if (!entry) return;

    auto** pairArrayPtr = reinterpret_cast<int32_t**>(entry + CACHE_ITEM_ARRAY);
    auto* pairArray = *pairArrayPtr;
    if (!pairArray || reinterpret_cast<uint64_t>(pairArray) <= 0x10000) return;

    SaveOriginals(missionId, entry);

    pairArray[0] = 1000000 + missionId;
    pairArray[1] = 1;
    *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT) = 1;
    *reinterpret_cast<int32_t*>(entry + CACHE_MACCA) = 0;
    *reinterpret_cast<int32_t*>(entry + CACHE_EXP) = 0;
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
}

static void BlockRewards(int32_t missionId) {
    bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
    ZeroCacheEntry(missionId, keepExp);
}

// ── Hook: items/macca evaluator ────────────────────────────────────────
static __int64 __fastcall HkRewardEval(int a1, int* a2, int* a3) {
    auto result = reinterpret_cast<decltype(&HkRewardEval)>(s_OrigRewardEval)(a1, a2, a3);
    if (ShouldBlock(a1)) {
        BlockRewards(a1);
        PatchCacheForDisplay(a1);
    }
    return result;
}

// ── Hook: exp evaluator ────────────────────────────────────────────────
static __int64 __fastcall HkExpEval(int a1) {
    auto result = reinterpret_cast<decltype(&HkExpEval)>(s_OrigExpEval)(a1);
    if (ShouldBlock(a1)) {
        bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
        ZeroCacheEntry(a1, keepExp);
        PatchCacheForDisplay(a1);
    }
    return result;
}

// ── Hook: GetMissionInfoTagMessage (completion popup text) ─────────────
static __int64 __fastcall HkGetMissionInfoTagMessage(void* this_, void* execContext, __int64 output) {
    auto result = reinterpret_cast<decltype(&HkGetMissionInfoTagMessage)>(s_OrigInfoTag)(this_, execContext, output);

    int32_t missionId = s_InfoTagMissionId.load(std::memory_order_relaxed);
    uint8_t textType = s_InfoTagTextType.load(std::memory_order_relaxed);

    if (textType == 2 && missionId >= 0) {
        std::wstring customText;
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            auto it = s_CustomTexts.find(missionId);
            if (it != s_CustomTexts.end()) customText = it->second;
        }

        if (!customText.empty()) {
            FText** textArray = reinterpret_cast<FText**>(output + 0x10);
            int32_t* count = reinterpret_cast<int32_t*>(output + 0x18);
            if (textArray && *textArray && count && *count > 0) {
                (*textArray)[0] = FText(customText.c_str());
                *count = 1;
            }
        }
    }
    return result;
}

// ── Hook: GetMissionRewardMsg (menu reward display) ────────────────────
static __int64 __fastcall HkGetMissionRewardMsg(void* this_, void* execContext, __int64 output) {
    int32_t missionId = s_RewardMsgMissionId.load(std::memory_order_relaxed);

    if (missionId >= 0) {
        std::wstring customText;
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            auto it = s_CustomTexts.find(missionId);
            if (it != s_CustomTexts.end()) customText = it->second;
        }
        if (!customText.empty()) {
            PatchCacheForDisplay(missionId);
            std::lock_guard<std::mutex> lock(s_CallbackMutex);
            for (auto& cb : s_RewardShownCallbacks) {
                cb(missionId, customText);
            }
        }
    }

    return reinterpret_cast<decltype(&HkGetMissionRewardMsg)>(s_OrigRewardMsg)(this_, execContext, output);
}

// ── Hook: ItemGetName implementation (returns custom text for magic IDs) ─
static FText s_CustomItemNameFText;
static void* __fastcall HkItemGetNameDirect(int32_t itemId) {
    if (itemId >= 1000000) {
        std::wstring customName;
        {
            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
            auto it = s_CustomItemNames.find(itemId);
            if (it != s_CustomItemNames.end()) customName = it->second;
        }
        if (!customName.empty()) {
            s_CustomItemNameFText = FText(customName.c_str());
            return &s_CustomItemNameFText;
        }
    }
    return reinterpret_cast<decltype(&HkItemGetNameDirect)>(s_OrigItemGetName)(itemId);
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
    }

    void RemoveException(int32_t missionId) {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions.erase(missionId);
    }

    void ClearExceptions() {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions.clear();
    }

    void SetExceptions(std::initializer_list<int32_t> ids) {
        std::lock_guard<std::mutex> L(s_ExceptionMutex);
        s_Exceptions = std::unordered_set<int32_t>(ids);
    }

    bool IsException(int32_t missionId) {
        return IsException_Unlocked(missionId);
    }

    void SetCustomRewardText(int32_t missionId, const wchar_t* text) {
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            if (text && text[0]) {
                s_CustomTexts[missionId] = text;
            } else {
                s_CustomTexts.erase(missionId);
            }
        }
        int32_t magicId = 1000000 + missionId;
        {
            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
            s_CustomItemNames[magicId] = text;
        }
        LOG("[MissionRewardHook] Set custom reward text for mission {}: {}", missionId, text);
    }

    void ClearCustomRewardText(int32_t missionId) {
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            s_CustomTexts.erase(missionId);
        }
        {
            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
            s_CustomItemNames.erase(1000000 + missionId);
        }
    }

    void ClearAllCustomRewardTexts() {
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            s_CustomTexts.clear();
        }
        {
            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
            s_CustomItemNames.clear();
        }
    }

    void RegisterCustomItemName(int32_t itemId, const wchar_t* name) {
        std::lock_guard<std::mutex> lock(s_CustomItemMutex);
        if (name && name[0]) {
            s_CustomItemNames[itemId] = name;
        } else {
            s_CustomItemNames.erase(itemId);
        }
    }

    void ClearCustomItemName(int32_t itemId) {
        std::lock_guard<std::mutex> lock(s_CustomItemMutex);
        s_CustomItemNames.erase(itemId);
    }

    void ClearAllCustomItemNames() {
        std::lock_guard<std::mutex> lock(s_CustomItemMutex);
        s_CustomItemNames.clear();
    }

    void OnRewardShown(RewardShownCallback callback) {
        std::lock_guard<std::mutex> lock(s_CallbackMutex);
        s_RewardShownCallbacks.push_back(std::move(callback));
    }

    void Setup() {
        if (s_Resolved) return;
        LOG("[MissionRewardHook] Setup...");

        // Resolve helper native functions
        uint64_t mgrAddr = SignatureScanner::FindPattern(SIG_MANAGER_GET);
        if (mgrAddr) {
            s_GetManager = reinterpret_cast<FnGetManager>(mgrAddr);
        } else {
            LOG("[MissionRewardHook] ERROR: Manager getter signature NOT FOUND");
        }

        uint64_t cacheAddr = SignatureScanner::FindPattern(SIG_CACHE_LOOKUP);
        if (cacheAddr) {
            s_CacheLookup = reinterpret_cast<FnCacheLookup>(cacheAddr);
        } else {
            LOG("[MissionRewardHook] ERROR: Cache lookup signature NOT FOUND");
        }

        if (!s_GetManager || !s_CacheLookup) {
            LOG("[MissionRewardHook] Cannot hook — missing native function pointers");
            return;
        }

        // Reward evaluator hooks (items/macca and exp)
        uint64_t rewardEvalAddr = SignatureScanner::FindPattern(SIG_REWARD_EVAL);
        if (rewardEvalAddr) {
            uint64_t origAddr = 0;
            auto det = std::make_unique<PLH::x64Detour>(rewardEvalAddr,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkRewardEval, &s_OrigRewardEval)), &origAddr);
            if (det->hook()) {
                s_OrigRewardEval = PLH::FnCast(origAddr, s_OrigRewardEval);
                s_RewardEvalDetour = std::move(det);
                LOG("[MissionRewardHook] Reward eval hook installed");
            }
        }

        uint64_t expEvalAddr = SignatureScanner::FindPattern(SIG_EXP_EVAL);
        if (expEvalAddr) {
            uint64_t origAddr = 0;
            auto det = std::make_unique<PLH::x64Detour>(expEvalAddr,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkExpEval, &s_OrigExpEval)), &origAddr);
            if (det->hook()) {
                s_OrigExpEval = PLH::FnCast(origAddr, s_OrigExpEval);
                s_ExpEvalDetour = std::move(det);
                LOG("[MissionRewardHook] Exp eval hook installed");
            }
        }

        // CompleteMission pre-hook (restore originals, apply blocker)
        auto* compFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:CompleteMission"));
        if (!compFunc)
            compFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:CompleteMission"));

        if (compFunc) {
            auto* IdProp = compFunc->GetPropertyByName(STR("MissionId"));
            s_CompleteMissionHookId = compFunc->RegisterPreHook(
                [IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 id = -1;
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            id = *P;

                    if (id < 0) return;

                    // Clear custom text so reward evaluators stop re-patching
                    {
                        std::lock_guard<std::mutex> lock(s_CustomTextMutex);
                        s_CustomTexts.erase(id);
                    }
                    {
                        std::lock_guard<std::mutex> lock(s_CustomItemMutex);
                        s_CustomItemNames.erase(1000000 + id);
                    }

                    // Restore originals
                    {
                        std::lock_guard<std::mutex> lock(s_OriginalCacheMutex);
                        auto it = s_OriginalCache.find(id);
                        if (it != s_OriginalCache.end()) {
                            uint8_t* entry = GetCacheEntry(id);
                            if (entry) {
                                auto** pairArrayPtr = reinterpret_cast<int32_t**>(entry + CACHE_ITEM_ARRAY);
                                auto* pairArray = *pairArrayPtr;
                                if (pairArray && reinterpret_cast<uint64_t>(pairArray) > 0x10000) {
                                    auto& orig = it->second;
                                    for (int32_t i = 0; i < orig.count && i < 8; i++) {
                                        pairArray[i * 2]     = orig.itemIds[i];
                                        pairArray[i * 2 + 1] = orig.itemNums[i];
                                    }
                                    *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT) = orig.count;
                                    *reinterpret_cast<int32_t*>(entry + CACHE_MACCA) = orig.macca;
                                    *reinterpret_cast<int32_t*>(entry + CACHE_EXP) = orig.exp;
                                }
                            }
                            s_OriginalCache.erase(it);
                        }
                    }

                    // Apply blocker
                    if (ShouldBlock(id)) {
                        uint8_t* entry = GetCacheEntry(id);
                        if (entry) {
                            bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
                            *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT) = 0;
                            *reinterpret_cast<int32_t*>(entry + CACHE_MACCA)      = 0;
                            if (!keepExp)
                                *reinterpret_cast<int32_t*>(entry + CACHE_EXP)    = 0;
                        }
                        ZeroDescriptorStructs();
                    }
                }
            );
            LOG("[MissionRewardHook] CompleteMission hook registered");
        }

        // GetMissionInfoTagMessage hook (completion popup text)
        auto* infoTagFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:GetMissionInfoTagMessage"));
        if (!infoTagFunc)
            infoTagFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:GetMissionInfoTagMessage"));

        if (infoTagFunc) {
            auto* IdProp = infoTagFunc->GetPropertyByName(STR("MissionId"));
            auto* TextProp = infoTagFunc->GetPropertyByName(STR("infoText"));

            uint64_t infoTagAddr = reinterpret_cast<uint64_t>(infoTagFunc->GetFuncPtr());
            if (infoTagAddr) {
                uint64_t origAddr = 0;
                auto det = std::make_unique<PLH::x64Detour>(infoTagAddr,
                    reinterpret_cast<uint64_t>(PLH::FnCast(&HkGetMissionInfoTagMessage, &s_OrigInfoTag)), &origAddr);
                if (det->hook()) {
                    s_OrigInfoTag = PLH::FnCast(origAddr, s_OrigInfoTag);
                    s_InfoTagDetour = std::move(det);
                }
            }

            s_InfoTagHookId = infoTagFunc->RegisterPreHook(
                [IdProp, TextProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            s_InfoTagMissionId.store(*P, std::memory_order_relaxed);
                    if (TextProp)
                        if (auto* P = TextProp->ContainerPtrToValuePtr<uint8>(Ctx.TheStack.Locals()))
                            s_InfoTagTextType.store(*P, std::memory_order_relaxed);
                }
            );
        }

        // GetMissionRewardMsg hook (menu reward display)
        auto* rewardMsgFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:GetMissionRewardMsg"));
        if (!rewardMsgFunc)
            rewardMsgFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:GetMissionRewardMsg"));

        if (rewardMsgFunc) {
            auto* IdProp = rewardMsgFunc->GetPropertyByName(STR("MissionId"));

            uint64_t rewardMsgAddr = reinterpret_cast<uint64_t>(rewardMsgFunc->GetFuncPtr());
            if (rewardMsgAddr) {
                uint64_t origAddr = 0;
                auto det = std::make_unique<PLH::x64Detour>(rewardMsgAddr,
                    reinterpret_cast<uint64_t>(PLH::FnCast(&HkGetMissionRewardMsg, &s_OrigRewardMsg)), &origAddr);
                if (det->hook()) {
                    s_OrigRewardMsg = PLH::FnCast(origAddr, s_OrigRewardMsg);
                    s_RewardMsgDetour = std::move(det);
                }
            }

            s_RewardMsgPreHookId = rewardMsgFunc->RegisterPreHook(
                [IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            s_RewardMsgMissionId.store(*P, std::memory_order_relaxed);
                }
            );
        }

        // ItemGetName direct implementation hook (returns custom text for magic IDs)
        {
            uint64_t origAddr = 0;
            auto det = std::make_unique<PLH::x64Detour>(ITEM_GET_NAME_IMPL,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkItemGetNameDirect, &s_OrigItemGetName)), &origAddr);
            if (det->hook()) {
                s_OrigItemGetName = PLH::FnCast(origAddr, s_OrigItemGetName);
                s_ItemGetNameDetour = std::move(det);
                LOG("[MissionRewardHook] ItemGetName direct hook installed");
            } else {
                LOG("[MissionRewardHook] ERROR: ItemGetName direct hook FAILED");
            }
        }

        s_Resolved = true;
        LOG("[MissionRewardHook] Setup complete");
    }
}
