#include "SkillBlocker.hpp"
#include "src/HookHelper.hpp"
#include "src/Archipelago/APState.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <vector>
#include <set>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

namespace SkillBlocker {

static UFunction* s_GetSkillBaseDataFn = nullptr;
static UObject* s_SkillDataCDO = nullptr;
static bool s_SkillDataInitFailed = false;

static void InitSkillDataLookup() {
    if (s_GetSkillBaseDataFn || s_SkillDataInitFailed) return;
    for (auto* p : { STR("/Script/Project.BPL_SkillData:GetSkillBaseData"), STR("/Script/Project.BPL_SkillData_C:GetSkillBaseData") }) {
        s_GetSkillBaseDataFn = UObjectGlobals::FindObject<UFunction>(nullptr, p);
        if (s_GetSkillBaseDataFn) break;
    }
    if (!s_GetSkillBaseDataFn) { WARN("[SkillBlocker] GetSkillBaseData NOT FOUND"); s_SkillDataInitFailed = true; return; }
    for (auto* p : { STR("/Script/Project.Default__BPL_SkillData"), STR("/Script/Project.Default__BPL_SkillData_C") }) {
        s_SkillDataCDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, p);
        if (s_SkillDataCDO) break;
    }
    if (!s_SkillDataCDO) {
        auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_SkillData"));
        if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_SkillData_C"));
        if (cls) s_SkillDataCDO = cls->CreateDefaultObject();
    }
    if (!s_SkillDataCDO) { WARN("[SkillBlocker] CDO NOT FOUND"); s_SkillDataInitFailed = true; return; }
}

// FSkillBaseData::m_Icon is at offset 0x18 (24), NOT 36. 36 was reading into m_TekiseiType TArray.
static constexpr int32_t SKILL_ICON_OFFSET = 0x18;

static int32_t GetSkillIconFromData(int32_t skillId) {
    InitSkillDataLookup();
    if (!s_GetSkillBaseDataFn || !s_SkillDataCDO) return -1;
    auto* retProp = s_GetSkillBaseDataFn->GetPropertyByName(STR("ReturnValue"));
    if (!retProp) return -1;
    std::vector<uint8> params(0x400, 0);
    *reinterpret_cast<int32*>(params.data()) = skillId;
    s_SkillDataCDO->ProcessEvent(s_GetSkillBaseDataFn, params.data());
    uint8* structPtr = retProp->ContainerPtrToValuePtr<uint8>(params.data());
    return static_cast<int32_t>(structPtr[SKILL_ICON_OFFSET]);
}

static std::vector<int32_t> s_SkillIconCache;
static bool s_SkillIconCacheReady = false;

static void BuildSkillIconCache() {
    if (s_SkillIconCacheReady) return;
    InitSkillDataLookup();
    s_SkillIconCache.assign(2048, -1);
    if (s_GetSkillBaseDataFn && s_SkillDataCDO) {
        for (int id = 1; id < (int)s_SkillIconCache.size(); ++id) {
            int32_t icon = GetSkillIconFromData(id);
            if (icon >= 0 && icon <= 12) s_SkillIconCache[id] = icon;
        }
        LOG("[SkillBlocker] Cache built ({} ids)", (int)s_SkillIconCache.size());
    } else { WARN("[SkillBlocker] Cache NOT built"); }
    s_SkillIconCacheReady = true;
}

static int32_t CachedSkillIcon(int32_t skillId) {
    if (skillId <= 0 || (size_t)skillId >= s_SkillIconCache.size()) return -1;
    return s_SkillIconCache[skillId];
}

bool IsSkillCategoryBlocked(int32_t skillId) {
    int32_t icon = CachedSkillIcon(skillId);
    if (icon < 0 || icon >= APState::SkillCategories::CATEGORY_COUNT) return false;
    return APState::SkillCategories::IsCategoryBlocked(icon);
}

int32_t GetSkillIcon(int32_t skillId) {
    return CachedSkillIcon(skillId);
}

bool IsSkillBlockedInBattle(int32_t skillId, int32_t partyIndex) {
    int32_t icon = CachedSkillIcon(skillId);
    if (icon < 0 || icon >= APState::SkillCategories::CATEGORY_COUNT) return false;
    if (!APState::SkillCategories::IsCategoryBlocked(icon)) return false;

    // Check scope
    auto scope = APState::SkillCategories::GetScope();
    if (scope == APState::SkillCategories::Scope::Both) return true;

    // partyIndex 0 = protagonist (convention in battle system)
    bool isProtag = (partyIndex == 0);
    if (scope == APState::SkillCategories::Scope::ProtagOnly) return isProtag;
    if (scope == APState::SkillCategories::Scope::DemonOnly) return !isProtag;
    return true;
}

