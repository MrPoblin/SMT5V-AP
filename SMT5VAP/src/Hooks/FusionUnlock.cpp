#include "FusionUnlock.hpp"
#include "src/Log/Log.hpp"
#include "src/HookHelper.hpp"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Windows.h>
#include <atomic>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

// NKMBaseTable is a UMMIAsset. Binary data in MyFmtData (TArray<uint8> at +0x28).
// Entry layout: 1201 entries × 0x1D0 bytes, first at offset 0x69.
// unlockFlags at +0x60 within each entry (2 bytes).
constexpr int32 NKM_ENTRY_OFFSET = 0x69;
constexpr int32 NKM_ENTRY_STRIDE = 0x1D0;
constexpr int32 NKM_ENTRY_COUNT  = 1201;
constexpr int32 NKM_FLAGS_OFF    = 0x60;

// param table fields (FDevilBaseData, stride 0x108)
constexpr uint32_t PARAM_UNITE_LIMIT_OFF   = 0x6A; // m_UniteLimit
constexpr uint32_t PARAM_BIBLE_SUMMON_OFF  = 0x73; // m_BibleFromSummonCanNot
constexpr uint32_t PARAM_EVENT_LIMIT_OFF   = 0x74; // m_EventLimit (int32)

static std::atomic<bool> s_Patched{false};

static void PatchNKMBaseTable()
{
    auto* NKMAsset = UObjectGlobals::FindObject(STR("MMIAsset"), STR("NKMBaseTable"));
    if (!NKMAsset)
    {
        LOG("[FusionUnlock] NKMBaseTable not found");
        return;
    }

    auto* arr = reinterpret_cast<FScriptArray*>(reinterpret_cast<uint8*>(NKMAsset) + 0x28);
    if (!arr || arr->Num() <= 0)
    {
        LOG("[FusionUnlock] NKMBaseTable MyFmtData empty");
        return;
    }

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
    LOG("[FusionUnlock] NKMBaseTable: zeroed unlockFlags on {} entries", mod);
}

static void PatchRuntimeTables()
{
    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!modBase) return;

    auto getCtx = reinterpret_cast<uintptr_t(*)()>(modBase + 0xB29520);
    uintptr_t ctx = getCtx();
    if (!ctx) return;

    // ── param table (FDevilBaseData, getCtx + 0xD10, stride 0x108) ──
    {
        auto* paramBase = *reinterpret_cast<uint8_t**>(ctx + 0xD10);
        auto paramCount = *reinterpret_cast<uint32_t*>(ctx + 0xD10 + 8);
        if (paramBase && paramCount >= 1)
        {
            int zeroed = 0;
            for (uint32_t id = 1; id < paramCount && id <= 1200; id++)
            {
                uint8_t* entry = paramBase + (uintptr_t)id * 0x108;
                // Zero m_UniteLimit (blocks fusion)
                if (entry[PARAM_UNITE_LIMIT_OFF]) { entry[PARAM_UNITE_LIMIT_OFF] = 0; zeroed++; }
                // Zero m_BibleFromSummonCanNot
                if (entry[PARAM_BIBLE_SUMMON_OFF]) { entry[PARAM_BIBLE_SUMMON_OFF] = 0; zeroed++; }
                // Zero m_EventLimit (int32)
                auto* el = reinterpret_cast<int32*>(entry + PARAM_EVENT_LIMIT_OFF);
                if (*el) { *el = 0; zeroed++; }
                // Zero param[17] (backup)
                if (entry[17]) { entry[17] = 0; zeroed++; }
            }
            LOG("[FusionUnlock] Runtime param table: zeroed {} fields", zeroed);
        }
    }

    // ── NKM runtime table (getCtx + 0x838, stride 0xA8) ──
    {
        uint8_t* nkmBase = *reinterpret_cast<uint8_t**>(ctx + 0x838);
        int nkmCount = *reinterpret_cast<int*>(ctx + 0x838 + 8);
        if (nkmBase && nkmCount >= 1)
        {
            int fixed = 0;
            for (int id = 0; id < nkmCount && id < 1201; id++)
            {
                uint8_t* entry = nkmBase + (uintptr_t)id * 0xA8;
                // If compendium flag is set (+0x50), assume demon is valid;
                // clear any potential blocking fields at +0x52, +0x54, +0x58 etc.
                // Set +0x52 (unknown, may be unlockFlags-derived) to 0
                if (entry[0x52]) { entry[0x52] = 0; fixed++; }
                if (entry[0x53]) { entry[0x53] = 0; fixed++; }
                // Set a known "available for fusion" flag pattern
                if (entry[0x50] == 1)
                {
                    // Demon is in compendium — ensure it's fully unlocked
                    entry[0x58] = 1; // potential "fusion unlocked" flag
                }
            }
            LOG("[FusionUnlock] NKM runtime table: fixed {} flags for {} entries", fixed, nkmCount);
        }
    }
}

