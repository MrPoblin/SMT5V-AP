#include <cstdint>
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/World.hpp>
#include "src/Log/Log.hpp"
#include "src/Log/SMT5VAPLogDevice.hpp"
#include "src/GameState.hpp"
#include "src/Hooks/PopupSuppression.hpp"
#include "src/Hooks/ChestHooks.hpp"
#include "src/Hooks/RelicHooks.hpp"
#include "src/Hooks/BattleHook.hpp"
#include "src/CustomPopups.hpp"
#include "src/Items/ItemLimits.hpp"
#include "src/Items/ItemGet.hpp"
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
    }

    ~SMT5VAP() override
    {
    }

    auto on_update() -> void override
    {
        GameState::Update();

        if (GetAsyncKeyState(VK_F4) & 1) {
            GiveItem(1, 1);
            DEBUG("Sent item ID 1");
        }
    }

    auto on_unreal_init() -> void override
    {
        Output::set_default_devices<Output::SMT5VAPLogDevice>();
        DEBUG("Mod initializing");

        // Hooks
        PopupSuppression::Setup();
        ChestHooks::Setup();
        RelicHooks::Setup();
        BattleHook::Setup();
        BattleHook::SetSuppressItems(true);
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
                    ItemLimits::Raise(255);
                    afterSaveInitialized = true;
                }
            }
            else LOG("Save unloaded");
            });
        ChestHooks::OnChestOpened([](std::int32_t takaraSaveId) {
            LOG("[Chest] Opened: save ID={}", takaraSaveId);
            CustomPopups::ShowNotification(STR("Item received from Archipelago!"));
        });
        RelicHooks::OnRelicCollected([](std::int32_t relicId) {
            LOG("[Relic] Collected: ID={}", relicId);
            CustomPopups::ShowNotification(STR("Item received from Archipelago!"));
        });
        BattleHook::OnVictory([](int32_t encounterId, int32_t eventEncounterId, bool isBoss) {
            LOG("[BattleHook] Callback: enc={}, evtEnc={}, boss={}", encounterId, eventEncounterId, isBoss);
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
