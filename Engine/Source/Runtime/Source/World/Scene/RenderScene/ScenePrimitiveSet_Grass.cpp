#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "ScenePrimitiveSetInternal.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "MeshResolveCache.h"

namespace Lumina
{
    namespace ScenePrimitiveSetGrassInternal
    {
        // A grass block serves a request it holds, unless it is over four times larger, which only a need that fell away leaves.
        FORCEINLINE bool IsGrassBlockSized(uint32 Held, uint32 Wanted)
        {
            return Held >= Wanted && Held / 4u <= Wanted;
        }
    }

    FScenePrimitiveSet::FGrassSpeciesBinding FScenePrimitiveSet::AcquireGrassSpecies(const FGrassSpeciesKey& Key, CStaticMesh* Mesh, uint32 ResolveHandle, uint32 Capacity)
    {
        if (Mesh == nullptr || Capacity == 0u || !FMeshResolveCache::Get().IsValidHandle(ResolveHandle))
        {
            return {};
        }

        // One surface only, since a multi-material grass mesh would need a block per surface.
        const FResolvedMesh& Resolved = FMeshResolveCache::Get().GetEntry(ResolveHandle);
        if (Resolved.Surfaces.IsEmpty())
        {
            return {};
        }

        if (FGrassSpeciesBinding* Existing = GrassSpecies.Find(Key))
        {
            Existing->SeenSerial = GrassSerial;
            if (Existing->Mesh == Mesh && ScenePrimitiveSetGrassInternal::IsGrassBlockSized(Existing->Capacity, Capacity))
            {
                // The first resolve may have been the default material standing in for one still compiling or loading.
                if (Existing->ResolvedGeneration != Resolved.Generation)
                {
                    BindGrassSurface(*Existing, Resolved);
                }
                return *Existing;
            }

            // Outgrown, far too large or remeshed, so the old block goes back inactive rather than drawing its last blades forever.
            ReleaseGrassBlock(*Existing);
        }

        FGrassSpeciesBinding Result;
        Result.Mesh              = Mesh;
        Result.InstanceSlotBase  = TakeGrassBlock(Capacity);
        Result.SeenSerial        = GrassSerial;
        Result.Capacity          = Capacity;
        Result.MeshletHeaderSlot = Mesh->GetMeshResource().MeshBuffers.MeshletHeaderSlot;
        // Enclosing-sphere radius of the bind-pose AABB, which is what the cull's sphere test wants.
        const FVector3 Extent = Mesh->GetAABB().GetSize() * 0.5f;
        Result.MeshBoundsRadius  = Math::Sqrt(Extent.x * Extent.x + Extent.y * Extent.y + Extent.z * Extent.z);
        Result.bValid            = true;
        BindGrassSurface(Result, Resolved);

        GrassSpecies.Add(Key, Result);
        GrassSlotCount += Capacity;
        return Result;
    }

    bool FScenePrimitiveSet::GrassSpeciesFits(const FGrassSpeciesKey& Key, const CStaticMesh* Mesh, uint32 Capacity) const
    {
        const FGrassSpeciesBinding* Existing = GrassSpecies.Find(Key);
        return Existing != nullptr && Existing->Mesh == Mesh && ScenePrimitiveSetGrassInternal::IsGrassBlockSized(Existing->Capacity, Capacity);
    }

    // Freed blocks are kept whole for the next species, since single slots from the free list could never form one again.
    void FScenePrimitiveSet::ReleaseGrassBlock(const FGrassSpeciesBinding& Binding)
    {
        Batches.ReleaseBatchRef(Binding.BatchIndex, false);
        // The CPU side of a grass slot is never written, so only the device entries the scatter wrote need clearing.
        ZeroedSlotRanges.Emplace(Binding.InstanceSlotBase, Binding.Capacity);
        AddFreeSlotBlock(Binding.InstanceSlotBase, Binding.Capacity);
        GrassSlotCount -= Binding.Capacity;
    }

