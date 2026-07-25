#include "FusionGating.hpp"
#include "src/HookHelper.hpp"
#include "src/Archipelago/APState.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/World.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <vector>
#include <cstring>
#include <atomic>
#include <string>
#include <set>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

namespace FusionGating {

// ── Race name table ──
static const char* s_RaceNames[50] = {
    "None", "Unused", "Herald", "Megami", "Avian", "Divine", "Yoma", "Vile", "Raptor", "Unused9",
    "Deity", "Wargod", "Avatar", "Holy", "Genma", "Element", "Mitama", "Fairy", "Beast", "Jirae",
    "Fiend", "Jaki", "Wilder", "Fury", "Lady", "Dragon", "Kishin", "Kunitsu", "Femme", "Brute",
    "Fallen", "Night", "Snake", "Tyrant", "Drake", "Haunt", "Foul", "Chaos", "Devil", "Meta",
    "Nahobino", "Proto-fiend", "Matter", "Panagia", "Enigma", "UMA", "Qadistu", "Human", "Primal", "Void"
};

std::string RaceName(int32_t race) {
    if (race < 0 || race >= 50) return "Unknown";
    return s_RaceNames[race];
}

static std::atomic<bool> s_Enabled{true};
static std::atomic<bool> s_EssenceGatingEnabled{true};

void SetEnabled(bool enabled) {
    s_Enabled.store(enabled, std::memory_order_release);
    LOG("[FusionGating] SetEnabled({})", enabled);
}
bool IsEnabled() {
    return s_Enabled.load(std::memory_order_acquire);
}
void SetEssenceGatingEnabled(bool enabled) {
    s_EssenceGatingEnabled.store(enabled, std::memory_order_release);
    LOG("[FusionGating] SetEssenceGatingEnabled({})", enabled);
}
bool IsEssenceGatingEnabled() {
    return s_EssenceGatingEnabled.load(std::memory_order_acquire);
}

// ── Race lookup via BPL_DevilData:GetRaceIdByDevilId ──
static UFunction* s_GetRaceIdFn = nullptr;
static UObject*   s_DevilDataCDO = nullptr;
static bool       s_RaceInitFailed = false;

static void InitRaceLookup() {
    if (s_GetRaceIdFn || s_RaceInitFailed) return;

    for (auto* p : {
        STR("/Script/Project.BPL_DevilData:GetRaceIdByDevilId"),
        STR("/Script/Project.BPL_DevilData_C:GetRaceIdByDevilId"),
    }) {
        s_GetRaceIdFn = UObjectGlobals::FindObject<UFunction>(nullptr, p);
        if (s_GetRaceIdFn) break;
    }
    if (!s_GetRaceIdFn) { WARN("[FusionGating] BPL_DevilData:GetRaceIdByDevilId NOT FOUND"); s_RaceInitFailed = true; return; }

    for (auto* p : {
        STR("/Script/Project.Default__BPL_DevilData"),
        STR("/Script/Project.Default__BPL_DevilData_C"),
    }) {
        s_DevilDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, p);
        if (s_DevilDataCDO) break;
    }
    if (!s_DevilDataCDO) {
        auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_DevilData"));
        if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_DevilData_C"));
        if (cls) s_DevilDataCDO = cls->CreateDefaultObject();
    }
    if (!s_DevilDataCDO) { WARN("[FusionGating] BPL_DevilData CDO NOT FOUND"); s_RaceInitFailed = true; return; }

    LOG("[FusionGating] Race lookup ready");
}

static int32 GetDevilRaceId(int32 devilId) {
    InitRaceLookup();
    if (!s_GetRaceIdFn || !s_DevilDataCDO) return -1;
    auto* retProp = s_GetRaceIdFn->GetPropertyByName(STR("ReturnValue"));
    if (!retProp) return -1;
    std::vector<uint8> params(0x100, 0);
    *reinterpret_cast<int32*>(params.data()) = devilId;  // InDevilId at offset 0
    s_DevilDataCDO->ProcessEvent(s_GetRaceIdFn, params.data());
    return *retProp->ContainerPtrToValuePtr<int32>(params.data());
}

// ── Cached devilId→race table ──
// ProcessEvent from inside a list-build hook is unsafe (crashes). So we precompute
// the whole table ONCE at setup (outside any hook) and the hooks do a pure lookup.
static std::vector<int32> s_RaceCache;   // index by devilId; -1 = unknown
static bool s_RaceCacheReady = false;

static void BuildRaceCache() {
    if (s_RaceCacheReady) return;
    InitRaceLookup();
    s_RaceCache.assign(4096, -1);
    if (s_GetRaceIdFn && s_DevilDataCDO) {
        for (int id = 1; id < (int)s_RaceCache.size(); ++id) {
            int32 r = GetDevilRaceId(id);
            if (r >= 0 && r < 50) s_RaceCache[id] = r;
        }
        LOG("[FusionGating] Race cache built ({} ids)", (int)s_RaceCache.size());
    } else {
        WARN("[FusionGating] Race cache NOT built (lookup unavailable)");
    }
    s_RaceCacheReady = true;
}

static int32 CachedRace(int32 devilId) {
    if (devilId <= 0 || (size_t)devilId >= s_RaceCache.size()) return -1;
    return s_RaceCache[devilId];
}

// ── Compendium registration check via BPL_BibleData:CheckBibleEntry ──
static UFunction* s_CheckBibleEntry = nullptr;
static UObject* s_BibleCDO = nullptr;
static bool s_BibleInitFailed = false;

