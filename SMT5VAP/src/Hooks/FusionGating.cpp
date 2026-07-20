#include "FusionGating.hpp"
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

void SetEnabled(bool enabled) {
    s_Enabled.store(enabled, std::memory_order_release);
    LOG("[FusionGating] SetEnabled({})", enabled);
}
bool IsEnabled() {
    return s_Enabled.load(std::memory_order_acquire);
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
static void CallVoidInt(UObject* Obj, const wchar_t* funcName, int32 index) {
    if (!Obj) return;
    UFunction* F = Obj->GetFunctionByName(funcName);
    if (!F) F = Obj->GetFunctionByNameInChain(funcName);
    if (!F) return;
    struct { int32 InIndex; } p{ index };
    Obj->ProcessEvent(F, &p);
}

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
static std::set<std::wstring> s_DiagSeen;
static void DiagOnce(const std::wstring& key) {
    if (s_DiagSeen.insert(key).second) LOG("[FusionGating][DIAG] {}", key);
}

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
    char orig = s_OrigPred(devilId);
    int32_t race = (devilId > 0) ? GetDevilRaceId(devilId) : -1;
    bool gated = (race >= 0 && APState::FusionRaces::IsRaceGated(race));
    if (gated) DiagOnce(STR("[GATE] sub_140AB6E40 devil=") + std::to_wstring(devilId)
        + STR(" race=") + std::to_wstring(race) + STR(" orig=") + std::to_wstring((int)orig)
        + STR(" -> passthrough (gating handled by compaction)"));
    return orig;
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

struct ResultEntryView {
    bool valid = false;
    uint16_t devilId = 0;
    bool sel = false;
};

static ResultEntryView ReadEntry(void* base, int i, int selOff) {
    ResultEntryView v;
    uintptr_t e = reinterpret_cast<uintptr_t>(base) + 48 * static_cast<uintptr_t>(i);
    v.devilId = *reinterpret_cast<uint16_t*>(e + 4);
    v.sel = (*reinterpret_cast<uint8_t*>(e + selOff)) != 0;
    v.valid = true;
    return v;
}

static void WriteSelectable(void* base, int i, int selOff, uint8_t val) {
    uintptr_t e = reinterpret_cast<uintptr_t>(base) + 48 * static_cast<uintptr_t>(i);
    *reinterpret_cast<uint8_t*>(e + selOff) = val;
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
                                   int stride, int devidOff, const wchar_t* tag,
                                   bool dropZero = false) {
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
            DiagOnce(std::wstring(tag) + STR(" removed devil=") + std::to_wstring((int)did)
                + STR(" race=") + std::to_wstring(race)
                + (zero && !gated ? STR(" (N/A result)") : STR("")));
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
static inline void CompactGatedEntries(__int64 a1, int arrOff, int stride, int devidOff, const wchar_t* tag) {
    CompactGatedEntriesEx(a1, arrOff, 8, 12, stride, devidOff, tag);
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
    int selOffGray = 21;
    int Stride = 48;
    int DevidOff = 4;
    int ArrOff2 = 0;   // optional 2nd (source) array to compact
    int CountOff2 = 8;
    int CapOff2 = 12;
    int Stride2 = 48;
    int DevidOff2 = 4;
    std::string Tag = "";
    bool IsDispatcher = false; // read mode byte at a1+1098
    bool DoGray = false;
    bool DropZero = false;     // also drop entries whose result devil id is 0 (N/A / impossible fusion)
    bool ForceValid = false;   // for surviving ungated results, force the validity byte (+20) to 1 so the
                              // game's own "non-fusable" flag (sub_140BFD620) doesn't render them as N/A
};
static std::vector<DiagHook> s_Diag;

static char __fastcall HkDiagList(int idx, __int64 a1) {
    DiagHook& h = s_Diag[idx];
    char r = h.Orig(a1);
    static std::atomic<int> sCalls{0};
    int n = sCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 40) return r; // bound log volume
    int mode = -1;
    if (h.IsDispatcher) mode = *(uint8_t*)(a1 + 1098);
    void* base = *reinterpret_cast<void**>(a1 + h.ArrOff);
    int count = *reinterpret_cast<int*>(a1 + h.ArrOff + 8);
    int cap = *reinterpret_cast<int*>(a1 + h.ArrOff + 12);
    std::string msg = "[DIAG][" + h.Tag + "]";
    if (h.IsDispatcher) msg += " mode=" + std::to_string(mode) + " ";
    msg += "count=" + std::to_string(count) + " cap=" + std::to_string(cap);
    if (base && count > 0 && count <= 4096) {
        int stride = h.Stride > 0 ? h.Stride : 48;
        for (int i = 0; i < count && i < 6; ++i) {
            uintptr_t e = (uintptr_t)base + stride * (uintptr_t)i;
            uint16_t did = *reinterpret_cast<uint16_t*>(e + h.DevidOff);
            uint8_t sel = *reinterpret_cast<uint8_t*>(e + h.selOffGray);
            msg += " [" + std::to_string(i) + ":d" + std::to_string(did)
                  + ",s" + std::to_string(sel) + "]";
        }
    }
    LOG("[FusionGating]{}", std::wstring(msg.begin(), msg.end()));
    // Detailed full-entry dump for the dyad secondary builder so we can see why
    // some (non-gated) results render as N/A placeholders.
    if (h.Tag.find("DyadSec") != std::string::npos && base && count > 0 && count <= 4096) {
        std::string dmp = "[DYAD_DETAIL] cap=" + std::to_string(cap) + " (r+0=ingA +34=ingB r4/16=res v20=valid s21=sel | src+0=ingA +2=ingB +12=res | race)";
        int stride = h.Stride > 0 ? h.Stride : 48;
        for (int i = 0; i < count && i < 12; ++i) {
            uintptr_t e = (uintptr_t)base + stride * (uintptr_t)i;
            uint16_t r0  = *reinterpret_cast<uint16_t*>(e + 0);  // first ingredient from result entry
            uint16_t d4  = *reinterpret_cast<uint16_t*>(e + 4);
            uint16_t d16 = *reinterpret_cast<uint16_t*>(e + 16);
            uint8_t  valid = *reinterpret_cast<uint8_t*>(e + 20);
            uint8_t  sel   = *reinterpret_cast<uint8_t*>(e + 21);
            uint16_t r34 = *reinterpret_cast<uint16_t*>(e + 34); // second ingredient from result entry
            int32 race = CachedRace((int32)d4);
            // source array at a1+960, stride 32
            void* sbase = *reinterpret_cast<void**>(a1 + 960);
            int scount = *reinterpret_cast<int*>(a1 + 968);
            std::string sval = "?";
            if (sbase && i < scount) {
                uintptr_t se = (uintptr_t)sbase + 32 * (uintptr_t)i;
                uint16_t sd  = *reinterpret_cast<uint16_t*>(se + 12);
                uint16_t sA  = *reinterpret_cast<uint16_t*>(se + 0);  // content at +0
                uint16_t sB  = *reinterpret_cast<uint16_t*>(se + 2);  // content at +2
                sval = "A" + std::to_string(sA) + "/B" + std::to_string(sB) + "/res" + std::to_string(sd);
            }
            dmp += " #" + std::to_string(i) + "("
                 + "r0=" + std::to_string(r0) + ",r34=" + std::to_string(r34)
                 + ",r4=" + std::to_string(d4) + ",r16=" + std::to_string(d16)
                 + ",v" + std::to_string(valid) + ",s" + std::to_string(sel)
                 + " r" + std::to_string(race) + "|" + sval + ")";
        }
        LOG("[FusionGating]{}", std::wstring(dmp.begin(), dmp.end()));
    }
    return r;
}

// Per-index trampolines. Each forwards to HkDiagList(idx) then optionally compacts.
static inline int ArrCountEx(__int64 a1, int off, int countOff, int capOff) {
    void* base = *reinterpret_cast<void**>(a1 + off);
    int count = *reinterpret_cast<int*>(a1 + off + countOff);
    int cap = *reinterpret_cast<int*>(a1 + off + capOff);
    return (base && count >= 0 && count <= cap && count <= 4096) ? count : -1;
}
static inline int ArrCount(__int64 a1, int off) {
    return ArrCountEx(a1, off, 8, 12);
}
static inline void DoGrayCompact(DiagHook& h, __int64 a1) {
    if (!h.DoGray || !IsEnabled()) return;
    static std::atomic<uint64_t> sCall{0};
    uint64_t c = sCall.fetch_add(1, std::memory_order_relaxed);
    int pre1 = ArrCountEx(a1, h.ArrOff, h.CountOff, h.CapOff);
    int pre2 = h.ArrOff2 ? ArrCountEx(a1, h.ArrOff2, h.CountOff2, h.CapOff2) : -1;
    CompactGatedEntriesEx(a1, h.ArrOff, h.CountOff, h.CapOff, h.Stride, h.DevidOff, STR("[REMOVE]"), h.DropZero);
    if (h.ArrOff2 != 0)
        CompactGatedEntriesEx(a1, h.ArrOff2, h.CountOff2, h.CapOff2, h.Stride2, h.DevidOff2, STR("[REMOVE]"), h.DropZero);
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
            if (fixed > 0) DiagOnce(STR("[FORCEVALID] fixed ") + std::to_wstring(fixed)
                + STR(" ungated results in ") + std::wstring(h.Tag.begin(), h.Tag.end()));
        }
    }
    if (h.Tag.find("Special") != std::string::npos && c < 3 && h.ArrOff2 != 0) {
        void* base = *reinterpret_cast<void**>(a1 + h.ArrOff2);
        int cnt = *reinterpret_cast<int*>(a1 + h.ArrOff2 + h.CountOff2);
        std::wstring dump = L"SRC_FULL cnt=" + std::to_wstring(cnt) + L": ";
        for (int i = 0; i < cnt && i < 40; ++i) {
            uintptr_t e = (uintptr_t)base + h.Stride2 * (uintptr_t)i;
            int q0 = *(int*)(e + 0);
            int q4 = *(int*)(e + 4);
            int q8 = *(int*)(e + 8);
            int q12 = *(int*)(e + 12);
            int q16 = *(int*)(e + 16);
            int q20 = *(int*)(e + 20);
            int q24 = *(int*)(e + 24);
            int q28 = *(int*)(e + 28);
            int q32 = *(int*)(e + 32);
            int q36 = *(int*)(e + 36);
            int q40 = *(int*)(e + 40);
            dump += L"#" + std::to_wstring(i) + L"(" + std::to_wstring(q0) + L"," + std::to_wstring(q4) + L","
                  + std::to_wstring(q8) + L"," + std::to_wstring(q12) + L"," + std::to_wstring(q16) + L","
                  + std::to_wstring(q20) + L"," + std::to_wstring(q24) + L"," + std::to_wstring(q28) + L","
                  + std::to_wstring(q32) + L"," + std::to_wstring(q36) + L"," + std::to_wstring(q40) + L") ";
        }
        LOG("[FusionGating]{}", dump);
    }
}
static void CompactSpecialResultArrays(__int64 a1); // fwd decl; defined later
static char __fastcall HkDiagTramp0(__int64 a1) { char r = HkDiagList(0, a1); DoGrayCompact(s_Diag[0], a1); return r; }
static char __fastcall HkDiagTramp1(__int64 a1) { char r = HkDiagList(1, a1); DoGrayCompact(s_Diag[1], a1); return r; }
static char __fastcall HkDiagTramp2(__int64 a1) { char r = HkDiagList(2, a1); CompactSpecialResultArrays(a1); return r; }
static char __fastcall HkDiagTramp3(__int64 a1) { char r = HkDiagList(3, a1); DoGrayCompact(s_Diag[3], a1); return r; }
static char __fastcall HkDiagTramp4(__int64 a1) { char r = HkDiagList(4, a1); DoGrayCompact(s_Diag[4], a1); return r; }
static char __fastcall HkDiagTramp5(__int64 a1) { char r = HkDiagList(5, a1); DoGrayCompact(s_Diag[5], a1); return r; }
static char __fastcall HkDiagTramp6(__int64 a1) { char r = HkDiagList(6, a1); DoGrayCompact(s_Diag[6], a1); return r; }
static char(__fastcall* HkDiagTramp[])(__int64) = {
    HkDiagTramp0, HkDiagTramp1, HkDiagTramp2, HkDiagTramp3, HkDiagTramp4, HkDiagTramp5, HkDiagTramp6
};

