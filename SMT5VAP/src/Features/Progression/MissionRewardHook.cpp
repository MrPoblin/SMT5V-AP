#include "MissionRewardHook.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include "src/Features/Items/ItemTableInjector.hpp"
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

// ── PolyHook2 detours for mission menu reward text ─────────────────────
static std::unique_ptr<PLH::x64Detour> s_InfoTagDetour;
static uint64_t s_OrigInfoTag = 0;
static CallbackId s_InfoTagHookId{-1};

static std::unique_ptr<PLH::x64Detour> s_RewardMsgDetour;
static uint64_t s_OrigRewardMsg = 0;
static CallbackId s_RewardMsgHookId{-1};

static std::unique_ptr<PLH::x64Detour> s_ItemGetNameDetour;
static uint64_t s_OrigItemGetName = 0;

// ── Direct hook on ItemGetName implementation (FUN_1473a9130) ──────────
// The UFunction wrapper hook doesn't fire because the widget calls the
// native implementation directly, bypassing UFunction dispatch.
static std::unique_ptr<PLH::x64Detour> s_ItemGetNameDirectDetour;
static uint64_t s_OrigItemGetNameDirect = 0;

// ── State ──────────────────────────────────────────────────────────────
static std::atomic<MissionRewardHook::FilterMode> s_Mode{MissionRewardHook::FilterMode::Disabled};
static std::mutex s_ExceptionMutex;
static std::unordered_set<int32_t> s_Exceptions;
static bool s_Resolved = false;
static CallbackId s_CompleteMissionHookId{-1};

// ── Custom reward text state ───────────────────────────────────────────
static std::atomic<int32_t> s_InfoTagMissionId{-1};
static std::atomic<uint8_t> s_InfoTagTextType{0};
static std::mutex s_CustomTextMutex;
static std::unordered_map<int32_t, std::wstring> s_CustomTexts;

// ── Custom item name lookup (used by ItemGetName hook) ─────────────────
static std::mutex s_CustomItemMutex;
static std::unordered_map<int32_t, std::wstring> s_CustomItemNames;
static CallbackId s_ItemGetNameHookId{-1};

// ── Mission reward shown callback ──────────────────────────────────────
static std::mutex s_CallbackMutex;
static std::vector<MissionRewardHook::RewardShownCallback> s_RewardShownCallbacks;

// ── Original cache values (saved before patching, restored on complete) ─
struct OriginalCacheValues {
    int32_t itemIds[8];   // up to8 items
    int32_t itemNums[8];
    int32_t count;
    int32_t macca;
    int32_t exp;
};
static std::mutex s_OriginalCacheMutex;
static std::unordered_map<int32_t, OriginalCacheValues> s_OriginalCache;

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

    // Save originals before zeroing
    {
        auto** pairArrayPtr = reinterpret_cast<int32_t**>(entry + CACHE_ITEM_ARRAY);
        auto* pairArray = *pairArrayPtr;
        std::lock_guard<std::mutex> lock(s_OriginalCacheMutex);
        if (s_OriginalCache.find(missionId) == s_OriginalCache.end() && pairArray) {
            OriginalCacheValues orig{};
            int32_t saveCount = (oldCount > 8) ? 8 : oldCount;
            for (int32_t i = 0; i < saveCount; i++) {
                orig.itemIds[i]  = pairArray[i * 2];
                orig.itemNums[i] = pairArray[i * 2 + 1];
            }
            orig.count = oldCount;
            orig.macca = oldMacca;
            orig.exp   = oldExp;
            s_OriginalCache[missionId] = orig;
        }
    }

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

