#pragma once
#include <cstdint>

namespace ExpGive {

    // ── Flat debug grants (raw, NO curve) ──
    // Add raw EXP to the protagonist (Nahobino) only.
    void GivePlayerExp(int32_t amount);
    // Add raw EXP to a single party demon by its Nkm index.
    void GiveNkmExp(int32_t nkmIndex, int32_t amount);
    // Add raw EXP to every party demon (active 2x / inactive 1x). Guests excluded.
    void GiveAllDemonsExp(int32_t amount);
    // Add raw EXP to the protagonist + guests + all demons (2x active / 1x inactive).
    void GiveEveryoneExp(int32_t amount);

    // ── Accessors / tuning helpers ──
    // Required EXP for the protagonist to reach the next level from `level`.
    int32_t GetProtagonistNextExp(int32_t level);
    // Required EXP for a demon (`nkmIndex`) to reach the next level from `level`.
    int32_t GetNkmNextExp(int32_t nkmIndex, int32_t level);
    // Relative EXP weight for a demon vs the protagonist. <1 level or >1 level
    // => more or less EXP respectively. See ExpGive.cpp for the tunable curve.
    double DemonWeight(int32_t demonLevel, int32_t protagLevel);

    // ── Relative / bundle grants ──
    // `relative` is a fraction of the protagonist's next-level required EXP.
    // Grants the protagonist that amount (protagonist is always active -> 1x unit),
    // then each demon scaled by level (lower demons get more) and participation
    // (active = 1x unit, inactive = 0.5x unit). Guests are active (1x unit).
    void GiveRelativeExp(double relative);
    // Protagonist only.
    void GiveRelativeExpToPlayer(double relative);

    // ── Tiered EXP bundles ──
    enum class ExpTier {
        Tiny,
        Small,
        Medium,
        Large,
        Huge,
        Boss,
    };
    // Grants an EXP bundle of the given tier (relative fraction from the tunable
    // table in ExpGive.cpp). This is the primary API for Archipelago EXP items.
    void GiveExpBundle(ExpTier tier);

}