static void TryInstallBuildHook() {
    if (s_BuildInstalled.exchange(true)) return;
    auto* Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.AUniteCharaPanelCtrlBase:CanBeSelectedAsSearchFusion"));
    if (!Fn) Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.UniteCharaPanelCtrlBase:CanBeSelectedAsSearchFusion"));
    if (!Fn) { LOG("[FusionGating] UFunction CanBeSelectedAsSearchFusion NOT FOUND"); s_BuildInstalled.store(false); return; }
    void* thunk = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(Fn->GetFunc()));
    if (!thunk) { LOG("[FusionGating] GetFunc() null"); s_BuildInstalled.store(false); return; }
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(thunk) - 0x140D5E440;

    // Hook every candidate fusion-list builder with a diagnostic trampoline.
    // arrOff = offset of the TArray (base ptr) in `this`.
    // countOff/capOff = dword offsets relative to arrOff (standard TArray: 8/12).
    struct Spec {
        uint64_t off; int arrOff; int countOff; int capOff; const char* tag;
        bool disp; bool gray; int selOff; int stride; int devidOff;
        int arrOff2; int countOff2; int capOff2; int stride2; int devidOff2;
        bool dropZero = false;
        bool forceValid = false;
    };
    Spec specs[] = {
        { 0x140BB6460, 1216, 8, 12, "DyadSec(6460)", false, true,  20, 48, 4, 960, 8, 12, 32, 12, true, true },  // dyad secondary: drop gated + N/A results, force-valid ungated, lockstep result(1216)+source(960)
        { 0x140BB6CF0, 1216, 8, 12, "RevSec(6cf0)",  false, true,  21, 48, 4, 0, 8, 12, 48, 4, true, true },  // reverse secondary: drop gated + N/A results, force-valid ungated
        { 0x140BB7950, 1200, 8, 12, "Special(7950)", false, false, 21, 48, 4, 0, 8, 12, 48, 4 },  // special: lockstep-compact group+display via CompactSpecialResultArrays (HkDiagTramp2). display[+0] is a group index, so both arrays must move together.
        { 0x140BB7500, 1216, 8, 12, "Disp(7500)",    true,  false, 21, 48, 4, 0, 8, 12, 48, 4 },  // dispatcher (mode byte@1098)
        { 0x140BB7570, 1024, 8, 12, "Dyprim(7570)",  false, true,  21,  4, 0, 0, 8, 12, 4, 0 },  // reverse-primary result list (gray)
        { 0x140BB7760, 1008, 8, 12, "RevSecPop(7760)",false,false, 21, 48, 4, 0, 8, 12, 48, 4 },  // reverse secondary populator
        { 0x140BB8060, 1216, 8, 12, "RevPrim(8060)", false, true,  21, 48, 4, 0, 8, 12, 48, 4 },  // reverse result list (gray, if it fires)
    };
    s_Diag.clear();
    for (auto& s : specs) {
        void* m = reinterpret_cast<void*>(moduleBase + s.off);
        if (m < (void*)0x140000000 || m > (void*)0x160000000) {
            LOG("[FusionGating] method {:p} out of range", m); continue;
        }
        DiagHook dh;
        dh.ArrOff = s.arrOff; dh.CountOff = s.countOff; dh.CapOff = s.capOff;
        dh.Tag = s.tag; dh.IsDispatcher = s.disp; dh.selOffGray = s.selOff;
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
}