// Patch cache with magic item ID for display (called after zeroing)
static void PatchCacheForDisplay(int32_t missionId) {
    std::wstring customText;
    {
        std::lock_guard<std::mutex> lock(s_CustomTextMutex);
        auto it = s_CustomTexts.find(missionId);
        if (it == s_CustomTexts.end()) return;
    }

    int32_t magicId = 1000000 + missionId;
    uint8_t* entry = GetCacheEntry(missionId);
    if (!entry) return;

    auto** pairArrayPtr = reinterpret_cast<int32_t**>(entry + CACHE_ITEM_ARRAY);
    auto* pairArray = *pairArrayPtr;
    if (!pairArray || reinterpret_cast<uint64_t>(pairArray) <= 0x10000) return;

    // Save originals if not already saved (for restoration on complete)
    {
        int32_t currentCount = *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT);
        int32_t macca = *reinterpret_cast<int32_t*>(entry + CACHE_MACCA);
        int32_t exp = *reinterpret_cast<int32_t*>(entry + CACHE_EXP);
        std::lock_guard<std::mutex> lock(s_OriginalCacheMutex);
        if (s_OriginalCache.find(missionId) == s_OriginalCache.end()) {
            OriginalCacheValues orig{};
            int32_t saveCount = (currentCount > 8) ? 8 : currentCount;
            for (int32_t i = 0; i < saveCount; i++) {
                orig.itemIds[i]  = pairArray[i * 2];
                orig.itemNums[i] = pairArray[i * 2 + 1];
            }
            orig.count = currentCount;
            orig.macca = macca;
            orig.exp   = exp;
            s_OriginalCache[missionId] = orig;
        }
    }

    pairArray[0] = magicId;
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
    LOG("[MissionRewardHook] Zeroed descriptor structs (type 1+2)");
}

static void BlockRewards(int32_t missionId) {
    bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
    ZeroCacheEntry(missionId, keepExp);
}

// ── Hook: items/macca evaluator (sub_1474F1B90) ────────────────────────
static __int64 __fastcall HkRewardEval(int a1, int* a2, int* a3) {
    auto result = reinterpret_cast<decltype(&HkRewardEval)>(s_OrigRewardEval)(a1, a2, a3);
    if (ShouldBlock(a1)) {
        BlockRewards(a1);
        PatchCacheForDisplay(a1);
    }
    return result;
}

// ── Hook: exp evaluator (sub_140AEA250) ────────────────────────────────
static __int64 __fastcall HkExpEval(int a1) {
    auto result = reinterpret_cast<decltype(&HkExpEval)>(s_OrigExpEval)(a1);
    if (ShouldBlock(a1)) {
        bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
        ZeroCacheEntry(a1, keepExp);
        PatchCacheForDisplay(a1);
    }
    return result;
}

// ── Hook: GetDataTableRowFromName via UFunction post-hook ────────────
// Intercepts all BP-callable DataTable lookups.
static void SetupDataTableRowHook() {
    auto* func = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Engine.DataTableFunctionLibrary:GetDataTableRowFromName"));
    if (!func) {
        WARN("[MissionRewardHook] GetDataTableRowFromName UFunction NOT FOUND");
        return;
    }

    auto* RowNameProp = func->GetPropertyByName(STR("RowName"));
    if (!RowNameProp) return;

    func->RegisterPostHook(
        [RowNameProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            auto* rowName = RowNameProp->ContainerPtrToValuePtr<FName>(Ctx.TheStack.Locals());
            if (!rowName) return;

            auto str = rowName->ToString();
            // Check if the row name is a numeric item ID >= 1000000
            if (rowName->ToString().find(L"10000") != std::wstring::npos) {
                LOG("[MissionRewardHook] DataTable RowName={} queried", str);
            }
        });
    LOG("[MissionRewardHook] GetDataTableRowFromName diag hook installed");
}
// Intercepts ALL calls (BP and native) to return custom names for magic IDs
static void __fastcall HkItemGetName(void* this_, FFrame* Stack, void* Result) {
    auto* IdProp = Stack->Node()->GetPropertyByName(STR("ID"));
    int32_t itemId = -1;
    if (IdProp) {
        auto* idPtr = IdProp->ContainerPtrToValuePtr<int32_t>(Stack->Locals());
        if (idPtr) itemId = *idPtr;
    }

    if (itemId >= 1000000) {
        std::wstring customName;
        {
            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
            auto it = s_CustomItemNames.find(itemId);
            if (it != s_CustomItemNames.end()) customName = it->second;
        }
        if (!customName.empty()) {
            FText ft(customName.c_str());
            memcpy(Result, &ft, sizeof(FText));
            LOG("[MissionRewardHook] Binary ItemGetName(ID={}) => \"{}\"", itemId, customName);
            return;
        }
    }

    reinterpret_cast<decltype(&HkItemGetName)>(s_OrigItemGetName)(this_, Stack, Result);
}

