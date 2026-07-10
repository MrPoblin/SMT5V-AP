#include "AogamiHooks.hpp"
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

namespace AogamiHooks {
    static CallbackId s_SetAogamiDebrisFlagHookId{-1};
    static std::vector<AogamiDebrisCollectedCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetAogamiDebrisFlag"));
        if (!Func) {
            WARN("[AogamiHooks] Failed to find SetAogamiDebrisFlag");
            return;
        }
        LOG("[AogamiHooks] Found SetAogamiDebrisFlag");

        FProperty* TableIndexProp = Func->GetPropertyByName(STR("tableIndex"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

        s_SetAogamiDebrisFlagHookId = Func->RegisterPostHook(
            [TableIndexProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 tableIndex = -1;
                bool flag = false;
                if (TableIndexProp) {
                    if (auto* P = TableIndexProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        tableIndex = *P;
                }
                if (FlagProp) {
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        flag = *P;
                }

                if (flag && tableIndex >= 0) {
                    LOG("[Aogami] Debris collected: tableIndex={}", tableIndex);

                    if (PopupSuppression::IsBlockingAogamiDebris()) {
                        PopupSuppression::SetBlockNextSpawn(true);
                    }

                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) {
                        cb(tableIndex);
                    }
                }
            }
        );
        LOG("[AogamiHooks] SetAogamiDebrisFlag post-hook registered (id={})", s_SetAogamiDebrisFlagHookId);
    }

    void OnAogamiDebrisCollected(AogamiDebrisCollectedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
