#include "NaviDevilHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace NaviDevilHooks {
    static CallbackId s_SaveIdHookId{-1};
    static std::vector<NaviGimmickCollectedCallback> s_Callbacks;
    static std::mutex s_Mutex;

    void Setup() {
        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_MapData:SetNaviDevilGimmickUniqueSaveID"));
        if (!Func) {
            WARN("[NaviDevilHooks] Failed to find SetNaviDevilGimmickUniqueSaveID");
            return;
        }
        LOG("[NaviDevilHooks] Found SetNaviDevilGimmickUniqueSaveID");

        FProperty* SaveIdProp = Func->GetPropertyByName(STR("SaveId"));
        FProperty* FlagProp = Func->GetPropertyByName(STR("flag"));

        s_SaveIdHookId = Func->RegisterPostHook(
            [SaveIdProp, FlagProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                std::int32_t saveId = -1;
                bool flag = false;
                if (SaveIdProp) {
                    if (auto* P = SaveIdProp->ContainerPtrToValuePtr<std::int32_t>(Ctx.TheStack.Locals()))
                        saveId = *P;
                }
                if (FlagProp) {
                    if (auto* P = FlagProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals()))
                        flag = *P;
                }

                if (flag && saveId >= 0) {
                    LOG("[NaviDevil] Gimmick saved: SaveId={}", saveId);
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    for (auto& cb : s_Callbacks) {
                        cb(saveId);
                    }
                }
            }
        );
        LOG("[NaviDevilHooks] SetNaviDevilGimmickUniqueSaveID post-hook registered (id={})", s_SaveIdHookId);
    }

    void OnNaviGimmickCollected(NaviGimmickCollectedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