static void ApplyFusionUnlocks()
{
    if (s_Patched.load(std::memory_order_acquire)) return;
    PatchNKMBaseTable();
    PatchRuntimeTables();
    s_Patched.store(true, std::memory_order_release);
    LOG("[FusionUnlock] All unlock patches applied");
}

// ── Native hook on execMakeUpUniteTable ──
// When MakeUpUniteTable(UMMIAsset*) is called, we intercept BEFORE
// it processes the data, zero unlockFlags in MyFmtData, then let
// the original run with clean data.
using MakeUpUniteTableFn = void(__fastcall*)(void* self, void* stack);
static MakeUpUniteTableFn s_OrigMakeUp = nullptr;
static std::unique_ptr<PLH::x64Detour> s_MakeUpDetour;

static void __fastcall HkExecMakeUpUniteTable(void* self, void* stack)
{
    // 1) Patch NKMBaseTable source data BEFORE MakeUpUniteTable consumes it.
    //    MakeUpUniteTable reads NKMBaseTable to build UniteTable entries.
    PatchNKMBaseTable();

    // 2) Let the original build the UniteTable from the now-clean data.
    s_OrigMakeUp(self, stack);

    // 3) Force-patch runtime tables that were already populated from the
    //    original (stale) NKMBaseTable data earlier in the boot sequence.
    PatchRuntimeTables();
}

void FusionUnlock::Setup()
{

    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!modBase) return;

    // ── 0) Try patching NKMBaseTable right now — catches assets that
    //        are already loaded during engine init, before any processing. ──
    PatchNKMBaseTable();

    // ── 1) Hook execMakeUpUniteTable (VA 0x14D58B140, RVA 0xD58B140) ──
    uint64_t makeUpTarget = modBase + 0xD58B140;
    uint64_t makeUpHook = reinterpret_cast<uint64_t>(PLH::FnCast(HkExecMakeUpUniteTable, &s_OrigMakeUp));
    uint64_t makeUpOrig = 0;
    auto makeUpDet = std::make_unique<PLH::x64Detour>(makeUpTarget, makeUpHook, &makeUpOrig);
    if (!makeUpDet->hook())
    {
        LOG("[FusionUnlock] FAILED: execMakeUpUniteTable hook at {:x}", makeUpTarget);
        // Fall back to StartDataLoad timing
        goto fallback;
    }
    s_OrigMakeUp = PLH::FnCast(makeUpOrig, s_OrigMakeUp);
    s_MakeUpDetour = std::move(makeUpDet);
    LOG("[FusionUnlock] Hooked execMakeUpUniteTable -> unlockFlags zeroed before processing");
    return;

fallback:
    // ── 2) Fallback: also try UFunction PreHook ──
    for (auto* path : {
        STR("/Script/Project.AUniteCtrl:MakeUpUniteTable"),
        STR("/Script/Project.AUniteCtrl_C:MakeUpUniteTable"),
        STR("/Script/Project.UniteCtrl:MakeUpUniteTable") })
    {
        auto hookId = HookHelper::HookPre(path, [](UnrealScriptFunctionCallableContext&, void*) {
            ApplyFusionUnlocks();
        });
        if (hookId >= 0)
        {
            LOG("[FusionUnlock] PreHook installed on {} (fallback)", path);
            return;
        }
    }

    // ── 3) Last resort: patch at StartDataLoad ──
    LOG("[FusionUnlock] No MakeUpUniteTable hook available; using StartDataLoad fallback");
    HookHelper::HookPost(STR("/Script/Project.SaveLoadBase:StartDataLoad"),
        [](UnrealScriptFunctionCallableContext&, void*) {
            ApplyFusionUnlocks();
        });
}
