#include "EventFlags.hpp"
#include "src/Hooks/EventFlagHook.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace EventFlags {

    static struct {
        UFunction* SetFlag = nullptr;
        UFunction* SetFlagValue = nullptr;
        UFunction* CheckFlag = nullptr;
        UFunction* CheckFlagValue = nullptr;
        UObject* CDO = nullptr;
        bool InitFailed = false;
    } s_EventFlag;

    void Setup() {
        if (s_EventFlag.SetFlag || s_EventFlag.InitFailed) return;

        auto Find = [](const TCHAR* path) -> UFunction* {
            return UObjectGlobals::FindObject<UFunction>(nullptr, path);
        };

        s_EventFlag.SetFlag = Find(STR("/Script/Project.BPL_EventFlag:SetEventFlag"));
        s_EventFlag.SetFlagValue = Find(STR("/Script/Project.BPL_EventFlag:SetEventFlagValue"));
        s_EventFlag.CheckFlag = Find(STR("/Script/Project.BPL_EventFlag:CheckEventFlag"));
        s_EventFlag.CheckFlagValue = Find(STR("/Script/Project.BPL_EventFlag:CheckEventFlagValue"));
        s_EventFlag.CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_EventFlag"));

        if (!s_EventFlag.CDO ||
            !s_EventFlag.SetFlag || !s_EventFlag.SetFlagValue ||
            !s_EventFlag.CheckFlag || !s_EventFlag.CheckFlagValue) {
            WARN("[EventFlags] Failed to resolve BPL_EventFlag functions/CDO");
            s_EventFlag.InitFailed = true;
        } else {
            LOG("[EventFlags] BPL_EventFlag resolved");
        }
    }

    void Set(int32_t flagId, bool value) {
        Setup();
        if (!s_EventFlag.SetFlagValue || !s_EventFlag.CDO) return;
        EventFlagHook::ScopedSuppressNotify guard;
        struct { int32 flag; bool sw; } params{flagId, value};
        s_EventFlag.CDO->ProcessEvent(s_EventFlag.SetFlagValue, &params);
        LOG("[EventFlags] Set flag {} -> {}", flagId, value ? STR("true") : STR("false"));
    }

    void Set(const RC::StringType& flagName, bool value) {
        Setup();
        if (!s_EventFlag.SetFlag || !s_EventFlag.CDO) return;
        EventFlagHook::ScopedSuppressNotify guard;
        struct { FName Name; bool sw; } params{FName(flagName), value};
        s_EventFlag.CDO->ProcessEvent(s_EventFlag.SetFlag, &params);
        LOG("[EventFlags] Set flag {} -> {}", flagName, value ? STR("true") : STR("false"));
    }

    bool Get(int32_t flagId) {
        Setup();
        if (!s_EventFlag.CheckFlagValue || !s_EventFlag.CDO) return false;
        struct { int32 flag; bool ReturnValue; } params{flagId, false};
        s_EventFlag.CDO->ProcessEvent(s_EventFlag.CheckFlagValue, &params);
        return params.ReturnValue;
    }

    bool Get(const RC::StringType& flagName) {
        Setup();
        if (!s_EventFlag.CheckFlag || !s_EventFlag.CDO) return false;
        struct { FName Name; bool ReturnValue; } params{FName(flagName), false};
        s_EventFlag.CDO->ProcessEvent(s_EventFlag.CheckFlag, &params);
        return params.ReturnValue;
    }

}
