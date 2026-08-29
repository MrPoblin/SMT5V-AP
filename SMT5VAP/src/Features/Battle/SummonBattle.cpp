#include "SummonBattle.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/CallbackIterationData.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/Transform.hpp>
#include <Windows.h>
#include <cstring>
#include <format>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace SummonBattle {

// Whether the most recent Summon/SummonEvent call successfully dispatched.
// Diagnostic only — the battle itself is owned by the engine.
static bool s_LastSummonDispatched{false};

// ── Deferred dispatch ──
// The encounter call mutates the map/event state machine, so it must run at
// a clean engine-tick boundary, never mid-tick (e.g. from inside on_update,
// which UE4SS runs while the engine is mid-frame). Mirrors
// DeathFunctions' engine-tick-pre deferral: a requested summon is stashed
// here and actually dispatched on the next engine tick PRE callback.
struct PendingSummon {
    int32_t mapEventId{0};
    bool valid{false};
};
static PendingSummon s_Pending{};

// ── Resolved reflection handles (lazily initialized) ──
static UFunction* s_CallEventEncountFn{nullptr};     // BPI_CallEventEncount (interface fn on MapCommonCtrl_C)
static UObject*   s_MapEventDataCDO{nullptr};        // BPL_MapEventData CDO for row discovery
static UFunction* s_GetMapEventDataFn{nullptr};      // BPL_MapEventData::GetMapEventData
static bool       s_ResolveFailed{false};

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

// Reads the player's current world transform (location + rotation) from the
// active PlayerBase_C so the encounter spawns at the right place. Mirrors
// GameState::UpdatePosition. Returns false if the player can't be resolved.
static bool GetPlayerTransform(FTransform& out) {
    std::vector<UObject*> players;
    UObjectGlobals::FindAllOf(STR("PlayerBase_C"), players);
    UObject* player = nullptr;
    for (auto* p : players) {
        if (p && p->GetWorld()) { player = p; break; }
    }
    if (!player) return false;

    static UFunction* s_GetLocation = nullptr;
    static UFunction* s_GetRotation = nullptr;
    if (!s_GetLocation) {
        s_GetLocation = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Engine.Actor:K2_GetActorLocation"));
    }
    if (!s_GetRotation) {
        s_GetRotation = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Engine.Actor:K2_GetActorRotation"));
    }
    if (!s_GetLocation || !s_GetRotation) return false;

    struct FVectorOut { float X, Y, Z; };
    struct FRotatorOut { float Pitch, Yaw, Roll; };
    FVectorOut loc{};
    FRotatorOut rot{};
    player->ProcessEvent(s_GetLocation, &loc);
    player->ProcessEvent(s_GetRotation, &rot);

    out = FTransform(
        FRotator(rot.Pitch, rot.Yaw, rot.Roll).Quaternion(),
        FVector(loc.X, loc.Y, loc.Z),
        FVector(1.0, 1.0, 1.0));
    return true;
}

// ── MapEvent row discovery ──
// The natural encounter path keys off a MapEventData row. We resolve the
// BPL_MapEventData library CDO + GetMapEventData the same way
// EventFlags.cpp does, then scan rows for one that targets the current
// map and whose Start flags are satisfied (so the engine will actually
// accept the call).
//
// FMapEventData layout (from Dumped Headers, 0x98 bytes):
//   m_MapEventID   int32  @0x00
//   m_StartFlagValues TArray<int32> @0x08
//   m_EndFlagValue int32  @0x18
//   m_AfterFlagValue int32 @0x1C
//   m_MapID        int32  @0x20
//   m_AreaId       int32  @0x24
//   ... (rest unused here)
struct FMapEventDataBuffer {
    int32_t MapEventID;
    int32_t StartFlagsPad[6];   // TArray<int32> (Data*, Count, Capacity)
    int32_t EndFlagValue;
    int32_t AfterFlagValue;
    int32_t MapID;
    int32_t AreaId;
    int32_t pad[72];            // rest of the 0x98 struct
};

