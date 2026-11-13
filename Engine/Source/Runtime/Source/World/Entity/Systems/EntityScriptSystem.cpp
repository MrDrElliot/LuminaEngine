#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "EntityScriptSystem.h"

#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "World/Entity/Systems/SystemContext.h"

namespace Lumina
{
    void CEntityScriptSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RequireUpdate(EUpdateStage::FixedUpdate);
        RequireUpdate(EUpdateStage::PostPhysics);
    }

    void CEntityScriptSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        ECS::FRegistry& Registry = Context.GetRegistry();

        switch (Context.GetUpdateStage())
        {
        case EUpdateStage::PrePhysics:
            EntityScripts::Tick(Registry, Context.GetDeltaTime(), EScriptUpdatePhase::PrePhysics);
            break;

        case EUpdateStage::FixedUpdate:
        {
            CWorld* World = Context.GetWorld();
            if (World == nullptr || World->GetPhysicsScene() == nullptr)
            {
                EntityScripts::TickFixed(Registry, Context.GetDeltaTime());
            }
            break;
        }

        default:
            // After the step, so an [Reflect(UpdatePhase = PostPhysics)] script reads settled transforms.
            EntityScripts::Tick(Registry, Context.GetDeltaTime(), EScriptUpdatePhase::PostPhysics);
            break;
        }
    }
}