static void TryInstallNativeHook() {
    if (s_Installed.exchange(true)) return;

    auto* Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.AUniteCharaPanelCtrlBase:CanBeSelectedAsSearchFusion"));
    if (!Fn) Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.UniteCharaPanelCtrlBase:CanBeSelectedAsSearchFusion"));
    if (!Fn) { LOG("[FusionGating] UFunction CanBeSelectedAsSearchFusion NOT FOUND"); s_Installed.store(false); return; }

    void* thunk = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(Fn->GetFunc()));
    if (!thunk) { LOG("[FusionGating] GetFunc() null"); s_Installed.store(false); return; }

    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(thunk) - 0x140D5E440;
    void* method = reinterpret_cast<void*>(moduleBase + 0x14720D3F0);
    if (method < (void*)0x140000000 || method > (void*)0x160000000) {
        LOG("[FusionGating] predicate method {:p} out of range", method);
        s_Installed.store(false); return;
    }

    uint64_t origAddr = 0;
    s_PredDetour = std::make_unique<PLH::x64Detour>(
        reinterpret_cast<uint64_t>(method),
        reinterpret_cast<uint64_t>(PLH::FnCast(&HkFusionPredicate, &s_OrigPred)),
        &origAddr);
    if (!s_PredDetour->hook()) {
        LOG("[FusionGating] predicate x64Detour FAILED at {:p}", method);
        s_PredDetour.reset(); s_Installed.store(false); return;
    }
    s_OrigPred = PLH::FnCast(origAddr, s_OrigPred);
    LOG("[FusionGating] Fusion predicate gate installed: method={:p} orig={:p}",
        method, (void*)origAddr);
}

