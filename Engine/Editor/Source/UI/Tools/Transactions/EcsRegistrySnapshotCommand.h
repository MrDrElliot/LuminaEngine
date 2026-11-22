#pragma once

#include "World/ECS/Registry.h"

#include "EditorTransaction.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CWorld;

    // TRANSITIONAL bridge: whole-registry before/after snapshot so the world/prefab editors run on the shared manager. Retired in Phase 3.
    class FEcsRegistrySnapshotCommand final : public IUndoableCommand
    {
    public:

        // Edited names the entities the step changes, or is empty when it may change any of them.
        explicit FEcsRegistrySnapshotCommand(CWorld* InWorld, TVector<ECS::FEntity> InEdited = {});

        void Undo() override;
        void Redo() override;
        void Finalize() override;   // captures the after-image

        // Without this every Begin and End pair pushes a step, and a spurious one reverts the whole registry. Byte equality is conservative, so it never calls a real change a no-op.
        bool IsNoOp() const override { return Before == After; }

        void MarkChanged() const override;

    private:

        void Capture(TVector<uint8>& Out) const;
        void Restore(const TVector<uint8>& In) const;

        // Weak, since a strong ref would keep a torn-down world alive and stop a stale undo from no-opping.
        TWeakObjectPtr<CWorld> World;
        TVector<ECS::FEntity> Edited;
        TVector<uint8>     Before;
        TVector<uint8>     After;
    };
}
