#pragma once

namespace PopupSuppression {
    void Setup();
    void SetBlockNextSpawn(bool block);

    // Per-source toggles
    void SetBlockChests(bool block);
    void SetBlockRelics(bool block);
    void SetBlockAogamiDebris(bool block);
    bool IsBlockingChests();
    bool IsBlockingRelics();
    bool IsBlockingAogamiDebris();
}
