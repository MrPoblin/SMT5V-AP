#include "InspectionPointHooks.hpp"
#include "src/GameState.hpp"
#include "src/Helper/HookHelper.hpp"
#include "src/Log/Log.hpp"

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace InspectionPointHooks {

    namespace {
        std::set<std::int64_t> s_Triggered;
        std::mutex s_Mutex;
        std::vector<GimmickPickedUpCallback> s_PickedUpCallbacks;

        std::atomic<bool> s_AlwaysShow{false};
        std::atomic<bool> s_OnlyOnce{false};
        std::atomic<bool> s_PendingPickup{false};
        std::atomic<std::int32_t> s_CurrentMapId{0};

        UObject* s_MapDataCDO = nullptr;
        UFunction* s_SetExistFn = nullptr;
        UFunction* s_SetExistAllFn = nullptr;
        UFunction* s_CheckExistFn = nullptr;

        // FNaviDevilGimmickData row (returned by value from GetNaviDevilGimmickData_FromID).
        constexpr std::int32_t GIMMICK_ROW_SIZE = 0x50;
        // Offsets inside the row.
        constexpr std::int32_t ROW_MAP_ID = 0x00;
        constexpr std::int32_t ROW_INDEX = 0x04;
        constexpr std::int32_t ROW_START_FLAG = 0x0C;
        constexpr std::int32_t ROW_END_FLAG = 0x10;

        UObject* s_NaviDevilDataCDO = nullptr;
        UFunction* s_GetGimmickDataMaxFn = nullptr;
        UFunction* s_GetGimmickDataFn = nullptr;

        // Wall-clock timing for the auto-show (frame counts are unreliable
        // because the game runs at uncapped FPS).
        std::chrono::steady_clock::time_point s_MapLoadTime{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point s_LastAutoRefresh{std::chrono::steady_clock::now()};
        std::atomic<bool> s_AutoShowDone{false};
        std::atomic<std::int32_t> s_RefreshLastShown{0};

        // (mapId,index) -> table index cache to avoid rescans on every FromID call.
        std::unordered_map<std::int64_t, std::int32_t> s_TableIndexCache;
        std::int32_t s_TableMax = -1;

        static std::int32_t GetGimmickDataMax() {
            if (!s_NaviDevilDataCDO || !s_GetGimmickDataMaxFn) return 0;
            struct Params {
                std::int32_t ReturnValue;
            } P{};
            s_NaviDevilDataCDO->ProcessEvent(s_GetGimmickDataMaxFn, &P);
            return P.ReturnValue;
        }

        // Fetch row i of the gimmick data table into out (0x50 bytes). Returns false on failure.
        static bool GetGimmickData(std::int32_t i, std::uint8_t out[GIMMICK_ROW_SIZE]) {
            if (!s_NaviDevilDataCDO || !s_GetGimmickDataFn) return false;
            struct Params {
                std::int32_t Index;
                std::uint8_t Result[GIMMICK_ROW_SIZE];
            } P{};
            P.Index = i;
            s_NaviDevilDataCDO->ProcessEvent(s_GetGimmickDataFn, &P);
            std::memcpy(out, P.Result, GIMMICK_ROW_SIZE);
            return true;
        }

        static constexpr std::int64_t MakeKey(std::int32_t mapId, std::int32_t id) {
            return (static_cast<std::int64_t>(mapId) << 32) | static_cast<std::uint32_t>(id);
        }

        // Walk the FFrame chain to reconstruct the BP call stack. Static library
        // calls (BPL_MapData) always run with the library CDO as Context, so the
        // real caller is only visible through the frame stack.
        static RC::StringType BuildStackString(FFrame& Frame, std::size_t maxDepth = 6) {
            RC::StringType out;
            FFrame* F = &Frame;
            for (std::size_t i = 0; F && i < maxDepth; ++i) {
                const RC::StringType node = F->Node() ? F->Node()->GetFullName() : RC::StringType(STR("?"));
                const RC::StringType obj = F->Object() ? F->Object()->GetFullName() : RC::StringType(STR("null"));
                if (i) out += STR(" -> ");
                out += node;
                out += STR("[");
                out += obj;
                out += STR("]");
                F = F->PreviousFrame();
            }
            return out;
        }

        static UFunction* FindFunc(std::initializer_list<const wchar_t*> paths) {
            for (auto* p : paths) {
                if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
                    return F;
            }
            return nullptr;
        }

        static UObject* FindCDO(std::initializer_list<const wchar_t*> paths) {
            for (auto* p : paths) {
                if (auto* Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, p))
                    return Obj;
            }
            return nullptr;
        }

        static UObject* FindMapDataCDO() {
            auto* Obj = FindCDO({STR("/Script/Project.Default__BPL_MapData"),
                                 STR("/Script/Project.Default__BPL_MapData_C")});
            if (Obj) return Obj;
            auto* Cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_MapData"));
            if (!Cls)
                Cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_MapData_C"));
            if (Cls) return Cls->CreateDefaultObject();
            return nullptr;
        }

        static UObject* FindNaviDevilDataCDO() {
            auto* Obj = FindCDO({STR("/Script/Project.Default__BPL_NaviDevilData"),
                                 STR("/Script/Project.Default__BPL_NaviDevilData_C")});
            if (Obj) return Obj;
            auto* Cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_NaviDevilData"));
            if (!Cls)
                Cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_NaviDevilData_C"));
            if (Cls) return Cls->CreateDefaultObject();
            return nullptr;
        }

        // Find the table index of the row matching (mapId,index); -1 if not found. Cached.
        static std::int32_t FindTableIndex(std::int32_t mapId, std::int32_t index) {
            const std::int64_t key = MakeKey(mapId, index);
            {
                std::lock_guard<std::mutex> lock(s_Mutex);
                auto it = s_TableIndexCache.find(key);
                if (it != s_TableIndexCache.end()) return it->second;
            }
            if (s_TableMax < 0)
                s_TableMax = GetGimmickDataMax();
            std::int32_t found = -1;
            for (std::int32_t i = 0; i < s_TableMax; ++i) {
                std::uint8_t row[GIMMICK_ROW_SIZE];
                if (!GetGimmickData(i, row)) break;
                std::int32_t rMap = 0;
                std::int32_t rIndex = 0;
                std::memcpy(&rMap, row + ROW_MAP_ID, sizeof(rMap));
                std::memcpy(&rIndex, row + ROW_INDEX, sizeof(rIndex));
                if (rMap == mapId && rIndex == index) {
                    found = i;
                    break;
                }
            }
            std::lock_guard<std::mutex> lock(s_Mutex);
            s_TableIndexCache.emplace(key, found);
            return found;
        }

        static void CallSetExist(std::int32_t id, bool exist) {
            if (!s_MapDataCDO || !s_SetExistFn) return;
            struct SetExistParams {
                std::int32_t ID;
                bool exist;
            } Params{id, exist};
            s_MapDataCDO->ProcessEvent(s_SetExistFn, &Params);
        }

        static void CallSetExistAll(bool exist) {
            if (!s_MapDataCDO || !s_SetExistAllFn) return;
            struct SetExistAllParams {
                bool exist;
            } Params{exist};
            s_MapDataCDO->ProcessEvent(s_SetExistAllFn, &Params);
        }

        // Diagnostic: count the spawned gimmick spot actors and list their class
        // names. Decides whether the teleport bug is "no spot actors exist after
        // teleport" vs "actors exist but never re-evaluated".
        //   - AMapCommonCtrl_C.NaviDevilGimmicks TArray<AActor*> at +0xDC0
        //   - AMapCommonCtrl_C.NaviDevilCtrl (ABP_NaviDevilCtrlBase_C*) at +0xDD0;
        //     ABP_NaviDevilCtrlBase_C.NaviDevilGimmicks TArray<AActor*> at +0x230
        //   - any live actors of the NaviDevil BP classes, whatever they are.


    }

    void Tick() {

        // Auto-show: after a map change, retry every ~5s until the BP ctrl asset
        // and spot interface functions are loaded (they load a few seconds AFTER
        // the map change), then force every non-triggered spot visible when
        // AlwaysShow is on (fixes spots vanishing after teleport).
        // Re-show spots on a slow cadence (~10s) starting 10s after a map change,
        // once the map/world is stable. Frequent re-shows raced map-teardown and
        // crashed on "loading a save / finished loading a map".
        // Auto-show using wall-clock time (frame counts are unreliable because
        // the game runs uncapped FPS). Retry every 5s until a refresh actually
        // showed spots (s_AutoShowDone), up to ~120s after a map change.
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_MapLoadTime).count();
        if (s_AlwaysShow.load() && !s_AutoShowDone.load() && elapsedMs >= 5000 && elapsedMs < 120000 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_LastAutoRefresh).count() >= 5000) {
            s_LastAutoRefresh = now;
            RefreshSpotVisibility();
            if (s_RefreshLastShown.load() > 0)
                s_AutoShowDone.store(true);
        }
    }

    void Setup() {
        s_MapDataCDO = FindMapDataCDO();
        if (!s_MapDataCDO) {
            WARN("[InspectionPoint] Failed to find BPL_MapData CDO");
            return;
        }

        // The /Game BP assets (BP_NaviDevilCtrlBase, BPL_NaviDevil) only exist
        // once a map is loaded. Defer their hook registration to the first map
        // load instead of doing it at mod init (where FindObject returns null).
        GameState::OnMapChanged([](const std::wstring&) {
            s_MapLoadTime = std::chrono::steady_clock::now();
            s_LastAutoRefresh = std::chrono::steady_clock::now();
            s_AutoShowDone.store(false);
        });

        if (auto* F = FindFunc({STR("/Script/Project.BPL_MapData:GetMapID"),
                                STR("/Script/Project.BPL_MapData_C:GetMapID")})) {
            F->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
                if (auto* Result = static_cast<std::int32_t*>(Ctx.RESULT_DECL)) {
                    const std::int32_t oldId = s_CurrentMapId.load();
                    if (*Result != oldId) {
                        s_CurrentMapId.store(*Result);
                        LOG("[InspectionPoint] MapID changed: {} -> {}", oldId, *Result);
                    }
                }
            });
        } else {
            WARN("[InspectionPoint] Failed to find GetMapID");
        }

        if (auto* F = FindFunc({STR("/Script/Project.BPL_MapData:AddNaviDevilGimmickCheckCounter"),
                                STR("/Script/Project.BPL_MapData_C:AddNaviDevilGimmickCheckCounter")})) {
            F->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) {
                s_PendingPickup.store(true);
            });
        } else {
            WARN("[InspectionPoint] Failed to find AddNaviDevilGimmickCheckCounter");
        }

        if (auto* F = FindFunc({STR("/Script/Project.BPL_MapData:SetNaviDevilGimmickExist"),
                                STR("/Script/Project.BPL_MapData_C:SetNaviDevilGimmickExist")})) {
            s_SetExistFn = F;
            auto* IdProp = F->GetPropertyByName(STR("ID"));
            auto* ExistProp = F->GetPropertyByName(STR("exist"));
            F->RegisterPreHook([IdProp, ExistProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                std::int32_t id = -1;
                bool exist = false;
                if (IdProp)
                    if (auto* P = IdProp->ContainerPtrToValuePtr<std::int32_t>(Ctx.TheStack.Locals()))
                        id = *P;
                bool* ExistPtr = nullptr;
                if (ExistProp)
                    ExistPtr = ExistProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                if (ExistPtr) exist = *ExistPtr;

                const std::int32_t mapId = s_CurrentMapId.load();

                // Real pickup: AddNaviDevilGimmickCheckCounter fired right before SetExist(id, false).
                if (s_PendingPickup.exchange(false) && !exist) {
                    AddTriggered(mapId, id);
                    LOG("[InspectionPoint] Triggered: map={} id={}", mapId, id);
                    {
                        std::lock_guard<std::mutex> lock(s_Mutex);
                        for (auto& cb : s_PickedUpCallbacks) cb(mapId, id);
                    }
                    return;
                }

                if (s_OnlyOnce.load() && exist && IsTriggered(mapId, id)) {
                    // Never let a triggered gimmick come back.
                    if (ExistPtr) *ExistPtr = false;
                    return;
                }

                if (s_AlwaysShow.load() && !exist && !IsTriggered(mapId, id)) {
                    // All gimmicks are always visible (unless already triggered).
                    if (ExistPtr) *ExistPtr = true;
                }
            });
        } else {
            WARN("[InspectionPoint] Failed to find SetNaviDevilGimmickExist");
        }

        if (auto* F = FindFunc({STR("/Script/Project.BPL_MapData:SetNaviDevilGimmickExistAll"),
                                STR("/Script/Project.BPL_MapData_C:SetNaviDevilGimmickExistAll")})) {
            s_SetExistAllFn = F;
            auto* ExistProp = F->GetPropertyByName(STR("exist"));
            F->RegisterPreHook([ExistProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                if (!s_AlwaysShow.load()) return;
                if (!ExistProp) return;
                if (auto* P = ExistProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals())) {
                    if (!*P) *P = true; // never let the game wipe all exist flags
                }
            });
            F->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) {
                if (!s_OnlyOnce.load()) return;
                // SetExistAll(true) would resurrect triggered gimmicks - clear them again.
                const std::int32_t mapId = s_CurrentMapId.load();
                std::lock_guard<std::mutex> lock(s_Mutex);
                for (const auto key : s_Triggered) {
                    const std::int32_t kMap = static_cast<std::int32_t>(key >> 32);
                    const std::int32_t kId = static_cast<std::int32_t>(key & 0xFFFFFFFF);
                    if (kMap == mapId)
                        CallSetExist(kId, false);
                }
            });
        } else {
            WARN("[InspectionPoint] Failed to find SetNaviDevilGimmickExistAll");
        }

        if (auto* F = FindFunc({STR("/Script/Project.BPL_MapData:CheckNaviDevilGimmickExist"),
                                STR("/Script/Project.BPL_MapData_C:CheckNaviDevilGimmickExist")})) {
            s_CheckExistFn = F;
            auto* IdProp = F->GetPropertyByName(STR("ID"));
            F->RegisterPostHook([IdProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                std::int32_t id = -1;
                if (IdProp)
                    if (auto* P = IdProp->ContainerPtrToValuePtr<std::int32_t>(Ctx.TheStack.Locals()))
                        id = *P;
                auto* Result = static_cast<bool*>(Ctx.RESULT_DECL);
                if (!Result) return;
                const std::int32_t mapId = s_CurrentMapId.load();
                // Diagnostic: log every exist check + the CALLER object (the BP that
                // invokes this native BPL function bypasses ProcessEvent, so UE4SS
                // object-hooks can't see it - only the native post-hook can).
                if (s_OnlyOnce.load() && IsTriggered(mapId, id)) {
                    *Result = false;
                    return;
                }
                if (s_AlwaysShow.load() && !IsTriggered(mapId, id)) {
                    *Result = true;
                }
            });
        } else {
            WARN("[InspectionPoint] Failed to find CheckNaviDevilGimmickExist");
        }

        s_NaviDevilDataCDO = FindNaviDevilDataCDO();
        if (!s_NaviDevilDataCDO) {
            WARN("[InspectionPoint] Failed to find BPL_NaviDevilData CDO");
        }

        // GetNaviDevilGimmickData_FromID is the REAL gate: rows are filtered by
        // event flags (m_StartFlag must be SET, m_EndFlag must be CLEAR). The
        // BP spawner only gets a row when the filter passes, so this is where
        // AlwaysShow/OnlyOnce must act.
        if (auto* F = FindFunc({STR("/Script/Project.BPL_NaviDevilData:GetNaviDevilGimmickData_FromID"),
                                STR("/Script/Project.BPL_NaviDevilData_C:GetNaviDevilGimmickData_FromID")})) {
            auto* MapIdProp = F->GetPropertyByName(STR("MapID"));
            auto* IndexProp = F->GetPropertyByName(STR("Index"));
            auto* TableIndexProp = F->GetPropertyByName(STR("TableIndex"));
            F->RegisterPostHook([MapIdProp, IndexProp, TableIndexProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                auto* Result = static_cast<std::uint8_t*>(Ctx.RESULT_DECL);
                if (!Result) return;

                std::int32_t mapId = -1;
                std::int32_t index = -1;
                if (MapIdProp)
                    if (auto* P = MapIdProp->ContainerPtrToValuePtr<std::int32_t>(Ctx.TheStack.Locals()))
                        mapId = *P;
                if (IndexProp)
                    if (auto* P = IndexProp->ContainerPtrToValuePtr<std::int32_t>(Ctx.TheStack.Locals()))
                        index = *P;
                std::int32_t* TableIndex = nullptr;
                if (TableIndexProp)
                    TableIndex = TableIndexProp->ContainerPtrToValuePtr<std::int32_t>(Ctx.TheStack.Locals());

                (void)mapId; (void)index; (void)TableIndex;

                // OnlyOnce: a triggered gimmick must never be returned again.
                // We deliberately DO NOT mutate the returned row or TableIndex
                // here: the crash bisect showed OnlyOnce's row-zeroing crashes
                // around the 4th pickup (the game reads the row during the
                // pickup/reward flow). The exist-gating hooks (CheckExist -> false
                // for triggered ids, SetExist re-enable blocked) are sufficient to
                // keep triggered spots from appearing, since ForSaveData hides any
                // spot whose exist check returns false.
                if (s_OnlyOnce.load() && IsTriggered(mapId, index)) {
                    return;
                }

                // AlwaysShow: if the event-flag gate rejected the row, find the raw
                // table row and hand it back with its flag gates cleared.
                if (s_AlwaysShow.load() && TableIndex && *TableIndex == -1) {
                    const std::int32_t tIdx = FindTableIndex(mapId, index);
                    if (tIdx >= 0) {
                        std::uint8_t row[GIMMICK_ROW_SIZE];
                        if (GetGimmickData(tIdx, row)) {
                            // Clear the event-flag gates so any later re-check passes.
                            std::memset(row + ROW_START_FLAG, 0, 4);
                            std::memset(row + ROW_END_FLAG, 0, 4);
                            std::memcpy(Result, row, GIMMICK_ROW_SIZE);
                            *TableIndex = tIdx;
                            LOG("[InspectionPoint] AlwaysShow forced row: map={} id={} tableIndex={}", mapId, index, tIdx);
                        }
                    }
                }
            });
        } else {
            WARN("[InspectionPoint] Failed to find GetNaviDevilGimmickData_FromID");
        }



        if (auto* F = FindFunc({STR("/Script/Project.BPL_NaviDevilData:GetNaviDevilGimmickDataMax"),
                                STR("/Script/Project.BPL_NaviDevilData_C:GetNaviDevilGimmickDataMax")})) {
            s_GetGimmickDataMaxFn = F;
        } else {
            WARN("[InspectionPoint] Failed to find GetNaviDevilGimmickDataMax");
        }

        if (auto* F = FindFunc({STR("/Script/Project.BPL_NaviDevilData:GetNaviDevilGimmickData"),
                                STR("/Script/Project.BPL_NaviDevilData_C:GetNaviDevilGimmickData")})) {
            s_GetGimmickDataFn = F;
        } else {
            WARN("[InspectionPoint] Failed to find GetNaviDevilGimmickData");
        }

        LOG("[InspectionPoint] Setup OK");
    }

    void SetAlwaysShow(bool enabled) {
        s_AlwaysShow.store(enabled);
        LOG("[InspectionPoint] SetAlwaysShow({})", enabled);
        if (enabled)
            CallSetExistAll(true);
    }

    void SetOnlyOnce(bool enabled) {
        s_OnlyOnce.store(enabled);
        LOG("[InspectionPoint] SetOnlyOnce({})", enabled);
        if (enabled) {
            const std::int32_t mapId = s_CurrentMapId.load();
            std::lock_guard<std::mutex> lock(s_Mutex);
            for (const auto key : s_Triggered) {
                const std::int32_t kMap = static_cast<std::int32_t>(key >> 32);
                const std::int32_t kId = static_cast<std::int32_t>(key & 0xFFFFFFFF);
                if (kMap == mapId)
                    CallSetExist(kId, false);
            }
        }
    }

    void OnGimmickPickedUp(GimmickPickedUpCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_PickedUpCallbacks.push_back(std::move(cb));
    }

    void AddTriggered(std::int32_t mapId, std::int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Triggered.insert(MakeKey(mapId, id));
    }

    void RemoveTriggered(std::int32_t mapId, std::int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Triggered.erase(MakeKey(mapId, id));
    }

    void ClearTriggered() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Triggered.clear();
    }

    bool IsTriggered(std::int32_t mapId, std::int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        return s_Triggered.contains(MakeKey(mapId, id));
    }

    bool IsTriggered(std::int32_t id) {
        return IsTriggered(s_CurrentMapId.load(), id);
    }

    std::size_t TriggeredCount() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        return s_Triggered.size();
    }

    std::size_t TriggeredCount(std::int32_t mapId) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        std::size_t count = 0;
        for (const auto key : s_Triggered) {
            if (static_cast<std::int32_t>(key >> 32) == mapId)
                ++count;
        }
        return count;
    }

    std::vector<std::pair<std::int32_t, std::int32_t>> GetTriggeredList() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        std::vector<std::pair<std::int32_t, std::int32_t>> out;
        out.reserve(s_Triggered.size());
        for (const auto key : s_Triggered)
            out.emplace_back(static_cast<std::int32_t>(key >> 32),
                             static_cast<std::int32_t>(key & 0xFFFFFFFF));
        return out;
    }

    std::int32_t CurrentMapId() {
        return s_CurrentMapId.load();
    }

    void RefreshSpotVisibility() {
        std::vector<UObject*> ctrls;
        UObjectGlobals::FindAllOf(STR("BP_NaviDevilCtrl_C"), ctrls);
        if (ctrls.empty()) {
            return;
        }

        auto* SaveIDFn = FindFunc({STR("/Game/Blueprints/Map/Gimic/Daath/BP_NaviDevilGimmick_IgnorePause.BP_NaviDevilGimmick_IgnorePause_C:BPI_GetNaviGimmickSaveID")});
        auto* SetActiveFn = FindFunc({STR("/Game/Blueprints/Map/Gimic/Daath/BP_NaviDevilGimmick_IgnorePause.BP_NaviDevilGimmick_IgnorePause_C:BPI_SetNaviGimmickActive")});
        if (!SaveIDFn || !SetActiveFn) {
            return;
        }

        const std::int32_t mapId = s_CurrentMapId.load();
        for (auto* ctrl : ctrls) {
            if (!ctrl) continue;
            auto* ArrProp = ctrl->GetPropertyByName(STR("NaviDevilGimmicks"));
            FScriptArray* arr = ArrProp ? ArrProp->ContainerPtrToValuePtr<FScriptArray>(ctrl) : nullptr;
            if (!arr)
                arr = reinterpret_cast<FScriptArray*>(reinterpret_cast<std::uint8_t*>(ctrl) + 0x230);
            const std::int32_t n = arr ? arr->Num() : -1;
            if (n <= 0 || !arr) {
                continue;
            }

            auto** data = static_cast<UObject**>(arr->GetData());
            std::int32_t shown = 0, skipped = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                UObject* spot = data[i];
                // Skip destroyed/pending-kill spots: picked-up gimmicks get torn
                // down, leaving stale pointers in NaviDevilGimmicks → ProcessEvent
                // on them crashes ("sometimes crashes when i pickup a spot").
                if (!spot || spot->HasAnyInternalFlags(EInternalObjectFlags::PendingKill) || spot->IsUnreachable()) {
                    ++skipped;
                    continue;
                }
                struct { std::int32_t SaveID; } P2{};
                spot->ProcessEvent(SaveIDFn, &P2);
                // OnlyOnce: never re-show a picked-up spot (also avoids the
                // crash from re-activating a collected gimmick).
                if (IsTriggered(mapId, P2.SaveID)) {
                    ++skipped;
                    continue;
                }
                struct { bool Active; } P3{};
                P3.Active = true;
                spot->ProcessEvent(SetActiveFn, &P3);
                ++shown;
            }
            LOG("[InspectionPoint] Refresh: {} spots shown={} skipped(triggered)={}", ctrl->GetFullName(), shown, skipped);
            s_RefreshLastShown.store(shown);
        }
    }

}
