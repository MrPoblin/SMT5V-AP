#include "ChestHooks.hpp"
#include "PopupSuppression.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace ChestHooks {
    static CallbackId s_SetTakaraFlagHookId{-1};
    static std::vector<ChestOpenCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetTakaraFlag"));
        if (!Func) {
            WARN("[ChestHooks] Failed to find SetTakaraFlag");
            return;
        }
        LOG("[ChestHooks] Found SetTakaraFlag");

        // Get param properties for logging
        FProperty* IdProp = Func->GetPropertyByName(STR("ID"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

        s_SetTakaraFlagHookId = Func->RegisterPostHook(
            [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                // Extract the ID parameter from the stack locals
                int32 chestId = -1;
                bool flag = false;
                if (IdProp) {
                    auto* IdPtr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                    if (IdPtr) chestId = *IdPtr;
                }
                if (FlagProp) {
                    auto* FlagPtr = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                    if (FlagPtr) flag = *FlagPtr;
                }

                if (flag && chestId >= 0) {
                    LOG("[Chest] Flag set: chest save ID={}", chestId);

                    if (PopupSuppression::IsBlockingChests()) {
                        PopupSuppression::SetBlockNextSpawn(true);
                    }

                    // Notify registered callbacks
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) {
                        cb(chestId);
                    }
                }
            }
        );
        LOG("[ChestHooks] SetTakaraFlag post-hook registered (id={})", s_SetTakaraFlagHookId);
    }

    void OnChestOpened(ChestOpenCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
