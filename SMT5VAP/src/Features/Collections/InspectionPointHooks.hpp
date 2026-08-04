#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace InspectionPointHooks {
    // Feature toggles (no-op until Setup() has been called).
    // AlwaysShow : all navi gimmicks stay visible (forces exist=true unless triggered).
    // OnlyOnce    : a triggered gimmick can never reappear (blocks exist=true for triggered ids).
    void SetAlwaysShow(bool enabled);
    void SetOnlyOnce(bool enabled);

    // Registers all hooks. Call once from on_unreal_init.
    void Setup();

    // Called every frame from on_update (auto-show timing uses wall-clock time).
    void Tick();

    // ── Triggered-point tracking ──
    // Gimmick ids are unique per map, so every id is keyed together with the map id.
    // The pickup flow (AddNaviDevilGimmickCheckCounter then SetNaviDevilGimmickExist(id, false))
    // is detected automatically and adds the id to this set.
    void AddTriggered(std::int32_t mapId, std::int32_t id); 
    void RemoveTriggered(std::int32_t mapId, std::int32_t id);
    void ClearTriggered();
    bool IsTriggered(std::int32_t mapId, std::int32_t id);
    bool IsTriggered(std::int32_t id);                        // uses the current map id
    std::size_t TriggeredCount();                             // all maps
    std::size_t TriggeredCount(std::int32_t mapId);
    std::vector<std::pair<std::int32_t, std::int32_t>> GetTriggeredList();

    // Map id of the currently loaded map (cached from BPL_MapData:GetMapID).
    std::int32_t CurrentMapId();

    // Callbacks fired when a gimmick spot is collected (mapId + gimmick id).
    // For AP persistence: subscribe to know every picked-up inspection point.
    using GimmickPickedUpCallback = std::function<void(std::int32_t mapId, std::int32_t id)>;
    void OnGimmickPickedUp(GimmickPickedUpCallback cb);

    // Re-runs the ctrl's CalcNaviGimmickExist_ForSaveData pass (the same flow the
    // game runs on a fresh boot) on the live BP_NaviDevilCtrl_C instance(s). Used
    // to restore spot visibility after a teleport, where the game only re-rolls
    // the lottery (SetExist) but never re-reads/updates the spots.
    void RefreshSpotVisibility();
}
