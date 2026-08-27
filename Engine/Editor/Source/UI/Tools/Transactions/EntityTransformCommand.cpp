#include "EntityTransformCommand.h"
#include "World/ECS/Registry.h"

#include "Core/Object/Package/Package.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    FEntityTransformCommand::FEntityTransformCommand(CWorld* InWorld, TVector<ECS::FEntity> InEntities)
        : World(InWorld)
        , Entities(Move(InEntities))
    {
        Capture(Before);
    }

    void FEntityTransformCommand::Finalize()
    {
        Capture(After);
    }

    void FEntityTransformCommand::Capture(TVector<FTransform>& Out) const
    {
        // Index-aligned even if an entity dies mid-drag, and a missing one records identity and is skipped.
        Out.assign(Entities.size(), FTransform());

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const ECS::FEntity Entity = Entities[i];
            const STransformComponent* Transform =
                Registry.IsValid(Entity) ? Registry.TryGet<STransformComponent>(Entity) : nullptr;
            if (Transform != nullptr)
            {
                Out[i] = Transform->LocalTransform;
            }
        }
    }

    void FEntityTransformCommand::Apply(const TVector<FTransform>& In) const
    {
        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || In.size() != Entities.size())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const ECS::FEntity Entity = Entities[i];
            if (!Registry.IsValid(Entity) || !Registry.HasAll<STransformComponent>(Entity))
            {
                continue;   // destroyed since the drag; its own transaction owns bringing it back
            }

            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
            Transform.SetLocalTransform(In[i]);

            // The cached dirty signal can fail to raise bAnyDirty, leaving the render primitive pre-undo.
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityTransformCommand::Undo() { Apply(Before); }
    void FEntityTransformCommand::Redo() { Apply(After); }
}
