#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <string>
#include <vector>
#include <set>
#include <map>

using namespace RC;
using namespace RC::Unreal;

class ShinseiSurvey {
public:
    static void SetEnabled(bool enabled) { s_Enabled = enabled; }
    static void Setup();
    static void RunSurvey();
    static void Tick();

private:
    struct Vec3 { float X{}, Y{}, Z{}; };

    static void ScanMapShinsei();
    static void DiscoverProperties();
    static bool GetActorLocation(UObject* actor, Vec3& out);
    static void RecordShinsei(const std::wstring& mapName, int32 id, float x, float y, float z);
    static void WriteJson();

    struct ShinseiEntry { int32 id; float x; float y; float z; };
    static inline std::map<std::wstring, std::vector<ShinseiEntry>> s_Shinsei{};

    static bool s_Enabled;
    static inline int s_PendingScans{ 0 };
    static inline int s_FramesSinceMapLoad{ 0 };
};