static void InitBibleLookup() {
    if (s_CheckBibleEntry || s_BibleInitFailed) return;

    for (auto* p : {
        STR("/Script/Project.BPL_BibleData:CheckBibleEntry"),
        STR("/Script/Project.BPL_BibleData_C:CheckBibleEntry"),
    }) {
        s_CheckBibleEntry = UObjectGlobals::FindObject<UFunction>(nullptr, p);
        if (s_CheckBibleEntry) break;
    }
    if (!s_CheckBibleEntry) { WARN("[FusionGating] BPL_BibleData:CheckBibleEntry NOT FOUND"); s_BibleInitFailed = true; return; }

    for (auto* p : {
        STR("/Script/Project.Default__BPL_BibleData"),
        STR("/Script/Project.Default__BPL_BibleData_C"),
    }) {
        s_BibleCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, p);
        if (s_BibleCDO) break;
    }
    if (!s_BibleCDO) {
        auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_BibleData"));
        if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_BibleData_C"));
        if (cls) s_BibleCDO = cls->CreateDefaultObject();
    }
    if (!s_BibleCDO) { WARN("[FusionGating] BPL_BibleData CDO NOT FOUND"); s_BibleInitFailed = true; return; }

    LOG("[FusionGating] Bible lookup ready");
}

static bool IsInCompendium(int32_t devilId) {
    InitBibleLookup();
    if (!s_CheckBibleEntry || !s_BibleCDO) return true;
    struct { int32_t DevilID; bool ReturnValue; } params{ devilId, false };
    s_BibleCDO->ProcessEvent(s_CheckBibleEntry, &params);
    return params.ReturnValue;
}

// ── Compendium cache: precomputed after save data is available (not at Setup) ──
static std::vector<bool> s_CompendiumCache;
static bool s_CompendiumCacheReady = false;

static void BuildCompendiumCache() {
    if (s_CompendiumCacheReady) return;
    InitBibleLookup();
    s_CompendiumCache.assign(4096, false);
    if (s_CheckBibleEntry && s_BibleCDO) {
        int count = 0;
        for (int id = 1; id < (int)s_CompendiumCache.size(); ++id) {
            if (IsInCompendium(id)) {
                s_CompendiumCache[id] = true;
                ++count;
            }
        }
        LOG("[FusionGating] Compendium cache built ({} registered)", count);
    } else {
        WARN("[FusionGating] Compendium cache NOT built (bible lookup unavailable)");
    }
    s_CompendiumCacheReady = true;
}

static bool CachedIsInCompendium(int32_t devilId) {
    if (devilId <= 0 || (size_t)devilId >= s_CompendiumCache.size()) return true;
    return s_CompendiumCache[devilId];
}

// ── Call a no-arg / simple UFunction on an object, read int32 ReturnValue ──
[[maybe_unused]] static int32 CallInt32(UObject* Obj, const wchar_t* funcName) {
    if (!Obj) return -1;
    UFunction* F = Obj->GetFunctionByName(funcName);
    if (!F) F = Obj->GetFunctionByNameInChain(funcName);
    if (!F) return -1;
    struct { int32 ReturnValue; } p{};
    Obj->ProcessEvent(F, &p);
    return p.ReturnValue;
}

// ── Call a UFunction taking one 4-byte (int32/bool) arg, read int32 return ──
static int32 CallInt32Arg(UObject* Obj, const wchar_t* funcName, int32 arg) {
    if (!Obj) return -1;
    UFunction* F = Obj->GetFunctionByName(funcName);
    if (!F) F = Obj->GetFunctionByNameInChain(funcName);
    if (!F) return -1;
    struct { int32 InArg; int32 ReturnValue; } p{ arg, -1 };
    Obj->ProcessEvent(F, &p);
    return p.ReturnValue;
}

// ── Call an int32-index UFunction (no return), e.g. an unselectable setter ──
// ── Read the result DevilID at an absolute list index ──
static int32 GetResultDevilAt(UObject* Panel, UFunction* IdF, int32 absIndex) {
    if (!Panel || !IdF) return -1;
    struct { int32 InIndex; int32 ReturnValue; } p{ absIndex, -1 };
    Panel->ProcessEvent(IdF, &p);
    return p.ReturnValue;
}

static bool IsGatedResult(UObject* Panel, UFunction* IdF, int32 absIndex) {
    int32 devilId = GetResultDevilAt(Panel, IdF, absIndex);
    if (devilId <= 0) return false;
    int32 race = GetDevilRaceId(devilId);
    return (race >= 0 && APState::FusionRaces::IsRaceGated(race));
}

// ── Layer A: gray gated-race results at DRAW TIME ──
// The game's native CanBeSelectedAsSearchFusion is NOT reflective (can't be
// hooked). But BIESetInitializePanelAnimation(slot) fires each time the game
// (re)draws a visible panel slot — including after every scroll — so it's the
// correct, flash-free place to apply the gray. slot = InIndex; the absolute
// result index = offset + slot. If that result's race is gated we mark the slot
// unselectable via BIESetUniteCharaPanelUnselectablePanelOn(slot).
//
// We check whichever result list is currently populated (Search covers normal +
// dyad reverse-search; Special covers the special-fusion result screen).
// ── Layer C: native gate on the fusion result-validity predicate ──
// Analysis (IDA): the function that actually decides whether a fusion RESULT
// entry is selectable is sub_140BB6460 (CreateFusionResultEntryList). For each
// candidate result devil (v11) it computes an error code:
//     if ( sub_140AB6E40(v11) ) v24 = 2;   // CANNOT -> entry disabled
//     else { level / space / cost checks -> v24 = 0..5 }
//     bEnabled = (v24 == 0);                // written into the result entry
// The commit path consults bEnabled, so disabling the entry both grays it AND
// blocks confirming it. sub_140AB6E40 is the predicate that drives this.
//
// sub_140AB6E40 is a Blueprint thunk -> real native method sub_14720D3F0(int
// devilId) -> char, returning 1 if devilId is in a fixed 30-entry table (story
// / non-fusable demons), else 0. We hook the REAL method and make it ADDITIVELY
// return 1 for gated-race demons: return orig(a1) || IsRaceGated(race(a1)).
// That disables every fusion result whose race is gated, for all fusion types.
//
// Address derivation: the CanBeSelectedAsSearchFusion UFunction thunk lives at
// static 0x140D5E440; its runtime address gives the module base. The predicate
// real method is at static 0x14720D3F0.
using TPredicate = char(__fastcall*)(int devilId);
static TPredicate s_OrigPred = nullptr;
static std::unique_ptr<PLH::x64Detour> s_PredDetour;
static std::atomic<bool> s_Installed{false};

