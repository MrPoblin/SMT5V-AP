#pragma once

namespace PopupSuppression {
    void Setup();
    void SetBlockNextSpawn(bool block);

    // Per-source toggles (checked by ChestHooks/RelicHooks)
    void SetBlockChests(bool block);
    void SetBlockRelics(bool block);
    bool IsBlockingChests();
    bool IsBlockingRelics();
}
