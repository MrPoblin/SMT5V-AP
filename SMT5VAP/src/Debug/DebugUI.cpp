#include "DebugUI.hpp"
#include "src/Log/Log.hpp"
#include "src/Debug/NoEncounterMode.hpp"
#include "src/Debug/RegionSurvey.hpp"
#include "src/Features/Progression/EventFlags.hpp"
#include "src/Features/Progression/MissionRewardHook.hpp"
#include "src/Features/Collections/InspectionPointHooks.hpp"
#include "src/Archipelago/APState.hpp"
#include "src/Archipelago/ItemSync.hpp"
#include "src/Features/Save/SaveHooks.hpp"
#include "src/GameState.hpp"
#include "src/Features/Party/LevelUpHook.hpp"
#include "src/Features/Battle/DeathFunctions.hpp"
#include "src/Features/Battle/SummonBattle.hpp"
#include "src/Helper/StringHelper.hpp"
#include <format>

using namespace RC;

static char s_inputFlagName[256]{};
static int s_inputFlagId{0};
static int s_inputMapEventId{0};
static bool s_noEncounter{false};
static bool s_alwaysShow{false};
static bool s_onlyOnce{false};
static int s_summonEncID{0};
static int s_summonEnemyID{1};

void RenderDebugTab(CppUserModBase* instance)
{
    ImGui::BeginChild("APTabScroll", ImVec2(0, 0), ImGuiChildFlags_None);

    ImGui::InputText("Flag Name", s_inputFlagName, IM_ARRAYSIZE(s_inputFlagName));

    StringType flagName = StringHelper::ToWide(s_inputFlagName);
    if (ImGui::Button("Set true"))
    {
        EventFlags::Set(flagName, true);
        LOG("[Debug] Set flag {} -> true", flagName);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set false"))
    {
        EventFlags::Set(flagName, false);
        LOG("[Debug] Set flag {} -> false", flagName);
    }
    ImGui::SameLine();
    if (ImGui::Button("Get"))
    {
        bool val = EventFlags::Get(flagName);
        LOG("[Debug] Get flag {} -> {}", flagName, val ? STR("true") : STR("false"));
    }

    ImGui::Separator();
    ImGui::InputInt("Flag ID", &s_inputFlagId);
    if (ImGui::Button("Set ID true"))
    {
        EventFlags::Set(s_inputFlagId, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set ID false"))
    {
        EventFlags::Set(s_inputFlagId, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Get ID"))
    {
        bool val = EventFlags::Get(s_inputFlagId);
        LOG("[Debug] Get flag [{}] -> {}", s_inputFlagId, val ? STR("true") : STR("false"));
    }

    ImGui::Separator();
    if (ImGui::Checkbox("No Encounter Mode", &s_noEncounter)) {
        NoEncounterMode::SetEnabled(s_noEncounter);
    }
    ImGui::Separator();
    ImGui::Text("Map Event Flags (BPL_MapEventData)");
    ImGui::InputInt("Map Event ID", &s_inputMapEventId);
    if (ImGui::Button("Set Start true"))
    {
        EventFlags::SetMapEventStart(s_inputMapEventId, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set Start false"))
    {
        EventFlags::SetMapEventStart(s_inputMapEventId, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Is Active?"))
    {
        bool val = EventFlags::IsMapEventActive(s_inputMapEventId);
        LOG("[Debug] Map event {} active -> {}", s_inputMapEventId, val ? STR("true") : STR("false"));
    }
    if (ImGui::Button("Set End true"))
    {
        EventFlags::SetMapEventEnd(s_inputMapEventId, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set End false"))
    {
        EventFlags::SetMapEventEnd(s_inputMapEventId, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set After true"))
    {
        EventFlags::SetMapEventAfter(s_inputMapEventId, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set After false"))
    {
        EventFlags::SetMapEventAfter(s_inputMapEventId, false);
    }

    ImGui::Separator();
    ImGui::Text("Mission Reward Filter");
    {
        using Mode = MissionRewardHook::FilterMode;
        auto mode = MissionRewardHook::GetMode();
        int modeInt = static_cast<int>(mode);
        if (ImGui::RadioButton("Disabled", modeInt == 0)) { MissionRewardHook::SetMode(Mode::Disabled); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Block All", modeInt == 1)) { MissionRewardHook::SetMode(Mode::BlockAll); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Keep Exp", modeInt == 2)) { MissionRewardHook::SetMode(Mode::KeepExp); }
    }
    {
        static int s_exceptId = 0;
        ImGui::InputInt("Mission ID##except", &s_exceptId);
        if (ImGui::Button("Add Exception")) {
            MissionRewardHook::AddException(s_exceptId);
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove Exception")) {
            MissionRewardHook::RemoveException(s_exceptId);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All Exceptions")) {
            MissionRewardHook::ClearExceptions();
        }
    }

    ImGui::Separator();
    ImGui::Text("Skill Category Blocker");

    // Category toggles
    static const char* categoryNames[] = {
        "Physical", "Fire", "Ice", "Electric", "Force",
        "Light", "Dark", "Almighty", "Ailment", "Support", "Recovery"
    };

    for (int i = 0; i < APState::SkillCategories::CATEGORY_COUNT; i++) {
        bool blocked = APState::SkillCategories::IsCategoryBlocked(i);
        if (ImGui::Checkbox(categoryNames[i], &blocked)) {
            APState::SkillCategories::SetCategoryBlocked(i, blocked);
        }
        ImGui::SameLine();
    }

    // Scope selector
    ImGui::Spacing();
    auto scope = APState::SkillCategories::GetScope();
    int scopeInt = static_cast<int>(scope);
    ImGui::Text("Scope:");
    if (ImGui::RadioButton("Both##skill", scopeInt == 0)) {
        APState::SkillCategories::SetScope(APState::SkillCategories::Scope::Both);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Protag Only##skill", scopeInt == 1)) {
        APState::SkillCategories::SetScope(APState::SkillCategories::Scope::ProtagOnly);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Demon Only##skill", scopeInt == 2)) {
        APState::SkillCategories::SetScope(APState::SkillCategories::Scope::DemonOnly);
    }

    // Quick actions
    ImGui::Spacing();
    if (ImGui::Button("Block All##skill")) {
        for (int i = 0; i < APState::SkillCategories::CATEGORY_COUNT; i++) {
            APState::SkillCategories::SetCategoryBlocked(i, true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Unblock All##skill")) {
        APState::SkillCategories::Clear();
    }

    // Status display
    int blockedCount = 0;
    for (int i = 0; i < APState::SkillCategories::CATEGORY_COUNT; i++) {
        if (APState::SkillCategories::IsCategoryBlocked(i)) blockedCount++;
    }
    ImGui::Text("Blocked categories: %d / %d", blockedCount, APState::SkillCategories::CATEGORY_COUNT);

    // Inspection Points
    ImGui::Separator();
    ImGui::Text("Inspection Points");
    if (ImGui::Checkbox("Always Show All", &s_alwaysShow)) {
        InspectionPointHooks::SetAlwaysShow(s_alwaysShow);
        LOG("[Debug] AlwaysShow={}", s_alwaysShow);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Only Once", &s_onlyOnce)) {
        InspectionPointHooks::SetOnlyOnce(s_onlyOnce);
        LOG("[Debug] OnlyOnce={}", s_onlyOnce);
    }
    ImGui::Text("Triggered: %zu (current map %zu), MapId: %d",
        InspectionPointHooks::TriggeredCount(),
        InspectionPointHooks::TriggeredCount(InspectionPointHooks::CurrentMapId()),
        InspectionPointHooks::CurrentMapId());
    if (ImGui::Button("Clear Triggered")) {
        InspectionPointHooks::ClearTriggered();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Spots")) {
        InspectionPointHooks::RefreshSpotVisibility();
        LOG("[Debug] Refresh Spots");
    }

    ImGui::Separator();
    ImGui::Text("Region Survey");
    if (ImGui::Button("Dump Map Regions")) {
        RegionSurvey::RunSurvey();
        LOG("[Debug] Region survey run");
    }

    ImGui::Separator();
    ImGui::Text("Level Up (LevelUpTick)");
    ImGui::Text("Current cached level: %d", LevelUpTick::GetCachedLevel());
    int lastOld = LevelUpTick::LastLevelUpOld();
    int lastNew = LevelUpTick::LastLevelUpNew();
    if (lastOld > 0 && lastNew > 0) {
        ImGui::Text("Last level up: %d -> %d (gained %d)", lastOld, lastNew, lastNew - lastOld);
    } else {
        ImGui::Text("Last level up: none yet");
    }

    ImGui::Separator();
    ImGui::Text("ItemSync (save/regrant)");
    ImGui::Text("InResync: %s | Save loaded: %s | Save pending: %s | Queued grants: %zu",
        ItemSync::IsInResync() ? "yes" : "no",
        ItemSync::IsSaveLoaded() ? "yes" : "no",
        SaveHooks::IsSavePending() ? "yes" : "no",
        ItemSync::QueuedGrantCount());
    {
        static int s_confirmItemId = 0;
        static int s_confirmCount = 1;
        ImGui::InputInt("Item ID##confirm", &s_confirmItemId);
        ImGui::InputInt("Count##confirm", &s_confirmCount);
        if (ImGui::Button("Simulate receive")) ItemSync::DebugSimulateReceive(s_confirmItemId, s_confirmCount);
        ImGui::SameLine();
        if (ImGui::Button("Simulate refire")) ItemSync::DebugSimulateRefire(s_confirmItemId, s_confirmCount);
        ImGui::SameLine();
        if (ImGui::Button("Simulate death")) ItemSync::DebugSimulateDeath();
    }
    if (ImGui::Button("Start resync")) ItemSync::DebugStartResync();
    ImGui::SameLine();
    if (ImGui::Button("End resync")) ItemSync::DebugEndResync();
    ImGui::SameLine();
    if (ImGui::Button("Force confirm")) ItemSync::DebugForceConfirm();

    {
        ImGui::Text("PendingGrant:");
        auto pending = ItemSync::GetPendingList();
        for (auto& [id, count] : pending) {
            ImGui::Text("  %lld x %u", static_cast<long long>(id), count);
        }
        ImGui::Text("GrantedInWorld:");
        auto inWorld = ItemSync::GetInWorldList();
        for (auto& [id, count] : inWorld) {
            ImGui::Text("  %lld x %u", static_cast<long long>(id), count);
        }
    }

    ImGui::Separator();
    ImGui::Text("Summon Battle (spawn only — does not touch the resulting fight)");
    ImGui::InputInt("Encount ID##summon", &s_summonEncID);
    ImGui::InputInt("Enemy Devil ID (0 = use table row)##summon", &s_summonEnemyID);
    if (ImGui::Button("Summon")) {
        if (s_summonEnemyID > 0) {
            std::vector<int32_t> enemies{ s_summonEnemyID };
            SummonBattle::Summon(s_summonEncID, enemies);
        } else {
            // Empty enemy list = use the encount table row's
            // m_EnemyIDArray verbatim. Pass a dummy single-element
            // vector of 0 only if you want to force an explicit list;
            // for "spawn whatever this row says" just call with {}.
            SummonBattle::Summon(s_summonEncID, {});
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Summon + force game over")) {
        if (s_summonEnemyID > 0) {
            std::vector<int32_t> enemies{ s_summonEnemyID };
            SummonBattle::Summon(s_summonEncID, enemies);
        } else {
            SummonBattle::Summon(s_summonEncID, {});
        }
        DeathFunctions::KillLocalPlayer();
    }
    if (SummonBattle::IsActive()) {
        ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "Last summon dispatched (engine owns the encounter now)");
    }

    ImGui::EndChild();

}