static void EnsureMapEventDataResolved() {
    if (s_MapEventDataCDO || s_ResolveFailed) return;
    s_GetMapEventDataFn = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_MapEventData:GetMapEventData"));
    s_MapEventDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
        STR("/Script/Project.Default__BPL_MapEventData"));
    if (!s_GetMapEventDataFn || !s_MapEventDataCDO) {
        WARN("[SummonBattle] BPL_MapEventData::GetMapEventData resolution failed - "
             "auto row discovery disabled (pass an explicit MapEvent id)");
        s_ResolveFailed = true;
        return;
    }
    LOG("[SummonBattle] BPL_MapEventData resolved for MapEvent row discovery");
}

// Reads a MapEventData row via the BP library. Returns true and fills `out`
// if the row resolved.
static bool GetMapEventRow(int32_t mapEventId, FMapEventDataBuffer& out) {
    EnsureMapEventDataResolved();
    if (!s_GetMapEventDataFn || !s_MapEventDataCDO) return false;
    FMapEventDataBuffer buf{};
    struct { int32 ID; FMapEventDataBuffer ReturnValue; } params{mapEventId, buf};
    s_MapEventDataCDO->ProcessEvent(s_GetMapEventDataFn, &params);
    out = params.ReturnValue;
    return true;
}

// Finds a MapEvent row for the current map. Priority:
//   1. explicit id (if > 0 and it resolves to a row for this map)
//   2. auto-discovery: scan the row space for rows whose Start flags are
//      already satisfied (IsMapEventActivateFlags).
// Returns 0 if nothing usable was found.
static int32_t ResolveMapEventId(int32_t explicitId) {
    // If we have an explicit id, just use it — the engine will reject
    // unusable rows gracefully through the SEH guard.
    if (explicitId > 0) {
        FMapEventDataBuffer row{};
        if (GetMapEventRow(explicitId, row)) {
            LOG("[SummonBattle] MapEvent row {} resolved (MapID={}, AreaId={})",
                explicitId, row.MapID, row.AreaId);
            return explicitId;
        }
        WARN("[SummonBattle] explicit MapEvent id {} did not resolve; falling back to discovery", explicitId);
    }

    // Auto-discovery: the row space is dense but sparse in the low range.
    // The game's MapEvent rows are in the low thousands; scan a generous
    // window and take the first row whose MapEventID is non-zero and whose
    // Start flags are already satisfied (IsMapEventActivateFlags).
    // Prefer smaller ids (story-intro rows tend to be valid on most maps).
    for (int32_t id = 1; id <= 3000; ++id) {
        FMapEventDataBuffer row{};
        if (!GetMapEventRow(id, row)) continue;
        if (row.MapEventID == 0) continue;
        if (row.MapID <= 0 && row.AreaId <= 0) continue;

        // Check activation state through the same BP library.
        static UFunction* s_IsActive = nullptr;
        if (!s_IsActive) {
            s_IsActive = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_MapEventData:IsMapEventActivateFlags"));
        }
        if (s_IsActive && s_MapEventDataCDO) {
            struct { int32 _mapEventID; bool ReturnValue; } act{id, false};
            s_MapEventDataCDO->ProcessEvent(s_IsActive, &act);
            if (act.ReturnValue) {
                LOG("[SummonBattle] auto-discovered usable MapEvent row {} (MapID={}, AreaId={})",
                    id, row.MapID, row.AreaId);
                return id;
            }
        }
    }

    WARN("[SummonBattle] no usable MapEvent row auto-discovered");
    return 0;
}

// ── The actual dispatch ──

// Forward declaration for the SEH-guarded inner call.
__declspec(noinline) static bool DispatchEventEncountInner(
    UObject* map, UFunction* fn, void* params);