static bool s_GrantBypass = false;

void SetGrantBypass(bool granting) {
    s_GrantBypass = granting;
}

// ── Discovery logging ──
static bool s_LogAllBattleFunctions = false;
static std::set<std::wstring> s_LoggedFunctions;

void SetDiscoveryLogging(bool enabled) { s_LogAllBattleFunctions = enabled; }
bool IsDiscoveryLoggingEnabled() { return s_LogAllBattleFunctions; }

// ── Native polyhook2 hooks ──
// CreatePartySkillList real body (FUN_140b9a9c0)
// Signature: int64_t __fastcall(int64_t widget, uint32_t partyIndex)
// Builds TArray at widget+0x228 (data), count at widget+0x220
// Each entry 0x18 bytes: skillId@+0, memberIndex@+8
using CreatePartySkillListFn = int64_t(*)(int64_t, uint32_t);
static CreatePartySkillListFn s_CreatePartySkillListOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_CreatePartySkillListDetour;

static constexpr int64_t CREATE_PARTY_SKILL_LIST_ADDR = 0x140b9a9c0;
static constexpr int64_t SKILL_LIST_COUNT_OFFSET = 0x220;
static constexpr int64_t SKILL_LIST_DATA_OFFSET = 0x228;
static constexpr int32_t SKILL_LIST_ENTRY_SIZE = 0x18;

int64_t __fastcall HkCreatePartySkillList(int64_t widget, uint32_t partyIndex) {
    int64_t result = s_CreatePartySkillListOrig(widget, partyIndex);

    int32_t count = *reinterpret_cast<int32_t*>(widget + SKILL_LIST_COUNT_OFFSET);
    uint8_t* data = *reinterpret_cast<uint8_t**>(widget + SKILL_LIST_DATA_OFFSET);

    LOG(STR("[SkillBlocker] CreatePartySkillList fired! widget={:#x} partyIndex={:#x} count={}"),
        static_cast<uint64_t>(widget), partyIndex, count);

    if (data && count > 0) {
        int32_t w = 0;
        for (int32_t i = 0; i < count; i++) {
            int32_t skillId = *reinterpret_cast<int32_t*>(data + SKILL_LIST_ENTRY_SIZE * i);
            if (!IsSkillCategoryBlocked(skillId)) {
                if (w != i) memcpy(data + SKILL_LIST_ENTRY_SIZE * w,
                                   data + SKILL_LIST_ENTRY_SIZE * i, SKILL_LIST_ENTRY_SIZE);
                w++;
            }
        }
        if (w < count) {
            *reinterpret_cast<int32_t*>(widget + SKILL_LIST_COUNT_OFFSET) = w;
            LOG("[SkillBlocker] Removed {} blocked skills from CreatePartySkillList!", count - w);
        }
    }

    return result;
}

// Exec thunk (FUN_140c89760) — Kismet VM entry point
// Signature: void __fastcall(int64_t context, int64_t frame, uint8_t* result)
using ExecCreatePartySkillListFn = void(*)(int64_t, int64_t, uint8_t*);
static ExecCreatePartySkillListFn s_ExecCreatePartySkillListOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_ExecCreatePartySkillListDetour;

static constexpr int64_t EXEC_CREATE_PARTY_SKILL_LIST_ADDR = 0x140c89760;

void __fastcall HkExecCreatePartySkillList(int64_t context, int64_t frame, uint8_t* result) {
    LOG(STR("[SkillBlocker] ExecCreatePartySkillList fired! context={:#x}"), static_cast<uint64_t>(context));
    s_ExecCreatePartySkillListOrig(context, frame, result);
}

static bool InstallNativeHook(uint64_t targetAddr, uint64_t hookFn,
    uint64_t* origOut, std::unique_ptr<PLH::x64Detour>& detourOut, const char* hookName)
{
    std::wstring wname(hookName, hookName + strlen(hookName));
    if (targetAddr < 0x140000000 || targetAddr > 0x160000000) {
        WARN(STR("[SkillBlocker] Native hook '{}' target {:#x} out of range"), wname, targetAddr);
        return false;
    }
    uint64_t origAddr = 0;
    auto det = std::make_unique<PLH::x64Detour>(targetAddr, hookFn, &origAddr);
    if (!det->hook()) {
        WARN(STR("[SkillBlocker] Native x64Detour '{}' FAILED at {:#x}"), wname, targetAddr);
        return false;
    }
    *origOut = origAddr;
    detourOut = std::move(det);
    LOG(STR("[SkillBlocker] Native hook '{}' installed at {:#x}"), wname, targetAddr);
    return true;
}

