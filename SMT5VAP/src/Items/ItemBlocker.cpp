#include "ItemBlocker.hpp"
#include "src/Hooks/GardenHauntHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace ItemBlocker {
    static std::unordered_set<int32_t> s_BlockedIds;
    static std::mutex s_Mutex;
    static std::vector<ItemBlockedCallback> s_Callbacks;
    static std::atomic<bool> s_BlockAll{false};

    // Bypass depth counter: >0 means "our own call, don't block"
    static thread_local int32_t s_BypassDepth{0};

    BypassGuard::BypassGuard() { ++s_BypassDepth; }
    BypassGuard::~BypassGuard() { --s_BypassDepth; }

    void SetBlockAll(bool block) {
        s_BlockAll.store(block, std::memory_order_release);
        LOG("[ItemBlocker] SetBlockAll({})", block);
    }

    void BlockItemId(int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_BlockedIds.insert(id);
    }

    void UnblockItemId(int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_BlockedIds.erase(id);
    }

    void SetBlockedIds(std::initializer_list<int32_t> ids) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_BlockedIds.clear();
        s_BlockedIds.insert(ids.begin(), ids.end());
    }

    bool IsItemBlocked(int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        return s_BlockAll.load(std::memory_order_acquire) || s_BlockedIds.count(id) > 0;
    }

    void OnItemBlocked(ItemBlockedCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }

    void Setup() {
        LOG("[ItemBlocker] Setup...");

        auto* Func = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_ItemData:ItemGet"));
        if (!Func) {
            WARN("[ItemBlocker] BPL_ItemData::ItemGet NOT FOUND");
            return;
        }
        LOG("[ItemBlocker] Found BPL_ItemData::ItemGet");

        auto* IdProp = Func->GetPropertyByName(STR("ID"));
        auto* NumProp = Func->GetPropertyByName(STR("Num"));

        if (!IdProp || !NumProp) {
            WARN("[ItemBlocker] Failed to find ID/Num properties");
            return;
        }

        static bool s_Logged = false;

        Func->RegisterPreHook([IdProp, NumProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (s_BypassDepth > 0) return;  // our own manual grant, let through

            auto* IdPtr = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            auto* NumPtr = NumProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            if (!IdPtr || !NumPtr) return;

            int32 id = *IdPtr;
            int32 num = *NumPtr;
            if (num <= 0) return;


            bool blocked = s_BlockAll.load(std::memory_order_acquire);
            if (!blocked) {
                std::lock_guard<std::mutex> lock(s_Mutex);
                blocked = s_BlockedIds.count(id) > 0;
            }
            // Garden/haunt item gifts: block the grant only within a short window
            // after a PickItemReward call (context-based, not by ID).
            if (!blocked) {
                if (GardenHauntHooks::IsSuppressingGardenGiftNow()) {
                    blocked = true;
                    GardenHauntHooks::ClearGardenGiftContext();
                }
            }
            if (!blocked) return;

            if (!s_Logged) {
                if (s_BlockAll.load()) {
                    LOG("[ItemBlocker] Active: blocking ALL items");
                } else {
                    LOG("[ItemBlocker] Active: blocking {} specific item IDs", s_BlockedIds.size());
                }
                s_Logged = true;
            }

            *NumPtr = 0;
            LOG("[ItemBlocker] Blocked item {} x{}", id, num);

            std::lock_guard<std::mutex> lock(s_Mutex);
            for (auto& cb : s_Callbacks) {
                cb(id, num);
            }
        });

        LOG("[ItemBlocker] Setup complete");
    }
}
