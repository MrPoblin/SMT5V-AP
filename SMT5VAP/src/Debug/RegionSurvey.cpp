#include "RegionSurvey.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include "src/Data/RegionNames.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <json/json.h>

using namespace RC;
using namespace RC::Unreal;

bool RegionSurvey::s_Enabled = true;

namespace {
    int32 GetIntProp(UObject* obj, const wchar_t* name) {
        if (!obj) return -1;
        auto* cls = obj->GetClassPrivate();
        if (!cls) return -1;
        FProperty* prop = cls->FindProperty(FName(name));
        if (!prop) return -1;
        auto* p = prop->ContainerPtrToValuePtr<int32>(obj);
        return p ? *p : -1;
    }

    FName GetNameProp(UObject* obj, const wchar_t* name) {
        if (!obj) return {};
        auto* cls = obj->GetClassPrivate();
        if (!cls) return {};
        FProperty* prop = cls->FindProperty(FName(name));
        if (!prop) return {};
        auto* p = prop->ContainerPtrToValuePtr<FName>(obj);
        return p ? *p : FName();
    }

    struct FVec3f { float X{}, Y{}, Z{}; };
}

bool RegionSurvey::GetVectorProp(UObject* obj, const wchar_t* name, float out[3]) {
    if (!obj) return false;
    auto* cls = obj->GetClassPrivate();
    if (!cls) return false;
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return false;
    auto* p = prop->ContainerPtrToValuePtr<FVec3f>(obj);
    if (!p) return false;
    out[0] = p->X; out[1] = p->Y; out[2] = p->Z;
    return true;
}

bool RegionSurvey::GetActorLocation(UObject* actor, Vec3& out) {
    static UFunction* s_LocFn = nullptr;
    if (!actor) return false;
    if (!s_LocFn) {
        s_LocFn = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Engine.Actor:K2_GetActorLocation"));
        if (!s_LocFn) return false;
    }
    actor->ProcessEvent(s_LocFn, &out);
    return true;
}

float RegionSurvey::GetRootYaw(UObject* actor) {
    auto* aa = static_cast<AActor*>(actor);
    if (!aa) return 0.f;
    return static_cast<float>(aa->K2_GetActorRotation().GetYaw());
}

std::wstring RegionSurvey::GetDisplayName(const std::wstring& label) {
    auto it = kRegionNames.find(label);
    if (it != kRegionNames.end()) return it->second;
    return label;
}

void RegionSurvey::ComputeAABB(RegionEntry& e) {
    const float rad = e.yaw * 3.14159265f / 180.f;
    const float c = std::cos(rad), s = std::sin(rad);
    float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
    float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
    for (int i = 0; i < 8; ++i) {
        float lx = ((i & 1) ? e.hx : -e.hx);
        float ly = ((i & 2) ? e.hy : -e.hy);
        float lz = ((i & 4) ? e.hz : -e.hz);
        float wx = e.cx + lx * c - ly * s;
        float wy = e.cy + lx * s + ly * c;
        float wz = e.cz + lz;
        minX = std::min(minX, wx); maxX = std::max(maxX, wx);
        minY = std::min(minY, wy); maxY = std::max(maxY, wy);
        minZ = std::min(minZ, wz); maxZ = std::max(maxZ, wz);
    }
    e.minX = minX; e.maxX = maxX;
    e.minY = minY; e.maxY = maxY;
    e.minZ = minZ; e.maxZ = maxZ;
}

void RegionSurvey::WriteTemplate() {
    Json::Value root;
    root["regions"] = Json::Value(Json::objectValue);

    for (auto& [mapName, entries] : s_Regions) {
        Json::Value arr(Json::arrayValue);
        for (auto& e : entries) {
            Json::Value obj;
            std::string actorStr;
            actorStr.reserve(e.actorName.size());
            for (wchar_t wc : e.actorName) actorStr.push_back(static_cast<char>(wc));
            std::string labelStr;
            labelStr.reserve(e.label.size());
            for (wchar_t wc : e.label) labelStr.push_back(static_cast<char>(wc));
            std::string nameStr;
            nameStr.reserve(e.displayName.size());
            for (wchar_t wc : e.displayName) nameStr.push_back(static_cast<char>(wc));

            obj["actor"] = actorStr;
            obj["label"] = labelStr;
            obj["name"] = nameStr;
            obj["priority"] = e.priority;
            obj["center"] = Json::Value(Json::arrayValue);
            obj["center"].append(e.cx);
            obj["center"].append(e.cy);
            obj["center"].append(e.cz);
            obj["extent"] = Json::Value(Json::arrayValue);
            obj["extent"].append(e.hx);
            obj["extent"].append(e.hy);
            obj["extent"].append(e.hz);
            obj["yaw"] = e.yaw;
            obj["min"] = Json::Value(Json::arrayValue);
            obj["min"].append(e.minX);
            obj["min"].append(e.minY);
            obj["min"].append(e.minZ);
            obj["max"] = Json::Value(Json::arrayValue);
            obj["max"].append(e.maxX);
            obj["max"].append(e.maxY);
            obj["max"].append(e.maxZ);
            obj["polygon"] = Json::Value(Json::arrayValue);
            for (int i = 0; i < 4; ++i) {
                Json::Value pt(Json::arrayValue);
                pt.append(e.px[i]);
                pt.append(e.py[i]);
                obj["polygon"].append(pt);
            }
            arr.append(obj);
        }
        std::string mapKey;
        mapKey.reserve(mapName.size());
        for (wchar_t wc : mapName) mapKey.push_back(static_cast<char>(wc));
        root["regions"][mapKey] = arr;
    }

    std::ofstream out("region_survey.json");
    if (out) {
        out << root.toStyledString();
        LOG("[RegionSurvey] Wrote region_survey.json ({} maps, total {} regions)",
            (int)s_Regions.size(),
            std::accumulate(s_Regions.begin(), s_Regions.end(), 0,
                [](int n, const auto& kv) { return n + (int)kv.second.size(); }));
    } else {
        WARN("[RegionSurvey] Failed to open region_survey.json for writing");
    }
}