// NOTE: This predicate is called for BOTH fusion RESULTS and fusion INGREDIENTS
// (e.g. the special-fusion ingredient build loop in sub_140BB7950). Forcing
// gated demons here would mark gated INGREDIENTS as owned/unfuseable -> they
// render as placeholders on the secondary (recipe) screen, and would also block
// the commit of an allowed special result. Result gating is handled reliably by
// in-place array COMPACTION (which removes gated results from every result list),
// so we deliberately DON'T force-gate here. We keep the hook installed only to
// LOG gated hits; it otherwise passes the original decision through unchanged.
static char __fastcall HkFusionPredicate(int devilId) {
    // Gate is applied via in-place array compaction (Compact* functions); the
    // original predicate decision passes through unchanged.
    return s_OrigPred(devilId);
}

// ── Layer D: native gray on the result-entry LIST builder ──
// sub_140BB6460 (CreateFusionResultEntryList) builds the result array the UI is
// bound to. Each entry is 48 bytes (3 x OWORD):
//   entry+0  : v39  (WORD2 at +4  == result devil id, uint16)
//   entry+16 : v40  (BYTE5 at +21 == selectable flag, 1=enabled)
//   entry+32 : v41
// The array base is *(a1+1216), count *(a1+1224), cap *(a1+1228).
// We post-process the array after the original runs: for any selectable entry
// whose result-race is gated we set the selectable byte to 0. This grays the
// result at the DATA level (the game then binds it grayed and refuses confirm),
// so it survives scrolling with no per-frame widget work and no reflection.
static std::atomic<bool> s_BuildInstalled{false};
static std::atomic<bool> s_Bfd620Installed{false};
static thread_local bool s_BypassBfd620 = false;
using TBfd620 = char(__fastcall*)(unsigned int);
static TBfd620 s_OrigBfd620 = nullptr;
static std::unique_ptr<PLH::x64Detour> s_Bfd620Detour;

// Bypass sub_140BFD620 (demon-available-for-fusion check) when called from
// within RevSecPop(7760). The populator's inner loop calls this to check each
// compendium demon, but it fails because the race-availability global table
// returns 0 for all demons in mode=2. We force it to return 1 so the populator
// builds the work list; gated RESULTS are removed by display compaction.
static char __fastcall HkBfd620(unsigned int a1) {
    if (IsEnabled() && s_BypassBfd620) {
        if (a1 >= 1 && a1 <= 1200) {
            return CachedIsInCompendium((int32)a1) ? 1 : 0;
        }
    }
    return s_OrigBfd620(a1);
}

// Standalone 2-arg hook for RevSecPop(7760) – preserves RDX (source devil ID)
// that the generic 1-arg trampoline would lose. Sets the Bfd620 bypass flag so
// the inner loop's sub_140BFD620 check returns 1 for all compendium demons.
using TRevSecPop = char(__fastcall*)(__int64, unsigned int);
static TRevSecPop s_OrigRevSecPop = nullptr;
static std::unique_ptr<PLH::x64Detour> s_RevSecPopDetour;
static char __fastcall HkRevSecPop7760(__int64 a1, unsigned int a2) {
    BuildCompendiumCache();
    s_BypassBfd620 = true;
    char r = s_OrigRevSecPop(a1, a2);
    s_BypassBfd620 = false;
    return r;
}

// Gray entries of a fusion result array: base=*(a1+arrOff), count=*(a1+arrOff+8),
// cap=*(a1+arrOff+12), stride 48, result devil id @ +4 (uint16),
// selectable flag @ +selOff. Used for the reverse (sub_140BB6CF0) and special
// (sub_140BB7950) builders.
// Remove gated entries COMPLETELY by compacting the TArray in place:
// shift kept entries down and decrement the count. This survives list rebuilds
// (scroll/refresh) because the builder re-runs and re-compacts every time.
// Layout: base=*(a1+arrOff), count=*(a1+arrOff+8), cap=*(a1+arrOff+12),
// entry i at base + stride*i, devil id (uint16) at +devidOff.
// Compact gated entries out of a fusion result array.
//   arrOff   : offset of the array header (base ptr lives at arrOff)
//   countOff : offset (relative to arrOff) of the count dword.
//              Standard TArray uses +8; the special-fusion GROUP array uses -4
//              because its count is stored at a1+1132 while the base ptr is at
//              a1+1136.
//   capOff   : offset (relative to arrOff) of the capacity dword (+12 standard).
static void CompactGatedEntriesEx(__int64 a1, int arrOff, int countOff, int capOff,
                                   int stride, int devidOff, bool dropZero = false) {
    void* base = *reinterpret_cast<void**>(a1 + arrOff);
    int count = *reinterpret_cast<int*>(a1 + arrOff + countOff);
    int cap = *reinterpret_cast<int*>(a1 + arrOff + capOff);
    if (!base || count <= 0 || cap <= 0 || count > cap || count > 4096) return;
    if (stride <= 0) stride = 48;
    int w = 0;
    for (int i = 0; i < count; ++i) {
        uintptr_t e = reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)i;
        uint16_t did = *reinterpret_cast<uint16_t*>(e + devidOff);
        int32 race = CachedRace((int32)did);
        bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        bool zero = (did == 0);
        if (gated || (dropZero && zero)) {
            continue;
        }
        if (w != i) {
            std::memmove(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)w),
                         reinterpret_cast<void*>(e), (size_t)stride);
        }
        ++w;
    }
    *reinterpret_cast<int*>(a1 + arrOff + countOff) = w;
}

