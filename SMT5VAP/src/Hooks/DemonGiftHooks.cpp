#include "DemonGiftHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace DemonGiftHooks {

static std::vector<DemonGiftCallback> s_GiftCallbacks;
static std::mutex s_Mutex;

// When true, demon level-up gifts are observed (logged + reported).
static std::atomic<bool> s_Observe{false};

static UFunction* FindFunc(const wchar_t* classAndFunc) {
    StringType s(classAndFunc);
    auto pos = s.find(STR(':'));
    if (pos == StringType::npos) return nullptr;
    StringType cls = s.substr(0, pos);
    StringType func = s.substr(pos + 1);
    for (const wchar_t* suffix : { STR(""), STR("_C") }) {
        if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr,
            (StringType(STR("/Script/Project.")) + cls + suffix + STR(":") + func).c_str()))
            return F;
    }
    return nullptr;
}

void Setup() {
    LOG("[DemonGift] Setup...");

    auto* GetGiftIdFunc = FindFunc(STR("LevelUpManager:GetGiftID"));
    if (!GetGiftIdFunc) WARN("[DemonGift] GetGiftID NOT FOUND");

    // TryGiftEventCreateAndGetGiftItem fires right before the native grant.
    // A manual ProcessEvent to GetGiftID from here is safe (unlike from
    // GetGiftID's own hook) and yields the real gift id. We log it and report
    // it to Archipelago via the callback (id 0 means "no gift", skipped).
    if (auto* F = FindFunc(STR("LevelUpManager:TryGiftEventCreateAndGetGiftItem"))) {
        LOG("[DemonGift] TryGiftEventCreateAndGetGiftItem found");
        if (!GetGiftIdFunc) WARN("[DemonGift] GetGiftID NOT FOUND (cannot capture)");

        F->RegisterPostHook([GetGiftIdFunc](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (!s_Observe.load(std::memory_order_acquire)) return;
            UObject* self = Ctx.Context;
            if (!self || !GetGiftIdFunc) return;

            struct { int32 ReturnValue; } params{};
            self->ProcessEvent(GetGiftIdFunc, &params);
            int32 giftId = params.ReturnValue;
            if (giftId != 0) {
                LOG("[DemonGift] Received gift id={}", giftId);
                FireGiftCallbacks(giftId, 1); // report to Archipelago
            }
        });
    } else {
        WARN("[DemonGift] TryGiftEventCreateAndGetGiftItem NOT FOUND");
    }

    LOG("[DemonGift] Setup complete");
}

void OnDemonGift(DemonGiftCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_GiftCallbacks.push_back(std::move(cb));
}

// Fire registered callbacks with a captured item id/amount.
void FireGiftCallbacks(int32_t itemId, int32_t num) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& cb : s_GiftCallbacks) {
        cb(itemId, num);
    }
}

void SetObserve(bool observe) {
    s_Observe.store(observe, std::memory_order_release);
    LOG("[DemonGift] SetObserve({})", observe);
}

} // namespace DemonGiftHooks
