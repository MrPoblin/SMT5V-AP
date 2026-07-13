#include "MiracleHook.hpp"
#include "src/Log/Log.hpp"
#include <cstdint>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace MiracleHook {

    // True while an AP grant is in progress, so the purchase-block hook ignores
    // the corresponding (forced) GodParameterSkillLearning call.
    static std::atomic<bool> s_Granting{ false };

    static std::atomic<bool> s_BlockUnlocks{ true };

    BypassGuard::BypassGuard() { s_Granting.store(true, std::memory_order_release); }
    BypassGuard::~BypassGuard() { s_Granting.store(false, std::memory_order_release); }

    void SetBlockUnlocks(bool block) { s_BlockUnlocks.store(block, std::memory_order_release); }

    // ── helper: locate a UFunction by "Class:Func" (tries /Script/Project._C) ──
    static UFunction* FindUFunction(const wchar_t* classAndFunc) {
        StringType s(classAndFunc);
        auto pos = s.find(STR(':'));
        if (pos == StringType::npos) return nullptr;
        StringType cls = s.substr(0, pos);
        StringType func = s.substr(pos + 1);
        for (const wchar_t* suffix : { STR(""), STR("_C") }) {
            StringType path = STR("/Script/Project.") + cls + suffix + STR(":") + func;
            auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, path.c_str());
            if (F) return F;
        }
        WARN(STR("[MIRACLE] FindUFunction '{}' NOT FOUND"), classAndFunc);
        return nullptr;
    }

    // ── helper: get the class default object for executing a static BPL function ──
    static UObject* FindCDO(const wchar_t* className) {
        for (const wchar_t* suffix : { STR(""), STR("_C") }) {
            auto* Obj = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
                (StringType(STR("/Script/Project.Default__")) + className + suffix).c_str());
            if (Obj) return Obj;
        }
        auto* Cls = UObjectGlobals::FindObject<UClass>(nullptr,
            (StringType(STR("/Script/Project.")) + className).c_str());
        if (!Cls) Cls = UObjectGlobals::FindObject<UClass>(nullptr,
            (StringType(STR("/Script/Project.")) + className + STR("_C")).c_str());
        if (Cls) return Cls->CreateDefaultObject();
        WARN(STR("[MIRACLE] FindCDO '{}' NOT FOUND"), className);
        return nullptr;
    }

    // ── force-learn a miracle (AP buy path) ──
    bool GrantMiracle(int32_t skillId) {
        UFunction* Func = FindUFunction(STR("BPL_GodParameter:GodParameterSkillLearning"));
        UObject* BPL = Func ? FindCDO(STR("BPL_GodParameter")) : nullptr;
        if (!Func || !BPL) { WARN(STR("[MIRACLE] GrantMiracle: func/CDO missing")); return false; }

        int32 bufSize = Func->GetPropertiesSize();
        if (bufSize <= 0) return false;
        std::vector<uint8_t> buf(bufSize, 0);

        auto* IdProp = Func->GetPropertyByName(STR("argSkillID"));
        auto* ForcedProp = Func->GetPropertyByName(STR("forced"));
        if (!IdProp || !ForcedProp) { WARN(STR("[MIRACLE] GrantMiracle: props missing")); return false; }
        *IdProp->ContainerPtrToValuePtr<int32>(buf.data()) = skillId;
        static_cast<FBoolProperty*>(ForcedProp)->SetPropertyValueInContainer(buf.data(), true);

        bool ret = false;
        {
            BypassGuard guard;
            try {
                BPL->ProcessEvent(Func, buf.data());
            } catch (...) {
                WARN(STR("[MIRACLE] GrantMiracle: ProcessEvent threw for skillId={}"), skillId);
                return false;
            }
        }
        if (auto* RetProp = Func->GetReturnProperty()) {
            ret = static_cast<FBoolProperty*>(RetProp)->GetPropertyValueInContainer(buf.data());
        }
        LOG(STR("[MIRACLE] GrantMiracle(skillId={}) -> ret={}"), skillId, ret ? 1 : 0);
        return ret;
    }

    // ── block unauthorized purchases ──
    static void HookGodParameterSkillLearning() {
        auto* Func = FindUFunction(STR("BPL_GodParameter:GodParameterSkillLearning"));
        if (!Func) return;
        auto* SkillIdProp = Func->GetPropertyByName(STR("argSkillID"));
        auto* ForcedProp = Func->GetPropertyByName(STR("forced"));
        if (!SkillIdProp || !ForcedProp) { WARN(STR("[MIRACLE] GodParameterSkillLearning: props missing")); return; }
        auto* ForcedBool = static_cast<FBoolProperty*>(ForcedProp);

        Func->RegisterPreHook([SkillIdProp, ForcedBool](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (s_Granting.load(std::memory_order_acquire)) return; // our own AP grant
            if (!s_BlockUnlocks.load(std::memory_order_acquire)) return;
            int32 skillId = *SkillIdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            LOG(STR("[MIRACLE] BLOCKED purchase of skillId={} (not AP-granted)"), skillId);
            // Neutralize the call so the learn fails.
            ForcedBool->SetPropertyValueInContainer(Ctx.TheStack.Locals(), false);
            *SkillIdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()) = -1;
        });
    }

    void Setup() {
        HookGodParameterSkillLearning();
        LOG(STR("[MIRACLE] MiracleHook setup complete (block unlocks={})"), s_BlockUnlocks.load() ? 1 : 0);
    }

} // namespace MiracleHook
