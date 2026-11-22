#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "ScriptSyncSystem.h"

#include "World/World.h"
#include "World/Entity/Systems/SystemContext.h"

namespace Lumina
{
    void CScriptSyncSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
    }

    void CScriptSyncSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        CWorld& World = *Context.GetWorld();

        // A change handler may add components, so the walk runs over a snapshot of the entities.
        ECS::FRegistry& Registry = Context.GetRegistry();
        TVector<ECS::FEntity> Entities;
        Registry.View<ECS::Read<SEntityScriptComponent>, ECS::Read<FScriptSyncState>>().ForEachEntity([&](ECS::FEntity Entity) { Entities.Add(Entity); });
        for (ECS::FEntity Entity : Entities)
        {
            const SEntityScriptComponent* Scripts = Registry.TryGet<SEntityScriptComponent>(Entity);
            FScriptSyncState* State = Registry.TryGet<FScriptSyncState>(Entity);
            if (Scripts != nullptr && State != nullptr)
            {
                ScriptNet::PollSync(World, Entity, *Scripts, *State);
            }
        }
    }
}