// Diagnostic-only hook: logs the populated array shape (and the dispatcher mode
// byte for sub_140BB7500) so we can map exactly which builder serves each fusion
// screen. No graying, no ProcessEvent -> safe.
struct DiagHook {
    using Fn = char(__fastcall*)(__int64);
    Fn Orig = nullptr;
    std::unique_ptr<PLH::x64Detour> Detour;
    int ArrOff = 1216;
    int CountOff = 8;   // offset (rel to ArrOff) of count dword
    int CapOff = 12;    // offset (rel to ArrOff) of capacity dword
    int Stride = 48;
    int DevidOff = 4;
    int ArrOff2 = 0;   // optional 2nd (source) array to compact
    int CountOff2 = 8;
    int CapOff2 = 12;
    int Stride2 = 48;
    int DevidOff2 = 4;
    std::wstring Tag = L"";
    bool DoGray = false;
    bool DropZero = false;     // also drop entries whose result devil id is 0 (N/A / impossible fusion)
    bool ForceValid = false;   // for surviving ungated results, force the validity byte (+20) to 1 so the
                              // game's own "non-fusable" flag (sub_140BFD620) doesn't render them as N/A
};
static std::vector<DiagHook> s_Diag;

// Trampoline helper: runs the original fusion-list builder. The actual gating
// (compaction of gated results) runs afterwards in the per-index trampolines
// below. Kept as a uniform hook target so every builder shares the machinery.
static char __fastcall HkDiagList(int idx, __int64 a1) {
    return s_Diag[idx].Orig(a1);
}

// Per-index trampolines. Each forwards to HkDiagList(idx) then optionally compacts.
static inline int ArrCountEx(__int64 a1, int off, int countOff, int capOff) {
    void* base = *reinterpret_cast<void**>(a1 + off);
    int count = *reinterpret_cast<int*>(a1 + off + countOff);
    int cap = *reinterpret_cast<int*>(a1 + off + capOff);
    return (base && count >= 0 && count <= cap && count <= 4096) ? count : -1;
}
static inline void DoGrayCompact(DiagHook& h, __int64 a1) {
    if (!h.DoGray || !IsEnabled()) return;
    int pre1 = ArrCountEx(a1, h.ArrOff, h.CountOff, h.CapOff);
    CompactGatedEntriesEx(a1, h.ArrOff, h.CountOff, h.CapOff, h.Stride, h.DevidOff, h.DropZero);
    if (h.ArrOff2 != 0)
        CompactGatedEntriesEx(a1, h.ArrOff2, h.CountOff2, h.CapOff2, h.Stride2, h.DevidOff2, h.DropZero);
    int post1 = ArrCountEx(a1, h.ArrOff, h.CountOff, h.CapOff);
    if (pre1 > post1)
        LOG("[FusionGating] {} removed {} gated result(s) ({}->{})", h.Tag, pre1 - post1, pre1, post1);
    // For surviving (ungated, non-zero) results, the game may still flag the entry
    // invalid via sub_140BFD620(result) -> entry+20 = 0 (renders as N/A placeholder).
    // Re-enable the validity byte for ungated results so they display as proper fusions.
    if (h.ForceValid) {
        void* vbase = *reinterpret_cast<void**>(a1 + h.ArrOff);
        int vcount = *reinterpret_cast<int*>(a1 + h.ArrOff + h.CountOff);
        int vcap = *reinterpret_cast<int*>(a1 + h.ArrOff + h.CapOff);
        int vstride = h.Stride > 0 ? h.Stride : 48;
        if (vbase && vcount > 0 && vcap > 0 && vcount <= vcap && vcount <= 4096) {
            int fixed = 0;
            for (int i = 0; i < vcount; ++i) {
                uintptr_t e = (uintptr_t)vbase + vstride * (uintptr_t)i;
                uint16_t did = *reinterpret_cast<uint16_t*>(e + h.DevidOff);
                int32 race = CachedRace((int32)did);
                // If race is unknown (-1) the entry survived compaction (not known-gated),
                // so treat it as potentially ungated and re-enable validity to clear the N/A.
                bool likelyUngated = (did > 0 && (race < 0 || !APState::FusionRaces::IsRaceGated(race)));
                if (likelyUngated && *(uint8_t*)(e + 20) == 0) {
                    *reinterpret_cast<uint8_t*>(e + 20) = 1;
                    ++fixed;
                }
            }
            if (fixed > 0)
                LOG("[FusionGating] {} force-enabled {} ungated result(s)", h.Tag, fixed);
        }
    }
}
static void CompactSpecialResultArrays(__int64 a1); // fwd decl; defined later
static void CompactDyadResultArrays(__int64 a1);     // fwd decl; defined later
static char __fastcall HkDiagTramp0(__int64 a1) { char r = HkDiagList(0, a1); CompactDyadResultArrays(a1); return r; }
static char __fastcall HkDiagTramp1(__int64 a1) { char r = HkDiagList(1, a1); DoGrayCompact(s_Diag[1], a1); return r; }
static char __fastcall HkDiagTramp2(__int64 a1) { char r = HkDiagList(2, a1); CompactSpecialResultArrays(a1); return r; }
static char __fastcall HkDiagTramp3(__int64 a1) { char r = HkDiagList(3, a1); DoGrayCompact(s_Diag[3], a1); return r; }
static char __fastcall HkDiagTramp4(__int64 a1) { char r = HkDiagList(4, a1); DoGrayCompact(s_Diag[4], a1); return r; }
static char __fastcall HkDiagTramp5(__int64 a1) { char r = HkDiagList(5, a1); if (IsEssenceGatingEnabled()) DoGrayCompact(s_Diag[5], a1); return r; }
static char(__fastcall* HkDiagTramp[])(__int64) = {
    HkDiagTramp0, HkDiagTramp1, HkDiagTramp2, HkDiagTramp3, HkDiagTramp4, HkDiagTramp5
};