// (Reflection dump removed — it has served its purpose and was spamming the log.)

static bool GrayIfGatedSlot(UObject* Panel, int32 slot,
                            const wchar_t* listName,
                            const wchar_t* countFn, const wchar_t* idFn, const wchar_t* offsetFn) {
    int32 count = CallInt32(Panel, countFn);
    UFunction* IdF = Panel->GetFunctionByName(idFn);
    if (!IdF) IdF = Panel->GetFunctionByNameInChain(idFn);
    int32 offset = CallInt32(Panel, offsetFn);
    int32 eff = (offset < 0) ? 0 : offset;
    int32 absIndex = eff + slot;
    int32 devil = (IdF && count > 0 && absIndex >= 0 && absIndex < count)
                    ? GetResultDevilAt(Panel, IdF, absIndex) : -1;
    int32 race = (devil > 0) ? GetDevilRaceId(devil) : -1;
    DiagOnce(std::wstring(listName)
        + STR(" slot=") + std::to_wstring(slot)
        + STR(" count=") + std::to_wstring(count)
        + STR(" off=") + std::to_wstring(offset)
        + STR(" abs=") + std::to_wstring(absIndex)
        + STR(" devil=") + std::to_wstring(devil)
        + STR(" race=") + std::to_wstring(race)
        + STR(" idF=") + (IdF ? STR("1") : STR("0")));
    if (count <= 0 || !IdF) return false;
    if (absIndex < 0 || absIndex >= count) return false;
    if (race >= 0 && APState::FusionRaces::IsRaceGated(race)) {
        CallVoidInt(Panel, STR("BIESetUniteCharaPanelUnselectablePanelOn"), slot);
        DiagOnce(std::wstring(listName) + STR(" GRAYED slot=") + std::to_wstring(slot));
        return true;
    }
    return false;
}