// Builds the BPI_CallEventEncount_ForUniqueSymbol params buffer and
// dispatches through ProcessEvent. This is the natural *symbol-encounter*
// entry: it drives the same MapEventData-row pipeline as
// BPI_CallEventEncount (models, BGM, camera, MapEvent setup) but with
// normal field-symbol-encounter return semantics — on victory the player
// returns to their field position instead of trying to continue an event
// chain. The plain BPI_CallEventEncount is a scripted-event step that
// expects NextEventID/NowEventID event chaining; calling it cold (with
// -1/-1) spawns the battle but leaves no event to return to, which breaks
// the post-battle transition (black screen).
//
// The UFunction lives on the interface class; GetFunctionByNameInChain
// (which includes EFieldIterationFlags::IncludeInterfaces) resolves it on
// the concrete MapCommonCtrl_C instance so ProcessEvent can execute it.
//
// Signature (Dumped Headers BPI_MapCommon.hpp line 44):
//   void BPI_CallEventEncount_ForUniqueSymbol(
//       int32               eventEncountId,      // MapEventData row id
//       FTransform          SpawnTransform,      // where the encounter spawns
//       bool                ManualWhenNotEscape, // don't auto-resolve escapes
//       bool                HitMapAttack,        // map-attack (player hit first)
//       E_BTL_SYMBOL_ENCOUNT EncountType);       // NORMAL = 0
static bool CallEventEncount(UObject* map, int32_t eventEncountId, const FTransform& spawn) {
    if (!s_CallEventEncountFn) {
        s_CallEventEncountFn = map->GetFunctionByNameInChain(STR("BPI_CallEventEncount_ForUniqueSymbol"));
        if (!s_CallEventEncountFn) {
            WARN("[SummonBattle] BPI_CallEventEncount_ForUniqueSymbol UFunction not found on MapCommonCtrl_C "
                 "(interface fn not in chain?)");
            return false;
        }
        LOG("[SummonBattle] resolved BPI_CallEventEncount_ForUniqueSymbol via GetFunctionByNameInChain");
    }

    // Resolve param offsets from reflection once. Fall back to the known
    // layout if reflection is unavailable (eventEncountId@0, FTransform@16
    // due to 16-byte alignment, ManualWhenNotEscape@64, HitMapAttack@68,
    // EncountType@72).
    static int32_t ofsEventId = -1, ofsManual = -1, ofsHit = -1, ofsType = -1, ofsSpawn = -1;
    if (ofsEventId < 0) {
        if (auto* p = s_CallEventEncountFn->GetPropertyByName(STR("eventEncountId"))) ofsEventId = p->GetOffset_ForInternal();
        if (auto* p = s_CallEventEncountFn->GetPropertyByName(STR("SpawnTransform"))) ofsSpawn = p->GetOffset_ForInternal();
        if (auto* p = s_CallEventEncountFn->GetPropertyByName(STR("ManualWhenNotEscape"))) ofsManual = p->GetOffset_ForInternal();
        if (auto* p = s_CallEventEncountFn->GetPropertyByName(STR("HitMapAttack"))) ofsHit = p->GetOffset_ForInternal();
        if (auto* p = s_CallEventEncountFn->GetPropertyByName(STR("EncountType"))) ofsType = p->GetOffset_ForInternal();
        LOG("[SummonBattle] BPI_CallEventEncount_ForUniqueSymbol param offsets: event@{} spawn@{} manual@{} hit@{} type@{}",
            ofsEventId, ofsSpawn, ofsManual, ofsHit, ofsType);
    }

    // Params buffer: the function has no return value. Size generously and
    // 16-byte align so the FTransform param lands on the alignment the
    // engine expects for its (alignas(16)) members.
    alignas(16) uint8_t params[0x100]{};
    auto put = [&](int32_t ofs, auto val) {
        if (ofs < 0) return;
        *reinterpret_cast<decltype(val)*>(params + ofs) = val;
    };
    put(ofsEventId, eventEncountId);
    if (ofsSpawn >= 0) {
        // The RC::Unreal::FTransform struct is sized for the largest engine
        // layout (UE5 double-based = 0x60), but the game (UE 4.27) uses the
        // pre-5.0 float layout = 0x30 bytes: FQuat(16) + FVector(16) +
        // FVector(16), with the FTransform itself 16-byte aligned. Copy only
        // the first 0x30 bytes into the param slot so we don't overflow into
        // the following params.
        constexpr size_t kGameTransformSize = 0x30;
        static_assert(sizeof(FTransform) >= kGameTransformSize, "RC FTransform smaller than game FTransform");
        std::memcpy(params + ofsSpawn, &spawn, kGameTransformSize);
    }
    put(ofsManual, false);  // ManualWhenNotEscape
    put(ofsHit, false);     // HitMapAttack (not a player-initiated attack)
    put(ofsType, 0);        // E_BTL_SYMBOL_ENCOUNT_NORMAL

    return DispatchEventEncountInner(map, s_CallEventEncountFn, params);
}

