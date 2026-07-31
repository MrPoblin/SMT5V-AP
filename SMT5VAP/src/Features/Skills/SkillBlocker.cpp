#include "SkillBlocker.hpp"
#include "src/Helper/HookHelper.hpp"
#include "src/Archipelago/APState.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

namespace SkillBlocker {

static UFunction* s_GetSkillBaseDataFn = nullptr;
static UObject* s_SkillDataCDO = nullptr;
static bool s_SkillDataInitFailed = false;

// Skills that should NEVER be blocked under any circumstances:
// - Magatsuhi skills (special gauge skills)
// - Basic attack (1) and guard (2) — essential gameplay skills
static const std::unordered_set<int32_t> s_NeverBlockSkills = {
    60,76,78,87,109,110,111,112,113,114,
    120,121,130,131,138,145,146,147,177,
    187,193,201,215,249,274,309,345,928,
    801,802,803,804,805,806,807,808,809,
    810,811,812,813,814,815,816,817,818,
    819,820,821,822,823,824,825,826,857
};

static bool IsNeverBlockSkill(int32_t skillId) {
    return s_NeverBlockSkills.count(skillId) > 0;
}

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
    if (IsNeverBlockSkill(skillId)) return false;
    int32_t icon = CachedSkillIcon(skillId);
    if (icon < 0 || icon >= APState::SkillCategories::CATEGORY_COUNT) return false;
    return APState::SkillCategories::IsCategoryBlocked(icon);
}

int32_t GetSkillIcon(int32_t skillId) {
    return CachedSkillIcon(skillId);
}

