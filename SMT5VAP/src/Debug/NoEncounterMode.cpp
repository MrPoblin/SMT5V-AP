#include "NoEncounterMode.hpp"
#include "src/Helper/HookHelper.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// AMapCommonCtrl_C layout (from dumped headers):
//   bool EncontOff      at +0x0410
//   bool ForceEncountOFF at +0x0411
static PropertyField<bool> s_EncontOff(0x0410);
static PropertyField<bool> s_ForceEncountOff(0x0411);

void NoEncounterMode::SetEnabled(bool enabled) {
    if (enabled == s_Enabled) return;
    s_Enabled = enabled;
    if (enabled) {
        LOG("[NoEncounterMode] ENABLED");
        s_LastApplyTime = std::chrono::steady_clock::time_point{};
        Apply();
    } else {
        LOG("[NoEncounterMode] DISABLED");
    }
}

void NoEncounterMode::Apply() {
    std::vector<UObject*> ctrls;
    UObjectGlobals::FindAllOf(L"MapCommonCtrl_C", ctrls);
    if (ctrls.empty()) return;
    for (auto* ctrl : ctrls) {
        if (!ctrl) continue;
        s_EncontOff.Set(ctrl, true);
        s_ForceEncountOff.Set(ctrl, true);
    }
}

void NoEncounterMode::Tick() {
    if (!s_Enabled) return;
    auto now = std::chrono::steady_clock::now();
    if (now - s_LastApplyTime < std::chrono::seconds(3)) return;
    s_LastApplyTime = now;
    Apply();
}

void NoEncounterMode::Setup() {
    GameState::OnMapChanged([](const std::wstring&) {
        if (s_Enabled) {
            s_LastApplyTime = std::chrono::steady_clock::time_point{};
            Apply();
        }
    });
    LOG("[NoEncounterMode] Setup – disables all field encounters");
}
