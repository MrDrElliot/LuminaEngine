#pragma once

#include "World/ECS/Registry.h"


#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EditorTransaction.h"

namespace Lumina
{
    class CWorld;

    // Undo record for a hierarchy edit, holding where each affected entity sat among its parent's children.
    class FEntityRelationshipCommand final : public IUndoableCommand
    {
    public:

        // Captures the before-image immediately, matching FEcsRegistrySnapshotCommand's contract.
        FEntityRelationshipCommand(CWorld* InWorld, TVector<ECS::FEntity> InEntities);

        void Undo() override;
        void Redo() override;

        // Captures the after-image.
        void Finalize() override;

        bool IsNoOp() const override;

        // Every entity whose links an edit on Seeds can rewrite, siblings of both parents included.
        static void CollectAffected(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Seeds,
                                    ECS::FEntity NewParent, TVector<ECS::FEntity>& Out);

    private:

        struct FRecord
        {
            ECS::FEntity Parent;
            uint32       SiblingIndex = 0;

            bool operator == (const FRecord&) const = default;
        };

        void Capture(TVector<FRecord>& Out) const;
        void Apply(const TVector<FRecord>& In) const;

        // Weak, since a strong ref would keep a torn-down world alive and stop a stale undo from no-opping.
        TWeakObjectPtr<CWorld>  World;

        TVector<ECS::FEntity>   Entities;
        TVector<FRecord>        Before;
        TVector<FRecord>        After;
    };
}
