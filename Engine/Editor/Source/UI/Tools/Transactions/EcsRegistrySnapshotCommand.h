#pragma once

#include "EditorTransaction.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CWorld;

    // TRANSITIONAL bridge: whole-registry before/after snapshot so the world/prefab editors run on the shared manager. Retired in Phase 3.
    class FEcsRegistrySnapshotCommand final : public IUndoableCommand
    {
    public:

        explicit FEcsRegistrySnapshotCommand(CWorld* InWorld);

        void Undo() override;
        void Redo() override;
        void Finalize() override;   // captures the after-image

        // Without this the base returns false and EVERY Begin/End pair pushes an undo step, including the
        // ones where nothing changed -- merely focusing a property row fires Started/Finished. That matters
        // more here than for a CObject: this command restores the WHOLE registry, so a spurious step does
        // not waste one press, it reverts every unrelated change made since that snapshot.
        //
        // Byte equality can only be conservative: two captures of identical state could differ if a
        // component pool was assured in between (storage iteration order), which keeps the transaction. It
        // cannot report a real change as a no-op.
        bool IsNoOp() const override { return Before == After; }

    private:

        void Capture(TVector<uint8>& Out) const;
        void Restore(const TVector<uint8>& In) const;

        // Validity-checked handle (not raw): a transaction can outlive a map swap, so Restore must not deref a freed world.
        TObjectPtr<CWorld> World;
        TVector<uint8>     Before;
        TVector<uint8>     After;
    };
}