// Inner SEH-guarded ProcessEvent dispatch. MSVC forbids __try in functions
// that need C++ object unwinding, so the params vector is built in the
// caller and only POD state lives on this stack.
__declspec(noinline) static bool DispatchEventEncountInner(
    UObject* map, UFunction* fn, void* params) {
    if (!map || !fn || !params) return false;
    __try {
        map->ProcessEvent(fn, params);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                ? EXCEPTION_EXECUTE_HANDLER
                : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

// Does the actual dispatch work. Runs on the engine-tick PRE callback (see
// s_Pending above) so it never executes mid engine-tick.
static void DispatchPendingSummon(int32_t mapEventId) {
    auto* map = GetActiveMapCommon();
    if (!map) { WARN("[SummonBattle] cannot summon: no active MapCommonCtrl_C"); return; }

    int32_t useId = ResolveMapEventId(mapEventId);
    if (useId <= 0) {
        WARN("[SummonBattle] no MapEvent row to summon; aborting");
        return;
    }

    FTransform spawn;
    if (!GetPlayerTransform(spawn)) {
        // Player transform unavailable; default to identity at origin.
        // The engine will still run the pipeline (encounters spawn at the
        // map's default position in the worst case).
        spawn = FTransform();
    }

    LOG("[SummonBattle] dispatching BPI_CallEventEncount_ForUniqueSymbol(MapEvent={}) at player transform", useId);
    bool ok = CallEventEncount(map, useId, spawn);

    if (ok) {
        s_LastSummonDispatched = true;
        LOG("[SummonBattle] BPI_CallEventEncount_ForUniqueSymbol returned (battle chain owned by the engine)");
    } else {
        WARN("[SummonBattle] BPI_CallEventEncount_ForUniqueSymbol raised or was rejected (AV / invalid row / bad map state)");
    }
}

// Engine-tick PRE callback: dispatches any pending summon at a clean tick
// boundary. Same pattern DeathFunctions uses for the death-link kill.
static void OnEngineTickPre(Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
    if (!s_Pending.valid) return;
    s_Pending.valid = false;
    DispatchPendingSummon(s_Pending.mapEventId);
}

void Setup() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    // The UFunction resolution is lazy (needs a live MapCommonCtrl_C), so
    // there's nothing to do up front except make sure the MapEventData
    // library is warm for later row discovery and register the tick
    // callback that performs the actual dispatch.
    EnsureMapEventDataResolved();
    Hook::RegisterEngineTickPreCallback(OnEngineTickPre, Hook::FCallbackOptions{});
    LOG("[SummonBattle] Setup complete (full-fidelity MapEvent summon path)");
}

bool IsActive() {
    return s_LastSummonDispatched;
}

void SummonEvent(int32_t eventEncountId) {
    // Stash the request; the actual dispatch happens on the next engine-tick
    // PRE callback so the encounter call never runs mid engine-tick (which
    // can corrupt the map/event state machine and crash, depending on what
    // the engine was doing when the request arrived — e.g. mid flag/event
    // processing).
    s_Pending.mapEventId = eventEncountId;
    s_Pending.valid = true;
    s_LastSummonDispatched = false;
    LOG("[SummonBattle] summon queued (MapEvent={}, will dispatch on next engine tick)", eventEncountId);
}

void Summon(int32_t encountID, const std::vector<int32_t>& enemyList) {
    // The old debug path (BPI_CallEncountForDebug with an EncountData row +
    // optional enemy override) is superseded by the full MapEvent path. The
    // encountID is kept for source compatibility but the full-fidelity
    // summon goes through the natural pipeline. If the caller passed a
    // MapEvent-compatible id it is honored; otherwise auto-discovery picks
    // a usable row.
    (void)enemyList;
    SummonEvent(encountID);
}

} // namespace SummonBattle
