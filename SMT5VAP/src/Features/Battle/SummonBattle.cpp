#include "SummonBattle.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Windows.h>
#include <format>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace SummonBattle {

// Whether the most recent Summon() call successfully dispatched.
// Diagnostic only — the encounter itself is owned by the engine and
// the caller can verify success by polling for a fresh
// EncountActor_New_C or BattleMainWorkBase.
static bool s_LastSummonDispatched{false};

// Returns the live MapCommonCtrl_C for the current map. The encounter
// system lives on this per-map actor, so any spawn request must be
// routed through it. Mirrors NoEncounterMode's lookup.
static UObject* GetActiveMapCommon() {
    std::vector<UObject*> ctrls;
    UObjectGlobals::FindAllOf(STR("MapCommonCtrl_C"), ctrls);
    for (auto* c : ctrls) {
        if (c && c->GetWorld()) return c;
    }
    return nullptr;
}

// Forward declaration for the SEH-guarded inner call. The wrapper
// itself is defined below Summon() — see that function for the
// rationale (MSVC requires __try to live in a function with no
// C++ object unwinding, so the FOutputDevice is constructed in
// the outer wrapper and passed in by pointer to the POD-only
// inner).
__declspec(noinline) static bool CallBPVM_SummonEncountForDebug(
    UObject* map, const TCHAR* cmd);

void Setup() {
    // No pre-resolution needed: the call goes through the BP VM
    // string dispatcher, which looks up the function by name at call
    // time. The BP VM hooks (CallFunctionByNameWithArguments) are
    // scanned by UE4SS at startup; if they failed to find a signature
    // we'll just log a warning on first Summon() call.
    LOG("[SummonBattle] Setup complete");
}

bool IsActive() {
    return s_LastSummonDispatched;
}

// Summon(encountID, enemyList)
//
// Spawns an encounter at the player's current position by invoking
// MapCommonCtrl_C::BPI_CallEncountForDebug via the BP VM (the same
// path the in-game debug menu uses). The engine then drives the
// normal EncountActor → BattleMain pipeline, so the resulting battle
// is indistinguishable from a natural one.
//
// Parameters:
//   encountID — row in the EncountData table. Pass a valid row for
//               the current map area; the engine will look up that
//               row's m_EnemyIDArray and spawn its enemies.
//   enemyList — explicit devil-ID override. When non-empty, the
//               engine uses this list INSTEAD of the table row's
//               m_EnemyIDArray. When empty, the table is used
//               verbatim. In practice you can pass {} and the
//               encountID alone is enough to spawn whatever the
//               game would have spawned at this row on its own.
//
// This function ONLY dispatches the spawn. It does not touch the
// resulting battle in any way. If a caller wants to force a defeat,
// do that through DeathFunctions::KillLocalPlayer after the
// BattleMainWorkBase has come up. SummonBattle deliberately keeps
// the two responsibilities separate so callers can chain them
// however they like (e.g. spawn-only for tests, spawn-then-kill
// for death-link).
void Summon(int32_t encountID, const std::vector<int32_t>& enemyList) {
    s_LastSummonDispatched = false;

    if (!UObject::CallFunctionByNameWithArgumentsInternal.is_ready()) {
        WARN("[SummonBattle] CallFunctionByNameWithArguments is not ready; "
             "this usually means UE4SS failed to scan the BP VM hook signature");
        return;
    }

    auto* map = GetActiveMapCommon();
    if (!map) { WARN("[SummonBattle] cannot summon: no active MapCommonCtrl_C"); return; }

    // BPI_CallEncountForDebug signature (from CXXHeaderDump
    // BPI_MapCommon.hpp line 45):
    //   void BPI_CallEncountForDebug(
    //       int32 encountID,
    //       const TArray<int32>& enemyList,
    //       int32 SafeZoneID,
    //       bool ManualSafeZone,
    //       E_BTL_SYMBOL_ENCOUNT EncountType);
    //
    // BPI_CallEncountForDebug is a BlueprintCallable interface method
    // whose UFUNCTION decoration lives only on the interface UClass,
    // not on MapCommonCtrl_C's direct field table. We invoke it via
    // the BP VM string-based dispatcher
    // (UObject::CallFunctionByNameWithArgumentsInternal), which goes
    // through the BP executor and handles interface methods correctly
    // — the same path the in-game debug menu uses.
    //
    // BP VM call-string syntax (positional):
    //   "FuncName arg1 arg2 ..."
    // with string args quoted, enum names as identifiers, TArrays
    // as `(1, 2, 3)` or `()` for empty. The BP executor unpacks the
    // literals onto the call frame as the correct typed values.
    //
    // SafeZoneID=-1 means "engine picks the closest safe zone".
    // ManualSafeZone=false (engine controls respawn point).
    // EncountType=E_BTL_SYMBOL_ENCOUNT_NORMAL (=0).

    StringType enemyListStr;
    if (enemyList.empty()) {
        enemyListStr = STR("()");
    } else {
        enemyListStr = STR("(");
        for (size_t i = 0; i < enemyList.size(); ++i) {
            if (i > 0) enemyListStr += STR(",");
            enemyListStr += std::to_wstring(enemyList[i]);
        }
        enemyListStr += STR(")");
    }

    StringType cmd = std::format(
        L"BPI_CallEncountForDebug {} {} -1 false E_BTL_SYMBOL_ENCOUNT_NORMAL",
        encountID, enemyListStr);

    LOG("[SummonBattle] invoking BP VM: {}", cmd);

    // SEH guard: the BP VM executor runs the function on the game
    // thread; if the function body or its callees hit a null pointer
    // (e.g. an invalid encountID, a stale map state, a bad EncountPawn),
    // the access violation would otherwise terminate the process.
    // Catching the exception lets us log the failure cleanly and
    // leave the rest of the mod's UI / callbacks functional.
    //
    // The actual call lives in a separate POD-only function
    // (CallBPVM_SummonEncountForDebug) because MSVC forbids __try
    // in functions that need C++ object unwinding — Summon() uses
    // std::vector, std::format, StringType, etc., so it cannot
    // contain __try directly.
    bool ok = CallBPVM_SummonEncountForDebug(map, cmd.c_str());

    if (ok) {
        s_LastSummonDispatched = true;
        LOG("[SummonBattle] BP VM call returned (encounter chain owned by the engine)");
    } else {
        WARN("[SummonBattle] BP VM call returned false or raised (encounter rejected / AV / invalid args)");
    }
}

