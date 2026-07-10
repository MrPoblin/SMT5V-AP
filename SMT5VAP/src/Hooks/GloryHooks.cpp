#include "GloryHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <vector>
#include <mutex>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

namespace GloryHooks {
    static std::vector<GloryCollectCallback> s_Callbacks;
    static std::mutex s_Mutex;
    static std::atomic<bool> s_BlockGlory{true};

    static UFunction* FindFunc(std::initializer_list<const wchar_t*> paths) {
        for (auto* p : paths) {
            if (auto* F = UObjectGlobals::FindObject<UFunction>(nullptr, p))
                return F;
        }
        return nullptr;
    }

    void Setup() {
        LOG("[GloryHooks] Setup...");

        // ── AddGodParameterPoint (glory grant) ──
        if (auto* F = FindFunc({
                STR("/Script/Project.BPL_GodParameter:AddGodParameterPoint"),
                STR("/Script/Project.BPL_GodParameter_C:AddGodParameterPoint"),
            })) {
            auto* ValProp = F->GetPropertyByName(STR("Value"));
            LOG("[GloryHooks] AddGodParameterPoint found, Value prop={}", ValProp ? 1 : 0);
            F->RegisterPreHook([ValProp](UnrealScriptFunctionCallableContext& Ctx, void*) {
                int32 val = 0;
                if (ValProp) {
                    if (auto* P = ValProp->ContainerPtrToValuePtr<int32>(Ctx.TheStack.Locals())) {
                        val = *P;
                        if (val > 0 && s_BlockGlory) {
                            // Values granted by Miman and Amalgams
                            if (val == 5 || val == 6 || val == 8 || val == 10 || val == 50 || val == 60 || val == 80 || val == 100) {
                                LOG("[Glory] Blocked value={}", val);
                                *P = 0;
                            } else {
                                LOG("[Glory] Allowed value={}", val);
                            }
                        }
                    }
                }
                // Fire callback for Amalgams
                if (val == 50 || val == 60 || val == 80 || val == 100) {
                    LOG("[Glory] AddGodParameterPoint Value={}", val);
                    std::lock_guard<std::mutex> L(s_Mutex);
                    for (auto& cb : s_Callbacks) cb(val);
                }
            });
        } else {
            WARN("[GloryHooks] AddGodParameterPoint NOT FOUND");
        }

        LOG("[GloryHooks] Setup complete");
    }

    void OnGloryCollected(GloryCollectCallback cb) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Callbacks.push_back(std::move(cb));
    }

    void SetBlockGlory(bool block) {
        s_BlockGlory = block;
        LOG("[GloryHooks] SetBlockGlory({})", block);
    }
}