static void TryInstallBuildHook() {
    if (s_BuildInstalled.exchange(true)) return;

    // Hook every candidate fusion-list builder with a diagnostic trampoline.
    // arrOff = offset of the TArray (base ptr) in `this`.
    // countOff/capOff = dword offsets relative to arrOff (standard TArray: 8/12).
    struct Spec {
        const char* sig; int arrOff; int countOff; int capOff; const char* tag;
        bool disp; bool gray; int selOff; int stride; int devidOff;
        int arrOff2; int countOff2; int capOff2; int stride2; int devidOff2;
        bool dropZero = false;
        bool forceValid = false;
    };
    Spec specs[] = {
        { "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC D8 01 00 00 4C 8D A9",
          1216, 8, 12, "DyadSec(6460)", false, false, 20, 48, 4, 960, 8, 12, 32, 12, true, true },
        { "48 89 4C 24 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 38 04 00 00",
          1216, 8, 12, "RevSec(6cf0)",  false, true,  21, 48, 4, 0, 8, 12, 48, 4, true, true },
        { "48 89 4C 24 ? 55 56 41 54 41 56 48 8D AC 24",
          1200, 8, 12, "Special(7950)", false, false, 21, 48, 4, 0, 8, 12, 48, 4 },
        { "40 53 48 83 EC 20 48 8B D9 48 89 4C 24 ? 0F B6 89",
          1200, 8, 12, "Disp(7500)",    true,  true, 21, 48, 4, 0, 8, 12, 48, 4, false, true },
        { "40 55 57 41 54 41 56 48 83 EC 28 48 8D B9",
          1024, 8, 12, "RevPrim(7570)",  false, true,  21,  4, 0, 0, 8, 12, 4, 0 },
        { "48 89 5C 24 ? 55 56 57 41 56 41 57 48 8D 6C 24 ? 48 81 EC 70 01 00 00 0F B7 B1",
          1216, 8, 12, "Essence(8060)", false, true,  21, 48, 4, 0, 8, 12, 48, 4 },
    };
    s_Diag.clear();
    for (auto& s : specs) {
        uint64_t addr = SignatureScanner::FindPattern(s.sig);
        if (!addr) { LOG("[FusionGating] signature not found: {}", std::wstring(s.tag, s.tag + std::strlen(s.tag))); continue; }
        void* m = reinterpret_cast<void*>(addr);
        DiagHook dh;
        dh.ArrOff = s.arrOff; dh.CountOff = s.countOff; dh.CapOff = s.capOff;
        dh.Tag = std::wstring(s.tag, s.tag + std::strlen(s.tag));
        dh.Stride = s.stride; dh.DevidOff = s.devidOff;
        dh.ArrOff2 = s.arrOff2; dh.CountOff2 = s.countOff2; dh.CapOff2 = s.capOff2;
        dh.Stride2 = s.stride2; dh.DevidOff2 = s.devidOff2;
        dh.DropZero = s.dropZero;
        dh.ForceValid = s.forceValid;
        uint64_t orig = 0;
        int idx = (int)s_Diag.size();
        s_Diag.push_back(std::move(dh));
        auto det = std::make_unique<PLH::x64Detour>(
            reinterpret_cast<uint64_t>(m),
            reinterpret_cast<uint64_t>(PLH::FnCast(HkDiagTramp[idx], &s_Diag[idx].Orig)),
            &orig);
        if (!det->hook()) {
            LOG("[FusionGating] diag x64Detour FAILED at {:p} ({})", m, std::wstring(s.tag, s.tag + std::strlen(s.tag)));
            s_Diag.pop_back();
        } else {
            s_Diag[idx].Orig = PLH::FnCast(orig, s_Diag[idx].Orig);
            s_Diag[idx].Detour = std::move(det);
            s_Diag[idx].DoGray = s.gray;
            LOG("[FusionGating] diag hook installed: {} method={:p} orig={:p} gray={}",
                std::wstring(s.tag, s.tag + std::strlen(s.tag)), m, (void*)orig, s.gray);
        }
    }

    // Standalone 2-arg hook for RevSecPop(7760) – the generic 1-arg trampoline
    // loses RDX (source devil ID), but this function needs both args.
    {
        uint64_t m7760 = SignatureScanner::FindPattern("40 53 55 41 56 41 57 48 83 EC 38 4C 8D B1");
        if (m7760) {
            uint64_t orig7760 = 0;
            auto det7760 = std::make_unique<PLH::x64Detour>(
                m7760,
                reinterpret_cast<uint64_t>(PLH::FnCast(HkRevSecPop7760, &s_OrigRevSecPop)),
                &orig7760);
            if (!det7760->hook()) {
                LOG("[FusionGating] 2arg x64Detour FAILED at {:p} (RevSecPop(7760))", (void*)m7760);
            } else {
                s_OrigRevSecPop = PLH::FnCast(orig7760, s_OrigRevSecPop);
                s_RevSecPopDetour = std::move(det7760);
                LOG("[FusionGating] 2arg hook installed: RevSecPop(7760) method={:p} orig={:p}", (void*)m7760, (void*)orig7760);
            }
        }
    }
}


static void TryInstallNativeHook() {
    if (s_Installed.exchange(true)) return;

    // Hook sub_14720D3F0 (fusion result-validity predicate):
    // Returns char (1=non-fusable) for a given devilId.
    // We make it additively return 1 for gated-race demons.
    uint64_t predAddr = SignatureScanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 89 CF");
    if (!predAddr) { LOG("[FusionGating] predicate signature NOT FOUND"); s_Installed.store(false); return; }
    void* method = reinterpret_cast<void*>(predAddr);

    uint64_t origAddr = 0;
    s_PredDetour = std::make_unique<PLH::x64Detour>(
        predAddr,
        reinterpret_cast<uint64_t>(PLH::FnCast(&HkFusionPredicate, &s_OrigPred)),
        &origAddr);
    if (!s_PredDetour->hook()) {
        LOG("[FusionGating] predicate x64Detour FAILED at {:p}", method);
        s_PredDetour.reset(); s_Installed.store(false); return;
    }
    s_OrigPred = PLH::FnCast(origAddr, s_OrigPred);
    LOG("[FusionGating] Fusion predicate gate installed: method={:p} orig={:p}",
        method, (void*)origAddr);

    // Install sub_140BFD620 bypass hook (used by RevSecPop compendium check)
    if (!s_Bfd620Installed.exchange(true)) {
        uint64_t bfdAddr = SignatureScanner::FindPattern("E9 ? ? ? ? CC CC CC CC CC CC CC CC CC CC CC 48 83 EC 28 85 C9");
        if (bfdAddr) {
            uint64_t origBfd = 0;
            s_Bfd620Detour = std::make_unique<PLH::x64Detour>(
                bfdAddr,
                reinterpret_cast<uint64_t>(PLH::FnCast(&HkBfd620, &s_OrigBfd620)),
                &origBfd);
            if (s_Bfd620Detour->hook()) {
                s_OrigBfd620 = PLH::FnCast(origBfd, s_OrigBfd620);
                LOG("[FusionGating] sub_140BFD620 hook installed at {:p}", (void*)bfdAddr);
            } else {
                LOG("[FusionGating] sub_140BFD620 hook FAILED at {:p}", (void*)bfdAddr);
                s_Bfd620Detour.reset();
                s_Bfd620Installed.store(false);
            }
        } else {
            LOG("[FusionGating] sub_140BFD620 signature NOT FOUND");
            s_Bfd620Installed.store(false);
        }
    }
}

