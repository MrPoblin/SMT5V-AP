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
    static std::atomic<bool> s_BlockChests{false};
    static std::atomic<bool> s_BlockRelics{false};
    static std::atomic<bool> s_BlockAogamiDebris{false};

    void SetBlockNextSpawn(bool block) {
        if (block) {
            s_BlockCount.fetch_add(1, std::memory_order_release);
        }
    }

    void SetBlockChests(bool block) { s_BlockChests.store(block, std::memory_order_release); }
    void SetBlockRelics(bool block) { s_BlockRelics.store(block, std::memory_order_release); }
    void SetBlockAogamiDebris(bool block) { s_BlockAogamiDebris.store(block, std::memory_order_release); }
    bool IsBlockingChests() { return s_BlockChests.load(std::memory_order_acquire); }
    bool IsBlockingRelics() { return s_BlockRelics.load(std::memory_order_acquire); }
    bool IsBlockingAogamiDebris() { return s_BlockAogamiDebris.load(std::memory_order_acquire); }

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
                    LOG("[PopupSuppression] Blocked BeginPlay on {}", Actor->GetClassPrivate()->GetName());
                    Data.PreventOriginalFunctionCall();
                    s_BlockCount.fetch_sub(1, std::memory_order_release);
                }
            },
            Hook::FCallbackOptions{
                .bReadonly = false,
                .OwnerModName = STR("SMT5VAP"),
                .HookName = STR("BlockPieceBeginPlay")
            }
        );
        LOG("[PopupSuppression] Active");
    }
}
