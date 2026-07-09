#include "ItemLimits.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace ItemLimits {

static bool s_Done{false};

void Raise(uint8_t maxAmount) {
    if (s_Done) return;

    // FindObject uses (ClassName, ObjectShortName) — NOT the combined Dumper-7 format
    auto* ItemAsset = UObjectGlobals::FindObject(STR("MMIAsset"), STR("ItemTable"));
    if (!ItemAsset) {
        WARN("[ItemLimits] MMIAsset ItemTable not found");
        return;
    }
    LOG("[ItemLimits] Found MMIAsset '{}'", ItemAsset->GetName().c_str());

    auto* ItemName = UObjectGlobals::FindObject(STR("ScriptMessageAsset"), STR("ItemName"));
    auto* ItemHelp = UObjectGlobals::FindObject(STR("ScriptMessageAsset"), STR("ItemHelpMess"));

    // MyFmtData is at offset 0x28 in UMMIAsset (TArray<uint8>)
    FScriptArray* arr = reinterpret_cast<FScriptArray*>(reinterpret_cast<uint8*>(ItemAsset) + 0x28);
    if (!arr || arr->Num() == 0) {
        WARN("[ItemLimits] MyFmtData empty (num: {})", arr ? arr->Num() : -1);
        return;
    }
    LOG("[ItemLimits] MyFmtData size: {} bytes", arr->Num());

    uint8* data = static_cast<uint8*>(arr->GetData());
    int32 numBytes = arr->Num();

    constexpr int ENTRY_OFFSET    = 0x55;
    constexpr int ENTRY_STRIDE    = 100;
    constexpr int HAVE_MAX_OFFSET = 0x15;
    constexpr int ITEM_COUNT      = 114;

    int modified = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        int pos = ENTRY_OFFSET + i * ENTRY_STRIDE + HAVE_MAX_OFFSET;
        if (pos < numBytes && data[pos] < maxAmount) {
            data[pos] = maxAmount;
            modified++;
        }
    }
    LOG("[ItemLimits] Patched {} of {} consumable m_HaveMax to {}", modified, ITEM_COUNT, maxAmount);

    auto* GI = UObjectGlobals::FindFirstOf(STR("ProjectGameInstance_C"));
    if (!GI) GI = UObjectGlobals::FindFirstOf(STR("ProjectGameInstanceBase"));
    if (!GI) {
        WARN("[ItemLimits] No GameInstance found");
        return;
    }

    auto* MakeUpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
        STR("/Script/Project.ProjectGameInstanceBase:MakeUpItemDataTable"));
    if (!MakeUpFunc) {
        WARN("[ItemLimits] MakeUpItemDataTable not found");
        return;
    }

    struct { UObject* pAsset; UObject* pItemName; UObject* pHelpMess; } params{ItemAsset, ItemName, ItemHelp};
    GI->ProcessEvent(MakeUpFunc, &params);
    LOG("[ItemLimits] MakeUpItemDataTable called");
    s_Done = true;
}

}
