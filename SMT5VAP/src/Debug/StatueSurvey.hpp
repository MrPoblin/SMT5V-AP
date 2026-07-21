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

class StatueSurvey {
public:
    static void SetEnabled(bool enabled) { s_Enabled = enabled; }
    static void Setup();
    static void RunSurvey();
    static void Tick();

private:
    struct Vec3 { float X{}, Y{}, Z{}; };

    static void DiscoverClasses();
    static void ScanClass(const wchar_t* className);
    static void ScanAllLoaded();
    static void SetupStatueFlagMapping();
    static bool GetActorLocation(UObject* actor, Vec3& out);
    static void RecordStatue(const std::wstring& mapName, const std::wstring& actorName, float x, float y, float z);
    static void WriteTemplate();

    struct StatueEntry { std::wstring actorName; float x; float y; float z; };
    static inline std::map<std::wstring, std::vector<StatueEntry>> s_Statues{};

    static bool s_Enabled;
    static inline std::set<UObject*> s_LoggedActors{};
    static inline UFunction* s_LocFn{ nullptr };
    static inline int s_PendingScans{ 0 };
    static inline int s_FramesSinceMapLoad{ 0 };
};
