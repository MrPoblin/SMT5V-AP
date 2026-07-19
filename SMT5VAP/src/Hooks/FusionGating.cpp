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

static char __fastcall HkFusionPredicate(int devilId) {
    char orig = s_OrigPred(devilId);
    int32_t race = (devilId > 0) ? GetDevilRaceId(devilId) : -1;
    bool gated = (race >= 0 && APState::FusionRaces::IsRaceGated(race));
    if (gated) DiagOnce(STR("[GATE] sub_140AB6E40 devil=") + std::to_wstring(devilId)
        + STR(" race=") + std::to_wstring(race) + STR(" orig=") + std::to_wstring((int)orig)
        + STR(" -> BLOCK(1)"));
    if (gated) return 1; // force "cannot fuse"
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
static void CompactGatedEntries(__int64 a1, int arrOff, int stride, int devidOff, const wchar_t* tag) {
    void* base = *reinterpret_cast<void**>(a1 + arrOff);
    int count = *reinterpret_cast<int*>(a1 + arrOff + 8);
    int cap = *reinterpret_cast<int*>(a1 + arrOff + 12);
    if (!base || count <= 0 || cap <= 0 || count > cap || count > 4096) return;
    if (stride <= 0) stride = 48;
    int w = 0;
    for (int i = 0; i < count; ++i) {
        uintptr_t e = reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)i;
        uint16_t did = *reinterpret_cast<uint16_t*>(e + devidOff);
        int32 race = CachedRace((int32)did);
        bool gated = (did > 0 && race >= 0 && APState::FusionRaces::IsRaceGated(race));
        if (gated) {
            DiagOnce(std::wstring(tag) + STR(" removed devil=") + std::to_wstring((int)did)
                + STR(" race=") + std::to_wstring(race));
            continue;
        }
        if (w != i) {
            std::memmove(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)w),
                         reinterpret_cast<void*>(e), (size_t)stride);
        }
        ++w;
    }
    *reinterpret_cast<int*>(a1 + arrOff + 8) = w;
}

// Diagnostic-only hook: logs the populated array shape (and the dispatcher mode
// byte for sub_140BB7500) so we can map exactly which builder serves each fusion
// screen. No graying, no ProcessEvent -> safe.
struct DiagHook {
    using Fn = char(__fastcall*)(__int64);
    Fn Orig = nullptr;
    std::unique_ptr<PLH::x64Detour> Detour;
    int ArrOff = 1216;
    int selOffGray = 21;
    int Stride = 48;
    int DevidOff = 4;
    int ArrOff2 = 0;   // optional 2nd (source) array to compact
    int Stride2 = 48;
    int DevidOff2 = 4;
    std::string Tag = "";
    bool IsDispatcher = false; // read mode byte at a1+1098
    bool DoGray = false;
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
    return r;
}

