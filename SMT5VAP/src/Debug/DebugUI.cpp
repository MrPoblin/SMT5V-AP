#include "DebugUI.hpp"
#include "src/Log/Log.hpp"
#include "src/Debug/NoEncounterMode.hpp"
#include "src/Features/Progression/EventFlags.hpp"
#include "src/Features/Progression/MissionRewardHook.hpp"
#include <format>

using namespace RC;

static char s_inputFlagName[256]{};
static int s_inputFlagId{0};
static int s_inputMapEventId{0};
static bool s_noEncounter{false};

static StringType ToWide(const char* s) {
    StringType out;
    if (s) for (; *s; ++s) out.push_back(static_cast<wchar_t>(*s));
    return out;
}

void RenderDebugTab(CppUserModBase* instance)
{
    ImGui::InputText("Flag Name", s_inputFlagName, IM_ARRAYSIZE(s_inputFlagName));

    StringType flagName = ToWide(s_inputFlagName);
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
}
