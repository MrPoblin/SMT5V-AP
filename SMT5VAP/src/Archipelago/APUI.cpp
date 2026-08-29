#include "APUI.hpp"
#include "APManager.hpp"
#include "Archipelago.h"
#include <format>
#include <cstring>

using namespace RC;

static char s_inputIP[256]{};
static char s_inputSlotName[256]{};
static char s_inputPassword[256]{""};

void RenderAPTab(CppUserModBase* instance)
{
    static bool s_initialized{ false };
    if (!s_initialized) {
        s_initialized = true;
        APManager::APConfig cfg;
        if (APManager::LoadConfig(cfg)) {
            strncpy_s(s_inputIP, cfg.IP.c_str(), _TRUNCATE);
            strncpy_s(s_inputSlotName, cfg.SlotName.c_str(), _TRUNCATE);
            strncpy_s(s_inputPassword, cfg.Password.c_str(), _TRUNCATE);
        }
    }

    ImGui::BeginChild("APTabScroll", ImVec2(0, 0), ImGuiChildFlags_None);

    ImGui::InputText("Address and Port", s_inputIP, IM_ARRAYSIZE(s_inputIP));
    ImGui::InputText("Slot Name (Player)", s_inputSlotName, IM_ARRAYSIZE(s_inputSlotName));
    ImGui::InputText("Password", s_inputPassword, IM_ARRAYSIZE(s_inputPassword));

    if (ImGui::Button("Connect"))
    {
        APManager::SaveConfig({ s_inputIP, s_inputSlotName, s_inputPassword });
        APManager::APInitialize(s_inputIP, s_inputSlotName, s_inputPassword);
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect"))
    {
        APManager::Shutdown();
    }
    ImGui::Text(std::format("AP Status: {}", APManager::getAPConnectedStatus()).c_str());

    ImGui::EndChild();
}
