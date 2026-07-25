#include "InfoWindow.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/FText.hpp>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

// ════════════════════════════════════════════════════════════════════
//  InfoWindow — Show custom notifications using the game's own
//  BP_InfoWindowCtrl_C + WB_InfoWindow_C.
// ════════════════════════════════════════════════════════════════════

namespace {

    enum class Phase { Idle, Opening, Holding, Closing, Gap };

    struct State {
        Phase phase = Phase::Idle;
        float timer = 0.0f;
        int openFrames = 0;
        float flushDelay = 0.0f;

        UClass* widgetClass = nullptr;
        UObject* widget = nullptr;
        UObject* ctrl = nullptr;      // cached controller

        std::wstring activeMsg;
        std::deque<std::wstring> queue;
        std::chrono::steady_clock::time_point lastUpdate;

        InfoWindow::ShownCallback shownCb;

        // If Tier1 succeeded, the controller manages the lifecycle.
        // If Tier1 failed, we use our own manual timers.
        bool usingController = false;

        static constexpr float ShowDuration = 3.2f;
        static constexpr float GapDuration = 0.2f;
        static constexpr float FadeDuration = 0.25f;
    };
    State S;

    std::atomic<bool> g_SetupAttempted{false};
    std::atomic<bool> g_SetupComplete{false};

    // ── Helpers ──

    UFunction* FindFunc(UStruct* S, const FName& N) {
        for (auto* F : TFieldRange<UFunction>(S, EFieldIterationFlags::IncludeAll))
            if (F && F->GetFName() == N) return F;
        return nullptr;
    }

