#include "ExpGive.hpp"
#include "src/Features/Party/LevelFunctions.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <vector>
#include <cmath>

using namespace RC;
using namespace RC::Unreal;

namespace ExpGive {

// ── BPL_PartyData accessor (shared by all exp functions) ──
static struct {
    UFunction* AddPlayerExpFunc = nullptr;
    UFunction* AddNkmExpFunc = nullptr;
    UFunction* GetNkmBattleUnitListFunc = nullptr;
    UFunction* GetEntryNkmNumFunc = nullptr;
    UFunction* GetNkmIndexByEntryListFunc = nullptr;
    UFunction* NkmIsBattleUnitFunc = nullptr;
    UFunction* GetPlayerNextExpFunc = nullptr;
    UFunction* GetNkmNextExpFunc = nullptr;
    UObject* CDO = nullptr;
    bool InitFailed = false;

    void Init() {
        if (AddPlayerExpFunc && AddNkmExpFunc && CDO) return;
        if (InitFailed) return;

        AddPlayerExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:AddPlayerExp"));
        if (!AddPlayerExpFunc)
            AddPlayerExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:AddPlayerExp"));

        AddNkmExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:AddNkmExp"));
        if (!AddNkmExpFunc)
            AddNkmExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:AddNkmExp"));

        GetNkmBattleUnitListFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:GetNkmBattleUnitList"));
        if (!GetNkmBattleUnitListFunc)
            GetNkmBattleUnitListFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:GetNkmBattleUnitList"));

        GetEntryNkmNumFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:GetEntryNkmNum"));
        if (!GetEntryNkmNumFunc)
            GetEntryNkmNumFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:GetEntryNkmNum"));

        GetNkmIndexByEntryListFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:GetNkmIndexByEntryList"));
        if (!GetNkmIndexByEntryListFunc)
            GetNkmIndexByEntryListFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:GetNkmIndexByEntryList"));

        NkmIsBattleUnitFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:NkmIsBattleUnit"));
        if (!NkmIsBattleUnitFunc)
            NkmIsBattleUnitFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:NkmIsBattleUnit"));

        GetPlayerNextExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:GetPlayerNextExp_Level"));
        if (!GetPlayerNextExpFunc)
            GetPlayerNextExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:GetPlayerNextExp_Level"));

        GetNkmNextExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.BPL_PartyData:GetNkmNextExp_Level"));
        if (!GetNkmNextExpFunc)
            GetNkmNextExpFunc = UObjectGlobals::FindObject<UFunction>(nullptr,
                STR("/Script/Project.BPL_PartyData_C:GetNkmNextExp_Level"));

        CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
            STR("/Script/Project.Default__BPL_PartyData"));
        if (!CDO)
            CDO = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr,
                STR("/Script/Project.Default__BPL_PartyData_C"));
        if (!CDO) {
            auto* cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData"));
            if (!cls) cls = UObjectGlobals::FindObject<UClass>(nullptr, STR("/Script/Project.BPL_PartyData_C"));
            if (cls) CDO = cls->CreateDefaultObject();
        }

        if (!AddPlayerExpFunc || !AddNkmExpFunc || !CDO) {
            WARN("[ExpGive] Failed to find BPL_PartyData exp functions or CDO");
            InitFailed = true;
        } else {
            LOG("[ExpGive] BPL_PartyData exp functions resolved");
        }
    }
} s_Party;

static bool NkmIsBattleUnit(int32_t nkmIndex) {
    if (!s_Party.NkmIsBattleUnitFunc || !s_Party.CDO) return false;
    struct { int32_t NkmIndex; bool ReturnValue; } params{ nkmIndex, false };
    s_Party.CDO->ProcessEvent(s_Party.NkmIsBattleUnitFunc, &params);
    return params.ReturnValue;
}

// Required EXP for the protagonist to go from `level` to `level+1`.
int32_t GetProtagonistNextExp(int32_t level) {
    s_Party.Init();
    if (!s_Party.GetPlayerNextExpFunc || !s_Party.CDO) return 1;
    struct { int32_t Level; int32_t ReturnValue; } params{ level, 1 };
    s_Party.CDO->ProcessEvent(s_Party.GetPlayerNextExpFunc, &params);
    return params.ReturnValue > 0 ? params.ReturnValue : 1;
}

