#include "RelicHooks.hpp"
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

namespace RelicHooks {
    static CallbackId s_SetRelicFlagHookId{-1};
    static CallbackId s_SetRelicOnceFlagHookId{-1};
    static std::vector<RelicCollectCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        // ── Hook SetRelicFlag (collected relic flags) ──
        auto* FlagFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetRelicFlag"));
        if (!FlagFunc) {
            WARN("[RelicHooks] Failed to find SetRelicFlag");
        } else {
            FProperty* IdProp = FlagFunc->GetPropertyByName(STR("ID"));
            FProperty* FlagProp = FlagFunc->GetPropertyByName(STR("flag"));

            s_SetRelicFlagHookId = FlagFunc->RegisterPostHook(
                [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 relicId = -1;
                    bool flag = false;
                    if (IdProp) {
                        auto* IdPtr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                        if (IdPtr) relicId = *IdPtr;
                    }
                    if (FlagProp) {
                        auto* FlagPtr = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                        if (FlagPtr) flag = *FlagPtr;
                    }

                    if (flag && relicId >= 0) {
                        LOG("[Relic] Flag set: relic ID={}", relicId);
                        if (PopupSuppression::IsBlockingRelics()) {
                            PopupSuppression::SetBlockNextSpawn(true);
                        }

                        std::lock_guard<std::mutex> lock(s_Mutex);
                        for (auto& cb : s_Callbacks) {
                            cb(relicId);
                        }
                    }
                }
            );
            LOG("[RelicHooks] SetRelicFlag post-hook registered (id={})", s_SetRelicFlagHookId);
        }

        // ── Hook SetRelicOnceFlag (one-time relics) ──
        auto* OnceFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetRelicOnceFlag"));
        if (!OnceFunc) {
            WARN("[RelicHooks] Failed to find SetRelicOnceFlag");
        } else {
            FProperty* IdProp = OnceFunc->GetPropertyByName(STR("ID"));
            FProperty* FlagProp = OnceFunc->GetPropertyByName(STR("flag"));

            s_SetRelicOnceFlagHookId = OnceFunc->RegisterPostHook(
                [IdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                    int32 relicId = -1;
                    bool flag = false;
                    if (IdProp) {
                        auto* IdPtr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
                        if (IdPtr) relicId = *IdPtr;
                    }
                    if (FlagProp) {
                        auto* FlagPtr = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                        if (FlagPtr) flag = *FlagPtr;
                    }

                    if (flag && relicId >= 0) {
                        LOG("[Relic] Once-flag set: relic ID={}", relicId);
                        if (PopupSuppression::IsBlockingRelics()) {
                            PopupSuppression::SetBlockNextSpawn(true);
                        }

                        std::lock_guard<std::mutex> lock(s_Mutex);
                        for (auto& cb : s_Callbacks) {
                            cb(relicId);
                        }
                    }
                }
            );
            LOG("[RelicHooks] SetRelicOnceFlag post-hook registered (id={})", s_SetRelicOnceFlagHookId);
        }
    }

    void OnRelicCollected(RelicCollectCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