    void SetByte(UObject* O, UClass* C, const TCHAR* P, uint8 V) {
        if (auto* Prop = C->GetPropertyByNameInChain(P))
            if (auto* D = Prop->ContainerPtrToValuePtr<uint8>(O)) *D = V;
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

    UObject* CreateW(UObject* WC, UClass* Cls, UObject* PC) {
        auto* WBL = UObjectGlobals::FindObject<UObject>(nullptr, STR("/Script/UMG.WidgetBlueprintLibrary"));
        if (!WBL) return nullptr;
        static UFunction* Fn = nullptr;
        if (!Fn) Fn = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        if (!Fn) return nullptr;
        struct { UObject* WC; UClass* T; UObject* PC; UObject* R; } P{ WC, Cls, PC, nullptr };
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
        S.activeMsg.clear();
    }

    void SetWBCtrl(UObject* C, UClass* CC, UObject* W) {
        if (auto* P = CC->GetPropertyByNameInChain(STR("WB_InfoWindow")))
            if (auto* D = P->ContainerPtrToValuePtr<UObject*>(C)) *D = W;
    }

    void CaptureSS() {
        if (!S.widget) return;
        auto* P = S.widget->GetClassPrivate()->GetPropertyByNameInChain(STR("SsPlayerInfoWindow"));
        if (!P) return;
        auto* SS = *static_cast<UObject**>(P->ContainerPtrToValuePtr<UObject*>(S.widget));
        if (SS) SetByte(SS, SS->GetClassPrivate(), STR("bReflectParentAlpha"), 1);
    }

    // ── Tier 1: Call the controller's IInfoWindowOpen (REAL game popup) ──

    static bool StartViaController(const wchar_t* msg) {
        auto* Ctrl = UObjectGlobals::FindFirstOf(STR("BP_InfoWindowCtrl_C"));
        if (!Ctrl) return false;
        UClass* CtrlClass = Ctrl->GetClassPrivate();

        // Resolve widget class
        if (!S.widgetClass) {
            if (auto* Prop = CtrlClass->GetPropertyByNameInChain(STR("WB_InfoWindow"))) {
                if (auto* Obj = CastField<FObjectPropertyBase>(Prop))
                    S.widgetClass = Obj->GetPropertyClass();
            }
            if (!S.widgetClass) {
                auto* Obj = UObjectGlobals::FindObject(STR("UClass"), STR("WB_InfoWindow_C"));
                if (Obj) S.widgetClass = static_cast<UClass*>(Obj);
            }
            if (!S.widgetClass) return false;
        }

        // Create widget
        auto* PC = GetPC(Ctrl);
        auto* W = CreateW(Ctrl, S.widgetClass, PC);
        if (!W) return false;

        // Assign to controller
        SetWBCtrl(Ctrl, CtrlClass, W);

        // Reflect alpha + add to viewport
        S.widget = W;
        CaptureSS();
        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("AddToViewport"), FNAME_Add))) {
            int32 Z = 0;
            S.widget->ProcessEvent(F, &Z);
        }
        CaptureSS();

        // Set custom text
        if (auto* Prop = S.widget->GetClassPrivate()->GetPropertyByNameInChain(STR("MessageRichTextInfo"))) {
            if (auto* RT = *static_cast<UObject**>(Prop->ContainerPtrToValuePtr<UObject*>(S.widget))) {
                UClass* RTC = RT->GetClassPrivate();
                if (auto* F = FindFunc(RTC, FName(STR("SetText"), FNAME_Add))) {
                    struct { FText T; } P;
                    P.T = FText(msg);
                    RT->ProcessEvent(F, &P);
                }
                SetByte(RT, RTC, STR("bReveal"), 0);
                if (auto* P = RTC->GetPropertyByNameInChain(STR("RevealedIndex")))
                    if (auto* D = P->ContainerPtrToValuePtr<int32>(RT)) *D = 9999;
            }
        }
        SetByte(S.widget, S.widget->GetClassPrivate(), STR("IsSetText"), 1);
        SetByte(S.widget, S.widget->GetClassPrivate(), STR("IsTextPlay"), 1);

        // Call IInfoWindowOpen — shows the REAL game popup with animation
        // NOTE: No pre-hook is registered (hooks break ProcessEvent).
        if (auto* Fn = FindFunc(CtrlClass, FName(STR("IInfoWindowOpen"), FNAME_Add))) {
            Ctrl->ProcessEvent(Fn, nullptr);
            S.ctrl = Ctrl;
            S.usingController = true;
            return true;
        }

        return false;
    }

    // ── Tier 2: Manual fallback ──

    static bool StartManual(const wchar_t* msg) {
        auto* Ctrl = UObjectGlobals::FindFirstOf(STR("BP_InfoWindowCtrl_C"));
        if (!Ctrl) return false;
        UClass* CtrlClass = Ctrl->GetClassPrivate();

        if (!S.widgetClass) {
            if (auto* Prop = CtrlClass->GetPropertyByNameInChain(STR("WB_InfoWindow"))) {
                if (auto* Obj = CastField<FObjectPropertyBase>(Prop))
                    S.widgetClass = Obj->GetPropertyClass();
            }
            if (!S.widgetClass) {
                auto* Obj = UObjectGlobals::FindObject(STR("UClass"), STR("WB_InfoWindow_C"));
                if (Obj) S.widgetClass = static_cast<UClass*>(Obj);
            }
            if (!S.widgetClass) return false;
        }

        auto* PC = GetPC(Ctrl);
        auto* W = CreateW(Ctrl, S.widgetClass, PC);
        if (!W) return false;
        S.widget = W;
        S.activeMsg = msg;
        SetWBCtrl(Ctrl, CtrlClass, S.widget);
        CaptureSS();
        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("AddToViewport"), FNAME_Add))) {
            int32 Z = 0;
            S.widget->ProcessEvent(F, &Z);
        }
        CaptureSS();

        if (auto* Prop = S.widget->GetClassPrivate()->GetPropertyByNameInChain(STR("MessageRichTextInfo"))) {
            if (auto* RT = *static_cast<UObject**>(Prop->ContainerPtrToValuePtr<UObject*>(S.widget))) {
                UClass* RTC = RT->GetClassPrivate();
                if (auto* F = FindFunc(RTC, FName(STR("SetText"), FNAME_Add))) {
                    struct { FText T; } P;
                    P.T = FText(msg);
                    RT->ProcessEvent(F, &P);
                }
                SetByte(RT, RTC, STR("bReveal"), 0);
                if (auto* P = RTC->GetPropertyByNameInChain(STR("RevealedIndex")))
                    if (auto* D = P->ContainerPtrToValuePtr<int32>(RT)) *D = 9999;
            }
        }
        SetByte(S.widget, S.widget->GetClassPrivate(), STR("IsSetText"), 1);
        SetByte(S.widget, S.widget->GetClassPrivate(), STR("IsTextPlay"), 1);

        // Manual opacity fade
        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("SetRenderOpacity"), FNAME_Add))) {
            struct { float V; } P{ 0.0f };
            S.widget->ProcessEvent(F, &P);
        }
        if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("ExecutionInfoWindowOpen"), FNAME_Add)))
            S.widget->ProcessEvent(F, nullptr);

        S.usingController = false;
        return true;
    }

    static bool StartNotification(const wchar_t* msg) {
        S.activeMsg = msg;
        S.phase = Phase::Opening;
        S.timer = 0.0f;
        S.openFrames = 0;

        if (StartViaController(msg)) return true;

        // Fallback
        Kill();
        return StartManual(msg);
    }

    static void BeginClose() {
        if (S.widget && Alive() && !S.usingController) {
            if (auto* F = FindFunc(S.widget->GetClassPrivate(), FName(STR("ExecutionInfoWindowClose"), FNAME_Add)))
                S.widget->ProcessEvent(F, nullptr);
        }
        S.phase = Phase::Closing;
        S.timer = 0.0f;
    }

    static void Abort() {
        if (S.phase != Phase::Idle || S.widget) {
            std::wstring replay = std::move(S.activeMsg);
            Kill();
            S.phase = Phase::Idle;
            S.timer = 0.0f;
            if (!replay.empty()) S.queue.push_front(std::move(replay));
        }
    }

    static void TryNext() {
        if (S.queue.empty()) { S.phase = Phase::Idle; return; }
        auto msg = std::move(S.queue.front());
        S.queue.pop_front();
        if (!StartNotification(msg.c_str())) {
            S.queue.push_back(std::move(msg));
            S.phase = Phase::Idle;
        }
    }

    static void Enq(const std::wstring& m, bool prio) {
        if (S.phase == Phase::Idle) {
            if (!StartNotification(m.c_str())) S.queue.push_back(m);
        } else if (prio) {
            if (S.queue.empty()) S.queue.push_back(m);
            else {
                S.queue.push_back(S.queue.front());
                S.queue.pop_front();
                S.queue.push_back(m);
            }
        } else S.queue.push_back(m);
    }

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════
//  Public API
// ════════════════════════════════════════════════════════════════════

