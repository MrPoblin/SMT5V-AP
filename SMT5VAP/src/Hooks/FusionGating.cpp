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

// One-time reflection dump of the live panel's UFunctions. Reveals the actual
// "decide/commit/fusion" Blueprint event name we must hook reflectively (since
// the commit path is Blueprint-driven, not the native CanBeSelected* pre-checks).
static std::atomic<bool> s_Dumped{false};
static void DumpPanelFunctions(UObject* Panel) {
    if (s_Dumped.exchange(true)) return;
    UClass* cls = Panel->GetClassPrivate();
    static const wchar_t* kKeys[] = {
        STR("Fusion"), STR("Unite"), STR("Decide"), STR("Combine"), STR("Exec"),
        STR("Result"), STR("Select"), STR("Confirm"), STR("Commit"), STR("Fuse"),
        STR("Search"), STR("Special"), STR("Able")
    };
    for (UFunction* Fn : TFieldRange<UFunction>(cls, EFieldIterationFlags::IncludeAll)) {
        if (!Fn) continue;
        std::wstring name = std::wstring(Fn->GetFName().ToString());
        bool hit = false;
        for (auto k : kKeys) if (name.find(k) != std::wstring::npos) { hit = true; break; }
        if (!hit) continue;
        bool native = Fn->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native);
        void* thunk = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(Fn->GetFunc()));
        DiagOnce(STR("[FN] ") + name + STR(" native=") + (native ? STR("1") : STR("0"))
            + STR(" thunk=") + std::wstring(fmt::format(L"{:p}", thunk)));
    }
}

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

void Setup() {
    LOG("[FusionGating] Setup...");

    // BIESetInitializePanelAnimation is a BlueprintImplementableEvent overridden
    // on the BP subclass (BP_UniteCharaPanelCtrl_C). It's dispatched through
    // ProcessEvent (it appeared in the SEEN diagnostic), but the UFunction lives
    // on the BP asset class — NOT on the native UniteCharaPanelCtrlBase — so a
    // per-UFunction RegisterPostHook on the base never fires. Instead we tap the
    // global ProcessEvent POST path (proven to fire) and filter by function name.
    Hook::RegisterProcessEventPostCallback(
        [](Hook::TCallbackIterationData<void>&, UObject* Context, UFunction* Function, void* Parms) {
            if (!IsEnabled() || !Function || !Context || !Parms) return;
            std::wstring fn = std::wstring(Function->GetFName().ToString());
            if (fn != STR("BIESetInitializePanelAnimation")) return;
            struct Params { int32 InIndex; };
            int32 slot = static_cast<Params*>(Parms)->InIndex;
            // Install the native CanBeSelected* hooks once we have a live panel.
            TryInstallNativeHook();
            DumpPanelFunctions(Context);
            if (GrayIfGatedSlot(Context, slot, STR("Search"),
                    STR("GetSearchFusionDevilCount"), STR("GetSearchFusionDevilIdByIndex"),
                    STR("GetSearchFusionDevilOffsetCursorPosition")))
                return;
            GrayIfGatedSlot(Context, slot, STR("Special"),
                    STR("GetSpecialFusionDevilCount"), STR("GetSpecialFusionDevilIdByIndex"),
                    STR("GetSpecialFusionDevilOffsetCursorPosition"));
        },
        Hook::FCallbackOptions{
            .OwnerModName = STR("SMT5VAP"),
            .HookName = STR("FusionGatingGray")
        }
    );
    LOG("[FusionGating] Registered ProcessEvent-post gray filter");

    LOG("[FusionGating] Setup complete");
}

} // namespace FusionGating