// (Reflection dump removed — it has served its purpose and was spamming the log.)


// ── Compendium reverse fusion list builder ──
// sub_140BE95F0(__int64 a1, __int64 a2, int a3..a6) builds the compendium
// result list. The TArray lives in `a2`: base=*(a2+160), count=*(a2+168),
// cap=*(a2+172). Each entry is 8 bytes: qword = (devilId<<32)|flag, so the
// devil id sits at entry offset +4 (low 32 bits of the high dword).
// The builder already calls our predicate (sub_140AB6E40) and flags gated
// entries, but to MATCH the main-panel "remove completely" behaviour we
// compact gated entries out of the array here.
// ── Universal result-list composer ──
// sub_140BB3AA0(a1, a2=resultArray, a3=source) builds the DISPLAYED result list
// (base=*(a2+0), count=*(a2+8), cap=*(a2+12), stride 48, devil id @ +4) for every
// fusion mode via the dispatcher sub_140BB7500: mode0 (reverse primary),
// mode1 (compendium reverse), mode2 (reverse secondary). It already calls our
// predicate and zeroes sel for gated entries, but does NOT remove them. We
// compact gated entries out of a2 so they are removed completely (survives
// scroll, since the dispatcher re-runs on rebuild).
using TCompose = __int64(__fastcall*)(__int64, __int64, __int64);
static TCompose s_ComposeOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_ComposeDetour;
static std::atomic<bool> s_ComposeInstalled{false};

// Lockstep-compact the dispatcher's compose output: sub_140BB3AA0 writes the
// displayed result list (a2, stride 48, devil id @ +4) 1:1 with the source list
// (a3, stride 4, each entry a single devil id) — every display entry's +0 dword
// IS the index into a3. Removing a gated entry must drop BOTH the display entry
// and its source entry, and rewrite the surviving display entries' +0 to the new
// (compacted) source index, otherwise the widget getters that dereference
// source[display+0] (sub_140BBACD0 / sub_140BB5630) read a stale/wrong devil id
// and render the slot as a placeholder. This mirrors CompactDyadResultArrays,
// which already does this lockstep for the normal-dyad (6460) path.
static void CompactComposeArrays(__int64 a2, __int64 a3) {
    if (!a2 || !a3) return;
    void* dbase = *reinterpret_cast<void**>(a2 + 0);
    int dcount = *reinterpret_cast<int*>(a2 + 8);
    int dcap   = *reinterpret_cast<int*>(a2 + 12);
    void* sbase = *reinterpret_cast<void**>(a3 + 0);
    int scount = *reinterpret_cast<int*>(a3 + 8);
    int scap   = *reinterpret_cast<int*>(a3 + 12);
    if (!dbase || !sbase) return;
    if (dcount <= 0 || scount <= 0 || dcap <= 0 || scap <= 0) return;
    if (dcount > scount) dcount = scount;   // 1:1 by construction
    if (dcount > 4096) return;
    int w = 0;
    for (int i = 0; i < dcount; ++i) {
        uintptr_t se = reinterpret_cast<uintptr_t>(sbase) + 4LL * (uintptr_t)i;
        uint16_t sid = *reinterpret_cast<uint16_t*>(se);
        int32 race = CachedRace((int32)sid);
        bool gated = (sid > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        if (gated) {
            continue;
        }
        if (w != i) {
            std::memmove(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dbase) + 48LL * (uintptr_t)w),
                         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dbase) + 48LL * (uintptr_t)i),
                         (size_t)48);
            std::memmove(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(sbase) + 4LL * (uintptr_t)w),
                         reinterpret_cast<void*>(se), (size_t)4);
        }
        // Rewrite the display entry's +0 source index to the compacted index.
        *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(dbase) + 48LL * (uintptr_t)w + 0) = w;
        ++w;
    }
    *reinterpret_cast<int*>(a2 + 8) = w;
    *reinterpret_cast<int*>(a3 + 8) = w;
    if (w != dcount)
        LOG("[COMPACT] compose removed {} ({}->{})", (int64_t)(dcount - w), (int64_t)dcount, (int64_t)w);
}

static __int64 __fastcall HkCompose(__int64 a1, __int64 a2, __int64 a3) {
    __int64 r = s_ComposeOrig(a1, a2, a3);
    if (IsEnabled()) {
        CompactComposeArrays(a2, a3);
    }
    return r;
}

static void TryInstallCompendiumHook() {
    if (s_ComposeInstalled.exchange(true)) return;

    uint64_t targetAddr = SignatureScanner::FindPattern("40 55 57 41 54 41 56 41 57 48 83 EC 60");
    if (!targetAddr) { LOG("[FusionGating] compose signature NOT FOUND"); s_ComposeInstalled.store(false); return; }

    uint64_t origAddr = 0;
    auto det = std::make_unique<PLH::x64Detour>(targetAddr, reinterpret_cast<uint64_t>(PLH::FnCast(HkCompose, &s_ComposeOrig)), &origAddr);
    if (!det->hook()) {
        LOG("[FusionGating] compose x64Detour FAILED at {:p}", (void*)targetAddr);
        s_ComposeInstalled.store(false);
        return;
    }
    s_ComposeOrig = PLH::FnCast(origAddr, s_ComposeOrig);
    s_ComposeDetour = std::move(det);
    LOG("[FusionGating] compose hook installed: method={:p} orig={:p}", (void*)targetAddr, (void*)origAddr);
}

