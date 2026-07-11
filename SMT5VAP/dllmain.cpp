#include <cstdint>
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/World.hpp>
#include "Archipelago.h"
#include "src/Log/Log.hpp"
#include "src/Log/SMT5VAPLogDevice.hpp"
#include "src/GameState.hpp"
#include "src/Hooks/PopupSuppression.hpp"
#include "src/Hooks/ChestHooks.hpp"
#include "src/Hooks/RelicHooks.hpp"
#include "src/Hooks/GloryHooks.hpp"
#include "src/Hooks/BattleHook.hpp"
#include "src/Hooks/MimanHooks.hpp"
#include "src/Hooks/MimanRewardHooks.hpp"
#include "src/Hooks/MissionHooks.hpp"
#include "src/Hooks/AogamiHooks.hpp"
#include "src/Hooks/DevilStatueHooks.hpp"
#include "src/Hooks/NaviDevilHooks.hpp"
#include "src/Hooks/GardenHauntHooks.hpp"
#include "src/CustomPopups.hpp"
#include "src/Items/ItemLimits.hpp"
#include "src/Items/ItemGet.hpp"
#include "src/Items/ItemBlocker.hpp"
#include "src/Archipelago/APManager.hpp"
#include <UE4SSProgram.hpp>
#include <Windows.h>
#include <format>

using namespace RC;
using namespace RC::Unreal;

class SMT5VAP : public RC::CppUserModBase
{
private:
    char m_inputIP[256]{};
    char m_inputSlotName[256]{};
    char m_inputPassword[256]{""};

public:
    SMT5VAP() : CppUserModBase()
    {
        ModName = STR("SMT5VAP");
        ModVersion = STR("0.1");
        ModDescription = STR("An Archipelago integration mod for Shin Megami Tensei V:Vengeance");
        ModAuthors = STR("Poblin");

        register_tab(STR("Archipelago"), [](CppUserModBase* instance) {
            auto mod = dynamic_cast<SMT5VAP*>(instance);
            if (!mod) return;

            ImGui::InputText("Address and Port", mod->m_inputIP, IM_ARRAYSIZE(mod->m_inputIP));
            ImGui::InputText("Slot Name (Player)", mod->m_inputSlotName, IM_ARRAYSIZE(mod->m_inputSlotName));
            ImGui::InputText("Password", mod->m_inputPassword, IM_ARRAYSIZE(mod->m_inputPassword));

            if (ImGui::Button("Connect"))
            {
                AP::APInitialize(mod->m_inputIP, mod->m_inputSlotName, mod->m_inputPassword);
            }
            ImGui::SameLine();
            if (ImGui::Button("Disconnect"))
            {
                AP::Shutdown();
            }
            ImGui::Text(std::format("AP Status: {}", AP::getAPConnectedStatus()).c_str());
            });
    }

    ~SMT5VAP() override
    {
    }

    auto on_ui_init() -> void override
    {
        UE4SS_ENABLE_IMGUI()
    }

    auto on_update() -> void override
    {
        GameState::Update();
        AP::CheckAPConnection();

        // For Debugging
        if (GetAsyncKeyState(VK_F4) & 1) {
            ItemGet::GiveItem(110, 1);
            ItemGet::GiveItem(109, 1);
            DEBUG("Sent debug items");
        }
        if (GetAsyncKeyState(VK_F5) & 1) {
            ItemGet::GiveGlory(1);
            ItemGet::GiveMacca(1);
            DEBUG("Sent debug stuff");
        }
    }

    auto on_unreal_init() -> void override
    {
        Output::set_default_devices<Output::SMT5VAPLogDevice>();
        DEBUG("Mod initializing{}");

        ItemBlocker::Setup();
        ItemBlocker::SetBlockAll(false);

        // Hooks
        ChestHooks::Setup();
        RelicHooks::Setup();

        PopupSuppression::Setup();
        PopupSuppression::SetBlockChests(true);
        PopupSuppression::SetBlockRelics(true);
        PopupSuppression::SetBlockAogamiDebris(true);

        BattleHook::Setup();
        BattleHook::SetSuppressItems(true);

        GloryHooks::Setup();
        GloryHooks::SetBlockGlory(true);

        MimanHooks::Setup();
        MimanRewardHooks::Setup();

        AogamiHooks::Setup();
        AogamiHooks::SetReplaceItemId(0); 

        DevilStatueHooks::Setup();

        //NaviDevilHooks::SetupUniqueSaveID();
        NaviDevilHooks::SetupAddCheckCounter();
        NaviDevilHooks::SetupSetGimmickExistFiltered();
        NaviDevilHooks::SetupBlockItems();
        NaviDevilHooks::SetBlockItems(true);      // false = passthrough (no changes)
        NaviDevilHooks::SetReplaceMacca(1);       // 0 = suppress, >0 = give N macca instead


        GardenHauntHooks::Setup();

        MissionHooks::Setup();

        CustomPopups::Setup();

        GameState::SetupSaveLoadedHook();


        // Callbacks
        GameState::OnWorldChanged([](UWorld* World) {
            if (World) LOG("World created");
            else LOG("World destroyed");
            });
        GameState::OnMapChanged([](const std::wstring& MapName) {LOG("Map changed: {}", MapName);});
        GameState::OnSaveLoaded([](bool isLoaded) {
            if (isLoaded) {
                LOG("Save loaded");
                static bool afterSaveInitialized{ false };
                if (!afterSaveInitialized) {
                    // Still need to do this on new game started as well
                    ItemLimits::Raise(255);
                    afterSaveInitialized = true;
                }
            }
            else LOG("Save unloaded");
            });

        //Callback tests
        ChestHooks::OnChestOpened([](std::int32_t takaraSaveId) {
            ;
        });

        RelicHooks::OnRelicCollected([](std::int32_t relicId) {
            ;
        });

        BattleHook::OnVictory([](int32_t encounterId, int32_t eventEncounterId, bool isBoss) {
            ;
        });

        GloryHooks::OnGloryCollected([](std::int32_t gloryAmount) {
            ;
        });

        MimanHooks::OnMimanFound([](std::int32_t mimanId) {
            ;
        });

        MimanRewardHooks::OnMimanRewardClaimed([](std::int32_t rewardId) {
            ;
        });

        AogamiHooks::OnAogamiDebrisCollected([](std::int32_t tableIndex) {
            ;
        });

        DevilStatueHooks::OnDevilStatueCollected([](const RC::Unreal::FName& flagName) {
            ;
        });

        NaviDevilHooks::OnNaviGimmickCollected([](std::int32_t saveId) {
            ;
        });

        GardenHauntHooks::OnGardenGift([](std::int32_t devilLevel, std::int32_t chosenItemId, std::int32_t chosenItemNum) {
            ;
        });

        GardenHauntHooks::OnGardenPowerUp([](std::int32_t nkmIndex) {
            ;
        });

        LOG("Mod initialized");
    }
};

#define SMT5VAP_MOD_API __declspec(dllexport)
extern "C"
{
    SMT5VAP_MOD_API RC::CppUserModBase* start_mod()
    {
        return new SMT5VAP();
    }

    SMT5VAP_MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
