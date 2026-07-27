#include "ItemTableInjector.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FFrame.hpp>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstring>

using namespace RC;
using namespace RC::Unreal;

static std::mutex s_Mutex;
static std::unordered_map<int32_t, std::wstring> s_CustomItems;
static bool s_Resolved = false;

static constexpr int32 FSM_SIZE  = 0x40;
static constexpr int32 FSM_ALIGN = 8;
static constexpr int32 SMA_MSGS_OFF = 0x28;

// Inject custom item names into the ScriptMessageAsset (SMA).
// This is a pre-hook on MakeUpItemDataTable so names are available
// when the DataTable is built.
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

        *reinterpret_cast<void**>(entry + 0x10)  = mem;
        *reinterpret_cast<int32_t*>(entry + 0x18) = 1;
        *reinterpret_cast<int32_t*>(entry + 0x1C) = 1;
        *reinterpret_cast<void**>(entry + 0x20)   = nullptr;
        *reinterpret_cast<int32_t*>(entry + 0x28) = 0;
        *reinterpret_cast<int32_t*>(entry + 0x2C) = 0;
        *reinterpret_cast<void**>(entry + 0x30)   = nullptr;
        *reinterpret_cast<int32_t*>(entry + 0x38) = 0;
        *reinterpret_cast<int32_t*>(entry + 0x3C) = 0;

        LOG("[ItemTableInjector] SMA injected ID {}: {}", id, text);
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
    }

    void ClearCustomItem(int32_t id) {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_CustomItems.erase(id);
    }

    void ClearAllCustomItems() {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_CustomItems.clear();
    }

    void Setup() {
        if (s_Resolved) return;

        auto* makeUp = UObjectGlobals::FindObject<UFunction>(nullptr,
            STR("/Script/Project.ProjectGameInstanceBase:MakeUpItemDataTable"));
        if (makeUp) {
            makeUp->RegisterPreHook(
                [](UnrealScriptFunctionCallableContext&, void*) {
                    auto* sma = UObjectGlobals::FindObject(STR("ScriptMessageAsset"), STR("ItemName"));
                    if (sma) InjectIntoSMA(sma);
                });
            LOG("[ItemTableInjector] MakeUpItemDataTable pre-hook registered");
        }

        s_Resolved = true;
    }
}
