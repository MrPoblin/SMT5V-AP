#include "MaccaBlocker.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace MaccaBlocker {
    static std::mutex s_Mutex;
    static std::vector<MaccaBlockedCallback> s_Callbacks;
    static std::atomic<bool> s_BlockMacca{false};

    // Bypass depth counter: >0 means "our own call, don't block"
    static thread_local int32_t s_BypassDepth{0};

    BypassGuard::BypassGuard() { ++s_BypassDepth; }
    BypassGuard::~BypassGuard() { --s_BypassDepth; }

    void SetBlockMacca(bool block) {
        s_BlockMacca.store(block, std::memory_order_release);
        LOG("[MaccaBlocker] SetBlockMacca({})", block);
    }

    void OnMaccaBlocked(MaccaBlockedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }

    void Setup() {
        LOG("[MaccaBlocker] Setup...");

        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:AddPartyMakka"));
        if (!Func) {
            Func = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:AddPartyMakka"));
        }
        if (!Func) {
            WARN("[MaccaBlocker] BPL_PartyData::AddPartyMakka NOT FOUND");
            return;
        }
        LOG("[MaccaBlocker] Found BPL_PartyData::AddPartyMakka");

        auto* MakkaProp = Func->GetPropertyByName(STR("Makka"));
        if (!MakkaProp) {
            WARN("[MaccaBlocker] Failed to find Makka property");
            return;
        }

        Func->RegisterPreHook([MakkaProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (s_BypassDepth > 0) return;  // our own manual grant, let through
            if (!s_BlockMacca.load(std::memory_order_acquire)) return;

            auto* MakkaPtr = MakkaProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            if (!MakkaPtr) return;

            int32 amount = *MakkaPtr;
            if (amount <= 0) return;

            // Block it by zeroing out the parameter
            *MakkaPtr = 0;
            LOG("[MaccaBlocker] Blocked macca x{}", amount);

            std::lock_guard<std::mutex> lock(s_Mutex);
            for (auto& cb : s_Callbacks) {
                cb(amount);
            }
        });

        LOG("[MaccaBlocker] Setup complete");
    }
}
