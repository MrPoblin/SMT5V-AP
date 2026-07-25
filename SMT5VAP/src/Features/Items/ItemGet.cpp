#include "ItemGet.hpp"
#include "ItemBlocker.hpp"
#include "MaccaBlocker.hpp"
#include "src/Features/Collections/GloryHooks.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace ItemGet {
    static struct {
        UFunction* Func = nullptr;
        UObject* CDO = nullptr;
        bool InitFailed = false;

        void Init() {
            if (Func || InitFailed) return;
            Func = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Project.BPL_ItemData:ItemGet"));
            CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/Project.Default__BPL_ItemData"));
            if (!Func || !CDO) {
                WARN("[GiveItem] Failed to find BPL_ItemData::ItemGet or its CDO");
                InitFailed = true;
            }
        }
    } s_ItemGet;

    void GiveItem(int32_t itemId, int32_t count) {
        s_ItemGet.Init();
        if (!s_ItemGet.Func || !s_ItemGet.CDO) return;
        //Bypass item blocker
        ItemBlocker::BypassGuard guard;
        struct { int32 ID; int32 Num; } params{itemId, count};
        s_ItemGet.CDO->ProcessEvent(s_ItemGet.Func, &params);
    }

    // ── GiveGlory ──
    static struct {
        UFunction* Func = nullptr;
        UObject* CDO = nullptr;
        bool InitFailed = false;

        void Init() {
            if (Func || InitFailed) return;
            Func = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Project.BPL_GodParameter:AddGodParameterPoint"));
            if (!Func) Func = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Project.BPL_GodParameter_C:AddGodParameterPoint"));
            CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/Project.Default__BPL_GodParameter"));
            if (!CDO) CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/Project.Default__BPL_GodParameter_C"));
            if (!CDO && Func) {
                auto* OwnerClass = Func->GetTypedOuter<UClass>();
                if (OwnerClass) {
                    CDO = OwnerClass->CreateDefaultObject();
                }
            }
            if (!Func || !CDO) {
                WARN("[GiveGlory] Failed to find AddGodParameterPoint or its CDO");
                InitFailed = true;
            }
        }
    } s_Glory;

    void GiveGlory(int32_t amount) {
        s_Glory.Init();
        if (!s_Glory.Func || !s_Glory.CDO) return;

        bool prev = GloryHooks::g_APInitiatedGlory;
        GloryHooks::g_APInitiatedGlory = true;
        struct { int32 Value; } params{amount};
        s_Glory.CDO->ProcessEvent(s_Glory.Func, &params);
        GloryHooks::g_APInitiatedGlory = prev;
    }

    // ── GiveMacca ──
    static struct {
        UFunction* Func = nullptr;
        UObject* CDO = nullptr;
        bool InitFailed = false;

        void Init() {
            if (Func || InitFailed) return;
            Func = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Project.BPL_PartyData:AddPartyMakka"));
            if (!Func) {
                Func = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Project.BPL_PartyData_C:AddPartyMakka"));
            }
            if (!Func) {
                WARN("[GiveMacca] Function AddPartyMakka NOT FOUND");
                InitFailed = true;
                return;
            }
            CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/Project.Default__BPL_PartyData"));
            if (!CDO) {
                CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/Project.Default__BPL_PartyData_C"));
            }
            if (!CDO) {
                auto* MapDataClass = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData"));
                if (!MapDataClass) MapDataClass = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData_C"));
                if (!MapDataClass) {
                    WARN("[GiveMacca] BPL_PartyData class NOT FOUND");
                    InitFailed = true;
                    return;
                }
                CDO = MapDataClass->CreateDefaultObject();
            }
            if (!CDO) {
                WARN("[GiveMacca] CDO is null after all attempts");
                InitFailed = true;
            }
        }
    } s_Macca;

    void GiveMacca(int32_t amount) {
        s_Macca.Init();
        if (!s_Macca.Func || !s_Macca.CDO) return;

        MaccaBlocker::BypassGuard guard;
        struct { int32 Makka; } params{amount};
        s_Macca.CDO->ProcessEvent(s_Macca.Func, &params);
    }
}
