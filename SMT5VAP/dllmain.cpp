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
#include "src/Debug/ShinseiSurvey.hpp"
#include "src/Debug/StatueSurvey.hpp"
#include "src/Hooks/MimanRewardHooks.hpp"
#include "src/Hooks/MissionHooks.hpp"
#include "src/Hooks/AogamiHooks.hpp"
#include "src/Hooks/DevilStatueHooks.hpp"
#include "src/Hooks/NaviDevilHooks.hpp"
#include "src/Hooks/GardenHauntHooks.hpp"
#include "src/Hooks/DemonGiftHooks.hpp"
#include "src/Hooks/SaveHooks.hpp"
#include "src/Hooks/EssenceShopHooks.hpp"
#include "src/Hooks/FusionGating.hpp"
#include "src/Hooks/MiracleHook.hpp"
#include "src/Hooks/AmalgamHooks.hpp"
#include "src/Tick/LevelUpTick.hpp"
#include "src/Tick/CompendiumTick.hpp"
#include "src/Hooks/EventFlagHook.hpp"
#include "src/CustomPopups.hpp"
#include "src/Items/ItemLimits.hpp"
#include "src/Items/ItemGet.hpp"
#include "src/Items/ItemBlocker.hpp"
#include "src/Items/MaccaBlocker.hpp"
#include "src/Items/ExpGive.hpp"
#include "src/Functions/DeathFunctions.hpp"
#include "src/Functions/EventFlags.hpp"
#include "src/Functions/LevelFunctions.hpp"
#include "src/Archipelago/APManager.hpp"
#include "src/Archipelago/APState.hpp"
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
    char m_inputFlagName[256]{""};
    int m_inputFlagId{0};
    int m_inputMapEventId{0};

    static StringType ToWide(const char* s) {
        StringType out;
        if (s) for (; *s; ++s) out.push_back(static_cast<wchar_t>(*s));
        return out;
    }

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

        register_tab(STR("Debug"), [](CppUserModBase* instance) {
            auto mod = dynamic_cast<SMT5VAP*>(instance);
            if (!mod) return;

            ImGui::InputText("Flag Name", mod->m_inputFlagName, IM_ARRAYSIZE(mod->m_inputFlagName));

            StringType flagName = mod->ToWide(mod->m_inputFlagName);
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
            ImGui::InputInt("Flag ID", &mod->m_inputFlagId);
            if (ImGui::Button("Set ID true"))
            {
                EventFlags::Set(mod->m_inputFlagId, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set ID false"))
            {
                EventFlags::Set(mod->m_inputFlagId, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Get ID"))
            {
                bool val = EventFlags::Get(mod->m_inputFlagId);
                LOG("[Debug] Get flag [{}] -> {}", mod->m_inputFlagId, val ? STR("true") : STR("false"));
            }

            ImGui::Separator();
            ImGui::Text("Map Event Flags (BPL_MapEventData)");
            ImGui::InputInt("Map Event ID", &mod->m_inputMapEventId);
            if (ImGui::Button("Set Start true"))
            {
                EventFlags::SetMapEventStart(mod->m_inputMapEventId, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set Start false"))
            {
                EventFlags::SetMapEventStart(mod->m_inputMapEventId, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Is Active?"))
            {
                bool val = EventFlags::IsMapEventActive(mod->m_inputMapEventId);
                LOG("[Debug] Map event {} active -> {}", mod->m_inputMapEventId, val ? STR("true") : STR("false"));
            }
            if (ImGui::Button("Set End true"))
            {
                EventFlags::SetMapEventEnd(mod->m_inputMapEventId, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set End false"))
            {
                EventFlags::SetMapEventEnd(mod->m_inputMapEventId, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set After true"))
            {
                EventFlags::SetMapEventAfter(mod->m_inputMapEventId, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set After false"))
            {
                EventFlags::SetMapEventAfter(mod->m_inputMapEventId, false);
            }
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
        AP::CheckAPConnection();
        CustomPopups::Update();
        if (GameState::IsSaveLoaded() && !GameState::IsTransitioning()) {
            CompendiumTick::Poll();
            LevelUpTick::Poll();
        }

        // For Debugging
        if (GetAsyncKeyState(VK_F4) & 1 && !GameState::IsTransitioning()) {
            ItemGet::GiveItem(110, 1);
            ItemGet::GiveItem(109, 1);
            ItemGet::GiveItem(82, 1);
            ItemGet::GiveItem(83, 1);
            ItemGet::GiveGlory(1);
            ItemGet::GiveMacca(1);
            DEBUG("Sent debug items");
        }
        if (GetAsyncKeyState(VK_F5) & 1) {
            APState::FusionRaces::SetRaceGated(3, false);
            DEBUG("Ungated");
        }
        if (GetAsyncKeyState(VK_F6) & 1 && GameState::IsSaveLoaded()) {
            APState::FusionRaces::SetRaceGated(3, true);
            DEBUG("Gated");
        }
        if (GetAsyncKeyState(VK_F7) & 1) {
            static int NotificationCounter{ 0 };
            CustomPopups::ShowNotificationF("Notification {}", NotificationCounter);
            NotificationCounter++;
        }
        if (GetAsyncKeyState(VK_F8) & 1) {
            DEBUG("Debug Coordinates:");
            GameState::UpdatePosition();
            DEBUG("X: {}, Y: {}, Z: {}", GameState::PosX(), GameState::PosY(), GameState::PosZ());
            DEBUG("Is in haunt: {}", GardenHauntHooks::IsInGardenLevel());
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            ShinseiSurvey::RunSurvey();
        }
        if (GetAsyncKeyState(VK_F10) & 1) {
            StatueSurvey::RunSurvey();
        }
    }

    auto on_unreal_init() -> void override
    {
        Output::set_default_devices<Output::SMT5VAPLogDevice>();
        DEBUG("Mod initializing{}");

        // Debug
        APState::Essences::AddEssence(544);
        APState::Essences::AddEssence(528);
        ItemBlocker::BlockItemId(661);
        APState::FusionRaces::Fill();
        APState::FusionRaces::SetRaceGated(5, false);

        // Hooks
        GameState::SetupMapLoadHook();
        GameState::SetupTransitionHooks();
        GameState::SetupSaveLoadedHook();

        ItemBlocker::Setup();
        ItemBlocker::SetBlockAll(false);

        MaccaBlocker::Setup();
        MaccaBlocker::SetBlockMacca(false);

        ChestHooks::Setup();
        RelicHooks::Setup();

        PopupSuppression::Setup();
        PopupSuppression::SetBlockChests(true);
        PopupSuppression::SetBlockRelics(true);
        PopupSuppression::SetBlockAogamiDebris(true);

        BattleHook::Setup();
        BattleHook::SetSuppressItems(false);
        BattleHook::SetSuppressMitamaItems(false);

        GloryHooks::Setup();
        GloryHooks::SetBlockGlory(true);

        MimanHooks::Setup();
        MimanRewardHooks::Setup();
        ShinseiSurvey::Setup();
        StatueSurvey::Setup();

        AogamiHooks::Setup();
        AogamiHooks::SetReplaceItemId(0);

        DevilStatueHooks::Setup();
        AmalgamHooks::Setup();

        EventFlagHook::Setup();
        EventFlags::Setup();
        LevelFunctions::Setup();

        //NaviDevilHooks::SetupUniqueSaveID();
        NaviDevilHooks::SetupAddCheckCounter();
        NaviDevilHooks::SetupSetGimmickExistFiltered();
        NaviDevilHooks::SetupBlockItems();
        NaviDevilHooks::SetBlockItems(true);
        NaviDevilHooks::SetReplaceMacca(1);
        NaviDevilHooks::SetupNaviDevilChanged();

        GardenHauntHooks::Setup();
        GardenHauntHooks::SetSuppressGifts(true);

        DemonGiftHooks::Setup();
        DemonGiftHooks::SetObserve(true);

        SaveHooks::Setup();

        MissionHooks::Setup();

        CustomPopups::Setup();

        DeathFunctions::Setup();

        EssenceShopHooks::Setup();


        MiracleHook::Setup();
        MiracleHook::SetBlockUnlocks(true);

        // Callbacks
        GameState::OnSaveLoaded([](bool isLoaded) {
            if (isLoaded) {
                LevelUpTick::Reset();
                static bool onceAfterSaveInitialized{ false };
                if (!onceAfterSaveInitialized) {
                    ItemLimits::Raise(255);
                    FusionGating::Setup();

                    onceAfterSaveInitialized = true;
                }
            }
            });

        //Callback tests
        GameState::OnMapChanged([](const std::wstring& MapName) {
            CustomPopups::OnMapChanged();
        });

        ChestHooks::OnChestOpened([](std::int32_t takaraSaveId) {
            ;
        });

        RelicHooks::OnRelicCollected([](std::int32_t relicId) {
            ;
        });

        BattleHook::OnVictory([](int32_t encounterId, int32_t eventEncounterId, bool isBoss, const std::vector<int32_t>& defeatedEnemyIds, const std::vector<int32_t>& encounteredEnemyIds) {
            ;
        });

        BattleHook::OnAllyDowned([](int32_t partyIndex, int32_t newHP, int32_t heroIndex) {
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

        AmalgamHooks::OnShinseiCollected([](std::int32_t shinseiId) {
            ;
        });

        NaviDevilHooks::OnNaviGimmickCollected([](std::int32_t saveId) {
            ;
        });

        NaviDevilHooks::OnNaviDevilChanged([](std::int32_t devilID) {
            ;
        });

        GardenHauntHooks::OnGardenGift([](std::int32_t devilLevel, std::int32_t chosenItemId, std::int32_t chosenItemNum) {
            ;
        });

        DemonGiftHooks::OnDemonGift([](std::int32_t nkmIndex, std::int32_t lvUp) {
            ;
        });

        GardenHauntHooks::OnGardenPowerUp([](std::int32_t nkmIndex) {
            ;
        });

        SaveHooks::OnGameSaved([](int32_t slotIndex, bool isInherit) {
            ;
        });

        LevelUpTick::OnLevelUp([](int32_t oldLevel, int32_t newLevel) {
            ;
        });

        CompendiumTick::OnDemonAcquired([](int32_t devilID) {
            ;
        });

        EventFlagHook::OnFlagSet([](const RC::StringType& flagName, bool newValue) {
            DEBUG("[EventFlag] {} -> {}", flagName, newValue ? STR("true") : STR("false"));
            if (flagName == STR("mis_m064_em2420_4") && newValue) {
                DEBUG("mis_m064_em2420_4 set to true - game won?");
            }
            if (flagName.contains(L"Statue_") && newValue) {
                DEBUG("Giant Devil Statue Checked: {}", flagName);
            }
        });

        EventFlagHook::OnMapEventFlagSet([](int32_t mapEventId, EventFlagHook::MapEventFlagKind kind, bool value) {
            const TCHAR* kindStr = kind == EventFlagHook::MapEventFlagKind::Start ? STR("Start")
                : kind == EventFlagHook::MapEventFlagKind::End ? STR("End") : STR("After");
            DEBUG("[MapEventFlag] event={} {} -> {}", mapEventId, kindStr, value ? STR("true") : STR("false"));
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
