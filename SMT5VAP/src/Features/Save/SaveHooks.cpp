#include "SaveHooks.hpp"
#include "src/Log/Log.hpp"
#include "src/Helper/HookHelper.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace SaveHooks {

static std::vector<SaveCallback> s_Callbacks;
static std::mutex s_Mutex;

// --- save completion state ---
static bool s_SavePending{ false };
static int32_t s_PendingSlot{ -1 };
static bool s_PendingInherit{ false };
static bool s_PrevComplete{ false };
static bool s_PrevLoadComplete{ false };
static std::chrono::steady_clock::time_point s_SaveStartTime{};
static constexpr auto SAVE_TIMEOUT = std::chrono::seconds(30);

// Params of Project.SaveLoadBase.IsEndDataSave / IsEndDataLoad (identical layout)
struct SaveLoadBase_IsEndDataSave_Params {
    bool complete;
    bool Success;
};

void Setup() {
    LOG("[SaveHooks] Setup...");

    auto* StartSaveF = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.SaveLoadBase:StartDataSave"));
    if (StartSaveF) {
        // Find params for reading
        auto* IndexProp = StartSaveF->GetPropertyByName(STR("Index"));
        auto* InheritProp = StartSaveF->GetPropertyByName(STR("isInheritSave"));

        StartSaveF->RegisterPreHook([IndexProp, InheritProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
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

            LOG("[Save] Save started - slot={}, inheritSave={}", slotIndex, isInherit);

            std::lock_guard<std::mutex> L(s_Mutex);
            s_SavePending = true;
            s_PendingSlot = slotIndex;
            s_PendingInherit = isInherit;
            s_PrevComplete = false;
            s_SaveStartTime = std::chrono::steady_clock::now();
        });
        LOG("[SaveHooks]   HOOKED: SaveLoadBase.StartDataSave");
    } else {
        LOG("[SaveHooks]   NOT FOUND: SaveLoadBase.StartDataSave");
    }

    // Save completion: the save-load BP polls IsEndDataSave repeatedly; the
    // OnGameSaved callbacks fire on the rising edge of 'complete' (i.e. once the
    // save has fully finished writing), and only if 'Success' is also set.
    auto* EndSaveF = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.SaveLoadBase:IsEndDataSave"));
    if (EndSaveF) {
        EndSaveF->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            auto& Params = Ctx.GetParams<SaveLoadBase_IsEndDataSave_Params>();

            std::vector<SaveCallback> callbacks;
            bool fireCallbacks = false;
            bool saveFailed = false;
            int32_t slot = -1;
            bool inherit = false;
            {
                std::lock_guard<std::mutex> L(s_Mutex);
                bool wasComplete = s_PrevComplete;
                s_PrevComplete = Params.complete;
                if (Params.complete && !wasComplete) {
                    slot = s_PendingSlot;
                    inherit = s_PendingInherit;
                    s_SavePending = false;
                    if (Params.Success) {
                        fireCallbacks = true;
                        callbacks = s_Callbacks;
                    } else {
                        saveFailed = true;
                    }
                }
            }

            if (saveFailed) {
                WARN("[Save] Save FAILED to complete - not firing save callbacks");
            } else if (fireCallbacks) {
                LOG("[Save] Save fully completed - slot={}, inheritSave={}", slot, inherit);
                for (auto& cb : callbacks) cb(slot, inherit);
            }
        });
        LOG("[SaveHooks]   HOOKED: SaveLoadBase.IsEndDataSave (completion)");
    } else {
        LOG("[SaveHooks]   NOT FOUND: SaveLoadBase.IsEndDataSave");
    }

    // Experimental: log IsEndDataLoad to learn when it activates relative to
    // StartDataLoad / map load. Rising edge + every poll (for cadence).
    auto* EndLoadF = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.SaveLoadBase:IsEndDataLoad"));
    if (EndLoadF) {
        EndLoadF->RegisterPostHook([](UnrealScriptFunctionCallableContext& Ctx, void*) {
            auto& Params = Ctx.GetParams<SaveLoadBase_IsEndDataSave_Params>();

            bool wasComplete;
            {
                std::lock_guard<std::mutex> L(s_Mutex);
                wasComplete = s_PrevLoadComplete;
                s_PrevLoadComplete = Params.complete;
            }

            DEBUG("[Save] IsEndDataLoad poll: complete={}, success={}", Params.complete, Params.Success);
            if (Params.complete && !wasComplete) {
                LOG("[Save] IsEndDataLoad reported complete={}, success={}", Params.complete, Params.Success);
            }
        });
        LOG("[SaveHooks]   HOOKED: SaveLoadBase.IsEndDataLoad (diagnostic)");
    } else {
        LOG("[SaveHooks]   NOT FOUND: SaveLoadBase.IsEndDataLoad");
    }

    LOG("[SaveHooks] Setup complete");
}

void Tick() {
    std::lock_guard<std::mutex> L(s_Mutex);
    if (!s_SavePending) return;
    if (std::chrono::steady_clock::now() - s_SaveStartTime > SAVE_TIMEOUT) {
        WARN("[Save] Save completion not detected within {}s - clearing pending save state", SAVE_TIMEOUT.count());
        s_SavePending = false;
    }
}

bool IsSavePending() {
    std::lock_guard<std::mutex> L(s_Mutex);
    return s_SavePending;
}

void OnGameSaved(SaveCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

} // namespace SaveHooks
