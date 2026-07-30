#include "TitleVersionHook.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/FText.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <chrono>

using namespace RC::Unreal;

namespace TitleVersionHook
{
    constexpr std::wstring_view VERSION_TEXT{L"SMT5VAP 0.1.0"};

    static auto s_LastPollTime = std::chrono::steady_clock::now();
    static constexpr auto POLL_INTERVAL = std::chrono::seconds(1);

    static void SetVersionText()
    {
        auto numElements = FUObjectArray::GetNumElements();
        for (int32_t i = 0; i < numElements; i++)
        {
            auto* item = FUObjectArray::IndexToObject(i);
            if (!item) continue;
            auto* obj = item->GetUObject();
            if (!obj || obj->HasAnyFlags(RF_ClassDefaultObject)) continue;
            if (obj->GetClassPrivate()->GetName() != STR("WB_TitleMainMenu_2_C")) continue;

            auto** textBlockPtr = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(obj) + 0x0328);
            if (!textBlockPtr || !*textBlockPtr) continue;

            auto* setTextFn = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/UMG.TextBlock:SetText"));
            if (!setTextFn) continue;

            struct { FText Text; } params;
            params.Text = FText(VERSION_TEXT);
            (*textBlockPtr)->ProcessEvent(setTextFn, &params);
        }
    }

    void Tick() {
        auto now = std::chrono::steady_clock::now();
        if (now - s_LastPollTime < POLL_INTERVAL) return;
        s_LastPollTime = now;

        SetVersionText();
    }
}