// ── CustomProperty-based accessors for fusion panel arrays ──
// These replace raw offset arithmetic with named field access.
// Panel special result display TArray at +1200
static PropertyArrayAccessor<uint8> s_SpecialResultArr(1200);
// Panel special group TArray at +1136
static PropertyArrayAccessor<uint8> s_SpecialGroupArr(1136);
// Panel dyad result TArray at +1216
static PropertyArrayAccessor<uint8> s_DyadResultArr(1216);
// Panel dyad source TArray at +960
static PropertyArrayAccessor<uint8> s_DyadSourceArr(960);

// ── Special-fusion displayed result array ──
// Builder sub_140BB7950 (Special(7950)) fills the displayed list into the
// TArray at panel offset 1200 (stride 48, count@1208, cap@1212) -- this is the
// array GetSpecialFusionDevilCount / GetSpecialFusionDevilByIndex read.
// We compact it (remove gated) and make the count getter return the new count
// so the widget iterates only valid, non-gated entries. The 7950 post-hook
// (spec arrOff=1200) compacts the same array once at build time; this hook
// re-asserts compaction (idempotent) and returns the authoritative count so
// count/getter and ByIndex always agree.
static const int kSpecialStride = 48;        // stride in builder sub_140BB7950
static const int kSpecialDevidOff = 4;       // DevilID dword within 48-byte entry

using TGetCount = int32_t(__fastcall*)(UObject*, void*);
static TGetCount s_GetCountOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_GetCountDetour;
static std::atomic<bool> s_GetCountInstalled{false};

static int32_t __fastcall HkGetSpecialCount(UObject* Ctx, void* Parms) {
    int32_t r = s_GetCountOrig(Ctx, Parms);
    if (!IsEnabled() || !Ctx) return r;
    // Compact the displayed member array; the widget will then read the new
    // (smaller) list. Return the post-compaction count so iteration is correct.
    void* base = s_SpecialResultArr.GetData(Ctx);
    int count = s_SpecialResultArr.GetCount(Ctx);
    int cap = s_SpecialResultArr.GetCapacity(Ctx);
    if (base && count > 0 && cap > 0 && count <= cap && count <= 4096) {
        int w = 0;
        for (int i = 0; i < count; ++i) {
            uintptr_t e = (uintptr_t)base + kSpecialStride * (uintptr_t)i;
            int did = *reinterpret_cast<int32_t*>(e + kSpecialDevidOff);
            int32 race = CachedRace(did);
            bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
            if (gated) continue;
            if (w != i) std::memmove((void*)((uintptr_t)base + kSpecialStride * (uintptr_t)w),
                                     (void*)e, (size_t)kSpecialStride);
            ++w;
        }
        s_SpecialResultArr.SetCount(Ctx, w);
        if (w != r) LOG(STR("[SPECIAL_FIX] count {}->{} (gated removed)"),
            (int64_t)r, (int64_t)w);
        r = w;
    }
    return r;
}

// ── Special-fusion RESULT removal (group + displayed arrays, lockstep) ──
// The special primary screen renders icons from the GROUP array (panel+1136):
//   each group entry (stride 48) holds the result devil id at +16 and an inner
//   ingredient TArray at +0 (+8 = count). The displayed list (panel+1200, stride
//   48) entry at +0 stores the GROUP INDEX into panel+1136, and +4 the devil id.
// Both arrays are 1:1 (display[i].groupIndex == i), so removing a gated group
// entry requires removing the matching display entry AND rewriting its +0 index.
// We compact BOTH arrays together so the gated RESULT (and its recipe) vanish
// from the primary screen, while survivors keep correct indices.
static void CompactSpecialResultArrays(__int64 a1) {
    if (!IsEnabled()) return;
    // Group array has non-standard layout: count at a1+1132, base at a1+1136, cap at a1+1144
    const int gOff = 1136, gCountOff = -4, gCapOff = 8, gStride = 48, gDevOff = 16;
    void* container = reinterpret_cast<void*>(a1);
    const int dStride = 48;
    void* gbase = *reinterpret_cast<void**>(a1 + gOff);
    int gcount = *reinterpret_cast<int*>(a1 + gOff + gCountOff);
    int gcap   = *reinterpret_cast<int*>(a1 + gOff + gCapOff);
    void* dbase = s_SpecialResultArr.GetData(container);
    int dcount = s_SpecialResultArr.GetCount(container);
    if (!gbase || gcount <= 0 || gcap <= 0 || gcount > gcap || gcount > 4096) return;
    if (!dbase || dcount <= 0) return;
    int w = 0;
    for (int i = 0; i < gcount; ++i) {
        uintptr_t ge = (uintptr_t)gbase + gStride * (uintptr_t)i;
        uint16_t did = *reinterpret_cast<uint16_t*>(ge + gDevOff);
        int32 race = CachedRace((int32)did);
        bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        if (gated) {
            continue;
        }
        if (w != i) {
            std::memmove((void*)((uintptr_t)gbase + gStride * (uintptr_t)w), (void*)ge, (size_t)gStride);
            if (i < dcount) {
                uintptr_t de  = (uintptr_t)dbase + dStride * (uintptr_t)i;
                uintptr_t dwe = (uintptr_t)dbase + dStride * (uintptr_t)w;
                std::memmove((void*)dwe, (void*)de, (size_t)dStride);
                *reinterpret_cast<int*>(dwe + 0) = w; // rewrite group index
            }
        }
        ++w;
    }
    *reinterpret_cast<int*>(a1 + gOff + gCountOff) = w;
    s_SpecialResultArr.SetCount(container, w);
    // Zero out inner TArray headers in stale group entries (beyond new count) so
    // that panel destruction -- if it iterates by capacity -- does NOT double-free
    // the inner buffers that were memmove'd to earlier positions.
    for (int z = w; z < gcount; ++z) {
        uintptr_t ze = (uintptr_t)gbase + gStride * (uintptr_t)z;
        *reinterpret_cast<void**>(ze + 0) = nullptr;
        *reinterpret_cast<int*>(ze + 8) = 0;
        *reinterpret_cast<int*>(ze + 12) = 0;
    }
    if (w != gcount)
        LOG("[SPECIAL_COMPACT] removed {} special results ({}->{})", (int64_t)(gcount - w), (int64_t)gcount, (int64_t)w);
}

