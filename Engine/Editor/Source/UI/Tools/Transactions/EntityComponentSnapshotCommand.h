#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EditorTransaction.h"
#include "World/Entity/Registry/EntityRegistry.h"

namespace Lumina
{
    class CStruct;
    class CWorld;
    struct FComponentOps;

    // Undo record for a property edit, costing O(edited component x selection) instead of O(world).
    class FEntityComponentSnapshotCommand final : public IUndoableCommand
    {
    public:

        // Captures the before-image immediately, matching FEcsRegistrySnapshotCommand's contract.
        FEntityComponentSnapshotCommand(CWorld* InWorld, TVector<entt::entity> InEntities, CStruct* InComponentType);

        // False for a type with no registered component ops, which has to fall back to the registry form.
        static bool CanSnapshotComponent(const CStruct* ComponentType);

        void Undo() override;
        void Redo() override;

        // Captures the after-image.
        void Finalize() override;

        // An edit that ended where it started leaves no undo step.
        bool IsNoOp() const override { return Before == After; }

    private:

        void Capture(TVector<uint8>& Out) const;
        void Restore(const TVector<uint8>& In) const;

        // Weak, since a strong ref would keep a torn-down world alive and stop a stale undo from no-opping.
        TWeakObjectPtr<CWorld>  World;

        TVector<entt::entity>   Entities;
        CStruct*                ComponentType = nullptr;

        // Null for a type with no registered ops, which disables the command rather than half-applying it.
        const FComponentOps*    Ops = nullptr;

        TVector<uint8>          Before;
        TVector<uint8>          After;
    };
}
