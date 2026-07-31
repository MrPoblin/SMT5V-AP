#include "StatueSurvey.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include "src/Helper/HookHelper.hpp"
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

bool StatueSurvey::s_Enabled = true;

static int32 GetIntProp(UObject* obj, const wchar_t* name) {
    if (!obj) return -1;
    auto* cls = obj->GetClassPrivate();
    if (!cls) return -1;
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return -1;
    auto* p = prop->ContainerPtrToValuePtr<int32>(obj);
    return p ? *p : -1;
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

static float GetFloatProp(UObject* obj, const wchar_t* name) {
    if (!obj) return -1.f;
    auto* cls = obj->GetClassPrivate();
    if (!cls) return -1.f;
    FProperty* prop = cls->FindProperty(FName(name));
    if (!prop) return -1.f;
    auto* p = prop->ContainerPtrToValuePtr<float>(obj);
    return p ? *p : -1.f;
}

bool StatueSurvey::GetActorLocation(UObject* actor, Vec3& out) {
    if (!actor) return false;
    if (!s_LocFn) {
        s_LocFn = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Engine.Actor:K2_GetActorLocation"));
        if (!s_LocFn) return false;
    }
    actor->ProcessEvent(s_LocFn, &out);
    return true;
}

void StatueSurvey::RecordStatue(const std::wstring& mapName, const std::wstring& actorName, float x, float y, float z) {
    if (actorName.empty()) return;
    auto& vec = s_Statues[mapName];
    for (auto& e : vec) {
        if (e.actorName == actorName) {
            if (e.x == 0.f && e.y == 0.f && e.z == 0.f && (x != 0.f || y != 0.f || z != 0.f)) {
                e.x = x; e.y = y; e.z = z;
            }
            return;
        }
    }
    vec.push_back({ actorName, x, y, z });
}

void StatueSurvey::DiscoverClasses() {
    if (!s_Enabled) return;
    LOG("[StatueSurvey] === BEGIN CLASS DISCOVERY ===");

    const wchar_t* candidates[] = {
        L"BP_StatueLvUp_C",
        L"BP_EventStatueLvUp_C",
        L"BP_EventHitBase_C",
        L"BP_MapEvtHit_C",
        L"BP_MapEventHit_C",
        L"BP_MapStatue_C",
        L"BP_MapDevilStatue_C",
        L"BP_DevilStatue_C",
        L"BP_SaltStatue_C",
    };

    for (const wchar_t* c : candidates) {
        std::vector<UObject*> actors;
        UObjectGlobals::FindAllOf(c, actors);
        int live = 0;
        for (auto* a : actors) if (a && a->GetWorld()) ++live;
        if (actors.size() > 0) {
            LOG("[StatueSurvey] CANDIDATE '{}' total={} live={}", c, (int)actors.size(), live);
            for (auto* a : actors) {
                if (!a) continue;
                auto* cls = a->GetClassPrivate();
                if (!cls) continue;

                struct Probe { const wchar_t* name; };
                Probe probes[] = {
                    L"MapEventID", L"m_MapEventID", L"MapEventId", L"m_MapEventId",
                    L"EventID", L"m_EventID",
                    L"ID", L"SaveID", L"m_ID",
                    L"FlagName", L"m_FlagName",
                    L"StatueID", L"m_StatueID",
                    L"FlagIndex", L"m_FlagIndex",
                };

                for (auto& p : probes) {
                    FProperty* prop = cls->FindProperty(FName(p.name));
                    if (prop) {
                        int32 val = GetIntProp(a, p.name);
                        LOG("[StatueSurvey]   {} int {} = {}", cls->GetName(), p.name, val);
                    }
                }

                FName fn = GetNameProp(a, L"FlagName");
                if (fn != FName()) {
                    LOG("[StatueSurvey]   {} FName FlagName = {}", cls->GetName(), fn.ToString());
                }
                fn = GetNameProp(a, L"m_FlagName");
                if (fn != FName()) {
                    LOG("[StatueSurvey]   {} FName m_FlagName = {}", cls->GetName(), fn.ToString());
                }
            }
        }
    }
    LOG("[StatueSurvey] === END CLASS DISCOVERY ===");
}

void StatueSurvey::ScanClass(const wchar_t* className) {
    if (!s_Enabled) return;

    std::vector<UObject*> actors;
    UObjectGlobals::FindAllOf(className, actors);

    const wchar_t* idCandidates[] = {
        L"MapEventID", L"m_MapEventID", L"MapEventId", L"m_MapEventId",
        L"EventID", L"m_EventID", L"ID", L"SaveID", L"m_ID",
        L"FlagIndex", L"m_FlagIndex",
    };

    int liveCount = 0;
    for (auto* a : actors) {
        if (!a) continue;
        auto* world = a->GetWorld();
        if (!world) continue;

        Vec3 loc{};
        bool gotLoc = GetActorLocation(a, loc);
        std::wstring clsName = a->GetClassPrivate() ? a->GetClassPrivate()->GetName().c_str() : L"?";
        std::wstring actName = a->GetName().c_str();

        int32 id = -1;
        std::wstring idPropName;
        for (const wchar_t* cand : idCandidates) {
            int32 val = GetIntProp(a, cand);
            if (val >= 0) {
                id = val;
                idPropName = cand;
                break;
            }
        }

        FName flagName{};
        flagName = GetNameProp(a, L"FlagName");
        if (flagName == FName()) flagName = GetNameProp(a, L"m_FlagName");

        LOG(R"(STATUE_SURVEY class="{}" actor="{}" MAP="{}" id={} idProp="{}" flagName="{}" X={:.2f} Y={:.2f} Z={:.2f})",
            clsName, actName, GameState::MapName(), id, idPropName,
            flagName != FName() ? flagName.ToString().c_str() : L"(none)",
            loc.X, loc.Y, loc.Z);

        if (gotLoc) {
            RecordStatue(GameState::MapName(), actName, loc.X, loc.Y, loc.Z);
        }

        s_LoggedActors.insert(a);
        ++liveCount;
    }

    if (actors.size() > 0) {
        LOG("[StatueSurvey] ScanClass '{}' -> total={} live={}", className, (int)actors.size(), liveCount);
    }
}

void StatueSurvey::ScanAllLoaded() {
    if (!s_Enabled) return;

    LOG("[StatueSurvey] === SCAN ALL LOADED (map: {}) ===", GameState::MapName());

    const wchar_t* classesToScan[] = {
        L"BP_StatueLvUp_C",
        L"BP_EventStatueLvUp_C",
        L"BP_EventHitBase_C",
        L"BP_MapEvtHit_C",
        L"BP_MapEventHit_C",
    };

    for (const wchar_t* c : classesToScan) {
        ScanClass(c);
    }

    WriteTemplate();
    LOG("[StatueSurvey] === END SCAN ALL ===");
}

void StatueSurvey::SetupStatueFlagMapping() {
    auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.BPL_EventFlag:SetEventFlag"));
    if (!Func) {
        WARN("[StatueSurvey] Failed to find SetEventFlag for ID mapping");
        return;
    }

    FProperty* NameProp = Func->GetPropertyByName(STR("Name"));
    FProperty* FlagProp = Func->GetPropertyByName(STR("sw"));

    Func->RegisterPostHook(
        [NameProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (!StatueSurvey::s_Enabled) return;

            FName flagName;
            bool flagValue = false;
            if (NameProp) {
                if (auto* P = NameProp->ContainerPtrToValuePtr<FName>(Ctx.TheStack.Locals()))
                    flagName = *P;
            }
            if (FlagProp) {
                if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                    flagValue = *P;
            }

            if (!flagValue) return;

            StringType nameStr = flagName.ToString();
            const TCHAR* buf = nameStr.c_str();
            if (!buf || nameStr.length() < 7 || wcsncmp(buf, STR("Statue_"), 7) != 0) return;

            bool isCheck = (wcsstr(buf, STR("_Check_")) != nullptr);
            if (isCheck) return;

            LOG("[StatueSurvey] Statue flag set: {}", nameStr);

            GameState::UpdatePosition();
            Vec3 player{ GameState::PosX(), GameState::PosY(), GameState::PosZ() };

            const wchar_t* classesToTry[] = {
                L"BP_StatueLvUp_C",
                L"BP_EventStatueLvUp_C",
                L"BP_EventHitBase_C",
                L"BP_MapEvtHit_C",
                L"BP_MapEventHit_C",
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
                    if (dist2 < 3000.0f * 3000.0f) {
                        int32 mapEventId = -1;
                        const wchar_t* idProps[] = {
                            L"MapEventID", L"m_MapEventID", L"MapEventId", L"m_MapEventId",
                            L"EventID", L"m_EventID", L"ID", L"SaveID",
                        };
                        for (const wchar_t* pid : idProps) {
                            mapEventId = GetIntProp(a, pid);
                            if (mapEventId >= 0) break;
                        }
                        std::wstring clsName = a->GetClassPrivate() ? a->GetClassPrivate()->GetName().c_str() : L"?";
                        std::wstring actName = a->GetName().c_str();
                        LOG(R"(STATUE_ID_MAP flag="{}" id={} MAP="{}" class="{}" actor="{}" X={:.2f} Y={:.2f} Z={:.2f} dist={:.1f})",
                            nameStr, mapEventId, GameState::MapName(), clsName, actName,
                            loc.X, loc.Y, loc.Z, std::sqrt(dist2));
                        RecordStatue(GameState::MapName(), actName, loc.X, loc.Y, loc.Z);
                        WriteTemplate();
                    }
                }
            }
        });
    LOG("[StatueSurvey] Statue flag mapping hook registered");
}

