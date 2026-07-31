#include "APUI.hpp"
#include "APManager.hpp"
#include "Archipelago.h"
#include <format>

using namespace RC;

static char s_inputIP[256]{};
static char s_inputSlotName[256]{};
static char s_inputPassword[256]{""};

void RenderAPTab(CppUserModBase* instance)
{
    ImGui::BeginChild("APTabScroll", ImVec2(0, 0), ImGuiChildFlags_None);

    ImGui::InputText("Address and Port", s_inputIP, IM_ARRAYSIZE(s_inputIP));
    ImGui::InputText("Slot Name (Player)", s_inputSlotName, IM_ARRAYSIZE(s_inputSlotName));
    ImGui::InputText("Password", s_inputPassword, IM_ARRAYSIZE(s_inputPassword));

    if (ImGui::Button("Connect"))
    {
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
