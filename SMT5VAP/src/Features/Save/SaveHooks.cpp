#include "SaveHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/Helper/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace SaveHooks {

static std::vector<SaveCallback> s_Callbacks;
static std::mutex s_Mutex;

void Setup() {
    LOG("[SaveHooks] Setup...");

    auto* F = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.SaveLoadBase:StartDataSave"));
    if (F) {
        // Find params for reading
        auto* IndexProp = F->GetPropertyByName(STR("Index"));
        auto* InheritProp = F->GetPropertyByName(STR("isInheritSave"));

        F->RegisterPreHook([IndexProp, InheritProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            int32_t slotIndex = -1;
            bool isInherit = false;

            if (IndexProp && Ctx.TheStack.Locals()) {
                auto* P = IndexProp->ContainerPtrToValuePtr<int32_t>(Ctx.TheStack.Locals());
                if (P) slotIndex = *P;
            }
            if (InheritProp && Ctx.TheStack.Locals()) {
                auto* P = InheritProp->ContainerPtrToValuePtr<bool>(Ctx.TheStack.Locals());
                if (P) isInherit = *P;
            }

            LOG("[Save] Game saving - slot={}, inheritSave={}", slotIndex, isInherit);

            std::lock_guard<std::mutex> L(s_Mutex);
            for (auto& cb : s_Callbacks) cb(slotIndex, isInherit);
        });
        LOG("[SaveHooks]   HOOKED: SaveLoadBase.StartDataSave");
    } else {
        LOG("[SaveHooks]   NOT FOUND: SaveLoadBase.StartDataSave");
    }

    LOG("[SaveHooks] Setup complete");
}

void OnGameSaved(SaveCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

} // namespace SaveHooks
