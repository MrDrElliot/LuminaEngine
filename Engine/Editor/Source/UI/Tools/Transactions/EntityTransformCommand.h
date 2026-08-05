#pragma once

#include "Containers/Array.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EditorTransaction.h"
#include "World/Entity/Registry/EntityRegistry.h"

namespace Lumina
{
    class CWorld;

    /**
     * Undo record for a transform edit: the local transforms of the entities that were moved, and nothing
     * else.
     *
     * This exists because FEcsRegistrySnapshotCommand reflectively serializes the ENTIRE registry, twice
     * per transaction (before-image on construct, after-image on Finalize), and then byte-compares the two
     * blobs in IsNoOp. That is fine for a one-off structural edit, but the gizmo opens a transaction the
     * instant a drag begins -- so grabbing an entity in a level holding a foliage component with hundreds
     * of thousands of reflected instances stalled the editor for hundreds of milliseconds, on a frame the
     * user is actively dragging through.
     *
     * The gizmo writes exactly one thing: STransformComponent::LocalTransform, on the selected entities.
     * Everything else that moves -- world matrices, children, the render scene's primitives -- is derived
     * from that by the normal transform resolve, so restoring it is sufficient to restore the edit.
     *
     * Cost is O(selected) instead of O(world).
     */
    class FEntityTransformCommand final : public IUndoableCommand
    {
    public:

        // Captures the before-image immediately, matching FEcsRegistrySnapshotCommand's contract.
        FEntityTransformCommand(CWorld* InWorld, TVector<entt::entity> InEntities);

        void Undo() override;
        void Redo() override;
        void Finalize() override;   // captures the after-image

        // A drag that ended where it started leaves no undo step. Same reasoning as the registry
        // snapshot's, but an exact comparison rather than a conservative one: these are the actual
        // transforms, so equal really does mean nothing happened.
        bool IsNoOp() const override { return Before == After; }

    private:

        void Capture(TVector<FTransform>& Out) const;
        void Apply(const TVector<FTransform>& In) const;

        // Validity-checked handle (not raw): a transaction can outlive a map swap, so Apply must not
        // deref a freed world.
        TObjectPtr<CWorld>      World;
        TVector<entt::entity>   Entities;
        TVector<FTransform>     Before;
        TVector<FTransform>     After;
    };
}
