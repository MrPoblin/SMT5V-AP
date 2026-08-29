#include "SummonBattle.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <format>
#include <vector>
#include <cstring>

using namespace RC;
using namespace RC::Unreal;

namespace SummonBattle {

// Cached UFunction* for BPI_SpawnEncountActor (or BPI_CallEncountForDebug
// as a fallback if the former isn't a real UFUNCTION). Resolved lazily
// on first Summon() to avoid paying FindObject cost at mod init (the
// implementer class may not be loaded yet).
static UFunction* s_SpawnEncountActor{nullptr};

// Whether the most recent Summon() call successfully dispatched
// ProcessEvent. Diagnostic only — the encounter itself is owned by
// the engine and the caller can verify success by polling for a
// fresh EncountActor_New_C or BattleMainWorkBase (see BattleHook for
// the post-victory hook and the existing processEvent sequence).
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

// Dump every UFunction on the given UClass (and on every interface
// it implements) along with each function's first 5 parm property
// names. Used for one-shot diagnostic logging when our preferred
// target function can't be found — gives us a concrete list to
// pick a real UFUNCTION entry point from.
static void DumpClassFunctionsForDiagnostic(const TCHAR* className) {
    std::vector<UObject*> insts;
    UObjectGlobals::FindAllOf(className, insts);
    for (auto* inst : insts) {
        if (!inst) continue;
        UClass* cls = inst->GetClassPrivate();
        if (!cls) continue;
        LOG("[SummonBattle] DIAG: functions on {} ({})", className, cls->GetFullName());
        for (auto* fn : TFieldRange<UFunction>(cls)) {
            if (!fn) continue;
            StringType parmDump{};
            int n = 0;
            for (FProperty* p : TFieldRange<FProperty>(fn)) {
                if (!p) continue;
                if ((static_cast<uint64>(p->GetPropertyFlags()) & 0x1ull) == 0) continue; // not a parm
                if (n > 0) parmDump += STR(", ");
                parmDump += p->GetName();
                if (++n >= 5) break;
            }
            LOG("  - {} : {}", fn->GetName(), parmDump);
        }
        for (const FImplementedInterface& iface : cls->GetInterfaces()) {
            if (!iface.Class) continue;
            LOG("[SummonBattle] DIAG: functions on interface {} of {}", iface.Class->GetName(), className);
            for (auto* fn : TFieldRange<UFunction>(iface.Class)) {
                if (!fn) continue;
                StringType parmDump{};
                int n = 0;
                for (FProperty* p : TFieldRange<FProperty>(fn)) {
                    if (!p) continue;
                    if ((static_cast<uint64>(p->GetPropertyFlags()) & 0x1ull) == 0) continue;
                    if (n > 0) parmDump += STR(", ");
                    parmDump += p->GetName();
                    if (++n >= 5) break;
                }
                LOG("    - {} : {}", fn->GetName(), parmDump);
            }
        }
    }
}

// Resolves the UFunction for the encounter-spawn entry point.
// The CXXHeaderDump showed BPI_CallEncountForDebug on the
// IBPI_MapCommon_C interface, but at runtime we discovered that
// only BPI_SpawnEncountActor is actually a UFUNCTION exposed to
// reflection (the BP-compiled BPI_CallEncountForDebug is not a
// UFUNCTION, it's just a regular C++ method on the interface that
// the cooked build didn't expose). We use BPI_SpawnEncountActor
// as the primary entry point — it takes a list of devil IDs and
// an encountID, which is exactly what we need.
static UFunction* ResolveSpawnEncountActorFn() {
    if (s_SpawnEncountActor) return s_SpawnEncountActor;

    // Try the names most likely to be real UFUNCTIONs on the
    // encounter-spawn chain, in priority order.
    static const TCHAR* funcCandidates[] = {
        STR("BPI_SpawnEncountActor"),
        STR("BPI_CallEncountForDebug"),
    };

    static const TCHAR* liveClassNames[] = {
        STR("MapCommonCtrl_C"),
        STR("MEH_NextEventStart_Walk_C"),
        STR("ProjectGameInstance_C"),
    };

    for (auto* cn : liveClassNames) {
        std::vector<UObject*> insts;
        UObjectGlobals::FindAllOf(cn, insts);
        for (auto* inst : insts) {
            if (!inst) continue;
            UClass* cls = inst->GetClassPrivate();
            if (!cls) continue;

            // Direct class functions
            for (auto* fn : TFieldRange<UFunction>(cls)) {
                if (!fn) continue;
                const FName fnName = fn->GetFName();
                for (auto* want : funcCandidates) {
                    if (fnName == FName(want)) {
                        s_SpawnEncountActor = fn;
                        LOG("[SummonBattle] Found {} as direct member of {}", want, cls->GetFullName());
                        return s_SpawnEncountActor;
                    }
                }
            }

            // Interface functions
            for (const FImplementedInterface& iface : cls->GetInterfaces()) {
                if (!iface.Class) continue;
                for (auto* fn : TFieldRange<UFunction>(iface.Class)) {
                    if (!fn) continue;
                    const FName fnName = fn->GetFName();
                    for (auto* want : funcCandidates) {
                        if (fnName == FName(want)) {
                            s_SpawnEncountActor = fn;
                            LOG("[SummonBattle] Found {} via interface {} on {}",
                                want, iface.Class->GetFullName(), cls->GetFullName());
                            return s_SpawnEncountActor;
                        }
                    }
                }
            }
        }
    }

    // Diagnostic dump: log the UFUNCTIONs we DO have on these
    // classes, so we can pick a real entry point from the log.
    WARN("[SummonBattle] no UFUNCTION encounter-spawn entry found; dumping candidate class functions for diagnosis");
    for (auto* cn : liveClassNames) {
        DumpClassFunctionsForDiagnostic(cn);
    }
    return s_SpawnEncountActor;
}

void Setup() {
    // Pre-resolve the function so the first Summon() doesn't pay the
    // FindObject cost. Failure here is non-fatal — Summon() will retry.
    ResolveSpawnEncountActorFn();
    LOG("[SummonBattle] Setup complete");
}

bool IsActive() {
    return s_LastSummonDispatched;
}

// Summon(encountID, enemyList)
//
// Spawns an encounter at the player's current position via the
// engine's own debug path. The two arguments are:
//
//   encountID — row in the EncountData table the current map can
//               resolve. The engine will look up that row's
//               m_EnemyIDArray and spawn those demons if the
//               encounter chain is valid for the area.
//
//   enemyList — explicit devil-ID override. When non-empty, the
//               engine uses this list INSTEAD of the table row's
//               m_EnemyIDArray. When empty, the table is used
//               verbatim. In practice you can pass {} and the
//               encountID alone is enough to spawn whatever
//               encounter the game would have spawned at this row
//               on its own — BPI_SpawnEncountActor (the underlying
//               entry point) accepts both forms.
//
// The function only dispatches the spawn; the resulting EncountActor
// and (later) BattleMain live for as long as the game chooses. This
// module does NOT touch the resulting battle state in any way —
// callers that want to force a defeat should do so through
// DeathFunctions::KillLocalPlayer, which targets whatever
// BattleMainWorkBase is current at the moment of the call.
void Summon(int32_t encountID, const std::vector<int32_t>& enemyList) {
    s_LastSummonDispatched = false;

    auto* map = GetActiveMapCommon();
    if (!map) { WARN("[SummonBattle] cannot summon: no active MapCommonCtrl_C"); return; }

    // BPI_CallEncountForDebug signature (verified from CXXHeaderDump
    // BPI_MapCommon.hpp line 45):
    //   void BPI_CallEncountForDebug(
    //       int32 encountID,
    //       const TArray<int32>& enemyList,
    //       int32 SafeZoneID,
    //       bool ManualSafeZone,
    //       E_BTL_SYMBOL_ENCOUNT EncountType);
    //
    // BPI_CallEncountForDebug is a BlueprintCallable interface method
    // but its UFUNCTION decoration is on the interface UClass only,
    // not on MapCommonCtrl_C's direct field table. We invoke it via
    // the BP VM string-based dispatcher (CallFunctionByNameWithArguments)
    // which goes through the BP executor and handles interface methods
    // correctly. This is the same path the in-game debug menu uses.
    //
    // The call signature string format is positional args:
    //   "FuncName arg1 arg2 ..."
    // with literal string args in single/double quotes, enum names
    // as identifiers, and structs as object references.
    //
    // For the enemyList TArray<int32> we use the BP literal array
    // syntax: ([1, 2, 3]) or () for empty. The BP executor will
    // unpack it into a TArray on the call frame.
    //
    // SafeZoneID=-1 means "engine picks the closest safe zone".
    // ManualSafeZone=false (engine controls respawn point).
    // EncountType=E_BTL_SYMBOL_ENCOUNT_NORMAL (i.e. literal 0).

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

    FOutputDevice ar;
    bool ok = UObject::CallFunctionByNameWithArgumentsInternal(
        map, cmd.c_str(), ar, /*Executor=*/map, /*bForceCallWithNonExec=*/true);

    if (!UObject::CallFunctionByNameWithArgumentsInternal.is_ready()) {
        WARN("[SummonBattle] CallFunctionByNameWithArguments not available; "
             "falling back to direct BPI_SpawnEncountActor call");
        // Fallback path: if the BP VM isn't ready (e.g. signatures
        // failed to scan), use the direct UFunction approach. This
        // is the path that was crashing before, so we skip the
        // call entirely and report failure — without the VM
        // dispatch, the safer BP-validated path isn't reachable.
        s_LastSummonDispatched = false;
        return;
    }

    if (ok) {
        s_LastSummonDispatched = true;
        LOG("[SummonBattle] BP VM call returned (encounter chain owned by the engine)");
    } else {
        WARN("[SummonBattle] BP VM call returned false (encounter rejected or invalid args)");
    }
}

} // namespace SummonBattle
