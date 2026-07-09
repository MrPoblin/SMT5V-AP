#include "PopupSuppression.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace PopupSuppression {
    static std::atomic<int32> s_BlockCount{0};

    void SetBlockNextSpawn(bool block) {
        if (block) {
            s_BlockCount.fetch_add(1, std::memory_order_release);
        }
    }

    void Setup() {
        // ── Block BeginPlay on move/chest pieces ──
        static FName MovePieceClassName = FName(STR("BP_MovePiece_C"), FNAME_Add);
        static FName ChestPieceClassName = FName(STR("BP_Gimic_Chest_Piece_C"), FNAME_Add);

        Hook::RegisterBeginPlayPreCallback(
            [](auto& Data, AActor* Actor) {
                if (!Actor) return;
                if (s_BlockCount.load(std::memory_order_acquire) <= 0) return;

                FName ClassName = Actor->GetClassPrivate()->GetNamePrivate();
                if (ClassName == MovePieceClassName || ClassName == ChestPieceClassName) {
                    DEBUG("[PopupSuppression] Blocked BeginPlay on {}", Actor->GetClassPrivate()->GetName());
                    Data.PreventOriginalFunctionCall();
                }
            },
            Hook::FCallbackOptions{
                .bReadonly = false,
                .OwnerModName = STR("SMT5VAP"),
                .HookName = STR("BlockPieceBeginPlay")
            }
        );

        // ── Backup: force-close item window on every tick ──
        static FName ItemWindowClassName = FName(STR("BP_ItemWindowCtrl_C"), FNAME_Add);

        Hook::RegisterProcessEventPostCallback(
            [](auto&, UObject* Object, UFunction* Function, void*) {
                if (!Object || !Function) return;
                if (Object->GetClassPrivate()->GetNamePrivate() != ItemWindowClassName) return;
                if (Function->GetNamePrivate() != FName(STR("ReceiveTick"), FNAME_Add)) return;

                UClass* CtrlClass = Object->GetClassPrivate();
                if (!CtrlClass) return;

                auto SetByte = [&](const TCHAR* PropName, uint8 Val) {
                    if (auto* Prop = CtrlClass->GetPropertyByNameInChain(PropName)) {
                        if (auto* Ptr = Prop->ContainerPtrToValuePtr<uint8>(Object)) *Ptr = Val;
                    }
                };
                auto SetInt = [&](const TCHAR* PropName, int32 Val) {
                    if (auto* Prop = CtrlClass->GetPropertyByNameInChain(PropName)) {
                        if (auto* Ptr = Prop->ContainerPtrToValuePtr<int32>(Object)) *Ptr = Val;
                    }
                };

                SetByte(STR("mainstatus"), 0);
                SetByte(STR("changestatus"), 0);
                SetByte(STR("IsGstatusChange"), 0);
                SetByte(STR("IsFinishedOpenWindow"), 1);
                SetByte(STR("IsFinishedCloseWindow"), 1);
                SetByte(STR("AlreadyAddWidget"), 0);
                SetByte(STR("AlreadyRemoveWidget"), 1);
                SetInt(STR("ItemId"), -1);
                SetInt(STR("itemNum"), 0);
                SetInt(STR("ItemAdd"), 0);
            },
            Hook::FCallbackOptions{
                .bReadonly = true,
                .OwnerModName = STR("SMT5VAP"),
                .HookName = STR("ItemWindowForceClose")
            }
        );
        LOG("[PopupSuppression] Active");
    }
}