// ── ProcessEvent pre-callback ──
void Setup() {
    LOG("[SkillBlocker] Setting up...");
    BuildSkillIconCache();

    // Find SetSkill UFunction to resolve parameter offsets
    static int32 s_SkillIdOfs = -1;
    static int32 s_UsableOfs = -1;
    static int32 s_CostOKOfs = -1;
    
    auto* obj = UObjectGlobals::FindObject(STR("Function"), STR("SetSkill"));
    if (obj) {
        auto* fn = Cast<UFunction>(obj);
        if (fn) {
            auto* idProp = fn->GetPropertyByName(STR("SkillId"));
            auto* usableProp = fn->GetPropertyByName(STR("Usable"));
            auto* costOkProp = fn->GetPropertyByName(STR("CostOK"));
            if (idProp) s_SkillIdOfs = idProp->GetOffset_ForInternal();
            if (usableProp) s_UsableOfs = usableProp->GetOffset_ForInternal();
            if (costOkProp) s_CostOKOfs = costOkProp->GetOffset_ForInternal();
            LOG("[SkillBlocker] SetSkill params: SkillId@{} Usable@{} CostOK@{}",
                s_SkillIdOfs, s_UsableOfs, s_CostOKOfs);
        }
    }

    // Pre-resolve ApplyUsable UFunction on WB_BtlListMenuPart_C (to update visuals)
    static UFunction* s_ApplyUsableFn = nullptr;
    if (!s_ApplyUsableFn) {
        for (auto* p : { 
            STR("/Game/Blueprints/UI/Battle/BtlListMenu/WB_BtlListMenuPart.WB_BtlListMenuPart_C:ApplyUsable"),
            STR("/Script/Project.WB_BtlListMenuPart_C:ApplyUsable"),
            STR("WB_BtlListMenuPart_C:ApplyUsable"),
            STR("ApplyUsable") 
        }) {
            auto* obj = UObjectGlobals::FindObject<UFunction>(nullptr, p);
            if (obj) { s_ApplyUsableFn = obj; LOG(STR("[SkillBlocker] ApplyUsable found via: {}"), p); break; }
        }
        if (!s_ApplyUsableFn) {
            // Try finding class first, then function on it
            for (auto* clsPath : {
                STR("/Game/Blueprints/UI/Battle/BtlListMenu/WB_BtlListMenuPart.WB_BtlListMenuPart_C"),
                STR("/Script/Project.WB_BtlListMenuPart_C"),
                STR("WB_BtlListMenuPart_C"),
                STR("/Script/Project.WB_BtlListMenuPart")
            }) {
                auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, clsPath);
                if (cls) {
                    s_ApplyUsableFn = cls->GetFunctionByNameInChain(STR("ApplyUsable"));
                    if (s_ApplyUsableFn) {
                        LOG(STR("[SkillBlocker] ApplyUsable found on class: {}"), clsPath);
                        break;
                    }
                }
            }
        }
        if (!s_ApplyUsableFn) {
            WARN(STR("[SkillBlocker] ApplyUsable UFunction NOT FOUND - visuals won't update. Need UE4SS inspector to find correct path."));
        }
    }

    Hook::ProcessEventCallback cb = [](UObject* Context, UFunction* Function, void* Parms) {
        if (!Function || !Parms || s_GrantBypass) return;
        auto name = Function->GetName();

        // Discovery logging
        if (s_LogAllBattleFunctions) {
            if (s_LoggedFunctions.find(name) == s_LoggedFunctions.end()) {
                s_LoggedFunctions.insert(name);
                LOG(STR("[SkillBlocker-DISC] new fn: {} ctx={:#x}"), name, reinterpret_cast<uint64_t>(Context));
            }
        }

        // SetSkillHelpText — fires on every cursor move in battle skill menu
        // Context = BattleCommand_C, m_ListMenu at +0x0460
        // ListParts entries are FBtl_ListMenu_T (0x20 bytes): MenuPart ptr at +0x00
        // Each WB_BtlListMenuPart_C: ID at +0x02DC, Usable at +0x02E8, CostOK at +0x0300
        if (name == STR("SetSkillHelpText") && Parms) {
            int32_t skillId = *reinterpret_cast<int32_t*>(Parms);
            if (skillId <= 0) return;

            // Get ListMenu from BattleCommand
            UObject* listMenu = *reinterpret_cast<UObject**>(
                static_cast<uint8_t*>(static_cast<void*>(Context)) + 0x0460);
            if (!listMenu) return;

            uint8_t* lmBase = static_cast<uint8_t*>(static_cast<void*>(listMenu));

            // Read SkillData array to also modify source data
            uint8_t* skillData = *reinterpret_cast<uint8_t**>(lmBase + 0x02E0);
            int32_t skillCount = *reinterpret_cast<int32_t*>(lmBase + 0x02E0 + 8);

            // Read ListParts array (FBtl_ListMenu_T entries, 0x20 bytes each)
            uint8_t* listPartsData = *reinterpret_cast<uint8_t**>(lmBase + 0x0300);
            int32_t partCount = *reinterpret_cast<int32_t*>(lmBase + 0x0300 + 8);

            // Also set m_IsUsed=true in SkillData source for blocked skills
            if (skillData && skillCount > 0) {
                constexpr int32_t SKILL_ENTRY = 0x16;
                for (int32_t i = 0; i < skillCount; i++) {
                    int32_t sid = *reinterpret_cast<int32_t*>(skillData + SKILL_ENTRY * i);
                    if (sid > 0 && IsSkillCategoryBlocked(sid)) {
                        skillData[SKILL_ENTRY * i + 0x04] = 1; // m_IsUsed = true
                    }
                }
            }

            // Gray out blocked ListParts and call ApplyUsable to update visuals
            if (listPartsData && partCount > 0) {
                constexpr int32_t LISTPART_ENTRY = 0x20;
                for (int32_t i = 0; i < partCount; i++) {
                    UObject* menuPart = *reinterpret_cast<UObject**>(listPartsData + LISTPART_ENTRY * i);
                    if (!menuPart) continue;
                    uint8_t* partBase = static_cast<uint8_t*>(static_cast<void*>(menuPart));
                    int32_t partSkillId = *reinterpret_cast<int32_t*>(partBase + 0x02DC);
                    if (partSkillId > 0 && IsSkillCategoryBlocked(partSkillId)) {
                        partBase[0x02E8] = 0; // Usable = false
                        partBase[0x0300] = 0; // CostOK = false
                        // Call ApplyUsable to update the visual rendering
                        if (s_ApplyUsableFn) {
                            s_GrantBypass = true;
                            menuPart->ProcessEvent(s_ApplyUsableFn, nullptr);
                            s_GrantBypass = false;
                        }
                    }
                }
            }
        }

        // Intercept SetSkill on BtlListMenuPart to gray out blocked skills at source
        if (name == STR("SetSkill") && s_SkillIdOfs >= 0 && s_UsableOfs >= 0) {
            int32_t skillId = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(Parms) + s_SkillIdOfs);
            if (skillId > 0 && IsSkillCategoryBlocked(skillId)) {
                *reinterpret_cast<bool*>(static_cast<uint8_t*>(Parms) + s_UsableOfs) = false;
                if (s_CostOKOfs >= 0) {
                    *reinterpret_cast<bool*>(static_cast<uint8_t*>(Parms) + s_CostOKOfs) = false;
                }
            }
        }
    };

    Hook::RegisterProcessEventPreCallback(cb);

    // ── Install native polyhook2 hooks ──
    // CreatePartySkillList real body — the function that builds the battle skill list
    InstallNativeHook(CREATE_PARTY_SKILL_LIST_ADDR,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkCreatePartySkillList, &s_CreatePartySkillListOrig)),
        reinterpret_cast<uint64_t*>(&s_CreatePartySkillListOrig),
        s_CreatePartySkillListDetour, "CreatePartySkillList");

    // Exec thunk — Kismet VM entry point (fallback, may not fire if VM dispatches differently)
    InstallNativeHook(EXEC_CREATE_PARTY_SKILL_LIST_ADDR,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkExecCreatePartySkillList, &s_ExecCreatePartySkillListOrig)),
        reinterpret_cast<uint64_t*>(&s_ExecCreatePartySkillListOrig),
        s_ExecCreatePartySkillListDetour, "ExecCreatePartySkillList");

    LOG(STR("[SkillBlocker] Setup complete (discovery logging={})"), s_LogAllBattleFunctions ? STR("ON") : STR("OFF"));
}

void BuildCache() {
    BuildSkillIconCache();
}

} // namespace SkillBlocker
