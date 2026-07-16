#include "MimanSurvey.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include "src/HookHelper.hpp"
#include "src/Hooks/MimanHooks.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <cmath>
#include <fstream>
#include <numeric>
#include <json/json.h>

using namespace RC;
using namespace RC::Unreal;

bool MimanSurvey::s_Enabled = true;

static int32 GetIntProp(UObject* obj, const wchar_t* name) {
    if (!obj) return -1;
    auto* cls = obj->GetClassPrivate();
    if (!cls) return -1;
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return -1;
    auto* p = prop->ContainerPtrToValuePtr<int32>(obj);
    return p ? *p : -1;
}

bool MimanSurvey::GetActorLocation(UObject* actor, Vec3& out) {
    if (!actor) return false;
    if (!s_LocFn) {
        s_LocFn = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Engine.Actor:K2_GetActorLocation"));
        if (!s_LocFn) return false;
    }
    // K2_GetActorLocation returns FVector = 3 floats (12 bytes), matching Vec3.
    actor->ProcessEvent(s_LocFn, &out);
    return true;
}

void MimanSurvey::RecordMiman(const std::wstring& mapName, int32 saveId, float x, float y) {
    auto& vec = s_Mimans[mapName];
    for (auto& e : vec) {
        if (e.saveId == saveId) {
            // Update if we previously recorded a zero/placeholder location.
            if (e.x == 0.f && e.y == 0.f && (x != 0.f || y != 0.f)) {
                e.x = x;
                e.y = y;
            }
            return;
        }
    }
    vec.push_back({ saveId, x, y });
}

void MimanSurvey::LoadTemplate() {
    std::ifstream in("miman_template.json");
    if (!in) return;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, in, &root, &errs)) {
        WARN(L"[MimanSurvey] Failed to parse existing miman_template.json");
        return;
    }
    Json::Value mimans = root["mimans"];
    if (!mimans.isObject()) return;
    for (auto it = mimans.begin(); it != mimans.end(); ++it) {
        std::string key = it.key().asString();
        std::wstring mapName(key.begin(), key.end());
        for (auto& entry : *it) {
            int32 saveId = entry.isMember("id") ? entry["id"].asInt() : -1;
            float x = entry.isMember("x") ? entry["x"].asFloat() : 0.f;
            float y = entry.isMember("y") ? entry["y"].asFloat() : 0.f;
            if (saveId >= 0) RecordMiman(mapName, saveId, x, y);
        }
    }
    LOG("[MimanSurvey] Loaded prior miman_template.json ({} maps)",
        (int)s_Mimans.size());
}

void MimanSurvey::WriteTemplate() {
    Json::Value root;
    root["mimans"] = Json::Value(Json::objectValue);
    for (auto& [mapName, entries] : s_Mimans) {
        Json::Value arr(Json::arrayValue);
        for (auto& e : entries) {
            Json::Value obj;
            obj["id"] = e.saveId;
            obj["x"] = e.x;
            obj["y"] = e.y;
            arr.append(obj);
        }
        // Numeric map key as string (json keys are always strings).
        std::string mapKey;
        mapKey.reserve(mapName.size());
        for (wchar_t c : mapName) mapKey.push_back(static_cast<char>(c));
        root["mimans"][mapKey] = arr;
    }
    std::ofstream out("miman_template.json");
    if (out) {
        out << root.toStyledString();
        LOG("[MimanSurvey] Wrote miman_template.json ({} maps, total {} miman)",
            (int)s_Mimans.size(),
            std::accumulate(s_Mimans.begin(), s_Mimans.end(), 0,
                [](int n, const auto& kv) { return n + (int)kv.second.size(); }));
    } else {
        WARN("[MimanSurvey] Failed to open miman_template.json for writing");
    }
}

void MimanSurvey::ScanClass(const wchar_t* className) {
    if (!s_Enabled) return;

    std::vector<UObject*> actors;
    UObjectGlobals::FindAllOf(className, actors);

    int liveCount = 0;
    for (auto* a : actors) {
        if (!a) continue;
        // Skip CDO / class defaults (no world).
        auto* world = a->GetWorld();
        if (!world) continue;

        Vec3 loc{};
        bool got = GetActorLocation(a, loc);

        // Class name + actor name for identification.
        std::wstring clsName = a->GetClassPrivate() ? a->GetClassPrivate()->GetName().c_str() : L"?";
        std::wstring actName = a->GetName().c_str();

        // Collectible Miman carry their true SaveID on the actor (no need to
        // collect them). Other classes just report -1.
        int32 saveId = GetIntProp(a, L"SaveID");

        LOG(R"(MIMAN_SURVEY class="{}" actor="{}" MAP="{}" SaveID={} X={:.2f} Y={:.2f} Z={:.2f} live={})",
            clsName, actName, GameState::MapName(), saveId,
            got ? loc.X : 0.f, got ? loc.Y : 0.f, got ? loc.Z : 0.f,
            (world != nullptr));

        // Only the real collectible Miman (BP_EventMiman_C) get recorded.
        if (saveId >= 0 && got) {
            RecordMiman(GameState::MapName(), saveId, loc.X, loc.Y);
        }

        s_LoggedActors.insert(a);
        ++liveCount;
    }

    LOG("[MimanSurvey] ScanClass '{}' -> total={} live={}", className, (int)actors.size(), liveCount);
}

