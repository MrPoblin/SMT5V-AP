#include "CustomPopups.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/FText.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace CustomPopups {
    static UObject* s_Widget = nullptr;
    static UClass* s_WidgetClass = nullptr;
    static float s_CloseTimer = -1.0f;
    static bool s_SetupDone = false;

    // FindFunctionByName isn't exposed in UE4SS headers, so we roll our own
    static UFunction* FindFuncByName(UStruct* Struct, const FName& Name) {
        for (auto* Func : TFieldRange<UFunction>(Struct, EFieldIterationFlags::IncludeAll)) {
            if (Func && Func->GetFName() == Name) {
                return Func;
            }
        }
        return nullptr;
    }

    // Helper: set a uint8 property on an object by name
    static void SetByteProp(UObject* Obj, UClass* ObjClass, const TCHAR* PropName, uint8 Val) {
        if (auto* Prop = ObjClass->GetPropertyByNameInChain(PropName)) {
            if (auto* Ptr = Prop->ContainerPtrToValuePtr<uint8>(Obj)) {
                *Ptr = Val;
            }
        }
    }

    // Helper: set a UObject* property on an object by name
    static void SetObjProp(UObject* Obj, UClass* ObjClass, const TCHAR* PropName, UObject* Val) {
        if (auto* Prop = ObjClass->GetPropertyByNameInChain(PropName)) {
            if (auto* Ptr = Prop->ContainerPtrToValuePtr<UObject*>(Obj)) {
                *Ptr = Val;
            }
        }
    }

    void Setup() {
        if (s_SetupDone) return;
        s_SetupDone = true;

        Hook::RegisterProcessEventPostCallback(
            [](auto&, UObject* Object, UFunction* Function, void* Params) {
                if (s_CloseTimer < 0.0f || !Object || !Function) return;
                static FName CtrlClassName(STR("BP_InfoWindowCtrl_C"), FNAME_Add);
                if (Object->GetClassPrivate()->GetNamePrivate() != CtrlClassName) return;
                if (Function->GetFName() != FName(STR("ReceiveTick"), FNAME_Add)) return;

                // While the notification is alive, keep reveal disabled (animation may fight us)
                if (s_Widget) {
                    if (auto* RP = s_Widget->GetClassPrivate()->GetPropertyByNameInChain(STR("MessageRichTextInfo"))) {
                        if (auto* RichText = *static_cast<UObject**>(RP->ContainerPtrToValuePtr<UObject*>(s_Widget))) {
                            UClass* RTC = RichText->GetClassPrivate();
                            SetByteProp(RichText, RTC, STR("bReveal"), 0);
                            if (auto* Prop = RTC->GetPropertyByNameInChain(STR("RevealedIndex"))) {
                                if (auto* Ptr = Prop->ContainerPtrToValuePtr<int32>(RichText)) *Ptr = 9999;
                            }
                        }
                    }
                }

                // Time-based countdown using DeltaSeconds from ReceiveTick params
                if (s_CloseTimer > 0.0f && Params) {
                    s_CloseTimer -= *static_cast<const float*>(Params);
                    if (s_CloseTimer > 0.0f) return;
                }
                if (s_CloseTimer > 0.0f) return;

                // Timer expired -> close notification
                if (s_Widget) {
                    if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("ExecutionInfoWindowClose"), FNAME_Add))) {
                        s_Widget->ProcessEvent(Fn, nullptr);
                    }
                    s_CloseTimer = -2.0f;
                }
            },
            Hook::FCallbackOptions{
                .bReadonly = true,
                .OwnerModName = STR("SMT5VAP"),
                .HookName = STR("NotificationCloseTimer")
            }
        );

        LOG("[CustomPopups] Ready");
    }

    void ShowNotification(const wchar_t* message) {
        // Destroy previous widget
        if (s_Widget) {
            if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("RemoveFromParent"), FNAME_Add))) {
                s_Widget->ProcessEvent(Fn, nullptr);
            }
            s_Widget = nullptr;
            s_CloseTimer = -1.0f;
        }

        // Find controller instance
        UObject* Ctrl = UObjectGlobals::FindFirstOf(STR("BP_InfoWindowCtrl_C"));
        if (!Ctrl) {
            WARN("[Notification] No BP_InfoWindowCtrl_C found");
            return;
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
                return;
            }
        }

        // Create widget instance
        {
            FName ObjName(STR("APNotification"), FNAME_Add);
            UObject* Widget = UObjectGlobals::NewObject<UObject>(
                Ctrl, s_WidgetClass, ObjName, RF_NoFlags,
                nullptr, false, nullptr, nullptr
            );
            if (!Widget) {
                WARN("[Notification] Failed to create widget");
                return;
            }
            s_Widget = Widget;
        }

        // Assign to controller
        SetObjProp(Ctrl, CtrlClass, STR("WB_InfoWindow"), s_Widget);

        // Add to viewport first — this triggers full initialization (Construct, child widgets, etc.)
        if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("AddToViewport"), FNAME_Add))) {
            int32 ZOrder = 0;
            s_Widget->ProcessEvent(Fn, &ZOrder);
        }

        // Play open animation (this may set bReveal=true, RevealedIndex=0, clipping text)
        if (auto* Fn = FindFuncByName(s_Widget->GetClassPrivate(), FName(STR("ExecutionInfoWindowOpen"), FNAME_Add))) {
            s_Widget->ProcessEvent(Fn, nullptr);
        }

        // Set text + force-disable reveal (override animation's initial state)
        if (auto* RP = s_Widget->GetClassPrivate()->GetPropertyByNameInChain(STR("MessageRichTextInfo"))) {
            if (auto* RichText = *static_cast<UObject**>(RP->ContainerPtrToValuePtr<UObject*>(s_Widget))) {
                UClass* RTC = RichText->GetClassPrivate();

                if (auto* SetTextFn = FindFuncByName(RTC, FName(STR("SetText"), FNAME_Add))) {
                    struct { FText InText; } SetParams;
                    SetParams.InText = FText(message);
                    RichText->ProcessEvent(SetTextFn, &SetParams);
                }

                SetByteProp(RichText, RTC, STR("bReveal"), 0);
                if (auto* Prop = RTC->GetPropertyByNameInChain(STR("RevealedIndex"))) {
                    if (auto* Ptr = Prop->ContainerPtrToValuePtr<int32>(RichText)) *Ptr = 9999;
                }

                LOG("[Notification] RichText set, bReveal=0");
            }
        }

        // Mark text as set so the widget renders it
        SetByteProp(s_Widget, s_Widget->GetClassPrivate(), STR("IsSetText"), 1);
        SetByteProp(s_Widget, s_Widget->GetClassPrivate(), STR("IsTextPlay"), 1);

        // Schedule auto-close
        s_CloseTimer = 3.5f;

        LOG("[Notification] {}", message);
    }
}
