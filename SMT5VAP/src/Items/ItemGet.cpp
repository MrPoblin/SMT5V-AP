#include "ItemGet.hpp"
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

        struct { int32 ID; int32 Num; } params{itemId, count};
        s_ItemGet.CDO->ProcessEvent(s_ItemGet.Func, &params);
    }
}

