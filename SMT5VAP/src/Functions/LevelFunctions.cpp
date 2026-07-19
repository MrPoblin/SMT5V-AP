#include "LevelFunctions.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace LevelFunctions {

    static UFunction* s_GetPlayerLevelFunc = nullptr;
    static UFunction* s_GetNkmLevelFunc = nullptr;
    static UObject* s_CDO = nullptr;
    static bool s_InitFailed = false;

    void Setup() {
        if (s_GetPlayerLevelFunc || s_InitFailed) return;

        auto Find = [](const TCHAR* path) -> UFunction* {
            return UObjectGlobals::FindObject<UFunction>(nullptr, path);
        };

        s_GetPlayerLevelFunc = Find(STR("/Script/Project.BPL_PartyData:GetPlayerLevel"));
        if (!s_GetPlayerLevelFunc)
            s_GetPlayerLevelFunc = Find(STR("/Script/Project.BPL_PartyData_C:GetPlayerLevel"));

        s_GetNkmLevelFunc = Find(STR("/Script/Project.BPL_PartyData:GetNkmLevel"));
        if (!s_GetNkmLevelFunc)
            s_GetNkmLevelFunc = Find(STR("/Script/Project.BPL_PartyData_C:GetNkmLevel"));

        s_CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_PartyData"));
        if (!s_CDO)
            s_CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
                STR("/Script/Project.Default__BPL_PartyData_C"));
        if (!s_CDO) {
            auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData"));
            if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData_C"));
            if (cls) s_CDO = cls->CreateDefaultObject();
        }

        if (!s_GetPlayerLevelFunc || !s_GetNkmLevelFunc || !s_CDO) {
            WARN("[LevelFunctions] Failed to resolve BPL_PartyData level functions or CDO");
            s_InitFailed = true;
        } else {
            LOG("[LevelFunctions] BPL_PartyData level functions resolved");
        }
    }

    int32_t GetProtagonistLevel() {
        if (!s_GetPlayerLevelFunc || !s_CDO) return -1;
        struct FParams { uint8_t playerType; int32_t ReturnValue; };
        FParams params{};
        params.playerType = kProtagonist;
        s_CDO->ProcessEvent(s_GetPlayerLevelFunc, &params);
        return params.ReturnValue;
    }

    int32_t GetNkmLevel(int32_t nkmIndex) {
        if (!s_GetNkmLevelFunc || !s_CDO) return 1;
        struct FParams { int32_t NkmIndex; int32_t ReturnValue; };
        FParams params{};
        params.NkmIndex = nkmIndex;
        s_CDO->ProcessEvent(s_GetNkmLevelFunc, &params);
        return params.ReturnValue > 0 ? params.ReturnValue : 1;
    }

}
