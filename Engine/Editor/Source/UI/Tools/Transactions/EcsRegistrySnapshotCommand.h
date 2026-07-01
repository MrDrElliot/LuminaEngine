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

    private:

        void Capture(TVector<uint8>& Out) const;
        void Restore(const TVector<uint8>& In) const;

        // Validity-checked handle (not raw): a transaction can outlive a map swap, so Restore must not deref a freed world.
        TObjectPtr<CWorld> World;
        TVector<uint8>     Before;
        TVector<uint8>     After;
    };
}