bool IsSkillBlockedInBattle(int32_t skillId, int32_t partyIndex) {
    if (IsNeverBlockSkill(skillId)) return false;
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

// ── Cached BattleCommand_C ──
static UObject* s_CachedBattleCommand = nullptr;



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

// SetAutoBattleSkillCommandAndTargetSelect real body (FUN_140a092d0)
// ABattleCharaActionBase member: void __fastcall(this, int32_t SkillId)
// Selects targets for the given skill during auto-battle.
// If the skill is blocked, we replace it with basic attack (skill 1).
using AutoBattleSkillSelectFn = void(*)(int64_t, int32_t);
static AutoBattleSkillSelectFn s_AutoBattleSkillSelectOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_AutoBattleSkillSelectDetour;

static constexpr int64_t AUTO_BATTLE_SKILL_SELECT_ADDR = 0x140a092d0;

// SetAutoBattleCommandAndTargetSelect real body (FUN_140a09220)
// Non-skill auto-battle (attack/guard/etc)
using AutoBattleCommandSelectFn = void(*)(int64_t);
static AutoBattleCommandSelectFn s_AutoBattleCommandSelectOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_AutoBattleCommandSelectDetour;

static constexpr int64_t AUTO_BATTLE_COMMAND_SELECT_ADDR = 0x140a09220;

// SetNowCommandSetSkill real body (FUN_146e4cc10)
// UBattlePartySystemComponentBase method:
//   void __fastcall(UBattlePartySystemComponentBase* this, int32_t partyIndex, int32_t skillId, int32_t target)
// Writes the SKILL command (type 2) to party member's m_NowCmd.
using SetNowCommandSetSkillFn = void(*)(uint64_t, int32_t, int32_t, int32_t);
static SetNowCommandSetSkillFn s_SetNowCommandSetSkillOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_SetNowCommandSetSkillDetour;

static constexpr int64_t SET_NOW_COMMAND_SET_SKILL_ADDR = 0x146e4cc10;

// SetNowCommand real body (FUN_146e3f5e0)
// UBattlePartySystemComponentBase method:
//   void __fastcall(UBattlePartySystemComponentBase* this, int32_t partyIndex, FBtlCommand* command)
// Writes the entire FBtlCommand (16 bytes) to party member's m_NowCmd.
// FBtlCommand layout: m_Command(byte@0), m_Index(byte@1), m_TargetArea(byte@2), pad(byte@3),
//   m_TargetForm(int32@4), m_SkillId(int32@8), m_Turn(int32@12)
using SetNowCommandFn = void(*)(uint64_t, int32_t, uint8_t*);
static SetNowCommandFn s_SetNowCommandOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_SetNowCommandDetour;

static constexpr int64_t SET_NOW_COMMAND_ADDR = 0x146e3f5e0;

// SetNowCommandSetAttack real body (FUN_146e3fee0)
// Writes ATTACK command (type 1, skillId=0) to party member's m_NowCmd.
static constexpr int64_t SET_NOW_COMMAND_SET_ATTACK_ADDR = 0x146e3fee0;

int64_t __fastcall HkCreatePartySkillList(int64_t widget, uint32_t partyIndex) {
    int64_t result = s_CreatePartySkillListOrig(widget, partyIndex);

    int32_t count = *reinterpret_cast<int32_t*>(widget + SKILL_LIST_COUNT_OFFSET);
    uint8_t* data = *reinterpret_cast<uint8_t**>(widget + SKILL_LIST_DATA_OFFSET);

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
    s_ExecCreatePartySkillListOrig(context, frame, result);
}

// Auto-battle skill selection hook
// ABattleCharaActionBase::SetAutoBattleSkillCommandAndTargetSelect(int32 SkillId)
// NOTE: Observed to NEVER fire — auto-battle uses a completely different code path.
void __fastcall HkAutoBattleSkillSelect(int64_t thisPtr, int32_t skillId) {
    s_AutoBattleSkillSelectOrig(thisPtr, skillId);
}

void __fastcall HkAutoBattleCommandSelect(int64_t thisPtr) {
    LOG(STR("[SkillBlocker] AutoBattleCommandSelect NATIVE HOOK FIRED! thisPtr={:#x}"), static_cast<uint64_t>(thisPtr));
    s_AutoBattleCommandSelectOrig(thisPtr);
}

// SetNowCommandSetSkill hook — THE definitive interception point.
// This function writes the SKILL command with skillId to the party member's m_NowCmd.
// Both manual and auto-battle go through this to commit the skill command.
void __fastcall HkSetNowCommandSetSkill(uint64_t thisPtr, int32_t partyIndex, int32_t skillId, int32_t target) {
    // Check if we're in ITEM mode: m_SelectedCommand at +0x04E0 == 2
    // If s_CachedBattleCommand is null (e.g., auto-battle), isItem defaults to false → blocking runs normally
    bool isItem = false;
    if (s_CachedBattleCommand) {
        uint8_t selCmd = *reinterpret_cast<uint8_t*>(
            static_cast<uint8_t*>(static_cast<void*>(s_CachedBattleCommand)) + 0x04E0);
        isItem = (selCmd == 2);
    }

    if (!isItem && partyIndex >= 0 && partyIndex <= 3) {
        if (skillId > 0 && !IsNeverBlockSkill(skillId) && IsSkillBlockedInBattle(skillId, partyIndex)) {
            reinterpret_cast<void(__fastcall*)(uint64_t, int32_t, int32_t)>(SET_NOW_COMMAND_SET_ATTACK_ADDR)(thisPtr, partyIndex, target);
            return;
        }
    }
    s_SetNowCommandSetSkillOrig(thisPtr, partyIndex, skillId, target);
}

// SetNowCommand hook — backup interception for cases where the command is set via FBtlCommand struct.
// If the command is SKILL (m_Command==2) with a blocked skill ID, overwrite to ATTACK.
void __fastcall HkSetNowCommand(uint64_t thisPtr, int32_t partyIndex, uint8_t* command) {
    if (partyIndex >= 0 && partyIndex <= 3 && command && command[0] == 2) {
        int32_t skillId = *reinterpret_cast<int32_t*>(command + 8);
        if (skillId > 0 && !IsNeverBlockSkill(skillId) && IsSkillBlockedInBattle(skillId, partyIndex)) {
            int32_t targetForm = *reinterpret_cast<int32_t*>(command + 4);
            reinterpret_cast<void(__fastcall*)(uint64_t, int32_t, int32_t)>(SET_NOW_COMMAND_SET_ATTACK_ADDR)(thisPtr, partyIndex, targetForm);
            return;
        }
    }
    s_SetNowCommandOrig(thisPtr, partyIndex, command);
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

    // Resolve BIESet*Panel UFunction param offsets (camp graying)
    struct BiePanelInfo { int32 skillIdOfs; int32 useSkillOfs; };
    static std::unordered_map<std::wstring, BiePanelInfo> s_BiePanelFns;

    auto resolveBie = [&](const std::wstring& name, const wchar_t* logName) {
        if (s_BiePanelFns.count(name)) return;
        std::vector<std::wstring> paths = {
            L"/Script/Project.CampSkillCtrlBase:" + name,
            L"/Script/Project.StatusSkillCtrlBase:" + name,
            L"/Game/Blueprints/UI/Camp/Skill/BP_CampSkillCtrl.BP_CampSkillCtrl_C:" + name,
            name,
        };
        for (auto& p : paths) {
            auto* fn = UObjectGlobals::FindObject<UFunction>(nullptr, p.c_str());
            if (fn) {
                int32 idOfs = -1, useOfs = -1;
                auto* idProp = fn->GetPropertyByName(STR("InSkillId"));
                auto* useProp = fn->GetPropertyByName(STR("InUseSkill"));
                if (idProp) idOfs = idProp->GetOffset_ForInternal();
                if (useProp) useOfs = useProp->GetOffset_ForInternal();
                s_BiePanelFns[name] = {idOfs, useOfs};
                LOG(STR("[SkillBlocker] {} found via: {} SkillId@{} UseSkill@{}"), logName, p, idOfs, useOfs);
                return;
            }
        }
        LOG(STR("[SkillBlocker] {} NOT FOUND"), logName);
    };

    resolveBie(L"BIESetSkillPanel", STR("BIESetSkillPanel"));
    resolveBie(L"BIESetUniquePanel", STR("BIESetUniquePanel"));
    resolveBie(L"BIESetMagatsuhiPanel", STR("BIESetMagatsuhiPanel"));

    // Also resolve BIESetSkillNameColor (fires after BIESetSkillPanel, resets text color)
    static int32 s_BieNameColor_IndexOfs = -1;
    static int32 s_BieNameColor_UseSkillOfs = -1;
    if (s_BieNameColor_IndexOfs < 0) {
        for (auto& p : {
            std::wstring(L"/Script/Project.CampSkillCtrlBase:BIESetSkillNameColor"),
            std::wstring(L"/Script/Project.StatusSkillCtrlBase:BIESetSkillNameColor"),
            std::wstring(L"BIESetSkillNameColor"),
        }) {
            auto* fn = UObjectGlobals::FindObject<UFunction>(nullptr, p.c_str());
            if (fn) {
                auto* idxProp = fn->GetPropertyByName(STR("InIndex"));
                auto* useProp = fn->GetPropertyByName(STR("InUseSkill"));
                if (idxProp) s_BieNameColor_IndexOfs = idxProp->GetOffset_ForInternal();
                if (useProp) s_BieNameColor_UseSkillOfs = useProp->GetOffset_ForInternal();
                LOG(STR("[SkillBlocker] BIESetSkillNameColor found via: {} Index@{} UseSkill@{}"), p, s_BieNameColor_IndexOfs, s_BieNameColor_UseSkillOfs);
                break;
            }
        }
        if (s_BieNameColor_IndexOfs < 0) {
            LOG(STR("[SkillBlocker] BIESetSkillNameColor NOT FOUND"));
        }
    }

    // Track panelIndex → skillId per Context (for BIESetSkillNameColor state tracking)
    // Updated on EVERY BIESetSkillPanel call so reused panel indices get the correct skillId
    static std::unordered_map<uint64_t, std::unordered_map<int32_t, int32_t>> s_PanelSkills;

    // Temp camp discovery — log every BIE/Camp/Status/Unite function once
    static std::unordered_set<std::wstring> s_CampDiscovered;
    static bool s_CampDiscoveryOn = true;

    // Helper: apply blocking to a BattleCommand_C's skill list
    auto applyBlockingToListMenu = [](UObject* ctx, UFunction* applyUsable) {
        UObject* listMenu = *reinterpret_cast<UObject**>(
            static_cast<uint8_t*>(static_cast<void*>(ctx)) + 0x0460);
        if (!listMenu) return;
        uint8_t* lmBase = static_cast<uint8_t*>(static_cast<void*>(listMenu));
        bool isSkill = *reinterpret_cast<bool*>(lmBase + 0x02C0);
        if (!isSkill) return;

        uint8_t* skillData = *reinterpret_cast<uint8_t**>(lmBase + 0x02E0);
        int32_t skillCount = *reinterpret_cast<int32_t*>(lmBase + 0x02E0 + 8);
        if (skillData && skillCount > 0) {
            constexpr int32_t SKILL_ENTRY = 0x16;
            for (int32_t i = 0; i < skillCount; i++) {
                int32_t sid = *reinterpret_cast<int32_t*>(skillData + SKILL_ENTRY * i);
                if (sid > 0 && IsSkillCategoryBlocked(sid)) {
                    skillData[SKILL_ENTRY * i + 0x04] = 1;
                }
            }
        }

        uint8_t* listPartsData = *reinterpret_cast<uint8_t**>(lmBase + 0x0300);
        int32_t partCount = *reinterpret_cast<int32_t*>(lmBase + 0x0300 + 8);
        if (listPartsData && partCount > 0) {
            constexpr int32_t LISTPART_ENTRY = 0x20;
            for (int32_t i = 0; i < partCount; i++) {
                UObject* menuPart = *reinterpret_cast<UObject**>(listPartsData + LISTPART_ENTRY * i);
                if (!menuPart) continue;
                uint8_t* partBase = static_cast<uint8_t*>(static_cast<void*>(menuPart));
                int32_t partSkillId = *reinterpret_cast<int32_t*>(partBase + 0x02DC);
                if (partSkillId > 0 && IsSkillCategoryBlocked(partSkillId)) {
                    partBase[0x02E8] = 0;
                    partBase[0x0300] = 0;
                    if (applyUsable) {
                        s_GrantBypass = true;
                        menuPart->ProcessEvent(applyUsable, nullptr);
                        s_GrantBypass = false;
                    }
                }
            }
        }
    };

    Hook::ProcessEventCallbackWithData cb = [applyBlockingToListMenu](Hook::TCallbackIterationData<void>&, UObject* Context, UFunction* Function, void* Parms) {
        if (!Function || !Parms || s_GrantBypass) return;
        auto name = Function->GetName();

        // Cache BattleCommand_C from events that fire on it
        if (name == STR("ExecuteUbergraph_BattleCommand") || name == STR("SetSkillHelpText") || name == STR("SetSkillAishouNotice") || name == STR("OnCommandListClicked")) {
            s_CachedBattleCommand = Context;
        }

        // ExecuteUbergraph_BattleCommand — re-block UI every frame to catch skip-turn desync
        if (name == STR("ExecuteUbergraph_BattleCommand")) {
            s_CachedBattleCommand = Context;
            applyBlockingToListMenu(Context, s_ApplyUsableFn);
        }
        
        // OnCommandListClicked — user selects from main command menu
        // Only re-block if the command is SKILL (1). ITEM/other commands should pass untouched.
        // Params[0] = E_BTL_UI_COMMAND (NONE=0, SKILL=1, ITEM=2, SUMMON=3, ...)
        if (name == STR("OnCommandListClicked")) {
            s_CachedBattleCommand = Context;
            if (Parms && *reinterpret_cast<uint8_t*>(Parms) == 1) {
                applyBlockingToListMenu(Context, s_ApplyUsableFn);
            }
        }
        
        // OnListMenuMoveCursor — fires when cursor moves within the skill sub-menu
        if (name == STR("OnListMenuMoveCursor")) {
            applyBlockingToListMenu(Context, s_ApplyUsableFn);
        }

        // Camp graying: intercept BIESetSkillPanel / BIESetUniquePanel / BIESetMagatsuhiPanel
        {
            auto it = s_BiePanelFns.find(name);
            if (it != s_BiePanelFns.end() && it->second.skillIdOfs >= 0 && it->second.useSkillOfs >= 0 && Parms) {
                int32_t skillId = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(Parms) + it->second.skillIdOfs);
                int32_t panelIndex = *reinterpret_cast<int32_t*>(Parms);
                // Always store panelIndex -> skillId mapping (updates on reuse)
                if (skillId > 0) s_PanelSkills[reinterpret_cast<uint64_t>(Context)][panelIndex] = skillId;
                if (skillId > 0 && IsSkillCategoryBlocked(skillId)) {
                    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(Parms) + it->second.useSkillOfs) = 0;
                }
            }
        }

        // BIESetSkillNameColor — fires AFTER BIESetSkillPanel and resets name color
        // Check stored skillId for the panel index: only override if that skill is blocked
        if (name == STR("BIESetSkillNameColor") && s_BieNameColor_IndexOfs >= 0 && s_BieNameColor_UseSkillOfs >= 0 && Parms) {
            int32_t panelIndex = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(Parms) + s_BieNameColor_IndexOfs);
            auto ctxIt = s_PanelSkills.find(reinterpret_cast<uint64_t>(Context));
            if (ctxIt != s_PanelSkills.end()) {
                auto skillIt = ctxIt->second.find(panelIndex);
                if (skillIt != ctxIt->second.end() && skillIt->second > 0 && IsSkillCategoryBlocked(skillIt->second)) {
                    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(Parms) + s_BieNameColor_UseSkillOfs) = 0;
                }
            }
        }

        // Temp camp discovery — log unknown BIE/Camp/Status/Unite/SkillPanel functions once
        if (s_CampDiscoveryOn && s_CampDiscovered.find(name) == s_CampDiscovered.end()) {
            bool isRelevant = (name.find(STR("BIE")) == 0 || name.find(STR("Camp")) == 0 ||
                name.find(STR("Status")) == 0 || name.find(STR("Unite")) == 0 ||
                name.find(STR("SkillPanel")) == 0 || name.find(STR("SkillList")) == 0 ||
                name.find(STR("TopMenu")) == 0 || name.find(STR("CharaPanel")) == 0 ||
                name.find(STR("Inherit")) == 0 || name.find(STR("Utsusemi")) == 0);
            if (isRelevant) {
                s_CampDiscovered.insert(name);
                LOG(STR("[SkillBlocker-DISC] {} on {:#x}"), name, reinterpret_cast<uint64_t>(Context));
            }
        }

        // SetSkillHelpText / SetSkillAishouNotice — fires on cursor move in battle skill menu; cache BattleCommand_C
        if ((name == STR("SetSkillHelpText") || name == STR("SetSkillAishouNotice")) && Parms) {
            applyBlockingToListMenu(Context, s_ApplyUsableFn);
        }


    };

    Hook::RegisterProcessEventPreCallback(cb, Hook::FCallbackOptions{});

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

    // Auto-battle skill selection — blocks AI from using blocked skills
    InstallNativeHook(AUTO_BATTLE_SKILL_SELECT_ADDR,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkAutoBattleSkillSelect, &s_AutoBattleSkillSelectOrig)),
        reinterpret_cast<uint64_t*>(&s_AutoBattleSkillSelectOrig),
        s_AutoBattleSkillSelectDetour, "AutoBattleSkillSelect");

    // Auto-battle command (non-skill) — debug: does this fire?
    InstallNativeHook(AUTO_BATTLE_COMMAND_SELECT_ADDR,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkAutoBattleCommandSelect, &s_AutoBattleCommandSelectOrig)),
        reinterpret_cast<uint64_t*>(&s_AutoBattleCommandSelectOrig),
        s_AutoBattleCommandSelectDetour, "AutoBattleCommandSelect");

    // SetNowCommandSetSkill — writes skill command to party m_NowCmd (catches manual + auto-battle)
    InstallNativeHook(SET_NOW_COMMAND_SET_SKILL_ADDR,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkSetNowCommandSetSkill, &s_SetNowCommandSetSkillOrig)),
        reinterpret_cast<uint64_t*>(&s_SetNowCommandSetSkillOrig),
        s_SetNowCommandSetSkillDetour, "SetNowCommandSetSkill");

    // SetNowCommand — writes generic command (FBtlCommand) to party m_NowCmd (backup)
    InstallNativeHook(SET_NOW_COMMAND_ADDR,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkSetNowCommand, &s_SetNowCommandOrig)),
        reinterpret_cast<uint64_t*>(&s_SetNowCommandOrig),
        s_SetNowCommandDetour, "SetNowCommand");

    LOG(STR("[SkillBlocker] Setup complete"));
}

void BuildCache() {
    BuildSkillIconCache();
}

} // namespace SkillBlocker
