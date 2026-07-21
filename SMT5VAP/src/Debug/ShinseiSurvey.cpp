#include "ShinseiSurvey.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <json/json.h>

using namespace RC;
using namespace RC::Unreal;

bool ShinseiSurvey::s_Enabled = true;

static int32 GetIntProp(UObject* obj, const wchar_t* name) {
    if (!obj) return -1;
    auto* cls = obj->GetClassPrivate();
    if (!cls) return -1;
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return -1;
    auto* p = prop->ContainerPtrToValuePtr<int32>(obj);
    return p ? *p : -1;
}

static float GetFloatProp(UObject* obj, const wchar_t* name) {
    if (!obj) return -1.f;
    auto* cls = obj->GetClassPrivate();
    if (!cls) return -1.f;
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return -1.f;
    auto* p = prop->ContainerPtrToValuePtr<float>(obj);
    return p ? *p : -1.f;
}

static FName GetNameProp(UObject* obj, const wchar_t* name) {
    if (!obj) return {};
    auto* cls = obj->GetClassPrivate();
    if (!cls) return {};
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return {};
    auto* p = prop->ContainerPtrToValuePtr<FName>(obj);
    return p ? *p : FName();
}

bool ShinseiSurvey::GetActorLocation(UObject* actor, Vec3& out) {
    if (!actor) return false;
    auto* aa = static_cast<AActor*>(actor);
    FVector loc = aa->K2_GetActorLocation();
    out.X = static_cast<float>(FVector::GetX_Internal(loc));
    out.Y = static_cast<float>(FVector::GetY_Internal(loc));
    out.Z = static_cast<float>(FVector::GetZ_Internal(loc));
    return true;
}

void ShinseiSurvey::RecordShinsei(const std::wstring& mapName, int32 id, float x, float y, float z) {
    auto& vec = s_Shinsei[mapName];
    for (auto& e : vec) {
        if (e.id == id) {
            e.x = x;
            e.y = y;
            e.z = z;
            return;
        }
    }
    vec.push_back({ id, x, y, z });
}

void ShinseiSurvey::DiscoverProperties() {
    std::vector<UObject*> actors;
    UObjectGlobals::FindAllOf(L"BP_MapShinsei_C", actors);

    for (auto* a : actors) {
        if (!a || !a->GetWorld()) continue;

        auto* cls = a->GetClassPrivate();
        if (!cls) continue;

        LOG("[ShinseiSurvey] === Class '{}' actor '{}' ===", cls->GetName(), a->GetName());

        struct Probe { const wchar_t* name; const wchar_t* type; };
        Probe probes[] = {
            { L"ShinseiID", L"int" },
            { L"ShinseiId", L"int" },
            { L"m_ShinseiFlag", L"int" },
            { L"ID", L"int" },
            { L"SaveID", L"int" },
            { L"Index", L"int" },
            { L"m_Index", L"int" },
            { L"FlagIndex", L"int" },
            { L"ShinseiIndex", L"int" },
            { L"m_ID", L"int" },
            { L"m_ShinseiID", L"int" },
            { L"m_ShinseiId", L"int" },
            { L"ShinseiFlag", L"int" },
            { L"m_Flag", L"int" },
            { L"MapID", L"int" },
            { L"m_MapID", L"int" },
        };

        for (auto& p : probes) {
            FProperty* prop = cls->FindProperty(FName(p.name));
            if (prop) {
                int32 val = GetIntProp(a, p.name);
                LOG("[ShinseiSurvey]   int {} = {}", p.name, val);
            }
        }

        FName nameVal = GetNameProp(a, L"FlagName");
        if (nameVal != FName()) {
            LOG("[ShinseiSurvey]   FName FlagName = {}", nameVal.ToString());
        }
    }
}

