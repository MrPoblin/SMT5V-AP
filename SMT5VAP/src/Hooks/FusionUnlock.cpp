#include "FusionUnlock.hpp"
#include "src/Log/Log.hpp"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Windows.h>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

constexpr int32 NKM_ENTRY_OFFSET = 0x69;
constexpr int32 NKM_ENTRY_STRIDE = 0x1D0;
constexpr int32 NKM_ENTRY_COUNT  = 1201;
constexpr int32 NKM_FLAGS_OFF    = 0x60;

static void PatchNKMBaseTable()
{
    auto* NKMAsset = UObjectGlobals::FindObject(STR("MMIAsset"), STR("NKMBaseTable"));
    if (!NKMAsset) return;

    auto* arr = reinterpret_cast<FScriptArray*>(reinterpret_cast<uint8*>(NKMAsset) + 0x28);
    if (!arr || arr->Num() <= 0) return;

    uint8* data = static_cast<uint8*>(arr->GetData());
    int32 totalBytes = arr->Num();
    int mod = 0;
    for (int i = 0; i < NKM_ENTRY_COUNT; i++)
    {
        int pos = NKM_ENTRY_OFFSET + i * NKM_ENTRY_STRIDE + NKM_FLAGS_OFF;
        if (pos + 1 >= totalBytes) break;
        if (data[pos] == 0 && data[pos + 1] == 0) continue;
        data[pos] = 0;
        data[pos + 1] = 0;
        mod++;
    }
    if (mod > 0) LOG("[FusionUnlock] NKMBaseTable: zeroed unlockFlags on {} entries", mod);
}

static void PatchRuntimeTables()
{
    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!modBase) return;

    auto getCtx = reinterpret_cast<uintptr_t(*)()>(modBase + 0xB29520);
    uintptr_t ctx = getCtx();
    if (!ctx) return;

    auto* paramBase = *reinterpret_cast<uint8_t**>(ctx + 0xD10);
    auto paramCount = *reinterpret_cast<uint32_t*>(ctx + 0xD10 + 8);
    if (paramBase && paramCount >= 1)
    {
        int zeroed = 0;
        for (uint32_t id = 1; id < paramCount && id <= 1200; id++)
        {
            uint8_t* entry = paramBase + (uintptr_t)id * 0x108;
            if (entry[0x6A]) { entry[0x6A] = 0; zeroed++; }
            if (entry[0x73]) { entry[0x73] = 0; zeroed++; }
            auto* el = reinterpret_cast<int32*>(entry + 0x74);
            if (*el) { *el = 0; zeroed++; }
            if (entry[0x11]) { entry[0x11] = 0; zeroed++; }
        }
        LOG("[FusionUnlock] Runtime param table: zeroed {} fields", zeroed);
    }
}

using MakeUpUniteTableFn = void(__fastcall*)(void* self, void* stack);
static MakeUpUniteTableFn s_OrigMakeUp = nullptr;
static std::unique_ptr<PLH::x64Detour> s_MakeUpDetour;

static void __fastcall HkExecMakeUpUniteTable(void* self, void* stack)
{
    PatchNKMBaseTable();
    s_OrigMakeUp(self, stack);
    PatchRuntimeTables();
}

void FusionUnlock::Setup()
{
    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!modBase) return;

    PatchNKMBaseTable();

    uint64_t makeUpTarget = modBase + 0xD58B140;
    uint64_t makeUpHook = reinterpret_cast<uint64_t>(PLH::FnCast(HkExecMakeUpUniteTable, &s_OrigMakeUp));
    uint64_t makeUpOrig = 0;
    auto det = std::make_unique<PLH::x64Detour>(makeUpTarget, makeUpHook, &makeUpOrig);
    if (!det->hook())
    {
        LOG("[FusionUnlock] FAILED: execMakeUpUniteTable hook at {:x}", makeUpTarget);
        return;
    }
    s_OrigMakeUp = PLH::FnCast(makeUpOrig, s_OrigMakeUp);
    s_MakeUpDetour = std::move(det);
    LOG("[FusionUnlock] Hooked execMakeUpUniteTable at RVA 0xD58B140");
}