// Required EXP for a demon at `nkmIndex` to go from `level` to `level+1`.
int32_t GetNkmNextExp(int32_t nkmIndex, int32_t level) {
    s_Party.Init();
    if (!s_Party.GetNkmNextExpFunc || !s_Party.CDO) return 1;
    struct { int32_t NkmIndex; int32_t Level; int32_t ReturnValue; } params{ nkmIndex, level, 1 };
    s_Party.CDO->ProcessEvent(s_Party.GetNkmNextExpFunc, &params);
    return params.ReturnValue > 0 ? params.ReturnValue : 1;
}

// ── Demon EXP weight ──
// Scales a demon's grant relative to the protagonist's level. A demon below the
// protagonist's level gets weight > 1 (more EXP); above gets < 1 (less). The
// CUSTOM curve is the exponential fit from battle data; the CSV_RATIO curve uses
// the game's own required-EXP tables (lower-level demons need less EXP per level,
// so granting them the protagonist's amount yields more effective progress).
// Tunable: flip kUseCsvRatio and/or adjust kW.
static constexpr bool kUseCsvRatio = false;
static constexpr double kW = 0.11029;

double DemonWeight(int32_t demonLevel, int32_t protagLevel) {
    if (kUseCsvRatio) {
        int32_t pNext = GetProtagonistNextExp(protagLevel);
        int32_t dNext = GetNkmNextExp(-1, demonLevel); // -1: level-only lookup path
        if (dNext <= 0) return 1.0;
        return static_cast<double>(pNext) / static_cast<double>(dNext);
    }
    // CUSTOM: exponential centered on the protagonist's level.
    return std::exp(-kW * static_cast<double>(demonLevel - protagLevel));
}

// ── Raw award helpers (direct UFunction calls, no scaling) ──
static void AwardPlayerExpRaw(int32_t amount) {
    if (!s_Party.AddPlayerExpFunc || !s_Party.CDO) return;
    struct { uint8_t playerType; int32_t getExp; } params{ LevelFunctions::kProtagonist, amount };
    s_Party.CDO->ProcessEvent(s_Party.AddPlayerExpFunc, &params);
}

static void AwardNkmExpRaw(int32_t nkmIndex, int32_t amount) {
    if (!s_Party.AddNkmExpFunc || !s_Party.CDO) return;
    struct { int32_t NkmIndex; int32_t getExp; } params{ nkmIndex, amount };
    s_Party.CDO->ProcessEvent(s_Party.AddNkmExpFunc, &params);
}

// Every Nkm in the party's entry (reserve) list: active (battle-unit) demons,
// inactive (reserve) demons, and guests. This is the full set the game iterates
// when distributing battle EXP.
static std::vector<int32_t> GetEntryNkmIndices() {
    std::vector<int32_t> indices;
    if (!s_Party.GetEntryNkmNumFunc || !s_Party.GetNkmIndexByEntryListFunc || !s_Party.CDO)
        return indices;

    struct FNumRet { int32_t ReturnValue; };
    FNumRet numRet{};
    s_Party.CDO->ProcessEvent(s_Party.GetEntryNkmNumFunc, &numRet);

    for (int32_t i = 0; i < numRet.ReturnValue; ++i) {
        struct FIdxParams { int32_t entryIndex; int32_t ReturnValue; };
        FIdxParams params{};
        params.entryIndex = i;
        s_Party.CDO->ProcessEvent(s_Party.GetNkmIndexByEntryListFunc, &params);
        if (params.ReturnValue >= 0) indices.push_back(params.ReturnValue);
    }
    return indices;
}

// ── Flat debug grants (raw, NO curve) ──
void GivePlayerExp(int32_t amount) {
    s_Party.Init();
    if (amount <= 0) return;
    AwardPlayerExpRaw(amount);
    LOG("[ExpGive] Gave {} raw EXP to protagonist", amount);
}