void ShinseiSurvey::ScanMapShinsei() {
    if (!s_Enabled) return;

    std::vector<UObject*> actors;
    UObjectGlobals::FindAllOf(L"BP_MapShinsei_C", actors);

    const wchar_t* idCandidates[] = {
        L"ShinseiID", L"ShinseiId", L"m_ShinseiFlag", L"ID",
        L"SaveID", L"Index", L"m_Index", L"FlagIndex",
        L"ShinseiIndex", L"m_ID", L"m_ShinseiID", L"m_ShinseiId",
        L"ShinseiFlag", L"m_Flag", L"MapID", L"m_MapID",
    };

    for (auto* a : actors) {
        if (!a || !a->GetWorld()) continue;

        Vec3 loc{};
        bool gotLoc = GetActorLocation(a, loc);
        std::wstring clsName = a->GetClassPrivate() ? a->GetClassPrivate()->GetName().c_str() : L"?";
        std::wstring actName = a->GetName().c_str();

        int32 id = -1;
        std::wstring idPropName;
        for (const wchar_t* candidate : idCandidates) {
            int32 val = GetIntProp(a, candidate);
            if (val >= 0) {
                id = val;
                idPropName = candidate;
                break;
            }
        }

        LOG(R"(SHINSEI_SURVEY class="{}" actor="{}" MAP="{}" id={} idProp="{}" X={:.2f} Y={:.2f} Z={:.2f})",
            clsName, actName, GameState::MapName(), id, idPropName,
            loc.X, loc.Y, loc.Z);

        if (id >= 0 && gotLoc) {
            RecordShinsei(GameState::MapName(), id, loc.X, loc.Y, loc.Z);
        }
    }

    LOG("[ShinseiSurvey] ScanMapShinsei -> total live={}, discovered={}",
        (int)actors.size(),
        std::accumulate(s_Shinsei.begin(), s_Shinsei.end(), 0,
            [](int n, const auto& kv) { return n + (int)kv.second.size(); }));
}

void ShinseiSurvey::WriteJson() {
    Json::Value root;
    root["shinsei"] = Json::Value(Json::objectValue);

    for (auto& [mapName, entries] : s_Shinsei) {
        Json::Value arr(Json::arrayValue);
        for (auto& e : entries) {
            Json::Value obj;
            obj["id"] = e.id;
            obj["x"] = e.x;
            obj["y"] = e.y;
            obj["z"] = e.z;
            arr.append(obj);
        }
        std::string mapKey;
        mapKey.reserve(mapName.size());
        for (wchar_t c : mapName) mapKey.push_back(static_cast<char>(c));
        root["shinsei"][mapKey] = arr;
    }

    std::ofstream out("shinsei_template.json");
    if (out) {
        out << root.toStyledString();
        LOG("[ShinseiSurvey] Wrote shinsei_template.json ({} maps, total {} entries)",
            (int)s_Shinsei.size(),
            std::accumulate(s_Shinsei.begin(), s_Shinsei.end(), 0,
                [](int n, const auto& kv) { return n + (int)kv.second.size(); }));
    } else {
        WARN("[ShinseiSurvey] Failed to open shinsei_template.json for writing");
    }
}

void ShinseiSurvey::RunSurvey() {
    if (!s_Enabled) {
        LOG("[ShinseiSurvey] disabled");
        return;
    }
    LOG("[ShinseiSurvey] === RUN SURVEY (map: {}) ===", GameState::MapName());
    DiscoverProperties();
    ScanMapShinsei();
    WriteJson();
    LOG("[ShinseiSurvey] === END SURVEY ===");
}

void ShinseiSurvey::Tick() {
    if (!s_Enabled) return;
    if (s_PendingScans > 0) {
        ++s_FramesSinceMapLoad;
        if (s_FramesSinceMapLoad == 60 || s_FramesSinceMapLoad == 180 ||
            s_FramesSinceMapLoad == 360 || s_FramesSinceMapLoad == 600) {
            RunSurvey();
            --s_PendingScans;
            if (s_PendingScans == 0) s_FramesSinceMapLoad = 0;
        }
    }
}

void ShinseiSurvey::Setup() {
    LOG("[ShinseiSurvey] Setup");

    GameState::OnMapChanged([](const std::wstring& mapName) {
        LOG("[ShinseiSurvey] Map changed -> {}", mapName);
        s_FramesSinceMapLoad = 0;
        s_PendingScans = 4;
        RunSurvey();
    });

    LOG("[ShinseiSurvey] Setup complete");
}
