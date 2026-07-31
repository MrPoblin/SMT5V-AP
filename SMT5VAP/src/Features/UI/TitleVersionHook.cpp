#include "TitleVersionHook.hpp"
#include "src/ModInfo.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include "src/Archipelago/APManager.hpp"
#include "src/Helper/StringHelper.hpp"
#include <Unreal/FText.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <chrono>

using namespace RC::Unreal;

namespace TitleVersionHook
{
    std::wstring VersionText{};

    static auto s_LastPollTime = std::chrono::steady_clock::now();
    static constexpr auto POLL_INTERVAL = std::chrono::seconds(1);

    static UClass* s_WidgetClass = nullptr;
    static UFunction* s_SetTextFunc = nullptr;
    static UObject* s_CachedTextBlock = nullptr;

    static void SetVersionText()
    {
        if (!s_CachedTextBlock)
        {
            auto numElements = FUObjectArray::GetNumElements();
            for (int32_t i = 0; i < numElements; i++)
            {
                auto* item = FUObjectArray::IndexToObject(i);
                if (!item) continue;
                auto* obj = item->GetUObject();
                if (!obj || obj->HasAnyFlags(RF_ClassDefaultObject)) continue;
                if (s_WidgetClass)
                {
                    if (obj->GetClassPrivate() != s_WidgetClass) continue;
                }
                else if (obj->GetClassPrivate()->GetName() != STR("WB_TitleMainMenu_2_C"))
                {
                    continue;
                }

                auto** textBlockPtr = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(obj) + 0x0328);
                if (!textBlockPtr || !*textBlockPtr) continue;

                s_WidgetClass = obj->GetClassPrivate();
                s_CachedTextBlock = *textBlockPtr;
                LOG("[TitleVersionHook] cached TextVersionNumber at 0x{:X}", uintptr_t(s_CachedTextBlock));
                break;
            }

            if (!s_CachedTextBlock) return;
        }

        if (!s_SetTextFunc)
            s_SetTextFunc = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/UMG.TextBlock:SetText"));
        if (!s_SetTextFunc)
        {
            LOG("[TitleVersionHook] failed to find UMG.TextBlock:SetText");
            return;
        }

        struct { FText Text; } params;
        params.Text = FText(VersionText);
        s_CachedTextBlock->ProcessEvent(s_SetTextFunc, &params);
    }

    void Setup() {
        GameState::OnMapChanged([](const std::wstring& MapName) {
            s_WidgetClass = nullptr;
            s_SetTextFunc = nullptr;
            s_CachedTextBlock = nullptr;
        });
    }

    void Tick() {
        auto now = std::chrono::steady_clock::now();
        if (now - s_LastPollTime < POLL_INTERVAL) return;
        s_LastPollTime = now;

        VersionText = std::wstring(ModInfo::Name) + L" " + std::wstring(ModInfo::Version) +
            L"\nby " + std::wstring(ModInfo::Authors) +
            L"\n" + StringHelper::ToWide(AP::getAPConnectedStatus());
        SetVersionText();
    }
}
