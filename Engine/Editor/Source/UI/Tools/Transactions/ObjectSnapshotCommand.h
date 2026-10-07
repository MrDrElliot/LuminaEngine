#pragma once

#include "EditorTransaction.h"
#include "Containers/Function.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CObject;
    class FArchive;

    // Before/after snapshot of a CObject's tagged properties; gives every property-grid asset editor undo for free.
    class FObjectSnapshotCommand final : public IUndoableCommand
    {
    public:

        FObjectSnapshotCommand(CObject* InObject, FName InName);

        // Writes or reads everything an undo of Object has to carry, for an object whose data is not all reflected.
        using FSerializer = TFunction<void(FArchive&, CObject*)>;

        // Both images already taken, for a caller that noticed the edit only after it happened.
        FObjectSnapshotCommand(CObject* InObject, FName InName, TVector<uint8> InBefore, TVector<uint8> InAfter,
            FSerializer InSerializer = FSerializer(), TVector<TStrongObjectPtr<CObject>> InReferenced = {});

        static void Capture(CObject* Object, TVector<uint8>& Out, const FSerializer& Serializer = FSerializer());

        // Holds every object Object's reflected fields point at, so a sub-object an edit dropped outlives it for an undo.
        static void CollectReferenced(CObject* Object, TVector<TStrongObjectPtr<CObject>>& Out);

        void Undo() override;
        void Redo() override;
        void Finalize() override;
        bool IsNoOp() const override { return Before == After; }
        FName GetName() const override { return Name; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override
        {
            Object = Func(Object.Get());
            for (TStrongObjectPtr<CObject>& Held : Referenced)
            {
                Held = Func(Held.Get());
            }
        }

    private:

        void Restore(const TVector<uint8>& In) const;

        TStrongObjectPtr<CObject> Object;
        FName               Name;
        TVector<uint8>      Before;
        TVector<uint8>      After;
        bool                bAfterCaptured = false;
        FSerializer         Serializer;
        TVector<TStrongObjectPtr<CObject>> Referenced;
    };
}
