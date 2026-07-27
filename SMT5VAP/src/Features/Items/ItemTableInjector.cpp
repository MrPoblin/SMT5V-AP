#include "ItemTableInjector.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FFrame.hpp>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstring>
#include <vector>
#include <string>

using namespace RC;
using namespace RC::Unreal;

static std::mutex s_Mutex;
static std::unordered_map<int32_t, std::wstring> s_CustomItems;
static bool s_Resolved = false;

static constexpr int32 FSM_SIZE  = 0x40;
static constexpr int32 FSM_ALIGN = 8;
static constexpr int32 SMA_MSGS_OFF = 0x28;
static constexpr int32 MMIA_FMT_OFF = 0x28;
static constexpr int32 FMT_DATA_OFF  = 0x55;
static constexpr int32 FMT_DATA_STRIDE = 100;
static constexpr int32 FMT_DATA_COUNT = 114;

static void InjectIntoSMA(UObject* asset) {
    auto* arr = reinterpret_cast<FScriptArray*>(
        reinterpret_cast<uint8_t*>(asset) + SMA_MSGS_OFF);
    if (!arr) return;

    int32 oldNum = arr->Num();

    for (auto& [id, text] : s_CustomItems) {
        bool found = false;
        uint8_t* data = static_cast<uint8_t*>(arr->GetData());
        for (int32 i = 0; i < oldNum; i++) {
            if (*reinterpret_cast<int32_t*>(data + i * FSM_SIZE) == id) {
                found = true;
                break;
            }
        }
        if (found) continue;

        int32 idx = arr->Add(1, FSM_SIZE, FSM_ALIGN);
        uint8_t* entry = static_cast<uint8_t*>(arr->GetData()) + idx * FSM_SIZE;

        *reinterpret_cast<int32_t*>(entry + 0x00) = id;
        *reinterpret_cast<int64_t*>(entry + 0x04) = 0;

        FText ft(text.c_str());
        void* mem = (*GMalloc)->Malloc(sizeof(FText), alignof(FText));
        memcpy(mem, &ft, sizeof(FText));
        if (ft.SharedRefCollector)
            _InterlockedIncrement((volatile long*)((char*)ft.SharedRefCollector + 8));

        *reinterpret_cast<void**>(entry + 0x10)   = mem;
        *reinterpret_cast<int32_t*>(entry + 0x18)  = 1;
        *reinterpret_cast<int32_t*>(entry + 0x1C)  = 1;
        *reinterpret_cast<void**>(entry + 0x20)    = nullptr;
        *reinterpret_cast<int32_t*>(entry + 0x28)  = 0;
        *reinterpret_cast<int32_t*>(entry + 0x2C)  = 0;
        *reinterpret_cast<void**>(entry + 0x30)    = nullptr;
        *reinterpret_cast<int32_t*>(entry + 0x38)  = 0;
        *reinterpret_cast<int32_t*>(entry + 0x3C)  = 0;

        LOG("[ItemTableInjector] SMA injected ID {}: {}", id, text);
    }
}

static void InjectIntoMMIA(UObject* asset) {
    auto* arr = reinterpret_cast<FScriptArray*>(
        reinterpret_cast<uint8_t*>(asset) + MMIA_FMT_OFF);
    if (!arr || arr->Num() == 0) return;

    uint8_t* data = static_cast<uint8_t*>(arr->GetData());
    int32 size = arr->Num();

    struct { int32 pos; int32 magicId; } mutations[32];
    int32 mutCount = 0;

    for (auto& [magicId, customText] : s_CustomItems) {
        if (mutCount >= 32) break;
        for (int32 pos = FMT_DATA_OFF; pos + 100 <= size; pos += FMT_DATA_STRIDE) {
            int32 existingId = *reinterpret_cast<int32*>(data + pos + 0x00);
            if (existingId == 0) {
                bool claimed = false;
                for (int32 m = 0; m < mutCount; m++) {
                    if (mutations[m].pos == pos) { claimed = true; break; }
                }
                if (!claimed) {
                    mutations[mutCount++] = {pos, magicId};
                    break;
                }
            }
        }
    }

    if (mutCount == 0) {
        LOG("[ItemTableInjector] No unused slots found in DataTable source");
        return;
    }

    for (int32 i = 0; i < mutCount; i++) {
        int32 pos = mutations[i].pos;
        *reinterpret_cast<int32*>(data + pos + 0x00) = mutations[i].magicId;
        *reinterpret_cast<int32*>(data + pos + 0x04) = mutations[i].magicId;
        LOG("[ItemTableInjector] MMIA wrote DataTable entry at +{:X}: ID={}", pos, mutations[i].magicId);
    }
}

static void Inject() {
    auto* sma = UObjectGlobals::FindObject(STR("ScriptMessageAsset"), STR("ItemName"));
    if (sma) InjectIntoSMA(sma);

    auto* mmia = UObjectGlobals::FindObject(STR("MMIAsset"), STR("ItemTable"));
    if (mmia) InjectIntoMMIA(mmia);
}