void GiveNkmExp(int32_t nkmIndex, int32_t amount) {
    s_Party.Init();
    if (amount <= 0) return;
    AwardNkmExpRaw(nkmIndex, amount);
    LOG("[ExpGive] Gave {} raw EXP to demon[{}]", amount, nkmIndex);
}

void GiveAllDemonsExp(int32_t amount) {
    s_Party.Init();
    if (!s_Party.AddNkmExpFunc || !s_Party.CDO) return;
    if (amount <= 0) return;
    int32_t activeCount = 0;
    int32_t inactiveCount = 0;
    for (int32_t idx : GetEntryNkmIndices()) {
        int32_t exp = NkmIsBattleUnit(idx) ? amount : amount / 2;
        AwardNkmExpRaw(idx, exp);
        if (NkmIsBattleUnit(idx)) ++activeCount; else ++inactiveCount;
    }
    LOG("[ExpGive] Gave {} raw EXP to {} active + {} inactive member(s)", amount, activeCount, inactiveCount);
}

void GiveEveryoneExp(int32_t amount) {
    s_Party.Init();
    if (amount <= 0) return;
    GivePlayerExp(amount);
    GiveAllDemonsExp(amount);
}

// ── Relative / bundle grants ──
// `relative` is a fraction of the protagonist's next-level required EXP.
// The protagonist receives `protagExp = relative * GetProtagonistNextExp(pl)`.
// Each demon receives `protagExp * DemonWeight(demonLevel, pl) * (2 if active else 1)`.
void GiveRelativeExp(double relative) {
    s_Party.Init();
    if (relative <= 0.0) return;

    int32_t pl = LevelFunctions::GetProtagonistLevel();
    if (pl < 1) {
        WARN("[ExpGive] GiveRelativeExp: protagonist level unavailable");
        return;
    }

    int32_t protagExp = static_cast<int32_t>(relative * GetProtagonistNextExp(pl));
    if (protagExp <= 0) return;

    // Protagonist is ALWAYS active -> gets the full 1x unit.
    AwardPlayerExpRaw(protagExp);
    LOG("[ExpGive] Relative grant: protagonist (L{}) +{} EXP (relative={})", pl, protagExp, relative);

    // Every party Nkm (demons AND guests): active (battle-unit) = 1x unit,
    // inactive (reserve) = 0.5x unit, each scaled by level via DemonWeight.
    int32_t activeCount = 0;
    int32_t inactiveCount = 0;
    for (int32_t idx : GetEntryNkmIndices()) {
        double w = DemonWeight(LevelFunctions::GetNkmLevel(idx), pl);
        double mult = NkmIsBattleUnit(idx) ? 1.0 : 0.5;
        int32_t exp = static_cast<int32_t>(protagExp * w * mult);
        AwardNkmExpRaw(idx, exp);
        if (NkmIsBattleUnit(idx)) ++activeCount; else ++inactiveCount;
    }
    LOG("[ExpGive] Relative grant: {} active + {} inactive member(s)", activeCount, inactiveCount);
}

void GiveRelativeExpToPlayer(double relative) {
    s_Party.Init();
    if (relative <= 0.0) return;
    int32_t pl = LevelFunctions::GetProtagonistLevel();
    if (pl < 1) return;
    int32_t protagExp = static_cast<int32_t>(relative * GetProtagonistNextExp(pl));
    if (protagExp <= 0) return;
    AwardPlayerExpRaw(protagExp);
    LOG("[ExpGive] Relative grant (player only): +{} EXP (relative={})", protagExp, relative);
}

// ── Tiered bundles ──
// Single tunable table: relative fraction (of protagonist's next-level EXP) per tier.
// Generated values, definitely not using them
static constexpr double kTierRelative[] = {
    0.05,   // Tiny
    0.15,   // Small
    0.40,   // Medium
    1.00,   // Large
    2.50,   // Huge
    5.00,   // Boss
};

void GiveExpBundle(ExpTier tier) {
    int32_t idx = static_cast<int32_t>(tier);
    if (idx < 0 || idx >= static_cast<int32_t>(std::size(kTierRelative))) {
        WARN("[ExpGive] GiveExpBundle: invalid tier {}", idx);
        return;
    }
    GiveRelativeExp(kTierRelative[idx]);
}

}