// Re-apply gray to a single visible slot across every known fusion result list.
//
// NOTE: The per-tick BIE-gray approach proved unstable (crashes in the fusion
// menu during scroll/transitions) and did not reliably persist across scroll,
// because the game rebinds list widgets at the UMG level without firing any
// hookable UFunction. The reliable gate is the native predicate (Layer C), which
// logically disables gated results. The widget-gray visual is left to a safer
// mechanism (see Setup / future work) — for now this is disabled to keep the
// menu stable.
static void ReGraySlot(UObject* Panel, int32 slot) {
    (void)Panel; (void)slot;
    // Intentionally empty: BIE gray disabled pending a safe implementation.
}

// Re-gray every visible slot of the result lists. Called each tick while the
// panel is open so the gray survives scrolling (the game rebinds list widgets
// on scroll without firing any hookable UFunction).
static void ReGrayAllVisible(UObject* Panel) {
    if (!IsEnabled() || !Panel) return;
    for (int32 slot = 0; slot < 16; ++slot) {
        ReGraySlot(Panel, slot);
    }
}

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

static void CompactArrayAt(__int64 arrPtr, int stride, int devidOff) {
    if (!arrPtr) return;
    void* base = *reinterpret_cast<void**>(arrPtr + 0);
    int count = *reinterpret_cast<int*>(arrPtr + 8);
    int cap = *reinterpret_cast<int*>(arrPtr + 12);
    if (!base || count <= 0 || cap <= 0 || count > cap || count > 4096) return;
    if (stride <= 0) stride = 48;
    int w = 0;
    for (int i = 0; i < count; ++i) {
        uintptr_t e = reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)i;
        uint16_t did = *reinterpret_cast<uint16_t*>(e + devidOff);
        int32 race = CachedRace((int32)did);
        bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        // Special-fusion source array (a1+1136) is a 2-level grouped structure:
        // group-container entries store a heap pointer at offset +0 (no devil id),
        // with their gated children already removed. Drop those emptied containers
        // too so the UI doesn't render them as placeholder slots.
        uint64_t head = *reinterpret_cast<uint64_t*>(e + 0);
        // Empty group-container entries (special fusion source) have no devil id
        // at devidOff but a non-null pointer at +0 -> drop them so they don't
        // render as placeholder slots.
        bool emptyContainer = (did == 0 && head != 0);
        if (gated || emptyContainer) {
            DiagOnce(STR("[COMP_REMOVE] removed devil=") + std::to_wstring((int)did)
                + STR(" race=") + std::to_wstring(race)
                + (emptyContainer ? STR(" (empty-container)") : STR("")));
            continue;
        }
        if (w != i) {
            std::memmove(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)w),
                         reinterpret_cast<void*>(e), (size_t)stride);
        }
        ++w;
    }
    *reinterpret_cast<int*>(arrPtr + 8) = w;
    if (count != w && (count - w) > 0) {
        LOG("[COMPACT] removed {} ({}->{}) stride={} devidOff={}",
            (int64_t)(count - w), (int64_t)count, (int64_t)w, (int64_t)stride, (int64_t)devidOff);
    }
}

