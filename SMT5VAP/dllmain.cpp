#include <cstdint>
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/World.hpp>
#include "Archipelago.h"
#include "src/Log/Log.hpp"
#include "src/Log/SMT5VAPLogDevice.hpp"
#include "src/GameState.hpp"
#include "src/Debug/ShinseiSurvey.hpp"
#include "src/Debug/StatueSurvey.hpp"
#include "src/Debug/DebugUI.hpp"
#include "src/Debug/NoEncounterMode.hpp"
#include "src/Features/Battle/BattleHook.hpp"
#include "src/Features/Battle/DeathFunctions.hpp"
#include "src/Features/Skills/SkillBlocker.hpp"
#include "src/Features/Collections/ChestHooks.hpp"
#include "src/Features/Collections/VendingHooks.hpp"
#include "src/Features/Collections/GloryHooks.hpp"
#include "src/Features/Collections/MimanHooks.hpp"
#include "src/Features/Collections/MimanRewardHooks.hpp"
#include "src/Features/Collections/AogamiHooks.hpp"
#include "src/Features/Collections/DevilStatueHooks.hpp"
#include "src/Features/Collections/AmalgamHooks.hpp"
#include "src/Features/Collections/EventFlagHook.hpp"
#include "src/Features/Fusion/FusionGating.hpp"
#include "src/Features/Fusion/FusionUnlock.hpp"
#include "src/Features/Items/ItemBlocker.hpp"
#include "src/Features/Items/ItemGet.hpp"
#include "src/Features/Items/ItemLimits.hpp"
#include "src/Features/Items/ItemTableInjector.hpp"
#include "src/Features/Items/MaccaBlocker.hpp"
#include "src/Features/Items/ExpGive.hpp"
#include "src/Features/Items/UseItemHook.hpp"
#include "src/Features/Party/LevelUpHook.hpp"
#include "src/Features/Party/CompendiumTick.hpp"
#include "src/Features/Party/LevelFunctions.hpp"
#include "src/Features/Collections/NaviDevilHooks.hpp"
#include "src/Features/Party/GardenHauntHooks.hpp"
#include "src/Features/Party/DemonGiftHooks.hpp"
#include "src/Features/Progression/MissionHooks.hpp"
#include "src/Features/Progression/MissionRewardHook.hpp"
#include "src/Features/Progression/MissionScoutManager.hpp"
#include "src/Features/Progression/EventFlags.hpp"
#include "src/Features/Save/SaveHooks.hpp"
#include "src/Features/Shop/EssenceShopHooks.hpp"
#include "src/Features/Shop/MiracleHooks.hpp"
#include "src/Features/UI/PopupSuppression.hpp"
#include "src/Features/UI/ItemWindow.hpp"
#include "src/Features/UI/InfoWindow.hpp"
#include "src/Archipelago/APUI.hpp"
#include "src/Archipelago/APManager.hpp"
#include "src/Archipelago/APState.hpp"
#include <UE4SSProgram.hpp>
#include <Windows.h>

using namespace RC;
using namespace RC::Unreal;