// Per-index trampolines. Each forwards to HkDiagList(idx) then optionally compacts.
static inline void DoGrayCompact(DiagHook& h, __int64 a1) {
    if (!h.DoGray || !IsEnabled()) return;
    CompactGatedEntries(a1, h.ArrOff, h.Stride, h.DevidOff, STR("[REMOVE]"));
    if (h.ArrOff2 != 0) {
        CompactGatedEntries(a1, h.ArrOff2, h.Stride2, h.DevidOff2, STR("[REMOVE]"));
    }
}
static char __fastcall HkDiagTramp0(__int64 a1) { char r = HkDiagList(0, a1); DoGrayCompact(s_Diag[0], a1); return r; }
static char __fastcall HkDiagTramp1(__int64 a1) { char r = HkDiagList(1, a1); DoGrayCompact(s_Diag[1], a1); return r; }
static char __fastcall HkDiagTramp2(__int64 a1) { char r = HkDiagList(2, a1); DoGrayCompact(s_Diag[2], a1); return r; }
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
    // arrOff = offset of the TArray (base/count/cap) in `this`. gray flag set
    // later once we confirm which builders serve reverse/special primary.
    struct Spec { uint64_t off; int arrOff; const char* tag; bool disp; bool gray; int selOff; int stride; int devidOff; int arrOff2; int stride2; int devidOff2; };
    Spec specs[] = {
        { 0x140BB6460, 1216, "DyadSec(6460)", false, true,  20, 48, 4, 960, 32, 12 },  // dyad secondary: result+source(a1+960,str32,devid+12)
        { 0x140BB6CF0, 1216, "RevSec(6cf0)",  false, true,  21, 48, 4, 0, 48, 4 },  // reverse secondary (gray)
        { 0x140BB7950, 1200, "Special(7950)", false, true,  21, 48, 4, 1136, 48, 16 },  // special: result(a1+1200,str48,d+4)+source(a1+1136,str48,d+16)
        { 0x140BB7500, 1216, "Disp(7500)",    true,  false, 21, 48, 4, 0, 48, 4 },  // dispatcher (mode byte@1098)
        { 0x140BB7570, 1024, "Dyprim(7570)",  false, true,  21,  4, 0, 0, 4, 0 },  // reverse-primary result list (gray)
        { 0x140BB7760, 1008, "RevSecPop(7760)",false,false, 21, 48, 4, 0, 48, 4 },  // reverse secondary populator
        { 0x140BB8060, 1216, "RevPrim(8060)", false, true,  21, 48, 4, 0, 48, 4 },  // reverse result list (gray, if it fires)
    };
    s_Diag.clear();
    for (auto& s : specs) {
        void* m = reinterpret_cast<void*>(moduleBase + s.off);
        if (m < (void*)0x140000000 || m > (void*)0x160000000) {
            LOG("[FusionGating] method {:p} out of range", m); continue;
        }
        DiagHook dh;
        dh.ArrOff = s.arrOff; dh.Tag = s.tag; dh.IsDispatcher = s.disp; dh.selOffGray = s.selOff;
        dh.Stride = s.stride; dh.DevidOff = s.devidOff;
        dh.ArrOff2 = s.arrOff2; dh.Stride2 = s.stride2; dh.DevidOff2 = s.devidOff2;
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
        if (gated) {
            DiagOnce(STR("[COMP_REMOVE] removed devil=") + std::to_wstring((int)did)
                + STR(" race=") + std::to_wstring(race));
            continue;
        }
        if (w != i) {
            std::memmove(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + stride * (uintptr_t)w),
                         reinterpret_cast<void*>(e), (size_t)stride);
        }
        ++w;
    }
    *reinterpret_cast<int*>(arrPtr + 8) = w;
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
    uintptr_t target = 0x140BB3AA0;
    uint64_t orig = 0;
    auto det = std::make_unique<PLH::x64Detour>(target, reinterpret_cast<uint64_t>(PLH::FnCast(HkCompose, &s_ComposeOrig)), &orig);
    if (!det->hook()) {
        LOG("[FusionGating] compose x64Detour FAILED at {:p}", (void*)target);
        s_ComposeInstalled.store(false);
        return;
    }
    s_ComposeOrig = PLH::FnCast(orig, s_ComposeOrig);
    s_ComposeDetour = std::move(det);
    LOG("[FusionGating] compose hook installed: method={:p} orig={:p}", (void*)target, (void*)orig);
}

void Setup() {
    LOG("[FusionGating] Setup...");

    // Install native hooks + one-time reflection dump the first time the panel
    // initializes.
    Hook::RegisterProcessEventPostCallback(
        [](Hook::TCallbackIterationData<void>&, UObject* Context, UFunction* Function, void* Parms) {
            if (!IsEnabled() || !Function || !Context || !Parms) return;
            std::wstring cls = std::wstring(Context->GetClassPrivate()->GetName().c_str());
            if (cls != STR("BP_UniteCharaPanelCtrl_C")) return;
            std::wstring fn = std::wstring(Function->GetFName().ToString());
            if (fn != STR("BIESetInitializePanelAnimation")) return;
            BuildRaceCache();
            TryInstallNativeHook();
            TryInstallBuildHook();
            TryInstallCompendiumHook();
        },
        Hook::FCallbackOptions{
            .OwnerModName = STR("SMT5VAP"),
            .HookName = STR("FusionGatingInit")
        }
    );
    LOG("[FusionGating] Registered fusion gating hooks");

    LOG("[FusionGating] Setup complete");
}

} // namespace FusionGating