// ── Post-build RowMap injection ────────────────────────────────────────
static void InjectIntoRowMap() {
    std::vector<UObject*> tables;
    UObjectGlobals::FindAllOf(STR("DataTable"), tables);

    LOG("[ItemTableInjector] Scanning {} DataTable objects for injection", tables.size());

    // Log all DataTables and find the best candidate
    UDataTable* bestTable = nullptr;
    int bestRowSize = 0;

    for (auto* obj : tables) {
        auto* table = static_cast<UDataTable*>(obj);
        auto& rowMap = table->GetRowMap();
        auto* rowStruct = table->GetRowStruct().Get();
        int32 rowSize = rowStruct ? static_cast<int32>(rowStruct->GetStructureSize()) : 0;
        std::wstring structName = rowStruct ? rowStruct->GetName() : L"null";

        LOG(L"[ItemTableInjector]   DT '{}': {} rows, size {}, struct '{}'",
            table->GetName(), rowMap.Num(), rowSize, structName);

        // Look for the item DataTable: has row "1" with item ID 1 at +0x14
        if (rowMap.Num() >= 10 && rowSize > 0) {
            FName testKey(STR("1"));
            auto* rowPtrPtr = rowMap.Find(testKey);
            if (rowPtrPtr && *rowPtrPtr) {
                int32 idAt14 = *reinterpret_cast<int32*>(*rowPtrPtr + 0x14);
                LOG("[ItemTableInjector]     -> Key '1': +14={}", idAt14);
                if (idAt14 == 1) {
                    bestTable = table;
                    bestRowSize = rowSize;
                }
            }
        }
    }

    if (!bestTable) {
        LOG("[ItemTableInjector] ERROR: No matching item DataTable found!");
        return;
    }

    auto& rowMap = bestTable->GetRowMap();
    auto* rowStruct = bestTable->GetRowStruct().Get();
    int32 rowSize = bestRowSize;

    LOG("[ItemTableInjector] Selected DataTable '{}': {} rows, row size {}",
        bestTable->GetName(), rowMap.Num(), rowSize);

    // Dump first 10 RowMap entries for diagnostics
    int keyIdx = 0;
    for (auto& pair : rowMap) {
        if (keyIdx >= 10) break;
        auto keyStr = pair.Key.ToString();
        int32_t idAt14 = *reinterpret_cast<int32*>(pair.Value + 0x14);
        int32_t idAt08 = *reinterpret_cast<int32*>(pair.Value + 0x08);
        LOG("[ItemTableInjector]   [{}] Key='{}' +14={} +08={}", keyIdx, keyStr, idAt14, idAt08);
        keyIdx++;
    }

    // Find template row (item ID 1)
    FName templateKey(STR("1"));
    auto* templatePtrPtr = rowMap.Find(templateKey);
    if (!templatePtrPtr || !*templatePtrPtr) {
        LOG("[ItemTableInjector] ERROR: Template row '1' not found!");
        return;
    }
    unsigned char* templateRow = *templatePtrPtr;

    std::lock_guard<std::mutex> lock(s_Mutex);
    for (auto& [magicId, customText] : s_CustomItems) {
        std::wstring idStr = std::to_wstring(magicId);
        FName newName(idStr.c_str());

        if (rowMap.Find(newName)) {
            LOG("[ItemTableInjector] Row {} already exists, skipping", magicId);
            continue;
        }

        // Clone template row
        std::vector<uint8_t> rowBuffer(rowSize);
        memcpy(rowBuffer.data(), templateRow, rowSize);

        // Set item ID at +0x14 and name message ID at +0x08
        *reinterpret_cast<int32*>(rowBuffer.data() + 0x14) = magicId;
        *reinterpret_cast<int32*>(rowBuffer.data() + 0x18) = magicId;
        *reinterpret_cast<int32*>(rowBuffer.data() + 0x08) = magicId;

        // Add to DataTable
        bestTable->AddRow(newName, rowBuffer.data(), rowStruct);

        // Verify
        auto* verifyPtr = rowMap.Find(newName);
        if (verifyPtr && *verifyPtr) {
            int32 vId = *reinterpret_cast<int32*>(*verifyPtr + 0x14);
            int32 vMsg = *reinterpret_cast<int32*>(*verifyPtr + 0x08);
            LOG("[ItemTableInjector] Injected ID {}: +14={} +08={} OK", magicId, vId, vMsg);
        } else {
            LOG("[ItemTableInjector] WARNING: Injection FAILED for ID {}!", magicId);
        }
    }
}

namespace ItemTableInjector {

    void RegisterCustomItem(int32_t id, const wchar_t* name) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        if (name && name[0]) {
            s_CustomItems[id] = name;
        } else {
            s_CustomItems.erase(id);
        }
        LOG("[ItemTableInjector] Registered custom item {}: {}", id, name);
    }

    void ClearCustomItem(int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_CustomItems.erase(id);
    }

    void ClearAllCustomItems() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_CustomItems.clear();
    }

    void InjectRowMap() {
        InjectIntoRowMap();
    }

    void Setup() {
        if (s_Resolved) return;

        auto* makeUp = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.ProjectGameInstanceBase:MakeUpItemDataTable"));
        if (makeUp) {
            makeUp->RegisterPreHook(
                [](UnrealScriptFunctionCallableContext&, void*) {
                    Inject();
                });
            LOG("[ItemTableInjector] MakeUpItemDataTable pre-hook registered");
        } else {
            WARN("[ItemTableInjector] MakeUpItemDataTable not found, injecting immediately");
            Inject();
        }

        s_Resolved = true;
    }
}
