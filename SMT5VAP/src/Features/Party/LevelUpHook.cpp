#include "LevelUpHook.hpp"
#include "src/Helper/HookHelper.hpp"
#include "src/Features/Party/LevelFunctions.hpp"
#include "src/Log/Log.hpp"
#include <polyhook2/Detour/x64Detour.hpp>
#include <mutex>
#include <vector>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

namespace LevelUpTick {

// ── Callbacks ──
static std::vector<LevelUpCallback> s_Callbacks;
static std::mutex s_Mutex;

// ── Level tracking / dedup ──
static int32 s_CachedLevel = -1;
static bool s_LevelKnown = false;

// ── Last level-up (for DebugUI) ──
static int32 s_LastLevelUpOld = -1;
static int32 s_LastLevelUpNew = -1;

// ── Native hook state ──
// FUN_147248ad0(uint8 playerType, int32 levelGain) is the protagonist level-up
// core: it loops levelGain times incrementing the level and applying growth.
// EVERY level-up source funnels through it — battle exp (FUN_140b68380 calls
// thunk_FUN_147248ad0(2, gained, 0) DIRECTLY from native code, bypassing the
// UFunction execs, which is why UFunction hooks never fire), PlayerGrowUp exec,
// PayerForceSpecifiedLevel and the level-sync. It is also exercised by boy/aogami
// slot sync (playerType 0/1) — those leave the NAHOBINO slot unchanged and are
// filtered by the level dedup.
static constexpr int64_t LEVEL_UP_CORE_ADDR = 0x147248AD0;
// First 16 bytes: MOV [rsp+0x10],edx; PUSH RBX/RSI/RDI/R13; SUB rsp,0x98
static const char* LEVEL_UP_CORE_SIG = "89 54 24 10 53 56 57 41 55 48 81 EC 98 00 00 00";

// GameDataInst getter (FUN_140b253b0 = FUN_140b29520() + 0x290).
static constexpr int64_t GAME_DATA_INST_ADDR = 0x140b253b0;
// Party char data layout (same as the game's own level-up core):
//   charBase = *(GameDataInst + 0x10)
//   charEntry = charBase + playerType * 0x130
//   level (int32) at charEntry + 0x68
static constexpr int64_t CHAR_DATA_BASE_OFFSET = 0x10;
static constexpr int64_t CHAR_ENTRY_STRIDE = 0x130;
static constexpr int64_t CHAR_LEVEL_OFFSET = 0x68;
static constexpr uint8_t kProtagonistType = 2; // E_PLAYER_NAHOBINO

using LevelUpCoreFn = int64_t(__fastcall*)(uint8_t playerType, int32_t levelGain);
static LevelUpCoreFn s_LevelUpCoreOrig = nullptr;
static std::unique_ptr<PLH::x64Detour> s_LevelUpCoreDetour;

static void Fire(int32 oldLevel, int32 newLevel) {
    s_LastLevelUpOld = oldLevel;
    s_LastLevelUpNew = newLevel;
    LOG("[LevelUpTick] Protagonist levelled up: {} -> {} (gained {})",
        oldLevel, newLevel, newLevel - oldLevel);
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& cb : s_Callbacks) cb(oldLevel, newLevel);
}

// Fires the callback at most once per level increase.
static void DetectLevelChange(int32 level) {
    if (level < 1) return;

    if (!s_LevelKnown) {
        s_CachedLevel = level;
        s_LevelKnown = true;
        DEBUG("[LevelUpTick] Baseline established at level {}", level);
        return;
    }

    if (level > s_CachedLevel) {
        Fire(s_CachedLevel, level);
        s_CachedLevel = level;
    } else if (level < s_CachedLevel) {
        // Level dropped (NG+ reset, etc.) — resync, don't fire
        s_CachedLevel = level;
    }
}

// Reads the protagonist (NAHOBINO) level directly from party data using the
// same layout the game's level-up core uses. No ProcessEvent, so this is safe
// to call from inside the detour.
static int32 ReadProtagonistLevel() {
    using GetGameDataInstFn = int64_t(*)();
    auto GetGameDataInst = reinterpret_cast<GetGameDataInstFn>(GAME_DATA_INST_ADDR);
    int64_t inst = GetGameDataInst();
    if (!inst) return -1;
    int64_t charBase = *reinterpret_cast<int64_t*>(inst + CHAR_DATA_BASE_OFFSET);
    if (!charBase) return -1;
    return *reinterpret_cast<int32_t*>(charBase + kProtagonistType * CHAR_ENTRY_STRIDE + CHAR_LEVEL_OFFSET);
}

// Native hook on the level-up core. The level-up is already applied when this
// runs, so we read the resulting protagonist level and fire on the increase.
int64_t __fastcall HkLevelUpCore(uint8_t playerType, int32_t levelGain) {
    int64_t result = s_LevelUpCoreOrig(playerType, levelGain);
    if (levelGain > 0) {
        DetectLevelChange(ReadProtagonistLevel());
    }
    return result;
}

static bool InstallLevelUpHook(uint64_t targetAddr) {
    if (targetAddr < 0x140000000 || targetAddr > 0x160000000) {
        WARN(STR("[LevelUpTick] Level-up core target {:#x} out of range"), targetAddr);
        return false;
    }
    uint64_t origAddr = 0;
    auto det = std::make_unique<PLH::x64Detour>(targetAddr,
        reinterpret_cast<uint64_t>(PLH::FnCast(HkLevelUpCore, &s_LevelUpCoreOrig)),
        &origAddr);
    if (!det->hook()) {
        WARN(STR("[LevelUpTick] x64Detour on level-up core FAILED at {:#x}"), targetAddr);
        return false;
    }
    s_LevelUpCoreOrig = reinterpret_cast<LevelUpCoreFn>(origAddr);
    s_LevelUpCoreDetour = std::move(det);
    LOG(STR("[LevelUpTick] Native hook installed on level-up core at {:#x}"), targetAddr);
    return true;
}

void Setup() {
    LevelFunctions::Setup();

    if (s_LevelUpCoreDetour) return;

    uint64_t target = SignatureScanner::FindPattern(LEVEL_UP_CORE_SIG);
    if (target) {
        LOG(STR("[LevelUpTick] Level-up core located via signature at {:#x}"), target);
    } else {
        target = LEVEL_UP_CORE_ADDR;
        WARN(STR("[LevelUpTick] Level-up core signature NOT FOUND, using hardcoded {:#x}"), target);
    }

    InstallLevelUpHook(target);
}

void OnLevelUp(LevelUpCallback cb) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Callbacks.push_back(std::move(cb));
}

void Reset() {
    s_LevelKnown = false;
    s_CachedLevel = -1;
    // Seed the baseline now (save-load context is safe) so the first real
    // level-up after load fires correctly.
    int32 level = ReadProtagonistLevel();
    if (level >= 1) {
        s_CachedLevel = level;
        s_LevelKnown = true;
    }
}

int32 GetCachedLevel() {
    return s_LevelKnown ? s_CachedLevel : -1;
}

int32 LastLevelUpOld() {
    return s_LastLevelUpOld;
}

int32 LastLevelUpNew() {
    return s_LastLevelUpNew;
}

}
