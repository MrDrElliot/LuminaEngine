#include "EntityTransformCommand.h"

#include "Core/Object/Package/Package.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    FEntityTransformCommand::FEntityTransformCommand(CWorld* InWorld, TVector<entt::entity> InEntities)
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
        // Sized to Entities unconditionally so Before and After stay index-aligned even if an entity is
        // destroyed mid-drag; a missing one records identity and is skipped again on the way back out.
        Out.assign(Entities.size(), FTransform());

        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const entt::entity Entity = Entities[i];
            if (Registry.valid(Entity) && Registry.all_of<STransformComponent>(Entity))
            {
                Out[i] = Registry.get<STransformComponent>(Entity).LocalTransform;
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const entt::entity Entity = Entities[i];
            if (!Registry.valid(Entity) || !Registry.all_of<STransformComponent>(Entity))
            {
                continue;   // destroyed since the drag; its own transaction owns bringing it back
            }

            STransformComponent& Transform = Registry.get<STransformComponent>(Entity);
            Transform.SetLocalTransform(In[i]);

            // Same tag the gizmo emplaces after writing: SetLocalTransform's cached dirty-signal can fail
            // to raise the registry's bAnyDirty flag, which would leave the world matrix -- and so the
            // render scene's primitive -- reading the pre-undo position until something else moved.
            Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityTransformCommand::Undo() { Apply(Before); }
    void FEntityTransformCommand::Redo() { Apply(After); }
}
