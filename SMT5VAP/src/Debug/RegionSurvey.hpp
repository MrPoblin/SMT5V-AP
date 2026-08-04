#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <string>
#include <vector>
#include <map>

using namespace RC;
using namespace RC::Unreal;

// Scans the currently loaded map for BP_MapAreaName_IgnorePause actors. Those are the
// trigger volumes that give every named region its bounds (the box shown in the map's
// top-right UI: Tamachi, Mita, Shiba, ...). For each region it logs/records the
// AreaNameLabel, display name, priority, oriented-box center + extents and the
// axis-aligned bounds so regions can be re-defined offline from coordinates.
class RegionSurvey {
public:
    static void Setup();
    static void RunSurvey();
    static void Tick();
    static void SetEnabled(bool enabled) { s_Enabled = enabled; }

private:
    struct Vec3 { float X{}, Y{}, Z{}; };

    struct RegionEntry {
        std::wstring actorName;
        std::wstring label;
        std::wstring displayName;
        int32 priority{};
        float cx{}, cy{}, cz{};        // box center (world)
        float hx{}, hy{}, hz{};        // box half extents
        float yaw{};                   // world yaw (degrees) of the oriented box
        float minX{}, maxX{}, minY{}, maxY{}, minZ{}, maxZ{}; // axis-aligned bounds
        float px[4]{}, py[4]{};        // 2D footprint corners (XY only, for map projection)
    };

    static bool GetActorLocation(UObject* actor, Vec3& out);
    static bool GetVectorProp(UObject* obj, const wchar_t* name, float out[3]);
    static float GetRootYaw(UObject* actor);
    static std::wstring GetDisplayName(const std::wstring& label);
    static void ComputeAABB(RegionEntry& e);
    static void WriteTemplate();

    static bool s_Enabled;
    static inline std::map<std::wstring, std::vector<RegionEntry>> s_Regions{};
    static inline int s_PendingScans{ 0 };
    static inline int s_FramesSinceMapLoad{ 0 };
};
