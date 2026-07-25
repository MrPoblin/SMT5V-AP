#include "MiracleHooks.hpp"
#include "src/HookHelper.hpp"
#include "src/Archipelago/APState.hpp"
#include "src/Log/Log.hpp"
#include <cstdint>
#include <memory>
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace MiracleHooks {

    static std::atomic<bool> s_Granting{ false };

    static std::atomic<bool> s_BlockUnlocks{ true };

    // Native function hook data
    // sub_147402B90 = native "IsAdded" check called by GetGodParameterSkillState
    // signature: bool __fastcall(uint32_t skillId)
    using IsAddedFn = bool(*)(uint32_t);
    static IsAddedFn s_IsAddedOrig = nullptr;
    static std::unique_ptr<PLH::x64Detour> s_IsAddedDetour;

    // sub_14735FF80 = native IsAddedSelectableGodParameterSkill implementation
    // signature: bool __fastcall(uint32_t skillId)
    using IsSelectableFn = bool(*)(uint32_t);
    static IsSelectableFn s_IsSelectableOrig = nullptr;
    static std::unique_ptr<PLH::x64Detour> s_IsSelectableDetour;

    // sub_1473649E0 = native IsLearningGodParameterSkill implementation
    // signature: bool __fastcall(uint32_t skillId)
    using IsLearningFn = bool(*)(uint32_t);
    static IsLearningFn s_IsLearningOrig = nullptr;
    static std::unique_ptr<PLH::x64Detour> s_IsLearningDetour;

    BypassGuard::BypassGuard() { s_Granting.store(true, std::memory_order_release); }
    BypassGuard::~BypassGuard() { s_Granting.store(false, std::memory_order_release); }

    void SetBlockUnlocks(bool block) { s_BlockUnlocks.store(block, std::memory_order_release); }

    // ── helpers ──

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

    static bool IsAPUnlocked(int32_t skillId) {
        return APState::Miracles::Has(skillId);
    }

    // ── native hooks ──

    bool __fastcall HkIsAdded(uint32_t skillId) {
        return IsAPUnlocked(static_cast<int32_t>(skillId));
    }

    bool __fastcall HkIsSelectable(uint32_t skillId) {
        return IsAPUnlocked(static_cast<int32_t>(skillId));
    }

    bool __fastcall HkIsLearning(uint32_t skillId) {
        if (!IsAPUnlocked(static_cast<int32_t>(skillId))) {
            return false;
        }
        return s_IsLearningOrig(skillId);
    }

    static bool InstallNativeHook(uint64_t targetAddr,
        uint64_t hookFn,
        uint64_t* origOut,
        std::unique_ptr<PLH::x64Detour>& detourOut)
    {
        if (targetAddr < 0x140000000 || targetAddr > 0x160000000) {
            WARN(STR("[MIRACLE] Native hook target {:p} out of range"), (void*)targetAddr);
            return false;
        }
        uint64_t origAddr = 0;
        auto det = std::make_unique<PLH::x64Detour>(targetAddr, hookFn, &origAddr);
        if (!det->hook()) {
            WARN(STR("[MIRACLE] Native x64Detour FAILED at {:p}"), (void*)targetAddr);
            return false;
        }
        *origOut = origAddr;
        detourOut = std::move(det);
        return true;
    }

    // ── UFunction hooks ──

    static void HookSkillLearning() {
        auto* Func = FindUFunction(STR("BPL_GodParameter:GodParameterSkillLearning"));
        if (!Func) return;
        auto* SkillIdProp = Func->GetPropertyByName(STR("argSkillID"));
        auto* ForcedProp = Func->GetPropertyByName(STR("forced"));
        if (!SkillIdProp || !ForcedProp) { WARN(STR("[MIRACLE] GodParameterSkillLearning: props missing")); return; }
        auto* ForcedBool = static_cast<FBoolProperty*>(ForcedProp);

        Func->RegisterPreHook([SkillIdProp, ForcedBool](UnrealScriptFunctionCallableContext& Ctx, void*) {
            if (s_Granting.load(std::memory_order_acquire)) return;
            if (!s_BlockUnlocks.load(std::memory_order_acquire)) return;

            int32 skillId = *SkillIdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals());
            bool forced = ForcedBool->GetPropertyValueInContainer(Ctx.TheStack.Locals());

            if (forced) {
                LOG(STR("[MIRACLE] BLOCKED forced grant of skillId={}"), skillId);
                ForcedBool->SetPropertyValueInContainer(Ctx.TheStack.Locals(), false);
                *SkillIdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()) = -1;
                return;
            }

            if (IsAPUnlocked(skillId)) {
                LOG(STR("[MIRACLE] AP-unlocked purchase allowed: skillId={}"), skillId);
                return;
            }

            LOG(STR("[MIRACLE] BLOCKED purchase of non-AP-unlocked skillId={}"), skillId);
            *SkillIdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()) = -1;
        });
    }

    // ── public API ──

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

    void UnlockForPurchase(int32_t skillId) {
        APState::Miracles::Add(skillId);
        LOG(STR("[MIRACLE] UnlockForPurchase(skillId={})"), skillId);
    }

    void ResetUnlocks() {
        APState::Miracles::Clear();
        LOG(STR("[MIRACLE] ResetUnlocks - cleared all AP-unlocked miracles"));
    }

    void Setup() {
        HookSkillLearning();

        // Hook 1: native "IsAdded" check called by GetGodParameterSkillState
        // Signature: 40 53 41 56 48 83 EC 38 48 89 6C 24 ? 45 31 F6
        {
            uint64_t target = SignatureScanner::FindPattern("40 53 41 56 48 83 EC 38 48 89 6C 24 ? 45 31 F6");
            if (!target) {
                WARN(STR("[MIRACLE] IsAdded signature NOT FOUND"));
                return;
            }
            uint64_t hookAddr = reinterpret_cast<uint64_t>(PLH::FnCast(HkIsAdded, &s_IsAddedOrig));
            if (InstallNativeHook(target, hookAddr, reinterpret_cast<uint64_t*>(&s_IsAddedOrig), s_IsAddedDetour)) {
                LOG(STR("[MIRACLE] IsAdded native hook installed: target={:p}"), (void*)target);
            }
        }

        // Hook 2: native IsAddedSelectableGodParameterSkill implementation
        // Signature: 48 89 5C 24 ? 57 48 83 EC 60 89 CF
        {
            uint64_t target = SignatureScanner::FindPattern("48 89 5C 24 ? 57 48 83 EC 60 89 CF");
            if (!target) {
                WARN(STR("[MIRACLE] IsSelectable signature NOT FOUND"));
                return;
            }
            uint64_t hookAddr = reinterpret_cast<uint64_t>(PLH::FnCast(HkIsSelectable, &s_IsSelectableOrig));
            if (InstallNativeHook(target, hookAddr, reinterpret_cast<uint64_t*>(&s_IsSelectableOrig), s_IsSelectableDetour)) {
                LOG(STR("[MIRACLE] IsSelectable native hook installed: target={:p}"), (void*)target);
            }
        }

        // Hook 3: native IsLearningGodParameterSkill implementation
        // Signature: 48 89 5C 24 ? 57 48 83 EC 40 48 63 F9 E8 ? ? ? ? 48 89 C3 ...
        {
            uint64_t target = SignatureScanner::FindPattern("48 89 5C 24 ? 57 48 83 EC 40 48 63 F9");
            if (!target) {
                WARN(STR("[MIRACLE] IsLearning signature NOT FOUND"));
                return;
            }
            uint64_t hookAddr = reinterpret_cast<uint64_t>(PLH::FnCast(HkIsLearning, &s_IsLearningOrig));
            if (InstallNativeHook(target, hookAddr, reinterpret_cast<uint64_t*>(&s_IsLearningOrig), s_IsLearningDetour)) {
                LOG(STR("[MIRACLE] IsLearning native hook installed: target={:p}"), (void*)target);
            }
        }

        LOG(STR("[MIRACLE] MiracleHooks setup complete (block unlocks={})"),
            s_BlockUnlocks.load() ? 1 : 0);
    }

} // namespace MiracleHooks
