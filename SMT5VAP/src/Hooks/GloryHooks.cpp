#include "GloryHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <vector>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace GloryHooks {
    static std::vector<GloryCollectCallback> s_Callbacks;
    static std::mutex s_Mutex;

    static UFunction* FindFunc(std::initializer_list<const wchar_t*> paths) {
        for (auto* p : paths) {
            if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
                return F;
        }
        return nullptr;
    }

    void Setup() {
        LOG("[GloryHooks] Setup (observation-only)...");

        // ── AddGodParameterPoint (glory grant) ──
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_GodParameter:AddGodParameterPoint"),
                STR("/Script/Project.BPL_GodParameter_C:AddGodParameterPoint"),
            })) {
            auto* ValProp = F->GetPropertyByName(STR("Value"));
            F->RegisterPreHook([ValProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 val = 0;
                if (ValProp)
                    if (auto* P = ValProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals()))
                        val = *P;
                LOG("[Glory] AddGodParameterPoint Value={}", val);
                std::lock_guard<std::mutex> L(s_Mutex);
                for (auto& cb : s_Callbacks) cb(-1, val);
            });
            LOG("[GloryHooks] AddGodParameterPoint hooked");
        } else {
            WARN("[GloryHooks] AddGodParameterPoint NOT FOUND");
        }

        // ── AddPieceHaveNum (glory/piece item pickups) ──
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_PieceData:AddPieceHaveNum"),
                STR("/Script/Project.BPL_PieceData_C:AddPieceHaveNum"),
            })) {
            auto* IdProp = F->GetPropertyByName(STR("ItemId"));
            auto* AddProp = F->GetPropertyByName(STR("Add"));
            F->RegisterPreHook([IdProp, AddProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 id = -1, amt = 0;
                if (IdProp)
                    if (auto* P = IdProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals())) id = *P;
                if (AddProp)
                    if (auto* P = AddProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals())) amt = *P;
                LOG("[Glory] AddPieceHaveNum ID={} Add={}", id, amt);
                std::lock_guard<std::mutex> L(s_Mutex);
                for (auto& cb : s_Callbacks) cb(id, amt);
            });
            LOG("[GloryHooks] AddPieceHaveNum hooked");
        } else {
            WARN("[GloryHooks] AddPieceHaveNum NOT FOUND");
        }

        LOG("[GloryHooks] Setup complete");
    }

    void OnGloryCollected(GloryCollectCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }
}