// ── Direct hook on ItemGetName native implementation ───────────────────
// FUN_1473a9130: takes int itemId, returns FText* (pointer to item name text).
// The widget calls this directly, bypassing the UFunction wrapper.
// For magic IDs (>=1000000), we return a pointer to our custom FText.
// For normal IDs, we call the original which has the range check (1-895).
static FText s_CustomItemNameFText; // static buffer for the returned FText
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
            LOG("[MissionRewardHook] Direct ItemGetName(ID={}) => \"{}\"", itemId, customName);
            return &s_CustomItemNameFText;
        }
    }
    return reinterpret_cast<decltype(&HkItemGetNameDirect)>(s_OrigItemGetNameDirect)(itemId);
}
// Called by Blueprint: GetMissionInfoTagMessage(MissionId, E_MISSION_INFO_TEXT infoText)
// Replaces reward text (infoText==2) in the output FScriptMessage with custom text
static __int64 __fastcall HkGetMissionInfoTagMessage(void* this_, void* execContext, __int64 output) {
    auto result = reinterpret_cast<decltype(&HkGetMissionInfoTagMessage)>(s_OrigInfoTag)(this_, execContext, output);

    int32_t missionId = s_InfoTagMissionId.load(std::memory_order_relaxed);
    uint8_t textType = s_InfoTagTextType.load(std::memory_order_relaxed);

    // Only intercept E_MISSION_INFO_TEXT_REWARD (2) for missions with custom text
    if (textType == 2 && missionId >= 0) {
        std::wstring customText;
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            auto it = s_CustomTexts.find(missionId);
            if (it != s_CustomTexts.end()) {
                customText = it->second;
            }
        }

        if (!customText.empty()) {
            // output = FScriptMessage*
            // TArray<FText> MessageTextPages at offset 0x10:
            //   +0x10: FText* Data  (pointer to FText array)
            //   +0x18: int32  Count (number of elements)
            FText** textArray = reinterpret_cast<FText**>(output + 0x10);
            int32_t* count = reinterpret_cast<int32_t*>(output + 0x18);

            if (textArray && *textArray && count && *count > 0) {
                (*textArray)[0] = FText(customText.c_str());
                *count = 1;
                LOG("[MissionRewardHook] Replaced reward text for mission {}: {}",
                    missionId, customText);
            } else {
                LOG("[MissionRewardHook] Cannot replace reward text for mission {}: no existing text pages", missionId);
            }
        }
    }

    return result;
}

// ── Hook: execGetMissionRewardMsg (sub_14B89A710) ──────────────────────
// Called by Blueprint: GetMissionRewardMsg(MissionId, int32& outTag1, int32& outTag2)
// This is the primary function the mission menu uses to display reward text.
// Strategy: save original cache values, patch to show custom text, restore on complete.
static CallbackId s_RewardMsgPreHookId{-1};
static std::atomic<int32_t> s_RewardMsgMissionId{-1};

