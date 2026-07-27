#pragma once
#include <cstdint>
#include <string>

namespace ItemTableInjector {
    void Setup();

    // Register a custom item name before the game builds the item DataTable.
    // id: magic item ID, name: display text
    void RegisterCustomItem(int32_t id, const wchar_t* name);
    void ClearCustomItem(int32_t id);
    void ClearAllCustomItems();

    // Inject custom rows into the DataTable's RowMap.
    // Call this AFTER MakeUpItemDataTable has been called (e.g., after ItemLimits::Raise).
    void InjectRowMap();
}