static void CompactDyadResultArrays(__int64 a1) {
    if (!IsEnabled()) return;
    void* container = reinterpret_cast<void*>(a1);
    const int rStride = 48, rDevOff = 4;
    const int sStride = 32;
    void* rbase = s_DyadResultArr.GetData(container);
    int rcount = s_DyadResultArr.GetCount(container);
    int rcap   = s_DyadResultArr.GetCapacity(container);
    void* sbase = s_DyadSourceArr.GetData(container);
    int scount = s_DyadSourceArr.GetCount(container);
    if (!rbase || rcount <= 0 || rcap <= 0 || rcount > rcap || rcount > 4096) return;
    if (!sbase || scount <= 0) return;
    int w = 0;
    for (int i = 0; i < rcount && i < scount; ++i) {
        uintptr_t re = (uintptr_t)rbase + rStride * (uintptr_t)i;
        uint16_t did = *reinterpret_cast<uint16_t*>(re + rDevOff);
        int32 race = CachedRace((int32)did);
        bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        if (gated || did == 0) {
            continue;
        }
        if (w != i) {
            std::memmove((void*)((uintptr_t)rbase + rStride * (uintptr_t)w), (void*)re, (size_t)rStride);
            uintptr_t se  = (uintptr_t)sbase + sStride * (uintptr_t)i;
            uintptr_t swe = (uintptr_t)sbase + sStride * (uintptr_t)w;
            std::memmove((void*)swe, (void*)se, (size_t)sStride);
        }
        // Fix the source index so the ingredient getter (sub_140BB54B0) finds the right source entry
        uintptr_t rwe = (uintptr_t)rbase + rStride * (uintptr_t)w;
        *reinterpret_cast<int*>(rwe + 0) = w;
        ++w;
    }
    s_DyadResultArr.SetCount(container, w);
    s_DyadSourceArr.SetCount(container, w);
    if (w != rcount)
        LOG("[DYAD_COMPACT] removed {} dyad results ({}->{})", (int64_t)(rcount - w), (int64_t)rcount, (int64_t)w);
    // ForceValid: for surviving ungated results, force the validity byte at +20
    if (w > 0) {
        int fixed = 0;
        for (int i = 0; i < w; ++i) {
            uintptr_t e = (uintptr_t)rbase + rStride * (uintptr_t)i;
            uint16_t d = *reinterpret_cast<uint16_t*>(e + 4); // m_DevilID at +4
            int32 r = CachedRace((int32)d);
            bool likelyUngated = (d > 0 && (r < 0 || !APState::FusionRaces::IsRaceGated(r)));
            if (likelyUngated && *(uint8_t*)(e + 20) == 0) {
                *reinterpret_cast<uint8_t*>(e + 20) = 1;
                ++fixed;
            }
        }
        if (fixed > 0)
            LOG("[DYAD_FORCEVALID] fixed {} ungated results", fixed);
    }
}

static void TryInstallSpecialGetterHook() {
    if (s_GetCountInstalled.exchange(true)) return;
    UFunction* Fn = nullptr;
    for (auto* p : {
        STR("/Script/Project.AUniteCharaPanelCtrlBase:GetSpecialFusionDevilCount"),
        STR("/Script/Project.UniteCharaPanelCtrlBase:GetSpecialFusionDevilCount") }) {
        Fn = UObjectGlobals::FindObject<UFunction>(nullptr, p);
        if (Fn) break;
    }
    if (!Fn) { LOG("[FusionGating] GetSpecialFusionDevilCount UFunction NOT FOUND"); s_GetCountInstalled.store(false); return; }
    void* thunk = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(Fn->GetFunc()));
    if (!thunk) { LOG("[FusionGating] GetSpecialFusionDevilCount GetFunc() null"); s_GetCountInstalled.store(false); return; }
    uint64_t orig = 0;
    auto det = std::make_unique<PLH::x64Detour>(
        reinterpret_cast<uint64_t>(thunk),
        reinterpret_cast<uint64_t>(PLH::FnCast(HkGetSpecialCount, &s_GetCountOrig)), &orig);
    if (!det->hook()) {
        LOG("[FusionGating] special count x64Detour FAILED at {:p}", thunk);
        s_GetCountInstalled.store(false); return;
    }
    s_GetCountOrig = PLH::FnCast(orig, s_GetCountOrig);
    s_GetCountDetour = std::move(det);
    LOG("[FusionGating] special count hook installed: {:p} orig={:p}", thunk, (void*)orig);
}

void Setup() {
    LOG("[FusionGating] Setup...");

    // Install everything up-front (module init), NOT on first panel animation.
    // The race lookup and all builder/predicate addresses are available now that
    // the game is loaded, and installing early guarantees the fusion list
    // builders are already hooked before the very first panel is drawn -- fixing
    // the "first open shows gated demons" bug.
    BuildRaceCache();
    TryInstallNativeHook();
    TryInstallBuildHook();
    TryInstallCompendiumHook();
    // NOTE: TryInstallSpecialGetterHook() intentionally NOT installed.
    // It resolved "GetSpecialFusionDevilCount" via GetFunc() but that thunk maps
    // to GetSpecialFusionDevilCursorPosition (sub_140D61F40), so the hook was
    // corrupting the cursor result and never compacting. The Special(7950)
    // post-hook already compacts the displayed list (a1+1200) and updates its
    // count (a1+1208), which every primary-screen accessor bounds-checks.

    LOG("[FusionGating] Setup complete");
}

} // namespace FusionGating