void RegionSurvey::RunSurvey() {
    if (!s_Enabled) {
        LOG("[RegionSurvey] disabled");
        return;
    }

    const std::wstring mapName = GameState::MapName();
    LOG("[RegionSurvey] === RUN SURVEY (map: {}) ===", mapName);

    std::vector<UObject*> actors;
    UObjectGlobals::FindAllOf(L"BP_MapAreaName_IgnorePause_C", actors);

    int liveCount = 0;
    auto& mapRegions = s_Regions[mapName];
    mapRegions.clear();

    for (auto* a : actors) {
        if (!a || !a->GetWorld()) continue;

        FName labelFn = GetNameProp(a, L"AreaNameLabel");
        int32 priority = GetIntProp(a, L"Priority");
        float hitSize[3]{};
        GetVectorProp(a, L"HitSize", hitSize);

        Vec3 loc{};
        GetActorLocation(a, loc);
        float yaw = GetRootYaw(a);

        RegionEntry e;
        e.actorName = a->GetName().c_str();
        e.label = labelFn != FName() ? labelFn.ToString().c_str() : L"(none)";
        e.displayName = GetDisplayName(e.label);
        e.priority = priority;
        e.hx = hitSize[0];
        e.hy = hitSize[1];
        e.hz = hitSize[2];
        e.yaw = yaw;
        // The Hit box is mounted on the actor root at a +100 Z offset (rotated by yaw,
        // which leaves the Z axis untouched), so the box center is root + (0,0,100).
        e.cx = loc.X;
        e.cy = loc.Y;
        e.cz = loc.Z + 100.f;
        ComputeAABB(e);

        // 2D footprint: rotate the +/- (hx,hy) rectangle by yaw around the center.
        // The boxes are vertical prisms (huge Z span), so Z is irrelevant for the
        // 2D map projection - this polygon is the region's shape on a flat map.
        {
            const float rad = e.yaw * 3.14159265f / 180.f;
            const float c = std::cos(rad), s = std::sin(rad);
            for (int i = 0; i < 4; ++i) {
                float lx = (i & 1) ? e.hx : -e.hx;
                float ly = (i & 2) ? e.hy : -e.hy;
                e.px[i] = e.cx + lx * c - ly * s;
                e.py[i] = e.cy + lx * s + ly * c;
            }
        }

        LOG(R"(REGION_SURVEY MAP="{}" actor="{}" label="{}" name="{}" priority={} center=({:.2f},{:.2f},{:.2f}) extent=({:.2f},{:.2f},{:.2f}) yaw={:.2f} aabb=({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f}) polygon=(({:.2f},{:.2f}),({:.2f},{:.2f}),({:.2f},{:.2f}),({:.2f},{:.2f})))",
            mapName, e.actorName, e.label, e.displayName, e.priority,
            e.cx, e.cy, e.cz, e.hx, e.hy, e.hz, e.yaw,
            e.minX, e.minY, e.minZ, e.maxX, e.maxY, e.maxZ,
            e.px[0], e.py[0], e.px[1], e.py[1], e.px[2], e.py[2], e.px[3], e.py[3]);

        mapRegions.push_back(std::move(e));
        ++liveCount;
    }

    if (actors.size() > 0) {
        LOG("[RegionSurvey] Found {} BP_MapAreaName_IgnorePause_C, {} live", (int)actors.size(), liveCount);
    }

    WriteTemplate();
    LOG("[RegionSurvey] === END SURVEY ===");
}

void RegionSurvey::Tick() {
    if (!s_Enabled) return;
    if (s_PendingScans > 0) {
        ++s_FramesSinceMapLoad;
        if (s_FramesSinceMapLoad == 60 || s_FramesSinceMapLoad == 180) {
            RunSurvey();
            --s_PendingScans;
            if (s_PendingScans == 0) s_FramesSinceMapLoad = 0;
        }
    }
}

void RegionSurvey::Setup() {
    LOG("[RegionSurvey] Setup");

    GameState::OnMapChanged([](const std::wstring& mapName) {
        LOG("[RegionSurvey] Map changed -> {}", mapName);
        s_FramesSinceMapLoad = 0;
        s_PendingScans = 2;
        RunSurvey();
    });

    LOG("[RegionSurvey] Setup complete (F11 to re-run survey)");
}