static __int64 __fastcall HkCompose(__int64 a1, __int64 a2, __int64 a3) {
    __int64 r = s_ComposeOrig(a1, a2, a3);
    if (IsEnabled()) {
        if (a2) CompactArrayAt(a2, 48, 4);   // displayed result list
        if (a3) CompactArrayAt(a3, 4, 0);     // source list (compendium reads this for display)
    }
    return r;
}

static void TryInstallCompendiumHook() {
    if (s_ComposeInstalled.exchange(true)) return;

    auto* Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.AUniteCharaPanelCtrlBase:CanBeSelectedAsSearchFusion"));
    if (!Fn) Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.UniteCharaPanelCtrlBase:CanBeSelectedAsSearchFusion"));
    if (!Fn) { LOG("[FusionGating] compose: CanBeSelectedAsSearchFusion NOT FOUND"); s_ComposeInstalled.store(false); return; }
    void* thunk = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(Fn->GetFunc()));
    if (!thunk) { LOG("[FusionGating] compose: GetFunc() null"); s_ComposeInstalled.store(false); return; }
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(thunk) - 0x140D5E440;

    void* target = reinterpret_cast<void*>(moduleBase + 0x140BB3AA0);
    if (target < (void*)0x140000000 || target > (void*)0x160000000) {
        LOG("[FusionGating] compose method target {:p} out of range", target);
        s_ComposeInstalled.store(false); return;
    }

    uint64_t targetAddr = reinterpret_cast<uint64_t>(target);
    uint64_t origAddr = 0;
    auto det = std::make_unique<PLH::x64Detour>(targetAddr, reinterpret_cast<uint64_t>(PLH::FnCast(HkCompose, &s_ComposeOrig)), &origAddr);
    if (!det->hook()) {
        LOG("[FusionGating] compose x64Detour FAILED at {:p}", target);
        s_ComposeInstalled.store(false);
        return;
    }
    s_ComposeOrig = PLH::FnCast(origAddr, s_ComposeOrig);
    s_ComposeDetour = std::move(det);
    LOG("[FusionGating] compose hook installed: method={:p} orig={:p}", target, (void*)origAddr);
}