    // Sorted by base and merged with its neighbors, so repeated release and reacquire cannot fragment the retained arrays.
    void FScenePrimitiveSet::AddFreeSlotBlock(uint32 Base, uint32 Count)
    {
        int32 Insert = 0;
        while (Insert < FreeSlotBlocks.Num() && FreeSlotBlocks[Insert].x < Base)
        {
            ++Insert;
        }
        FreeSlotBlocks.Insert(FUIntVector2(Base, Count), Insert);

        if (Insert + 1 < FreeSlotBlocks.Num() && FreeSlotBlocks[Insert].x + FreeSlotBlocks[Insert].y == FreeSlotBlocks[Insert + 1].x)
        {
            FreeSlotBlocks[Insert].y += FreeSlotBlocks[Insert + 1].y;
            FreeSlotBlocks.RemoveAt(Insert + 1);
        }
        if (Insert > 0 && FreeSlotBlocks[Insert - 1].x + FreeSlotBlocks[Insert - 1].y == FreeSlotBlocks[Insert].x)
        {
            FreeSlotBlocks[Insert - 1].y += FreeSlotBlocks[Insert].y;
            FreeSlotBlocks.RemoveAt(Insert);
        }
    }

    FScenePrimitiveSet::FMeshletDemand FScenePrimitiveSet::GetGrassMeshletDemand() const
    {
        FMeshletDemand Demand;
        for (const auto& [Key, Binding] : GrassSpecies)
        {
            const uint32 Meshlets = VisibilityMeshletsOfSurface(Binding.SurfaceDescIndex);
            Demand.Words    += (uint64)Binding.Capacity * RenderUtils::GetGroupCount(Meshlets, MESHLET_CULL_GROUP_SIZE);
            Demand.Meshlets += (uint64)Binding.Capacity * Meshlets;
        }
        return Demand;
    }

    void FScenePrimitiveSet::KeepGrassSpecies(ECS::FEntity Terrain)
    {
        for (auto& [Key, Binding] : GrassSpecies)
        {
            if (Key.Terrain == Terrain)
            {
                Binding.SeenSerial = GrassSerial;
            }
        }
    }

    uint32 FScenePrimitiveSet::TakeGrassBlock(uint32 Capacity)
    {
        for (int32 Index = 0; Index < FreeSlotBlocks.Num(); ++Index)
        {
            FUIntVector2& Block = FreeSlotBlocks[Index];
            if (Block.y < Capacity)
            {
                continue;
            }
            const uint32 Base = Block.x;
            Block.x += Capacity;
            Block.y -= Capacity;
            if (Block.y == 0u)
            {
                FreeSlotBlocks.RemoveAt(Index);
            }
            // The block starts inactive so a frame that dispatches no scatter draws nothing rather than garbage.
            ZeroedSlotRanges.Emplace(Base, Capacity);
            return Base;
        }
        return AllocateInstanceSlotBlock(Capacity);
    }

    void FScenePrimitiveSet::ReleaseUnseenGrassSpecies()
    {
        for (auto It = GrassSpecies.begin(); It != GrassSpecies.end();)
        {
            if (It->Value.SeenSerial != GrassSerial)
            {
                ReleaseGrassBlock(It->Value);
                It = GrassSpecies.Remove(It);
            }
            else
            {
                ++It;
            }
        }
        ++GrassSerial;
    }

    void FScenePrimitiveSet::BindGrassSurface(FGrassSpeciesBinding& Binding, const FResolvedMesh& Resolved)
    {
        const FResolvedSurface& Surface = Resolved.Surfaces[0];

        const uint32 BatchIndex = Batches.FindOrAddBatch(Surface);
        Batches.AddBatchRef(BatchIndex, false);
        if (Binding.ResolvedGeneration != 0u)
        {
            Batches.ReleaseBatchRef(Binding.BatchIndex, false);
        }

        // Grass is never skinned and always has geometry once its mesh resolved; Active is set per blade by the scatter.
        EInstanceFlags Flags = EInstanceFlags::HasGeometry | Surface.MaterialFlags;
        // Blades re-scatter every frame and sit far under a field voxel, so millions of them would only crowd the world distance field's casters.
        Flags &= ~EInstanceFlags::HasDistanceField;

        Binding.BatchIndex         = BatchIndex;
        Binding.DrawIDAndFlags     = PackDrawIDAndFlags(BatchIndex, Flags);
        Binding.SurfaceDescIndex   = InternSurfaceDesc(Surface);
        Binding.MaterialIndex      = Surface.MaterialIdx;
        Binding.ResolvedGeneration = Resolved.Generation;
    }
}
