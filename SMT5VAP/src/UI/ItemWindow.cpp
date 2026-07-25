#include "ItemWindow.hpp"
#include "src/Data/IdToItemName.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/FText.hpp>
#include <string>
#include <string_view>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace {

    enum class Phase { Idle, Opening, Holding, Closing, Gap };

    struct Entry {
        std::int32_t itemId;
        std::int32_t itemNum;
        std::int32_t itemAdd;
        std::wstring textOverride; // non-empty = use this text instead of lookup
    };

    struct State {
        Phase phase = Phase::Idle;
        float timer = 0.0f;
        int openFrames = 0;
        float flushDelay = 0.0f;

        UClass* widgetClass = nullptr;
        UObject* widget = nullptr;
        UObject* ctrl = nullptr;

        Entry active{ -1, 0, 0 };
        std::deque<Entry> queue;
        std::chrono::steady_clock::time_point lastUpdate;

        ItemWindow::ShownCallback shownCb;

        bool usingController = false;
        bool ctrlExists = false;

        static constexpr float ShowDuration = 3.2f;
        static constexpr float GapDuration = 0.2f;
        static constexpr float FadeDuration = 0.25f;
    };
    State S;

    std::atomic<bool> g_SetupAttempted{ false };
    std::atomic<bool> g_SetupComplete{ false };

    UFunction* FindFunc(UStruct* S, const FName& N) {
        for (auto* F : TFieldRange<UFunction>(S, EFieldIterationFlags::IncludeAll))
            if (F && F->GetFName() == N) return F;
        return nullptr;
    }

    void SetByte(UObject* O, UClass* C, const TCHAR* P, uint8 V) {
        if (auto* Prop = C->GetPropertyByNameInChain(P))
            if (auto* D = Prop->ContainerPtrToValuePtr<uint8>(O)) *D = V;
    }

    void SetObj(UObject* O, UClass* C, const TCHAR* P, UObject* V) {
        if (auto* Prop = C->GetPropertyByNameInChain(P))
            if (auto* D = Prop->ContainerPtrToValuePtr<UObject*>(O)) *D = V;
    }

    UObject* GetPC(UObject* WC) {
        auto* GS = UObjectGlobals::FindObject<UObject>(nullptr, STR("/Script/Engine.GameplayStatics"));
        if (!GS || !WC) return nullptr;
        static UFunction* Fn = nullptr;
        if (!Fn) Fn = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Engine.GameplayStatics:GetPlayerController"));
        if (!Fn) return nullptr;
        struct { const UObject* W; int32 I; UObject* R; } P{ WC, 0, nullptr };
        GS->ProcessEvent(Fn, &P);
        return P.R;
    }

    UObject* CreateW(UClass* Cls, UObject* PC, UObject* outer) {
        auto* WBL = UObjectGlobals::FindObject<UObject>(nullptr, STR("/Script/UMG.WidgetBlueprintLibrary"));
        if (!WBL) return nullptr;
        static UFunction* Fn = nullptr;
        if (!Fn) Fn = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        if (!Fn) return nullptr;
        struct { UObject* WC; UClass* T; UObject* PC; UObject* R; } P{ outer, Cls, PC, nullptr };
        WBL->ProcessEvent(Fn, &P);
        return P.R;
    }

    bool Alive() {
        if (!S.widget) return false;
        auto* I = FUObjectArray::IndexToObject(S.widget->GetInternalIndex());
        return I && FUObjectArray::IsValid(I, false);
    }

    void Kill() {
        if (S.widget && Alive()) {
            if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("RemoveFromParent"), FNAME_Add)))
                S.widget->ProcessEvent(F, nullptr);
        }
        S.widget = nullptr;
        S.ctrl = nullptr;
        S.active = { -1, 0, 0 };
        S.usingController = false;
    }

    void CaptureSS() {
        if (!S.widget) return;
        auto* P = S.widget->GetClassPrivate()->GetPropertyByNameInChain(STR("SsPlayerItemWindow"));
        if (!P) P = S.widget->GetClassPrivate()->GetPropertyByNameInChain(STR("SsPlayer"));
        if (!P) return;
        auto* SS = *static_cast<UObject**>(P->ContainerPtrToValuePtr<UObject*>(S.widget));
        if (SS) SetByte(SS, SS->GetClassPrivate(), STR("bReflectParentAlpha"), 1);
    }

    static void SetItemText(const std::wstring& txt) {
        if (!S.widget) return;
        UClass* WC = S.widget->GetClassPrivate();

        auto setRichText = [&](const TCHAR* propName) {
            auto* RP = WC->GetPropertyByNameInChain(propName);
            if (!RP) return;
            auto* RT = *static_cast<UObject**>(RP->ContainerPtrToValuePtr<UObject*>(S.widget));
            if (!RT) return;
            UClass* RTC = RT->GetClassPrivate();
            if (auto* Fn = FindFunc(RTC, FName(STR("SetText"), FNAME_Add))) {
                struct { FText T; } P;
                P.T = FText(txt.c_str());
                RT->ProcessEvent(Fn, &P);
            }
            SetByte(RT, RTC, STR("bReveal"), 0);
            if (auto* P = RTC->GetPropertyByNameInChain(STR("RevealedIndex")))
                if (auto* D = P->ContainerPtrToValuePtr<int32>(RT)) *D = 9999;
        };
        setRichText(STR("MessageRichTextWidget_102"));

        if (auto* RP = WC->GetPropertyByNameInChain(STR("TextBlockItemName"))) {
            if (auto* TB = *static_cast<UObject**>(RP->ContainerPtrToValuePtr<UObject*>(S.widget))) {
                UClass* TBC = TB->GetClassPrivate();
                if (auto* Fn = FindFunc(TBC, FName(STR("SetText"), FNAME_Add))) {
                    struct { FText T; } P;
                    P.T = FText(txt.c_str());
                    TB->ProcessEvent(Fn, &P);
                }
            }
        }

        SetByte(S.widget, WC, STR("IsSetText"), 1);
        SetByte(S.widget, WC, STR("IsTextPlay"), 1);
    }

    bool InitWidget() {
        if (S.widget) return true;

        if (!S.widgetClass) {
            auto* Ctrl = UObjectGlobals::FindFirstOf(STR("BP_ItemWindowCtrl_C"));
            if (Ctrl) {
                S.ctrl = Ctrl;
                S.ctrlExists = true;
                UClass* CC = Ctrl->GetClassPrivate();
                if (auto* Prop = CC->GetPropertyByNameInChain(STR("WB_ItemWindow")))
                    if (auto* Obj = CastField<FObjectPropertyBase>(Prop))
                        S.widgetClass = Obj->GetPropertyClass();
            }

            if (!S.widgetClass) {
                auto* Obj = UObjectGlobals::FindObject(STR("UClass"), STR("WB_ItemWindow_C"));
                if (Obj) S.widgetClass = static_cast<UClass*>(Obj);
            }

            if (!S.widgetClass) {
                LOG("[ItemWindow] InitWidget: cannot resolve WB_ItemWindow_C");
                return false;
            }
        }

        if (!S.ctrl)
            S.ctrl = UObjectGlobals::FindFirstOf(STR("BP_ItemWindowCtrl_C"));

        UObject* outer = S.ctrl ? S.ctrl : (UObject*)UObjectGlobals::FindFirstOf(STR("GameInstance"));
        if (!outer) outer = UObjectGlobals::FindFirstOf(STR("World"));

        auto* PC = GetPC(outer);
        if (!PC) {
            LOG("[ItemWindow] InitWidget: no PlayerController");
            return false;
        }

        auto* W = CreateW(S.widgetClass, PC, outer);
        if (!W) {
            LOG("[ItemWindow] InitWidget: CreateW failed");
            return false;
        }

        S.widget = W;

        if (S.ctrl)
            SetObj(S.ctrl, S.ctrl->GetClassPrivate(), STR("WB_ItemWindow"), S.widget);

        CaptureSS();
        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("AddToViewport"), FNAME_Add))) {
            int32 Z = 0;
            S.widget->ProcessEvent(F, &Z);
        }
        CaptureSS();
        return true;
    }

    static std::wstring MakeText(int32 itemId, int32 itemNum, const std::wstring& overrideText) {
        if (!overrideText.empty()) {
            return itemNum > 1 ? std::format(L"{} x{}", overrideText, itemNum) : overrideText;
        }
        auto name = LookupItemName(itemId);
        if (!name.empty()) {
            return itemNum > 1 ? std::format(L"{} x{}", name, itemNum) : name;
        }
        return itemNum > 1 ? std::format(L"Item {} x{}", itemId, itemNum) : std::format(L"Item {}", itemId);
    }

    static bool StartViaController(int32 itemId, int32 itemNum, int32 itemAdd, const std::wstring& textOverride = {}) {
        if (!S.ctrl) {
            S.ctrl = UObjectGlobals::FindFirstOf(STR("BP_ItemWindowCtrl_C"));
            if (!S.ctrl) return false;
            S.ctrlExists = true;
        }

        if (!S.widget)
            if (!InitWidget()) return false;

        UClass* CC = S.ctrl->GetClassPrivate();

        auto* Fn = FindFunc(CC, FName(STR("IItemWindowSetParameter"), FNAME_Add));
        if (!Fn) return false;
        struct { int32 InItemType; int32 InItemNum; int32 InItemAdd; } P{ itemId, itemNum, itemAdd };
        S.ctrl->ProcessEvent(Fn, &P);
        S.active = { itemId, itemNum, itemAdd };
        S.usingController = true;

        if (S.widget) {
            SetItemText(MakeText(itemId, itemNum, textOverride));
            SetByte(S.widget, S.widget->GetClassPrivate(), STR("IsSetText"), 1);
            SetByte(S.widget, S.widget->GetClassPrivate(), STR("IsTextPlay"), 1);
        }

        if (auto* OpenFn = FindFunc(CC, FName(STR("IItemWindowOpen"), FNAME_Add)))
            S.ctrl->ProcessEvent(OpenFn, nullptr);
        else if (S.widget)
            if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("ExecutionItemWindowOpen"), FNAME_Add)))
                S.widget->ProcessEvent(F, nullptr);

        LOG("[ItemWindow] Shown id={} num={}", itemId, itemNum);
        return true;
    }

    static bool StartManual(int32 itemId, int32 itemNum, int32 itemAdd, const std::wstring& textOverride = {}) {
        if (!InitWidget()) return false;
        S.active = { itemId, itemNum, itemAdd };
        S.usingController = false;

        SetItemText(MakeText(itemId, itemNum, textOverride));

        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("SetRenderOpacity"), FNAME_Add))) {
            struct { float V; } P{ 0.0f };
            S.widget->ProcessEvent(F, &P);
        }
        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("ExecutionItemWindowOpen"), FNAME_Add)))
            S.widget->ProcessEvent(F, nullptr);
        return true;
    }

    static bool StartPopup(int32 itemId, int32 itemNum, int32 itemAdd, const std::wstring& textOverride = {}) {
        S.phase = Phase::Opening;
        S.timer = 0.0f;
        S.openFrames = 0;

        if (StartViaController(itemId, itemNum, itemAdd, textOverride)) return true;

        Kill();
        return StartManual(itemId, itemNum, itemAdd, textOverride);
    }

    static void BeginClose() {
        if (S.widget && Alive() && !S.usingController) {
            if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("ExecutionItemWindowClose"), FNAME_Add)))
                S.widget->ProcessEvent(F, nullptr);
        }
        if (S.usingController && S.ctrl) {
            auto* F = FindFunc(S.ctrl->GetClassPrivate(), FName(STR("IItemWindowClose"), FNAME_Add));
            if (!F) F = FindFunc(S.ctrl->GetClassPrivate(), FName(STR("IInfoWindowClose"), FNAME_Add));
            if (F) S.ctrl->ProcessEvent(F, nullptr);
        }
        S.phase = Phase::Closing;
        S.timer = 0.0f;
    }

    static void Abort() {
        if (S.phase != Phase::Idle || S.widget) {
            Entry replay = S.active;
            Kill();
            S.phase = Phase::Idle;
            S.timer = 0.0f;
            if (replay.itemId >= 0) S.queue.push_front(std::move(replay));
        }
    }

    static void TryNext() {
        if (S.queue.empty()) { S.phase = Phase::Idle; return; }
        if (GameState::IsTransitioning()) { S.phase = Phase::Idle; return; }
        auto e = std::move(S.queue.front());
        S.queue.pop_front();
        if (!StartPopup(e.itemId, e.itemNum, e.itemAdd, e.textOverride)) {
            S.queue.push_back(std::move(e));
            S.phase = Phase::Idle;
        }
    }

    static void Enq(int32 itemId, int32 itemNum, int32 itemAdd, const std::wstring& textOverride = {}) {
        if (GameState::IsTransitioning()) {
            S.queue.push_back({ itemId, itemNum, itemAdd, textOverride });
            return;
        }
        if (S.phase == Phase::Idle) {
            if (!StartPopup(itemId, itemNum, itemAdd, textOverride))
                S.queue.push_back({ itemId, itemNum, itemAdd, textOverride });
        } else {
            S.queue.push_back({ itemId, itemNum, itemAdd, textOverride });
        }
    }

} // anonymous namespace

