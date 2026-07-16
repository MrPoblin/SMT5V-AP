#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <string>
#include <vector>
#include <set>
#include <functional>

using namespace RC;
using namespace RC::Unreal;

class MimanSurvey {
public:
    // Enable/disable the diagnostic survey. ON by default for data gathering.
    static void SetEnabled(bool enabled) { s_Enabled = enabled; }

    // Called once from mod init.
    static void Setup();

    // Trigger an immediate full discovery + scan of the current map.
    static void RunSurvey();

    // Called every frame from on_update; performs delayed re-scans after a map
    // change so late-spawning Gray Miman are captured.
    static void Tick();

private:
    struct Vec3 { float X{}, Y{}, Z{}; };

    // Scan a single candidate actor class by name and log every live instance.
    static void ScanClass(const wchar_t* className);

    // Discover which candidate classes actually exist in the loaded game.
    static void DiscoverClasses();

    // Capture SetMimanFlag(ID) -> nearest live Miman actor transform mapping.
    static void SetupMimanFlagMapping();

    // Returns the player's current world location (reuses K2_GetActorLocation).
    static bool GetActorLocation(UObject* actor, Vec3& out);

    // Accumulate discovered collectible Miman and write miman_template.json.
    static void RecordMiman(const std::wstring& mapName, int32 saveId, float x, float y);
    static void WriteTemplate();
    // Load any previously-written miman_template.json so data survives crashes.
    static void LoadTemplate();

    // Map numeric id -> (saveId, x, y) discovered this run.
    struct MimanEntry { int32 saveId; float x; float y; };
    static inline std::map<std::wstring, std::vector<MimanEntry>> s_Mimans{};

    static bool s_Enabled;
    static inline std::set<UObject*> s_LoggedActors{};   // dedupe by pointer
    static inline UFunction* s_LocFn{ nullptr };
    static inline int s_PendingScans{ 0 };               // delayed re-scan counter
    static inline int s_FramesSinceMapLoad{ 0 };
};
