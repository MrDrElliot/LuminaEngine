#include "RuntimePCH.h"
#include "StateTreeSystem.h"
#include "PathFollowSystem.h"
#include "PerceptionSystem.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "World/World.h"

namespace Lumina
{
    void* SStateTreeComponent::GetParameterMemoryAs(FName TypeName) const
    {
        return GetParameters(ResolveStructByName(TypeName));
    }

    EStateTreeRunStatus SStateTreeDestroyOwnerTask::EnterState(FStateTreeExecutionContext& Context) const
    {
        Context.GetWorld()->DestroyEntity(Context.GetOwner());
        return EStateTreeRunStatus::Running;
    }

    void CStateTreeSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RunsAfter<CPerceptionSystem>();
        RunsBefore<CPathFollowSystem>();
        ExcludeFromHasWork(NodeAccess);
    }

    void CStateTreeSystem::OnUpdate(const SSystemContext& Context)
    {
        const double Time = Context.GetTime();
        CWorld* World = GetOwningWorld();

        // Tasks run on workers here, so their structural changes defer to the command bus like any parallel system's.
        Trees.ParallelForEach([&](ECS::FEntity Entity, ECS::TMut<SStateTreeComponent> TreeMut)
        {
            SStateTreeComponent& Tree = TreeMut.BypassChangeDetection();
            FStateTreeRunningInstance& Running = Tree.Running;
            if (Running.bStopped)
            {
                if (Running.Instance != nullptr)
                {
                    Running.Instance->Stop(Time);
                }
                return;
            }
            if (!Running.bRestart && Running.Tree == Tree.StateTree && (Running.Instance == nullptr || !Running.Instance->IsSuperseded()))
            {
                if (Running.Instance != nullptr)
                {
                    Running.Instance->Tick(Time);
                }
                return;
            }

            if (Running.Instance == nullptr)
            {
                Running.Instance = MakeUnique<FStateTreeInstance>();
            }
            Running.Instance->Stop(Time);
            Running.bRestart = false;
            Running.Tree = Tree.StateTree;
            if (Running.Tree != nullptr)
            {
                Running.Instance->Start(Running.Tree->GetRuntime(), World, Entity, Time, Tree.Parameters.IsValid() ? &Tree.Parameters : nullptr, (uint32)Entity);
            }
        });
    }
}
