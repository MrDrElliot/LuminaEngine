#pragma once

#include "World/ECS/Registry.h"


#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EditorTransaction.h"

namespace Lumina
{
    class CWorld;

    /**
     * Undo record for an operation that only ADDS entities: duplicate, paste, new entity, new primitive.
     *
     * FEcsRegistrySnapshotCommand reflectively serializes the entire registry twice per transaction, and
     * for a duplicate that is almost entirely wasted: the before-image describes a world the operation
     * does not touch, and the after-image re-describes it. In a large level that was over a second of
     * stall and ~60 MB retained on the undo stack, to record the creation of one entity.
     *
     * This records nothing up front. It captures the live entity HANDLE set -- a flat array of 32-bit
     * ids, no reflection -- and at commit diffs it to find what the operation created. Only those
     * entities are serialized, so the stored image is proportional to what was added.
     *
     * CREATION ONLY. An entity DESTROYED inside one of these transactions cannot be brought back, because
     * no before-image of it was ever taken. Finalize logs an error if it sees that happen rather than
     * silently recording an undo step that cannot restore the world; use BeginTransaction (whole-registry)
     * for anything that removes or restructures.
     */
    class FEntityCreationCommand final : public IUndoableCommand
    {
    public:

        explicit FEntityCreationCommand(CWorld* InWorld);

        void Undo() override;
        void Redo() override;
        void Finalize() override;   // diffs the handle set, then serializes what was created

        // Nothing was created -> no undo step, same contract as the registry snapshot's.
        bool IsNoOp() const override { return Created.empty(); }

    private:

        void CaptureLiveEntities(TVector<ECS::FEntity>& Out) const;

        // Weak, since a strong ref would keep a torn-down world alive and stop a stale undo from no-opping.
        TWeakObjectPtr<CWorld>  World;

        // Sorted live handles at the moment the transaction opened.
        TVector<ECS::FEntity>   LiveBefore;

        // What the operation added. Sorted (it is built by walking the sorted after-set), which is what
        // lets Finalize binary-search it when classifying parent links as internal or external.
        TVector<ECS::FEntity>   Created;

        // Serialized image of every entity in Created, for Redo.
        TVector<uint8>          CreatedData;

        // Re-attaching a created root to a PRE-EXISTING parent is the one link the per-entity images
        // cannot carry: the child's own relationship component is restored with it, but the parent's
        // child list belongs to an entity this command never captured.
        struct FExternalParent
        {
            ECS::FEntity Child;
            ECS::FEntity Parent;
        };
        TVector<FExternalParent> ExternalParents;
    };
}