static __int64 __fastcall HkGetMissionRewardMsg(void* this_, void* execContext, __int64 output) {
    int32_t missionId = s_RewardMsgMissionId.load(std::memory_order_relaxed);

    // Also patch cache here (in case reward evaluators haven't fired yet)
    if (missionId >= 0) {
        std::wstring customText;
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            auto it = s_CustomTexts.find(missionId);
            if (it != s_CustomTexts.end()) customText = it->second;
        }
        if (!customText.empty()) {
            PatchCacheForDisplay(missionId);

            // Fire callbacks
            {
                std::lock_guard<std::mutex> lock(s_CallbackMutex);
                for (auto& cb : s_RewardShownCallbacks) {
                    cb(missionId, customText);
                }
            }
        }
    }

    auto result = reinterpret_cast<decltype(&HkGetMissionRewardMsg)>(s_OrigRewardMsg)(this_, execContext, output);
    return result;
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

    void SetCustomRewardText(int32_t missionId, const wchar_t* text) {
        {
            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
            if (text && text[0]) {
                s_CustomTexts[missionId] = text;
            } else {
                s_CustomTexts.erase(missionId);
            }
        }
        // Register magic item ID for the binary ItemGetName hook
        int32_t magicId = 1000000 + missionId;
        {
            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
            s_CustomItemNames[magicId] = text;
        }
        // Also inject into the item DataTable via ItemTableInjector
        ItemTableInjector::RegisterCustomItem(magicId, text);
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
            LOG("[MissionRewardHook] Registered custom name for item {}: {}", itemId, name);
        } else {
            s_CustomItemNames.erase(itemId);
            LOG("[MissionRewardHook] Cleared custom name for item {}", itemId);
        }
    }

    void ClearCustomItemName(int32_t itemId) {
        std::lock_guard<std::mutex> lock(s_CustomItemMutex);
        s_CustomItemNames.erase(itemId);
        LOG("[MissionRewardHook] Cleared custom name for item {}", itemId);
    }

    void ClearAllCustomItemNames() {
        std::lock_guard<std::mutex> lock(s_CustomItemMutex);
        s_CustomItemNames.clear();
        LOG("[MissionRewardHook] Cleared all custom item names");
    }

    void OnRewardShown(RewardShownCallback callback) {
        std::lock_guard<std::mutex> lock(s_CallbackMutex);
        s_RewardShownCallbacks.push_back(std::move(callback));
        LOG("[MissionRewardHook] Registered reward shown callback");
    }

    // ── Diagnostic: hook every script message lookup to see what the menu queries ──
    // We'll hook UMessageUI::GetMessageDataFromId and similar to trace the exact lookup path
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

                    if (id >= 0) {
                        // Clear custom text so reward evaluators stop re-patching
                        {
                            std::lock_guard<std::mutex> lock(s_CustomTextMutex);
                            s_CustomTexts.erase(id);
                        }
                        {
                            std::lock_guard<std::mutex> lock(s_CustomItemMutex);
                            s_CustomItemNames.erase(1000000 + id);
                        }

                        // Restore originals (undo display patch)
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
                                        LOG("[MissionRewardHook] Restored originals for mission {} ({} items)", id, orig.count);
                                    }
                                }
                                s_OriginalCache.erase(it);
                            }
                        }

                        // Apply blocker: force-zero
                        if (ShouldBlock(id)) {
                            uint8_t* entry = GetCacheEntry(id);
                            if (entry) {
                                bool keepExp = s_Mode.load(std::memory_order_relaxed) == MissionRewardHook::FilterMode::KeepExp;
                                *reinterpret_cast<int32_t*>(entry + CACHE_ITEM_COUNT)  = 0;
                                *reinterpret_cast<int32_t*>(entry + CACHE_MACCA)       = 0;
                                if (!keepExp)
                                    *reinterpret_cast<int32_t*>(entry + CACHE_EXP)     = 0;
                                LOG("[MissionRewardHook] CompleteMission: blocked rewards for mission {}", id);
                            }
                            ZeroDescriptorStructs();
                        }
                    }
                }
            );
            LOG("[MissionRewardHook] CompleteMission pre-hook registered (id={})", s_CompleteMissionHookId);
        } else {
            LOG("[MissionRewardHook] ERROR: CompleteMission UFunction NOT FOUND");
        }

        // ── UFunction pre-hook + binary hook on GetMissionInfoTagMessage ──
        auto* infoTagFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:GetMissionInfoTagMessage"));
        if (!infoTagFunc)
            infoTagFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:GetMissionInfoTagMessage"));

        if (infoTagFunc) {
            auto* IdProp = infoTagFunc->GetPropertyByName(STR("MissionId"));
            auto* TextProp = infoTagFunc->GetPropertyByName(STR("infoText"));
            LOG("[MissionRewardHook] GetMissionInfoTagMessage UFunction found, MissionId prop={}, infoText prop={}",
                IdProp ? 1 : 0, TextProp ? 1 : 0);

            // ── Resolve native function address from UFunction ──
            uint64_t infoTagAddr = reinterpret_cast<uint64_t>(infoTagFunc->GetFuncPtr());
            LOG("[MissionRewardHook] GetMissionInfoTagMessage native exec at 0x{:x}", infoTagAddr);

            if (infoTagAddr) {
                uint64_t origAddr = 0;
                auto det = std::make_unique<PLH::x64Detour>(
                    infoTagAddr,
                    reinterpret_cast<uint64_t>(PLH::FnCast(&HkGetMissionInfoTagMessage, &s_OrigInfoTag)),
                    &origAddr
                );
                if (det->hook()) {
                    s_OrigInfoTag = PLH::FnCast(origAddr, s_OrigInfoTag);
                    s_InfoTagDetour = std::move(det);
                    LOG("[MissionRewardHook] InfoTag binary hook installed at 0x{:x}", infoTagAddr);
                } else {
                    LOG("[MissionRewardHook] ERROR: InfoTag binary hook FAILED at 0x{:x}", infoTagAddr);
                }
            } else {
                LOG("[MissionRewardHook] ERROR: GetMissionInfoTagMessage native func ptr is null");
            }

            // ── UFunction pre-hook to capture missionId and infoText type ──
            s_InfoTagHookId = infoTagFunc->RegisterPreHook(
                [IdProp, TextProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 id = -1;
                    uint8 textType = 0;
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            id = *P;
                    if (TextProp)
                        if (auto* P = TextProp->ContainerPtrToValuePtr<uint8>(Ctx.TheStack.Locals()))
                            textType = *P;

                    s_InfoTagMissionId.store(id, std::memory_order_relaxed);
                    s_InfoTagTextType.store(textType, std::memory_order_relaxed);
                }
            );
            LOG("[MissionRewardHook] GetMissionInfoTagMessage pre-hook registered (id={})", s_InfoTagHookId);
        } else {
            LOG("[MissionRewardHook] ERROR: GetMissionInfoTagMessage UFunction NOT FOUND");
        }

        // ── UFunction pre-hook + binary hook on GetMissionRewardMsg ──
        auto* rewardMsgFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MissionData:GetMissionRewardMsg"));
        if (!rewardMsgFunc)
            rewardMsgFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MissionData_C:GetMissionRewardMsg"));

        if (rewardMsgFunc) {
            auto* IdProp = rewardMsgFunc->GetPropertyByName(STR("MissionId"));
            LOG("[MissionRewardHook] GetMissionRewardMsg UFunction found, MissionId prop={}",
                IdProp ? 1 : 0);

            // Native exec address from UFunction
            uint64_t rewardMsgAddr = reinterpret_cast<uint64_t>(rewardMsgFunc->GetFuncPtr());
            LOG("[MissionRewardHook] GetMissionRewardMsg native exec at 0x{:x}", rewardMsgAddr);

            if (rewardMsgAddr) {
                uint64_t origAddr = 0;
                auto det = std::make_unique<PLH::x64Detour>(
                    rewardMsgAddr,
                    reinterpret_cast<uint64_t>(PLH::FnCast(&HkGetMissionRewardMsg, &s_OrigRewardMsg)),
                    &origAddr
                );
                if (det->hook()) {
                    s_OrigRewardMsg = PLH::FnCast(origAddr, s_OrigRewardMsg);
                    s_RewardMsgDetour = std::move(det);
                    LOG("[MissionRewardHook] RewardMsg binary hook installed at 0x{:x}", rewardMsgAddr);
                } else {
                    LOG("[MissionRewardHook] ERROR: RewardMsg binary hook FAILED");
                }
            }

            // Pre-hook to capture missionId
            s_RewardMsgHookId = rewardMsgFunc->RegisterPreHook(
                [IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 id = -1;
                    if (IdProp)
                        if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                            id = *P;
                    s_RewardMsgMissionId.store(id, std::memory_order_relaxed);
                }
            );
            LOG("[MissionRewardHook] GetMissionRewardMsg pre-hook registered (id={})", s_RewardMsgHookId);
        } else {
            LOG("[MissionRewardHook] GetMissionRewardMsg UFunction NOT FOUND");
        }

        // ── Diagnostic hooks to find what the mission menu queries for items ──
        auto* itemGetNameFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_ItemData:ItemGetName"));
        if (itemGetNameFunc) {
            auto* idP = itemGetNameFunc->GetPropertyByName(STR("ID"));
            // Binary PolyHook on the native exec function to intercept ALL calls
            uint64_t itemGetNameAddr = reinterpret_cast<uint64_t>(itemGetNameFunc->GetFuncPtr());
            LOG("[MissionRewardHook] ItemGetName native exec at 0x{:x}", itemGetNameAddr);
            if (itemGetNameAddr) {
                uint64_t origAddr = 0;
                auto det = std::make_unique<PLH::x64Detour>(
                    itemGetNameAddr,
                    reinterpret_cast<uint64_t>(PLH::FnCast(&HkItemGetName, &s_OrigItemGetName)),
                    &origAddr
                );
                if (det->hook()) {
                    s_OrigItemGetName = PLH::FnCast(origAddr, s_OrigItemGetName);
                    s_ItemGetNameDetour = std::move(det);
                    LOG("[MissionRewardHook] ItemGetName binary hook installed at 0x{:x}", itemGetNameAddr);
                } else {
                    LOG("[MissionRewardHook] ERROR: ItemGetName binary hook FAILED");
                }
            }
            // Also keep BP post-hook for diagnostics
            s_ItemGetNameHookId = itemGetNameFunc->RegisterPostHook(
                [idP](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    if (!idP) return;
                    auto* ptr = idP->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                    if (!ptr) return;
                    if (*ptr >= 1000000)
                        LOG("[MissionRewardHook] +++ ItemGetName called for ID={}", *ptr);
                });
            LOG("[MissionRewardHook] ItemGetName diag hook (id={})", s_ItemGetNameHookId);
        }

        auto* getItemDataFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_ItemData:GetUseItemData"));
        if (getItemDataFunc) {
            auto* idP = getItemDataFunc->GetPropertyByName(STR("ID"));
            getItemDataFunc->RegisterPostHook(
                [idP](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    if (!idP) return;
                    auto* ptr = idP->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                    if (!ptr) return;
                    if (*ptr >= 1000000)
                        LOG("[MissionRewardHook] +++ GetUseItemData called for ID={}", *ptr);
                });
            LOG("[MissionRewardHook] GetUseItemData diag hook registered");
        }

        // ── Direct hook on the ItemGetName IMPLEMENTATION (not UFunction wrapper) ──
        // FUN_1473a9130 is the actual native function that resolves item ID to name.
        // The UFunction wrapper at 0x140CCBAF0 jumps to a dispatch stub, but the
        // widget may call the implementation directly, bypassing our UFunction hook.
        // This function has a range check: (param_1 - 1) < 0x37f (IDs 1-895 only).
        // Our magic IDs (1000000+) fail this check and return empty text.
        {
            constexpr uint64_t ITEM_GET_NAME_IMPL = 0x1473a9130;
            uint64_t origAddr = 0;
            auto det = std::make_unique<PLH::x64Detour>(
                ITEM_GET_NAME_IMPL,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkItemGetNameDirect, &s_OrigItemGetNameDirect)),
                &origAddr
            );
            if (det->hook()) {
                s_OrigItemGetNameDirect = PLH::FnCast(origAddr, s_OrigItemGetNameDirect);
                s_ItemGetNameDirectDetour = std::move(det);
                LOG("[MissionRewardHook] ItemGetName DIRECT hook installed at 0x{:x}", ITEM_GET_NAME_IMPL);
            } else {
                LOG("[MissionRewardHook] ERROR: ItemGetName direct hook FAILED at 0x{:x}", ITEM_GET_NAME_IMPL);
            }
        }

        s_Resolved = true;
        LOG("[MissionRewardHook] Setup complete");
    }
}