void StatueSurvey::WriteTemplate() {
    Json::Value root;
    root["statues"] = Json::Value(Json::objectValue);

    for (auto& [mapName, entries] : s_Statues) {
        Json::Value arr(Json::arrayValue);
        for (auto& e : entries) {
            Json::Value obj;
            std::string idStr;
            idStr.reserve(e.actorName.size());
            for (wchar_t c : e.actorName) idStr.push_back(static_cast<char>(c));
            obj["id"] = idStr;
            obj["x"] = e.x;
            obj["y"] = e.y;
            obj["z"] = e.z;
            arr.append(obj);
        }
        std::string mapKey;
        mapKey.reserve(mapName.size());
        for (wchar_t c : mapName) mapKey.push_back(static_cast<char>(c));
        root["statues"][mapKey] = arr;
    }

    std::ofstream out("statue_template.json");
    if (out) {
        out << root.toStyledString();
        LOG("[StatueSurvey] Wrote statue_template.json ({} maps, total {} entries)",
            (int)s_Statues.size(),
            std::accumulate(s_Statues.begin(), s_Statues.end(), 0,
                [](int n, const auto& kv) { return n + (int)kv.second.size(); }));
    } else {
        WARN("[StatueSurvey] Failed to open statue_template.json for writing");
    }
}

void StatueSurvey::RunSurvey() {
    if (!s_Enabled) {
        LOG("[StatueSurvey] disabled");
        return;
    }
    LOG("[StatueSurvey] === RUN SURVEY (map: {}) ===", GameState::MapName());
    DiscoverClasses();
    ScanAllLoaded();
    LOG("[StatueSurvey] === END SURVEY ===");
}

void StatueSurvey::Tick() {
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

void StatueSurvey::Setup() {
    LOG("[StatueSurvey] Setup");

    SetupStatueFlagMapping();

    GameState::OnMapChanged([](const std::wstring& mapName) {
        LOG("[StatueSurvey] Map changed -> {}", mapName);
        s_FramesSinceMapLoad = 0;
        s_PendingScans = 4;
        RunSurvey();
    });

    LOG("[StatueSurvey] Setup complete (F10 to re-run survey)");
}
