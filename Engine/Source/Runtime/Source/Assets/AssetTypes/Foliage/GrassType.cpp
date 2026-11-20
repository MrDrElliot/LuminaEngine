#include "RuntimePCH.h"
#include "GrassType.h"

#include "GrassPatch.h"
#include "Containers/HashPrimitives.h"
#include "Core/Object/Package/Package.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    // Filled by a worker and read once its flag is set.
    struct FGrassPatchBuild
    {
        TAtomic<bool>             bDone{false};
        FMeshResource             Blade;
        TUniquePtr<FMeshResource> Patch;
    };

    CStaticMesh* CGrassType::GetPatchMesh()
    {
        CStaticMesh* Blade = Mesh.Get();
        const FMeshResource* Resource = Blade != nullptr ? Blade->TryGetMeshResource() : nullptr;
        if (PatchDistance <= 0.0f || Resource == nullptr || Resource->MeshletData.IsEmpty())
        {
            return nullptr;
        }

        const float Scatter[] = { Density, ScaleMin, ScaleMax, Blade->LODDistanceScale };
        uint64 Key = Containers::CombineHash((uint64)(uintptr_t)Blade, Blade->GetGeometryVersion());
        Key = Containers::CombineHash(Key, Containers::HashBytes(Scatter, sizeof(Scatter)));
        Key = Containers::CombineHash(Key, ((uint64)Seed << 1) | (bRandomYaw ? 1u : 0u));

        if (Key != PatchKey)
        {
            PatchKey = Key;
            PatchBuild.reset();
            TSharedPtr<FGrassPatchBuild> Build = MakeShared<FGrassPatchBuild>();
            if (!RestoreSourceStreamsFromMeshlets(*Resource, Build->Blade))
            {
                return PatchMesh.Get();
            }
            PatchBuild = Build;

            GrassPatch::FParams Params;
            Params.Density    = Density;
            Params.ScaleMin   = ScaleMin;
            Params.ScaleMax   = ScaleMax;
            Params.bRandomYaw = bRandomYaw;
            Params.Seed       = Seed;
            Jobs::Spawn(Jobs::EJobPriority::Background, [](TSharedPtr<FGrassPatchBuild> Target, GrassPatch::FParams Input) -> Jobs::TTask<void>
            {
                TUniquePtr<FMeshResource> Patch = MakeUnique<FMeshResource>();
                if (GrassPatch::Build(Target->Blade, Input, *Patch))
                {
                    Target->Patch = Move(Patch);
                }
                Target->Blade = FMeshResource{};
                Target->bDone.store(true, std::memory_order_release);
                co_return;
            }, Build, Params);
        }

        if (PatchBuild && PatchBuild->bDone.load(std::memory_order_acquire))
        {
            if (PatchBuild->Patch)
            {
                CStaticMesh* Patch = NewObject<CStaticMesh>(CPackage::GetTransientPackage());
                Patch->Materials        = Blade->Materials;
                Patch->LODDistanceScale = Blade->LODDistanceScale;
                Patch->SetMeshResource(Move(PatchBuild->Patch));
                PatchMesh = Patch;
            }
            PatchBuild.reset();
        }

        // An older patch keeps standing in while its replacement builds.
        return PatchMesh.Get();
    }
}