namespace InfoWindow {

    void Setup() {
        if (g_SetupAttempted.load()) return;
        g_SetupAttempted.store(true);
        S.lastUpdate = std::chrono::steady_clock::now();
        g_SetupComplete.store(true);
        LOG("[InfoWindow] Ready");
    }

    void Update() {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - S.lastUpdate).count();
        S.lastUpdate = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 1.0f) dt = 1.0f;

        if (S.flushDelay > 0.0f) S.flushDelay -= dt;

        try {

        if (S.usingController) {
            // Tier 1: we manage the timing, the controller manages the animation.
            if (S.phase == Phase::Opening) {
                S.timer += dt;
                if (S.timer >= 0.5f) {
                    S.phase = Phase::Holding;
                    S.timer = 0.0f;
                    if (S.shownCb) S.shownCb();
                }
            } else if (S.phase == Phase::Holding) {
                S.timer += dt;
                if (S.timer >= State::ShowDuration) {
                    // Call controller's IInfoWindowClose — plays native close animation
                    if (S.ctrl) {
                        if (auto* F = FindFunc(S.ctrl->GetClassPrivate(), FName(STR("IInfoWindowClose"), FNAME_Add)))
                            S.ctrl->ProcessEvent(F, nullptr);
                    }
                    S.phase = Phase::Closing;
                    S.timer = 0.0f;
                }
            } else if (S.phase == Phase::Closing) {
                S.timer += dt;
                if (S.timer >= 0.5f) {
                    // Close animation should be done
                    S.widget = nullptr;
                    S.ctrl = nullptr;
                    S.phase = Phase::Gap;
                    S.timer = 0.0f;
                }
            } else if (S.phase == Phase::Gap) {
                S.timer += dt;
                if (S.timer >= State::GapDuration) TryNext();
            }
            return;
        }

        // Tier 2: manual state machine
        switch (S.phase) {
        case Phase::Idle:
            if (S.flushDelay <= 0.0f && !S.queue.empty()) TryNext();
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
                if (S.shownCb) S.shownCb();
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

        } catch (...) {
            Kill();
            S.phase = Phase::Idle;
            S.timer = 0.0f;
        }
    }

    void ShowNotification(const wchar_t* m) { Enq(std::wstring(m), false); }
    void ShowNotification(const std::string& msg) {
        std::wstring w; w.reserve(msg.size());
        for (unsigned char c : msg) w.push_back(wchar_t(c));
        Enq(w, false);
    }
    void ShowNotificationPriority(const wchar_t* m) { Enq(std::wstring(m), true); }
    void ShowNotificationPriority(const std::string& msg) {
        std::wstring w; w.reserve(msg.size());
        for (unsigned char c : msg) w.push_back(wchar_t(c));
        Enq(w, true);
    }

    void ClearQueue(bool ca) {
        S.queue.clear();
        if (ca && S.widget) { Kill(); S.phase = Phase::Idle; S.timer = 0.0f; }
    }

    void OnMapChanged() { S.widget = nullptr; S.ctrl = nullptr; S.activeMsg.clear(); S.phase = Phase::Idle; S.timer = 0.0f; S.flushDelay = 0.5f; }
    void OnShown(ShownCallback cb) { S.shownCb = std::move(cb); }

} // namespace InfoWindow