class SMT5VAP : public RC::CppUserModBase
{
public:
    SMT5VAP() : CppUserModBase()
    {
        ModName = STR("SMT5VAP");
        ModVersion = STR("0.1");
        ModDescription = STR("An Archipelago integration mod for Shin Megami Tensei V:Vengeance");
        ModAuthors = STR("Poblin");

        register_tab(STR("Archipelago"), RenderAPTab);

        register_tab(STR("Debug"), RenderDebugTab);
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
        ItemWindow::Update();
        InfoWindow::Update();

        if (GameState::IsSaveLoaded() && !GameState::IsTransitioning()) {
            CompendiumTick::Poll();
        }
        // Debug tools
        if (GetAsyncKeyState(VK_F4) & 1 && !GameState::IsTransitioning()) {
            ItemGet::GiveItem(110, 1);
            ItemGet::GiveItem(109, 1);
            ItemGet::GiveItem(82, 1);
            ItemGet::GiveItem(83, 1);
            ItemGet::GiveGlory(1);
            ItemGet::GiveMacca(100000);
            DEBUG("Sent debug items");
        }
        if (GetAsyncKeyState(VK_F5) & 1) {
            MiracleHooks::UnlockForPurchase(11);
            MiracleHooks::UnlockForPurchase(60);
            MiracleHooks::GrantMiracle(60);

        }
        if (GetAsyncKeyState(VK_F7) & 1) {
            static int NotificationCounter{ 0 };
            InfoWindow::ShowNotificationF("Notification {}", NotificationCounter);
            NotificationCounter++;
        }
        if (GetAsyncKeyState(VK_F8) & 1) {
            
            DEBUG("Debug Coordinates:");
            GameState::UpdatePosition();
            DEBUG("X: {}, Y: {}, Z: {}", GameState::PosX(), GameState::PosY(), GameState::PosZ());
            DEBUG("Is in haunt: {}", GardenHauntHooks::IsInGardenLevel());
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            static int ItemCounter{ 0 };
            ItemWindow::ShowItemPopupCustom(1, L"Something obtained");
            ItemCounter++;
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
        FusionGating::SetEnabled(false);
        APState::FusionRaces::SetRaceUnlocked(5, true);
        APState::FusionRaces::SetRaceUnlocked(3, true);
        FusionGating::SetEssenceGatingEnabled(false);
        APState::SkillCategories::SetCategoryBlocked(0, true);

        // Hooks
        GameState::SetupMapLoadHook();
        GameState::SetupTransitionHooks();
        GameState::SetupSaveLoadedHook();

        ItemBlocker::Setup();
        ItemBlocker::SetBlockAll(false);

        MaccaBlocker::Setup();
        MaccaBlocker::SetBlockMacca(false);

        ChestHooks::Setup();
        ChestHooks::SetEmptyAllChests(true);
        ChestHooks::SetReplacementItem(70);
        ChestHooks::SetReplacementAmount(0);
        ChestHooks::SetReplacementMacca(0);
        ChestHooks::SetExcludedChests({ 29, 76, 77, 78, 79, 142, 231, 232, 233, 254 });

        VendingHooks::Setup();
        VendingHooks::SetReplacement(70, 0, 100);


        PopupSuppression::Setup();
        PopupSuppression::SetBlockChests(false);
        PopupSuppression::SetBlockRelics(false);
        PopupSuppression::SetBlockAogamiDebris(true);

        BattleHook::Setup();
        BattleHook::SetSuppressItems(false);
        BattleHook::SetSuppressMitamaItems(false);


        GloryHooks::Setup();
        GloryHooks::SetBlockGlory(true);

        MimanHooks::Setup();
        MimanRewardHooks::Setup();
        MimanRewardHooks::SetBlocking(false);
        MimanRewardHooks::SetCustomText(1, L"Mothman Plushie");
        MimanRewardHooks::SetCustomText(0, L"Fortnite Battle Pass");
        MimanRewardHooks::SetCustomText(8, L"Gustave's Sloppy Toppy");


        AogamiHooks::Setup();
        AogamiHooks::SetReplaceItemId(0);

        DevilStatueHooks::Setup();
        AmalgamHooks::Setup();
        UseItemHook::Setup();

        EventFlagHook::Setup();
        EventFlags::Setup();
        LevelFunctions::Setup();
        LevelUpTick::Setup();

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

        // Must be BEFORE save callbacks that trigger MakeUpItemDataTable
        ItemTableInjector::Setup();

        MissionHooks::Setup();
        MissionRewardHook::Setup();
        MissionScoutManager::Setup();
        MissionRewardHook::SetMode(MissionRewardHook::FilterMode::Disabled);
        MissionRewardHook::SetCustomRewardText(6, L"Super Reward");
        MissionRewardHook::SetCustomRewardText(85, L"Good Reward");
        MissionRewardHook::SetCustomRewardText(107, L"Mega Reward");

        ItemWindow::Setup();
        InfoWindow::Setup();

        DeathFunctions::Setup();

        NoEncounterMode::Setup();

        EssenceShopHooks::Setup();

        MiracleHooks::Setup();
        MiracleHooks::SetBlockUnlocks(true);

        FusionUnlock::Setup();

        // Callbacks
        GameState::OnSaveLoaded([](bool isLoaded) {
            if (isLoaded) {
                LevelUpTick::Reset();
                static bool onceAfterSaveInitialized{ false };
                if (!onceAfterSaveInitialized) {
                    ItemLimits::Raise(255);
                    MissionScoutManager::Rescan();
                    FusionGating::Setup();
                    SkillBlocker::Setup();

                    SkillBlocker::BuildCache();

                    onceAfterSaveInitialized = true;
                }
            }
            });

        GameState::OnMapChanged([](const std::wstring& MapName) {
            InfoWindow::OnMapChanged();
            ItemWindow::OnMapChanged();
        });

        ChestHooks::OnChestOpened([](std::int32_t takaraSaveId) {
            ;
        });

        VendingHooks::OnVendingCollected([](std::int32_t relicId) {
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

        UseItemHook::OnItemUsed([](std::int32_t itemId) {
            if (itemId == 109) {
                LOG("[ItemUse] Small Glory Crystal -> GiveGlory(10)");
                ItemGet::GiveGlory(10);
            } else if (itemId == 110) {
                LOG("[ItemUse] Large Glory Crystal -> GiveGlory(100)");
                ItemGet::GiveGlory(100);
            }
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

        MissionScoutManager::OnMissionsLoaded([](const std::vector<int32_t>& ids) {
            ; 
        });

        MissionScoutManager::OnMissionAdded([](int32_t id) {
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
