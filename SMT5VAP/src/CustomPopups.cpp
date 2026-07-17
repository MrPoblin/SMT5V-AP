#include "CustomPopups.hpp"
#include "src/Log/Log.hpp"
#include "src/GameState.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/FText.hpp>
#include <string>
#include <vector>
#include <queue>
#include <deque>
#include <chrono>
#include <format>

using namespace RC;
using namespace RC::Unreal;

namespace CustomPopups {

    // ── Configurable constants ──
    constexpr float CFG_ShowDuration = 3.2f;   // seconds a message stays fully open
    constexpr float CFG_GapDuration  = 0.2f;   // seconds between close-finished and next open
    constexpr float CFG_FadeDuration = 0.25f;   // seconds for manual fade in/out

    // ── State ──
    enum class Phase { Idle, Opening, Holding, Closing, Gap };

    static UObject* s_Widget = nullptr;
    static UClass* s_WidgetClass = nullptr;
    static bool s_SetupDone = false;

    static Phase s_Phase = Phase::Idle;
    static float s_PhaseTimer = 0.0f;          // seconds elapsed in current phase
    static int   s_OpenFrames = 0;             // frames since widget creation (readiness)
    static float s_FlushDelay = 0.0f;          // grace timer after a map change before
                                               // we start the next queued notification
                                               // (lets the new map's viewport settle)
    static std::chrono::steady_clock::time_point s_LastUpdate;
    static UObject* s_SsPlayer = nullptr;      // captured for fade (SsPlayerInfoWindow)
    static std::wstring s_ActiveMsg{};          // text of the currently shown message

    // FIFO queue of pending messages (wide strings). The message currently being
    // shown is NOT in the queue; it lives in s_Widget. If a transition interrupts a
    // show, the in-flight message is re-enqueued to the FRONT so it replays first
    // after load (rather than jumping the line behind everything else).
    static std::deque<std::wstring> s_Queue;

    // ── Helpers ──
    static UFunction* FindFuncByName(UStruct* Struct, const FName& Name) {
        for (auto* Func : TFieldRange<UFunction>(Struct, EFieldIterationFlags::IncludeAll)) {
            if (Func && Func->GetFName() == Name) {
                return Func;
            }
        }
        return nullptr;
    }

    static void SetByteProp(UObject* Obj, UClass* ObjClass, const TCHAR* PropName, uint8 Val) {
        if (auto* Prop = ObjClass->GetPropertyByNameInChain(PropName)) {
            if (auto* Ptr = Prop->ContainerPtrToValuePtr<uint8>(Obj)) {
                *Ptr = Val;
            }
        }
    }

    static void SetObjProp(UObject* Obj, UClass* ObjClass, const TCHAR* PropName, UObject* Val) {
        if (auto* Prop = ObjClass->GetPropertyByNameInChain(PropName)) {
            if (auto* Ptr = Prop->ContainerPtrToValuePtr<UObject*>(Obj)) {
                *Ptr = Val;
            }
        }
    }

    // Resolve the local PlayerController to use as the widget's owning player. A
    // widget created via raw NewObject with an actor outer has NO owning player, so
    // AddToViewport adds it without a valid player context and it frequently fails to
    // paint (especially right after a map load). Using UGameplayStatics::GetPlayerController
    // gives the real controller.
    static UObject* GetLocalPlayerController(UObject* WorldContext) {
        UObject* GS = UObjectGlobals::FindObject<UObject>(nullptr, STR("/Script/Engine.GameplayStatics"));
        if (!GS || !WorldContext) return nullptr;
        static UFunction* Fn = nullptr;
        if (!Fn) {
            Fn = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/Engine.GameplayStatics:GetPlayerController"));
        }
        if (!Fn) return nullptr;
        struct { const UObject* WorldContextObject; int32 PlayerIndex; UObject* ReturnValue; } Params{ WorldContext, 0, nullptr };
        GS->ProcessEvent(Fn, &Params);
        return Params.ReturnValue;
    }

