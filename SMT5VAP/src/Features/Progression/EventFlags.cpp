#include "EventFlags.hpp"
#include "src/Features/Collections/EventFlagHook.hpp"
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

    static struct {
        UFunction* SetStart = nullptr;
        UFunction* SetEnd = nullptr;
        UFunction* SetAfter = nullptr;
        UFunction* IsActive = nullptr;
        UFunction* GetData = nullptr;
        UObject* CDO = nullptr;
        bool InitFailed = false;
    } s_MapEvent;

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

        s_MapEvent.SetStart = Find(STR("/Script/Project.BPL_MapEventData:SetMapEventStartFlags"));
        s_MapEvent.SetEnd = Find(STR("/Script/Project.BPL_MapEventData:SetMapEventEndFlag"));
        s_MapEvent.SetAfter = Find(STR("/Script/Project.BPL_MapEventData:SetMapEventAfterFlag"));
        s_MapEvent.IsActive = Find(STR("/Script/Project.BPL_MapEventData:IsMapEventActivateFlags"));
        s_MapEvent.GetData = Find(STR("/Script/Project.BPL_MapEventData:GetMapEventData"));
        s_MapEvent.CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_MapEventData"));

        if (!s_EventFlag.CDO ||
            !s_EventFlag.SetFlag || !s_EventFlag.SetFlagValue ||
            !s_EventFlag.CheckFlag || !s_EventFlag.CheckFlagValue) {
            WARN("[EventFlags] Failed to resolve BPL_EventFlag functions/CDO");
            s_EventFlag.InitFailed = true;
        } else {
            LOG("[EventFlags] BPL_EventFlag resolved");
        }

        if (!s_MapEvent.CDO ||
            !s_MapEvent.SetStart || !s_MapEvent.SetEnd || !s_MapEvent.SetAfter ||
            !s_MapEvent.IsActive || !s_MapEvent.GetData) {
            WARN("[EventFlags] Failed to resolve BPL_MapEventData functions/CDO");
            s_MapEvent.InitFailed = true;
        } else {
            LOG("[EventFlags] BPL_MapEventData resolved");
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

    // ── Map-event flags ──
    static void SetMapEvent(UFunction* func, int32_t mapEventId, bool value) {
        Setup();
        if (!func || !s_MapEvent.CDO) return;
        EventFlagHook::ScopedSuppressNotify guard;
        struct { int32 _mapEventID; bool Value; } params{mapEventId, value};
        s_MapEvent.CDO->ProcessEvent(func, &params);
        LOG("[EventFlags] Set map-event {} -> {}", mapEventId, value ? STR("true") : STR("false"));
    }

    void SetMapEventStart(int32_t mapEventId, bool value) { SetMapEvent(s_MapEvent.SetStart, mapEventId, value); }
    void SetMapEventEnd(int32_t mapEventId, bool value) { SetMapEvent(s_MapEvent.SetEnd, mapEventId, value); }
    void SetMapEventAfter(int32_t mapEventId, bool value) { SetMapEvent(s_MapEvent.SetAfter, mapEventId, value); }

    bool IsMapEventActive(int32_t mapEventId) {
        Setup();
        if (!s_MapEvent.IsActive || !s_MapEvent.CDO) return false;
        struct { int32 _mapEventID; bool ReturnValue; } params{mapEventId, false};
        s_MapEvent.CDO->ProcessEvent(s_MapEvent.IsActive, &params);
        return params.ReturnValue;
    }

    bool GetMapEventFlagIds(int32_t mapEventId, int32_t& outStart, int32_t& outEnd, int32_t& outAfter) {
        outStart = outEnd = outAfter = -1;
        Setup();
        if (!s_MapEvent.GetData || !s_MapEvent.CDO) return false;
        // FMapEventData is a USTRUCT with an array (m_StartFlagValues) as the first
        // field, which makes a fixed-size buffer layout fragile. We only reliably
        // read the scalar End/After flag values here; the Start flag is an array
        // whose first element can be pulled later if needed. For gating you almost
        // always care about the End flag (completion) / the event's activation.
        struct FMapEventDataBuffer {
            int32_t MapEventID;
            int32_t StartArrayPad[6];  // m_StartFlagValues TArray (3x int32) - skip
            int32_t EndFlagValue;
            int32_t AfterFlagValue;
            int32_t pad[64];
        };
        FMapEventDataBuffer buf{};
        struct { int32 ID; FMapEventDataBuffer ReturnValue; } params{mapEventId, buf};
        s_MapEvent.CDO->ProcessEvent(s_MapEvent.GetData, &params);
        outEnd = params.ReturnValue.EndFlagValue;
        outAfter = params.ReturnValue.AfterFlagValue;
        LOG("[EventFlags] Map event {} end={} after={}", mapEventId, outEnd, outAfter);
        return true;
    }

}