// Inner SEH-guarded BP VM call. MSVC forbids __try in functions
// whose bodies need C++ object unwinding, so the FOutputDevice is
// constructed in the caller (this function takes a pointer to a
// caller-owned instance) and only POD state lives on this stack.
// We use raw __try/__except because UE4SS's SEH_TRY/SEH_EXCEPT
// calls std::exit on any access violation — which would tear
// down the whole mod, defeating the purpose of catching.
__declspec(noinline) static bool CallBPVMInner(
    UObject* map, const TCHAR* cmd, FOutputDevice* ar_out) {
    if (!UObject::CallFunctionByNameWithArgumentsInternal.is_ready()) return false;
    if (!map || !cmd || !ar_out) return false;

    using BPVMFn = bool(*)(UObject*, const TCHAR*, FOutputDevice&, UObject*, bool);
    auto addr = UObject::CallFunctionByNameWithArgumentsInternal.get_function_address();
    if (!addr) return false;
    auto fn = reinterpret_cast<BPVMFn>(addr);

    bool ok = false;
    __try {
        ok = fn(map, cmd, *ar_out, map, true);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                ? EXCEPTION_EXECUTE_HANDLER
                : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return ok;
}

// Public wrapper: constructs the FOutputDevice (which has a
// virtual destructor so it can't live in the __try function) and
// delegates to the POD-only inner. This is the function Summon()
// calls. Returns true on a clean dispatch, false on a
// UE4SS-unready condition or an access violation inside the
// engine's encounter path.
__declspec(noinline) static bool CallBPVM_SummonEncountForDebug(
    UObject* map, const TCHAR* cmd) {
    if (!UObject::CallFunctionByNameWithArgumentsInternal.is_ready()) return false;
    if (!map || !cmd) return false;

    FOutputDevice ar;
    return CallBPVMInner(map, cmd, &ar);
}

} // namespace SummonBattle