void MimanSurvey::DiscoverClasses() {
    if (!s_Enabled) return;
    LOG("[MimanSurvey] === BEGIN CLASS DISCOVERY ===");

    // Candidate class names to probe. The real collectible Miman actor class is
    // unknown until runtime; probe all plausible names so one run reveals it.
    const wchar_t* candidates[] = {
        L"BP_EventMiman_C",
        L"BP_EventMiman_Tutorial_C",
        L"BP_EventMiman_Answer_C",
        L"Npc631_C",
        L"BP_EventMiman",
        L"Npc631",
        L"BP_MapSymbolSpawner_C",
        L"BP_MapSymbol_C",
    };

    for (const wchar_t* c : candidates) {
        std::vector<UObject*> actors;
        UObjectGlobals::FindAllOf(c, actors);
        int live = 0;
        for (auto* a : actors) if (a && a->GetWorld()) ++live;
        if (actors.size() > 0) {
            LOG("[MimanSurvey] CANDIDATE '{}' total={} live={}", c, (int)actors.size(), live);
        }
    }
    LOG("[MimanSurvey] === END CLASS DISCOVERY ===");
}

void MimanSurvey::SetupMimanFlagMapping() {
    // Hook SetMimanFlag(ID, flag). When a Miman is found (flag=true), find the
    // nearest live Miman-type actor to the player and log ID<->transform.
    auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_MapData:SetMimanFlag"));
    if (!Func) {
        WARN("[MimanSurvey] Failed to find SetMimanFlag for ID mapping");
        return;
    }

    FProperty* IdProp = Func->GetPropertyByName(STR("ID"));
    FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

    Func->RegisterPostHook(
        [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (!MimanSurvey::s_Enabled) return;

            int32 mimanId = -1;
            bool flag = false;
            if (IdProp) {
                if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                    mimanId = *P;
            }
            if (FlagProp) {
                if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                    flag = *P;
            }
            if (!flag || mimanId < 0) return;

            // Player position to find nearest Miman actor.
            GameState::UpdatePosition();
            Vec3 player{ GameState::PosX(), GameState::PosY(), GameState::PosZ() };

            const wchar_t* classesToTry[] = {
                L"BP_EventMiman_C", L"BP_EventMiman_Tutorial_C", L"BP_EventMiman_Answer_C", L"Npc631_C"
            };

            for (const wchar_t* cls : classesToTry) {
                std::vector<UObject*> actors;
                UObjectGlobals::FindAllOf(cls, actors);
                for (auto* a : actors) {
                    if (!a || !a->GetWorld()) continue;
                    Vec3 loc{};
                    if (!GetActorLocation(a, loc)) continue;
                    float dx = loc.X - player.X;
                    float dy = loc.Y - player.Y;
                    float dz = loc.Z - player.Z;
                    float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 < 2000.0f * 2000.0f) {
                        std::wstring clsName = a->GetClassPrivate() ? a->GetClassPrivate()->GetName().c_str() : L"?";
                        LOG(R"(MIMAN_ID_MAP id={} MAP="{}" class="{}" X={:.2f} Y={:.2f} Z={:.2f} dist={:.1f})",
                            mimanId, GameState::MapName(), clsName,
                            loc.X, loc.Y, loc.Z, std::sqrt(dist2));
                    }
                }
            }
        });
    LOG("[MimanSurvey] SetMimanFlag ID-mapping hook registered");
}

void MimanSurvey::RunSurvey() {
    if (!s_Enabled) {
        LOG("[MimanSurvey] disabled");
        return;
    }
    LOG("[MimanSurvey] === RUN SURVEY (map: {}) ===", GameState::MapName());
    DiscoverClasses();
    ScanClass(L"BP_EventMiman_C");
    ScanClass(L"BP_EventMiman_Tutorial_C");
    ScanClass(L"BP_EventMiman_Answer_C");
    ScanClass(L"Npc631_C");
    WriteTemplate();
    LOG("[MimanSurvey] === END SURVEY ===");
}

void MimanSurvey::Tick() {
    if (!s_Enabled) return;
    if (s_PendingScans > 0) {
        ++s_FramesSinceMapLoad;
        // Re-scan at ~1s (60f), ~3s (180f), ~6s (360f) after a map load.
        if (s_FramesSinceMapLoad == 60 || s_FramesSinceMapLoad == 180 ||
            s_FramesSinceMapLoad == 360 || s_FramesSinceMapLoad == 600) {
            RunSurvey();
            --s_PendingScans;
            if (s_PendingScans == 0) s_FramesSinceMapLoad = 0;
        }
    }
}

void MimanSurvey::Setup() {
    LOG("[MimanSurvey] Setup");

    LoadTemplate();

    SetupMimanFlagMapping();

    // Schedule delayed re-scans after each map loads (Gray Miman spawn post-load).
    GameState::OnMapChanged([](const std::wstring& mapName) {
        LOG("[MimanSurvey] Map changed -> {}", mapName);
        s_FramesSinceMapLoad = 0;
        s_PendingScans = 4;   // re-scan at 1s/3s/6s/10s
        RunSurvey();           // also scan immediately
    });

    LOG("[MimanSurvey] Setup complete (F9 to re-run survey)");
}
