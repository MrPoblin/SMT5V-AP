#pragma once
#include <cstdint>
#include <string>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

inline std::wstring LookupItemName(int32_t id) {
    using namespace RC::Unreal;
    static UObject* CDO = nullptr;
    static UFunction* Fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_ItemData"));
        Fn = static_cast<UFunction*>(UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.BPL_ItemData:ItemGetName")));
    }
    if (!CDO || !Fn) return {};
    struct { int32 ItemId; FText ReturnValue; } P{ id, {} };
    CDO->ProcessEvent(Fn, &P);
    if (!P.ReturnValue.Data) return {};
    return P.ReturnValue.ToString();
}