namespace ItemWindow {

    void Setup() {
        if (g_SetupAttempted.load()) return;
        g_SetupAttempted.store(true);
        S.lastUpdate = std::chrono::steady_clock::now();
        g_SetupComplete.store(true);

        GameState::OnTransitionStart([]() {
            if (S.phase != Phase::Idle || S.widget) {
                Entry replay = S.active;
                Kill();
                S.phase = Phase::Idle;
                S.timer = 0.0f;
                if (replay.itemId >= 0) S.queue.push_front(std::move(replay));
            }
        });

        LOG("[ItemWindow] Ready");
    }

    void Update() {
        if (!g_SetupComplete.load()) return;
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - S.lastUpdate).count();
        S.lastUpdate = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 1.0f) dt = 1.0f;

        if (S.flushDelay > 0.0f) { S.flushDelay -= dt; }

        try {

        if (S.usingController) {
            switch (S.phase) {
            case Phase::Idle:
                if (S.flushDelay > 0.0f) break;
                if (!S.queue.empty() && !GameState::IsTransitioning()) { TryNext(); }
                break;
            case Phase::Opening:
                S.timer += dt;
                if (S.timer >= 0.5f) {
                    S.phase = Phase::Holding;
                    S.timer = 0.0f;
                    if (S.shownCb) S.shownCb(S.active.itemId, S.active.itemNum, S.active.itemAdd);
                }
                break;
            case Phase::Holding:
                S.timer += dt;
                if (S.timer >= State::ShowDuration) { BeginClose(); }
                break;
            case Phase::Closing:
                S.timer += dt;
                if (S.timer >= 0.5f) {
                    S.widget = nullptr;
                    S.ctrl = nullptr;
                    S.phase = Phase::Gap;
                    S.timer = 0.0f;
                }
                break;
            case Phase::Gap:
                S.timer += dt;
                if (S.timer >= State::GapDuration) TryNext();
                break;
            }
        } else {
            switch (S.phase) {
            case Phase::Idle:
                if (S.flushDelay > 0.0f) break;
                if (!S.queue.empty() && !GameState::IsTransitioning()) TryNext();
                break;

            case Phase::Opening:
                if (!S.widget) { S.phase = Phase::Idle; break; }
                if (S.openFrames < 3) {
                    ++S.openFrames;
                    if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("SetRenderOpacity"), FNAME_Add))) {
                        struct { float V; } P{ 0.0f };
                        S.widget->ProcessEvent(F, &P);
                    }
                    break;
                }
                S.timer += dt;
                if (S.widget) {
                    float o = (S.timer / State::FadeDuration) < 1.0f ? (S.timer / State::FadeDuration) : 1.0f;
                    if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("SetRenderOpacity"), FNAME_Add))) {
                        struct { float V; } P{ o };
                        S.widget->ProcessEvent(F, &P);
                    }
                }
                if (S.timer >= State::FadeDuration) {
                    S.phase = Phase::Holding;
                    S.timer = 0.0f;
                    if (S.shownCb) S.shownCb(S.active.itemId, S.active.itemNum, S.active.itemAdd);
                }
                break;

            case Phase::Holding:
                S.timer += dt;
                if (S.timer >= State::ShowDuration) BeginClose();
                break;

            case Phase::Closing:
                S.timer += dt;
                if (S.timer >= State::FadeDuration) {
                    Kill();
                    S.phase = Phase::Gap;
                    S.timer = 0.0f;
                }
                break;

            case Phase::Gap:
                S.timer += dt;
                if (S.timer >= State::GapDuration) TryNext();
                break;
            }
        }

        if (S.widget && GameState::IsTransitioning() && S.phase != Phase::Idle) {
            Entry replay = S.active;
            Kill();
            S.phase = Phase::Idle;
            S.timer = 0.0f;
            if (replay.itemId >= 0) S.queue.push_front(std::move(replay));
        }

        } catch (...) {
            Kill();
            S.phase = Phase::Idle;
            S.timer = 0.0f;
        }
    }

    void OnMapChanged() {
        S.widget = nullptr;
        S.ctrl = nullptr;
        S.active = { -1, 0, 0 };
        S.phase = Phase::Idle;
        S.timer = 0.0f;
        S.flushDelay = 0.5f;
    }

    void ShowItemPopup(std::int32_t itemId, std::int32_t itemNum) {
        if (!g_SetupComplete.load()) return;
        Enq(itemId, itemNum, 0);
    }

    void ShowItemPopupCustom(std::int32_t itemId, const wchar_t* customText) {
        if (!g_SetupComplete.load()) return;
        Enq(itemId, 0, 0, customText ? customText : L"");
    }

    void OnShown(ShownCallback cb) { S.shownCb = std::move(cb); }

} // namespace ItemWindow
