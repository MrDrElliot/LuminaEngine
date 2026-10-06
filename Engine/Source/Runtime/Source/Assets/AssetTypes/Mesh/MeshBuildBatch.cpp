#include "MeshBuildBatch.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Profiler/Profile.h"
#include "TaskSystem/TaskSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/DynamicMeshComponent.h"

namespace Lumina
{
    namespace
    {
        thread_local uint32 GBatchDepth = 0;
        thread_local TVector<TStrongObjectPtr<CMesh>> GPendingMeshes;

        void FinishDynamicMeshes(ECS::FRegistry& Registry)
        {
            TVector<SDynamicMeshComponent*> Pending;
            auto View = Registry.View<SDynamicMeshComponent>();
            for (ECS::FEntity Entity : View)
            {
                SDynamicMeshComponent& Mesh = View.Get<SDynamicMeshComponent>(Entity);
                if (Mesh.bCommitPending)
                {
                    Mesh.bCommitPending = false;
                    Pending.push_back(&Mesh);
                }
            }

            if (Pending.empty())
            {
                return;
            }

            // Each commit touches only its own component, and nothing adds or removes components while this runs.
            LUMINA_PROFILE_SECTION("Batched Dynamic Mesh Commits");
            Task::ParallelFor((uint32)Pending.size(), [&Pending](uint32 Index)
            {
                Task::FInlineNestedScope Inline;
                Pending[Index]->CommitNow();
            });
        }

        void FinishStaticMeshes()
        {
            if (GPendingMeshes.empty())
            {
                return;
            }

            TVector<TStrongObjectPtr<CMesh>> Meshes = Move(GPendingMeshes);
            GPendingMeshes.clear();

            LUMINA_PROFILE_SECTION("Batched Mesh Builds");
            Task::ParallelFor((uint32)Meshes.size(), [&Meshes](uint32 Index)
            {
                if (CMesh* Mesh = Meshes[Index].Get())
                {
                    Task::FInlineNestedScope Inline;
                    Mesh->BuildDeferredMeshlets();
                }
            });

            // Uploads and resolve-cache invalidation stay on this thread.
            for (const TStrongObjectPtr<CMesh>& Mesh : Meshes)
            {
                if (Mesh.Get() != nullptr)
                {
                    Mesh->UploadDeferredBuffers();
                }
            }
        }
    }

    FMeshBuildBatchScope::FMeshBuildBatchScope(ECS::FRegistry& InRegistry)
        : Registry(InRegistry)
    {
        ++GBatchDepth;
    }

    FMeshBuildBatchScope::~FMeshBuildBatchScope()
    {
        // Closed first, so a build that finishing triggers runs at once rather than joining a batch nobody will flush.
        --GBatchDepth;
        FinishDynamicMeshes(Registry);
        if (GBatchDepth == 0)
        {
            FinishStaticMeshes();
        }
    }

    bool FMeshBuildBatchScope::IsOpen()
    {
        return GBatchDepth > 0;
    }

    void FMeshBuildBatchScope::DeferMeshlets(CMesh* Mesh)
    {
        GPendingMeshes.emplace_back(Mesh);
    }
}
