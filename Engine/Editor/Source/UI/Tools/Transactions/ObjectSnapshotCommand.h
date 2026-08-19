#pragma once

#include "EditorTransaction.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CObject;

    // Before/after snapshot of a CObject's tagged properties; gives every property-grid asset editor undo for free.
    class FObjectSnapshotCommand final : public IUndoableCommand
    {
    public:

        FObjectSnapshotCommand(CObject* InObject, FName InName);

        void Undo() override;
        void Redo() override;
        void Finalize() override;
        bool IsNoOp() const override { return Before == After; }
        FName GetName() const override { return Name; }

    private:

        void Capture(TVector<uint8>& Out) const;
        void Restore(const TVector<uint8>& In) const;

        TObjectPtr<CObject> Object;
        FName               Name;
        TVector<uint8>      Before;
        TVector<uint8>      After;
    };
}