// ── Special-fusion displayed result array ──
// Builder sub_140BB7950 (Special(7950)) fills the displayed list into the
// TArray at panel offset 1200 (stride 48, count@1208, cap@1212) -- this is the
// array GetSpecialFusionDevilCount / GetSpecialFusionDevilByIndex read.
// We compact it (remove gated) and make the count getter return the new count
// so the widget iterates only valid, non-gated entries. The 7950 post-hook
// (spec arrOff=1200) compacts the same array once at build time; this hook
// re-asserts compaction (idempotent) and returns the authoritative count so
// count/getter and ByIndex always agree.
static const int kSpecialResultOff = 1200;   // TArray<result> in panel (the displayed list)
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
    int64_t arrPtr = (int64_t)Ctx + kSpecialResultOff;
    void* base = *reinterpret_cast<void**>(arrPtr + 0);
    int count = *reinterpret_cast<int*>(arrPtr + 8);
    int cap = *reinterpret_cast<int*>(arrPtr + 12);
    if (base && count > 0 && cap > 0 && count <= cap && count <= 4096) {
        int w = 0;
        for (int i = 0; i < count; ++i) {
            uintptr_t e = (uintptr_t)base + kSpecialStride * (uintptr_t)i;
            int did = *reinterpret_cast<int32_t*>(e + kSpecialDevidOff);
            int race = CachedRace(did);
            bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
            if (gated) continue;
            if (w != i) std::memmove((void*)((uintptr_t)base + kSpecialStride * (uintptr_t)w),
                                     (void*)e, (size_t)kSpecialStride);
            ++w;
        }
        *reinterpret_cast<int*>(arrPtr + 8) = w;
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
    const int gOff = 1136, gCountOff = -4, gCapOff = 8, gStride = 48, gDevOff = 16;
    const int dOff = 1200, dCountOff = 8, dCapOff = 12, dStride = 48;
    void* gbase = *reinterpret_cast<void**>(a1 + gOff);
    int gcount = *reinterpret_cast<int*>(a1 + gOff + gCountOff);
    int gcap   = *reinterpret_cast<int*>(a1 + gOff + gCapOff);
    void* dbase = *reinterpret_cast<void**>(a1 + dOff);
    int dcount = *reinterpret_cast<int*>(a1 + dOff + dCountOff);
    if (!gbase || gcount <= 0 || gcap <= 0 || gcount > gcap || gcount > 4096) return;
    if (!dbase || dcount <= 0) return;
    int w = 0;
    for (int i = 0; i < gcount; ++i) {
        uintptr_t ge = (uintptr_t)gbase + gStride * (uintptr_t)i;
        uint16_t did = *reinterpret_cast<uint16_t*>(ge + gDevOff);
        int32 race = CachedRace((int32)did);
        bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        if (gated) {
            DiagOnce(STR("[SPECIAL_REMOVE] group devil=") + std::to_wstring((int)did)
                + STR(" race=") + std::to_wstring(race));
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
    *reinterpret_cast<int*>(a1 + dOff + dCountOff) = w;
    if (w != gcount)
        LOG("[SPECIAL_COMPACT] removed {} special results ({}->{})", (int64_t)(gcount - w), (int64_t)gcount, (int64_t)w);
    if (w != gcount && w > 0 && dbase) {
        std::string msg = "[SPECIAL_AFTER] count=" + std::to_string(w) + ": ";
        int show = (w < 6) ? w : 6;
        for (int i = 0; i < show; ++i) {
            uintptr_t e = (uintptr_t)dbase + dStride * (uintptr_t)i;
            uint16_t did = *reinterpret_cast<uint16_t*>(e + 4);
            int gi = *reinterpret_cast<int*>(e + 0);
            msg += " [" + std::to_string(i) + ":d" + std::to_string(did) + ",g" + std::to_string(gi) + "]";
        }
        LOG("[SPECIAL_AFTER]{}", std::wstring(msg.begin(), msg.end()));
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