    // Create a UUserWidget the proper way (UWidgetBlueprintLibrary::Create), which sets
    // the owning player so AddToViewport paints reliably. Returns nullptr on failure.
    static UObject* CreateWidgetProper(UObject* WorldContext, UClass* WidgetClass, UObject* OwningPC) {
        UObject* WBL = UObjectGlobals::FindObject<UObject>(nullptr, STR("/Script/UMG.WidgetBlueprintLibrary"));
        if (!WBL) return nullptr;
        static UFunction* Fn = nullptr;
        if (!Fn) {
            Fn = UObjectGlobals::FindObject<UFunction>(nullptr, STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        }
        if (!Fn) return nullptr;
        struct {
            UObject* WorldContextObject;
            UClass* WidgetType;
            UObject* OwningPlayer;
            UObject* ReturnValue;
        } Params{ WorldContext, WidgetClass, OwningPC, nullptr };
        WBL->ProcessEvent(Fn, &Params);
        return Params.ReturnValue;
    }

    // Manual fade. We call UWidget::SetRenderOpacity via ProcessEvent, which (if it
    // exists on this engine's UWidget) properly invalidates the widget and repaints.
    static bool s_SetRenderOpacityFound = false;
    static bool s_SetRenderOpacityChecked = false;
    static void SetOpacity(UObject* Widget, float Opacity) {
        if (!Widget) return;
        auto* Fn = FindFuncByName(Widget->GetClassPrivate(), FName(STR("SetRenderOpacity"), FNAME_Add));
        if (!s_SetRenderOpacityChecked) {
            s_SetRenderOpacityChecked = true;
            s_SetRenderOpacityFound = (Fn != nullptr);
            LOG("[Notification] SetRenderOpacity {} on this widget",
                Fn ? L"RESOLVED" : L"NOT FOUND (fade impossible via this path)");
        }
        if (Fn) {
            struct { float InRenderOpacity; } params{ std::clamp(Opacity, 0.0f, 1.0f) };
            Widget->ProcessEvent(Fn, &params);
        }
    }

    // Returns true only if s_Widget still points at a live, valid UObject that the
    // game has NOT begun tearing down. This replaces the old GetLiveWidget() address
    // comparison, which was unsafe: after a map change the old widget's memory can be
    // freed and later reused by a NEW widget at the same address, so an address check
    // would happily pass a dangling/freed pointer straight into ProcessEvent (UB that
    // wedged the whole per-frame tick — i.e. "popups stop working").
    static bool IsWidgetAlive() {
        if (!s_Widget) return false;
        // HasAnyInternalFlags(RF_BeginDestroyed | RF_FinishDestroyed) is unreliable on
        // a possibly-freed object, so go through the global object array, which knows
        // whether the index still maps to a live object.
        auto* Item = FUObjectArray::IndexToObject(s_Widget->GetInternalIndex());
        if (!Item) return false;
        // bEvenIfPendingKill = false: once the object is destroyed/unreachable this
        // returns false, so we never touch freed memory.
        return FUObjectArray::IsValid(Item, false);
    }

    static void DestroyWidget() {
        if (s_Widget) {
            // Only call RemoveFromParent when the object is still genuinely alive and
            // not mid-destruction. If it's already gone (map teardown wiped the world)
            // we must NOT touch it — the GC owns it now. IsWidgetAlive() is the correct
            // guard here; polling IsTransitioning() is NOT enough because by the time
            // OnMapChanged runs the flag is already cleared but the old widget's memory
            // may be gone, so a RemoveFromParent on it would corrupt state.
            if (IsWidgetAlive()) {
                if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("RemoveFromParent"), FNAME_Add))) {
                    s_Widget->ProcessEvent(Fn, nullptr);
                }
            }
            s_Widget = nullptr;
            s_SsPlayer = nullptr;
            s_ActiveMsg.clear();
        }
    }

    // Returns s_Widget if it is still a live, valid UObject we can safely
    // ProcessEvent, or nullptr otherwise. The dangling-pointer risk during a map
    // teardown is handled entirely by IsWidgetAlive() (which checks the global object
    // array, not freed memory). We deliberately do NOT require the controller's
    // WB_InfoWindow to still point at our widget: after a map load the game creates a
    // NEW controller instance, and requiring an address match there falsely aborts a
    // perfectly good freshly-spawned widget (the permanent "popups break after load"
    // bug). We own s_Widget's lifetime explicitly, so the controller reference is not
    // needed to validate it.
    static UObject* GetLiveWidget() {
        if (!s_Widget) return nullptr;
        if (!IsWidgetAlive()) return nullptr;
        return s_Widget;
    }

    // Begin showing a single message (assumes no widget is currently active and we are
    // not transitioning). Returns true if a widget was successfully created.
    static bool StartNotification(const wchar_t* message) {
        // Find controller instance
        UObject* Ctrl = UObjectGlobals::FindFirstOf(STR("BP_InfoWindowCtrl_C"));
        if (!Ctrl) {
            WARN("[Notification] No BP_InfoWindowCtrl_C found");
            return false;
        }
        UClass* CtrlClass = Ctrl->GetClassPrivate();

        // Resolve widget class (cache after first resolve)
        if (!s_WidgetClass) {
            if (auto* Prop = CtrlClass->GetPropertyByNameInChain(STR("WB_InfoWindow"))) {
                if (auto* ObjProp = CastField<FObjectPropertyBase>(Prop)) {
                    s_WidgetClass = ObjProp->GetPropertyClass();
                }
            }
            if (!s_WidgetClass) {
                if (auto* Obj = UObjectGlobals::FindObject(STR("UClass"), STR("WB_InfoWindow_C"))) {
                    s_WidgetClass = static_cast<UClass*>(Obj);
                }
            }
            if (!s_WidgetClass) {
                WARN("[Notification] Cannot resolve WB_InfoWindow_C class");
                return false;
            }
        }

        // Create the widget the PROPER way (with an owning PlayerController) so that
        // AddToViewport actually paints it. Raw NewObject + AddToViewport has no owning
        // player context and frequently fails to render after a map load.
        UObject* Widget = nullptr;
        UObject* PC = GetLocalPlayerController(Ctrl);
        for (int Attempt = 0; Attempt < 2 && !Widget; ++Attempt) {
            Widget = CreateWidgetProper(Ctrl, s_WidgetClass, PC);
            if (!Widget && Attempt == 0) {
                WARN("[Notification] Widget creation failed; re-resolving class");
                s_WidgetClass = nullptr;
                if (auto* Prop = CtrlClass->GetPropertyByNameInChain(STR("WB_InfoWindow"))) {
                    if (auto* ObjProp = CastField<FObjectPropertyBase>(Prop)) {
                        s_WidgetClass = ObjProp->GetPropertyClass();
                    }
                }
                if (!s_WidgetClass) {
                    if (auto* Obj = UObjectGlobals::FindObject(STR("UClass"), STR("WB_InfoWindow_C"))) {
                        s_WidgetClass = static_cast<UClass*>(Obj);
                    }
                }
            }
        }
        if (!Widget) {
            WARN("[Notification] Failed to create widget (PC={})", (PC != nullptr));
            return false;
        }
        s_Widget = Widget;
        s_ActiveMsg = message;

        // Assign to controller (so our bookkeeping matches the game's widget slot).
        SetObjProp(Ctrl, CtrlClass, STR("WB_InfoWindow"), s_Widget);

        // IMPORTANT: bReflectParentAlpha is read by the SsPlayer's paint logic at
        // Construct time (it is never read in C++ — only defaulted to false in the
        // constructor). CreateWidgetProper runs Construct (which builds the SsPlayer
        // sub-widget and its alpha-reflection setup) at CREATION time, BEFORE
        // AddToViewport. So we must set bReflectParentAlpha = 1 on the SsPlayer
        // BEFORE AddToViewport, otherwise the background ignores the parent's
        // RenderOpacity and only the text fades. Capture it now (it exists after
        // Construct) and re-capture after AddToViewport as a fallback.
        s_SsPlayer = nullptr;
        auto CaptureSsPlayer = [&]() {
            if (s_SsPlayer) return;
            if (auto* SsProp = s_Widget->GetClassPrivate()->GetPropertyByNameInChain(STR("SsPlayerInfoWindow"))) {
                if (auto* SsPlayer = *static_cast<UObject**>(SsProp->ContainerPtrToValuePtr<UObject*>(s_Widget))) {
                    s_SsPlayer = SsPlayer;
                    SetByteProp(SsPlayer, SsPlayer->GetClassPrivate(), STR("bReflectParentAlpha"), 1);
                }
            }
        };
        CaptureSsPlayer();

        // Add to viewport — triggers full initialization / first paint.
        if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("AddToViewport"), FNAME_Add))) {
            int32 ZOrder = 0;
            s_Widget->ProcessEvent(Fn, &ZOrder);
        }

        // Re-capture in case the SsPlayer was (re)created during AddToViewport, and
        // make sure the flag is set on whatever instance actually renders.
        CaptureSsPlayer();

        // NOTE: Do NOT force the widget's Visibility. The correct enum value differs
        // across engine versions and forcing the wrong one (e.g. Hidden) leaves it
        // permanently invisible. The SsPlayer open animation driven by
        // ExecutionInfoWindowOpen, combined with bReflectParentAlpha + our opacity
        // fade, is what reveals the window.

        // Trigger the SpriteStudio open animation. This is what actually makes the
        // info-window visible (the SsPlayer renders the window). We no longer WAIT on
        // IsFinishedOpenWindow for timing (that signal was unreliable) — the manual
        // timers in Update() drive the phase transitions instead.
        if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("ExecutionInfoWindowOpen"), FNAME_Add))) {
            s_Widget->ProcessEvent(Fn, nullptr);
        }

        // Start fully transparent; the Opening phase fades RenderOpacity up.
        SetOpacity(s_Widget, 0.0f);
        if (s_SsPlayer) SetOpacity(s_SsPlayer, 0.0f);

        // Do NOT force Visibility/RenderOpacity here. This widget renders at its
        // default (Visible, opacity 1.0) once added to the viewport — forcing
        // Visibility to 0 (or an unknown enum value) / opacity to 0 can leave it
        // permanently invisible. The manual fade below only adjusts opacity over
        // time if the engine actually repaints it; otherwise the widget simply
        // shows at full opacity (still visible).

        // Set text
        if (auto* RP = s_Widget->GetClassPrivate()->GetPropertyByNameInChain(STR("MessageRichTextInfo"))) {
            if (auto* RichText = *static_cast<UObject**>(RP->ContainerPtrToValuePtr<UObject*>(s_Widget))) {
                UClass* RTC = RichText->GetClassPrivate();
                if (auto* SetTextFn = FindFuncByName(RTC, FName(STR("SetText"), FNAME_Add))) {
                    struct { FText InText; } SetParams;
                    SetParams.InText = FText(message);
                    RichText->ProcessEvent(SetTextFn, &SetParams);
                }
                // Disable the typewriter reveal so the full message is visible
                // immediately (the original behaviour). Without this the text stays
                // hidden until the reveal animation (which may never tick) completes.
                SetByteProp(RichText, RTC, STR("bReveal"), 0);
                if (auto* Prop = RTC->GetPropertyByNameInChain(STR("RevealedIndex"))) {
                    if (auto* Ptr = Prop->ContainerPtrToValuePtr<int32>(RichText)) *Ptr = 9999;
                }
            }
        }

        // Mark text as set so the widget renders it
        SetByteProp(s_Widget, s_Widget->GetClassPrivate(), STR("IsSetText"), 1);
        SetByteProp(s_Widget, s_Widget->GetClassPrivate(), STR("IsTextPlay"), 1);

        LOG("[Notification] Showing: {}", message);
        s_Phase = Phase::Opening;
        s_PhaseTimer = 0.0f;
        s_OpenFrames = 0;
        return true;
    }

    static void BeginClose() {
        // NOTE: we deliberately do NOT call ExecutionInfoWindowClose() here. The
        // SsPlayer's close animation overrides RenderOpacity every frame (resetting
        // it to full), which cancelled our manual fade-out. The Opening phase already
        // proved the manual opacity fade works on its own, so we fade out manually
        // too and just destroy the widget when done.
        s_Phase = Phase::Closing;
        s_PhaseTimer = 0.0f;
    }

    // Tear down the in-flight show and re-enqueue the active message so it replays
    // after the map settles. Used both when a transition interrupts a show and when
    // the controller/widget vanishes out from under us. Does nothing if idle.
    static void AbortShow() {
        if (s_Phase == Phase::Idle && !s_Widget) return;
        std::wstring replay = std::move(s_ActiveMsg);
        DestroyWidget();
        s_Phase = Phase::Idle;
        s_PhaseTimer = 0.0f;
        if (!replay.empty()) {
            s_Queue.push_front(std::move(replay));
            LOG("[Notification] Show aborted (transition/world change); replay queued to front");
        }
    }

    // Pop the next queued message and start it (if appropriate).
    static void TryStartNext() {
        if (GameState::IsTransitioning()) return;     // defer until map settle
        if (s_Queue.empty()) {
            s_Phase = Phase::Idle;
            return;
        }
        std::wstring msg = std::move(s_Queue.front());
        s_Queue.pop_front();
        if (!StartNotification(msg.c_str())) {
            // Could not show (e.g. no controller yet). Re-enqueue and idle; will retry
            // on next OnMapChanged / Update when conditions are better.
            s_Queue.push_back(std::move(msg));
            s_Phase = Phase::Idle;
            LOG("[Notification] TryStartNext: StartNotification failed, will retry (queue={})", s_Queue.size());
        }
    }

    // Enqueue a wide message. If nothing is showing and we're settled, show it now.
    static void Enqueue(const std::wstring& message, bool priority) {
        if (GameState::IsTransitioning()) {
            s_Queue.push_back(message);
            LOG("[Notification] Deferred (transition in progress): {}", message);
            return;
        }
        if (s_Phase == Phase::Idle) {
            if (!StartNotification(message.c_str())) {
                s_Queue.push_back(message);
            }
        } else if (priority) {
            // Jump the line: insert right after the currently-active message.
            if (s_Queue.empty()) {
                s_Queue.push_back(message);
            } else {
                s_Queue.push_back(s_Queue.front());
                s_Queue.pop_front();
                s_Queue.push_back(message);
            }
        } else {
            s_Queue.push_back(message);
        }
    }

    void ShowNotification(const wchar_t* message) {
        Enqueue(std::wstring(message), /*priority=*/false);
    }

    void ShowNotification(const std::string& msg) {
        // APCpp delivers UTF-8 std::string. Convert to wide for the FText path.
        std::wstring wmsg;
        wmsg.reserve(msg.size());
        for (unsigned char c : msg) wmsg.push_back(static_cast<wchar_t>(c));
        Enqueue(wmsg, /*priority=*/false);
    }

    void ShowNotificationPriority(const wchar_t* message) {
        Enqueue(std::wstring(message), /*priority=*/true);
    }

    void ShowNotificationPriority(const std::string& msg) {
        std::wstring wmsg;
        wmsg.reserve(msg.size());
        for (unsigned char c : msg) wmsg.push_back(static_cast<wchar_t>(c));
        Enqueue(wmsg, /*priority=*/true);
    }

    void ClearQueue(bool clearActive) {
        while (!s_Queue.empty()) s_Queue.pop_front();
        if (clearActive && s_Widget) {
            LOG("[Notification] ClearQueue forced close of active widget");
            DestroyWidget();
            s_Phase = Phase::Idle;
            s_PhaseTimer = 0.0f;
        }
        LOG("[Notification] Queue cleared");
    }

    void OnMapChanged() {
        // The controller/widget from the old map is gone. The actual teardown is
        // handled by AbortShow() (invoked from the transition-start callback while the
        // old world is still alive). By the time we reach here the transition flag is
        // already cleared, so just make sure we're in a clean state and flush any
        // queued/deferred messages. No need to null s_WidgetClass: it is a global
        // UClass (not map-bound); we only re-resolve it lazily inside StartNotification
        // if creation actually fails.
        s_Widget = nullptr;
        s_SsPlayer = nullptr;
        s_ActiveMsg.clear();
        s_Phase = Phase::Idle;
        s_PhaseTimer = 0.0f;
        s_OpenFrames = 0;
        // Do NOT start queued messages immediately: the new map's viewport/UI may not
        // be ready yet, so AddToViewport would add to a not-yet-painted viewport and
        // the widget would never appear. Instead arm a short grace timer; Update()'s
        // Idle phase will flush once the delay elapses AND we're not transitioning.
        s_FlushDelay = 0.5f;
    }

    void Update() {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - s_LastUpdate).count();
        s_LastUpdate = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 1.0f) dt = 1.0f; // clamp huge frame gaps (e.g. after a stall)

        // A single bad ProcessEvent (e.g. the game destroyed the info-window
        // controller out from under us during a map change) must NOT throw out of
        // on_update — that would permanently kill the per-frame tick (and thus all
        // future notifications). Catch, log, and reset to a safe Idle state.
        try {
        switch (s_Phase) {
        case Phase::Idle:
            if (s_FlushDelay > 0.0f) {
                s_FlushDelay -= dt;
                break;
            }
            if (!s_Queue.empty() && !GameState::IsTransitioning()) {
                TryStartNext();
            }
            break;

        case Phase::Opening:
            // Manual fade-in: drive RenderOpacity 0 -> 1.
            if (!s_Widget) { s_Phase = Phase::Idle; break; }
            // Re-fetch live pointers so we never ProcessEvent a widget the game may
            // have destroyed/recreated during a map change (dangling pointer -> AV).
            if (!GetLiveWidget()) {
                AbortShow(); break;
            }
            // Wait a few frames after creation so the slate slot exists before we
            // start writing opacity. Early writes landed on a not-yet-ready widget
            // and were lost, making the window snap to full opacity (no fade).
            if (s_OpenFrames < 3) {
                ++s_OpenFrames;
                SetOpacity(s_Widget, 0.0f);
                if (s_SsPlayer) SetOpacity(s_SsPlayer, 0.0f);
                break;
            }
            s_PhaseTimer += dt;
            SetOpacity(s_Widget, s_PhaseTimer / CFG_FadeDuration);
            if (s_SsPlayer) SetOpacity(s_SsPlayer, s_PhaseTimer / CFG_FadeDuration);
            if (s_PhaseTimer >= CFG_FadeDuration) {
                SetOpacity(s_Widget, 1.0f);
                if (s_SsPlayer) SetOpacity(s_SsPlayer, 1.0f);
                s_Phase = Phase::Holding;
                s_PhaseTimer = 0.0f;
            }
            break;

        case Phase::Holding:
            s_PhaseTimer += dt;
            if (s_PhaseTimer >= CFG_ShowDuration) {
                BeginClose();
            }
            break;

        case Phase::Closing:
            // Manual fade-out: drive RenderOpacity 1 -> 0, then destroy.
            if (!s_Widget) { s_Phase = Phase::Idle; break; }
            if (!GetLiveWidget()) { AbortShow(); break; }
            s_PhaseTimer += dt;
            SetOpacity(s_Widget, 1.0f - (s_PhaseTimer / CFG_FadeDuration));
            if (s_SsPlayer) SetOpacity(s_SsPlayer, 1.0f - (s_PhaseTimer / CFG_FadeDuration));
            if (s_PhaseTimer >= CFG_FadeDuration) {
                DestroyWidget();
                s_Phase = Phase::Gap;
                s_PhaseTimer = 0.0f;
            }
            break;

        case Phase::Gap:
            s_PhaseTimer += dt;
            if (s_PhaseTimer >= CFG_GapDuration) {
                TryStartNext();
            }
            break;
        }

        // Abort immediately if a transition begins while a widget is up. (Primary
        // cleanup now also happens via OnTransitionStart so it's guaranteed even if no
        // on_update frame lands during the transition window.)
        if (s_Widget && GameState::IsTransitioning()) {
            AbortShow();
        }
        }
        catch (const std::exception&) {
            WARN(L"[Notification] Update() threw (std); resetting to Idle");
            DestroyWidget();
            s_Phase = Phase::Idle;
            s_PhaseTimer = 0.0f;
        }
        catch (...) {
            WARN(L"[Notification] Update() threw (unknown); resetting to Idle");
            DestroyWidget();
            s_Phase = Phase::Idle;
            s_PhaseTimer = 0.0f;
        }
    }

    void Setup() {
        if (s_SetupDone) return;
        s_SetupDone = true;
        s_LastUpdate = std::chrono::steady_clock::now();
        // Clean up the in-flight show the instant a transition begins, while the old
        // world is still alive. Polling IsTransitioning() from Update() is NOT enough:
        // if no on_update frame lands between the transition starting and the old
        // widget being destroyed, we'd otherwise keep a dangling pointer and only
        // null it (unsafely) later in OnMapChanged. The callback guarantees correct,
        // timely cleanup and re-queues the message for replay.
        GameState::OnTransitionStart([]() {
            if (s_Widget || s_Phase != Phase::Idle) {
                AbortShow();
            }
        });
        LOG("[CustomPopups] Ready");
    }

} // namespace CustomPopups
