#include "pch.h"
#include "Mesh.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "assets/assettypes/material/materialinstance.h"
#include "Core/Object/Cast.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"


namespace Lumina
{
    void CMesh::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Meshes");

        Super::Serialize(Ar);

        if (!MeshResources)
        {
            MeshResources = MakeUnique<FMeshResource>();
        }

        Ar << *MeshResources;
    }

    void CMesh::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Meshes");

        GenerateBoundingBox();

        // Fallback for procedurally-generated meshes that bypass the import finalize pass.
        if (MeshResources && MeshResources->MeshletData.IsEmpty() && !MeshResources->Indices.empty())
        {
            Import::Mesh::GenerateMeshlets(*MeshResources);
        }

        GenerateGPUBuffers();
    }

    CMaterialInterface* CMesh::GetMaterialAtSlot(size_t Slot) const
    {
        if (Materials.size() <= Slot)
        {
            return nullptr;
        }
        
        return Materials.empty() ? nullptr : Materials[Slot].Get();
    }

    void CMesh::SetMaterialAtSlot(size_t Slot, CMaterialInterface* NewMaterial)
    {
        if (Materials.size() <= Slot)
        {
            Materials.push_back(NewMaterial);
        }
        else
        {
            Materials[Slot] = NewMaterial;
        }

        FMeshResolveCache::BumpEpoch();
    }

    void CMesh::SetMeshResource(TUniquePtr<FMeshResource>&& NewResource)
    {
        MeshResources = eastl::move(NewResource);
        GenerateBoundingBox();

        // ThumbnailManager's primitive meshes arrive without baked meshlets.
        if (MeshResources && MeshResources->MeshletData.IsEmpty() && !MeshResources->Indices.empty())
        {
            Import::Mesh::GenerateMeshlets(*MeshResources);
        }

        GenerateGPUBuffers();
    }

    bool CMesh::IsReadyForRender() const
    {
        LUMINA_PROFILE_SCOPE();

        if (HasAnyFlag(OF_NeedsLoad))
        {
            return false;
        }

        for (CMaterialInterface* Material : Materials)
        {
            if (Material == nullptr)
            {
                return false;
            }

            if (Material->IsReadyForRender() == false)
            {
                return false;
            }
        }

        return !Materials.empty();
    }

    void CMesh::GenerateBoundingBox()
    {
        BoundingBox.Min = { FLT_MAX, FLT_MAX, FLT_MAX };
        BoundingBox.Max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

        if (MeshResources && MeshResources->GetNumVertices() > 0)
        {
            for (const FVector3& P : MeshResources->Positions)
            {
                BoundingBox.Min = Math::Min(BoundingBox.Min, P);
                BoundingBox.Max = Math::Max(BoundingBox.Max, P);
            }
            return;
        }
        
        if (MeshResources && !MeshResources->MeshletData.IsEmpty())
        {
            const FMeshletData& MD = MeshResources->MeshletData;

            if (!MD.MeshletBounds.empty())
            {
                for (const FMeshletBounds& B : MD.MeshletBounds)
                {
                    BoundingBox.Min = Math::Min(BoundingBox.Min, B.Center - FVector3(B.Radius));
                    BoundingBox.Max = Math::Max(BoundingBox.Max, B.Center + FVector3(B.Radius));
                }
            }
            else
            {
                // Fallback (no bounds stored): union the meshlet vertex positions (now full float3).
                for (const FMeshletVertex& V : MD.MeshletVertices)
                {
                    BoundingBox.Min = Math::Min(BoundingBox.Min, V.Position);
                    BoundingBox.Max = Math::Max(BoundingBox.Max, V.Position);
                }
                for (const FMeshletSkinnedVertex& V : MD.MeshletSkinnedVertices)
                {
                    BoundingBox.Min = Math::Min(BoundingBox.Min, V.Position);
                    BoundingBox.Max = Math::Max(BoundingBox.Max, V.Position);
                }
            }
        }
    }

    void CMesh::GenerateGPUBuffers()
    {
        if (!MeshResources->MeshletData.IsEmpty())
        {
            const FMeshletData& MData = MeshResources->MeshletData;
            const bool bSkinned       = MeshResources->bSkinnedMesh;
            FMeshResource::FMeshBuffers& MB = MeshResources->MeshBuffers;

            auto CreateAndUpload = [](const void* Data, uint64 Size) -> RHI::GPUPtr
            {
                const RHI::GPUPtr Memory = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                RHI::UploadBuffer(Memory, Data, Size);
                return Memory;
            };

            MB.MeshletBuffer       = CreateAndUpload(MData.Meshlets.data(), sizeof(FMeshlet) * MData.Meshlets.size());
            MB.MeshletBoundsBuffer = CreateAndUpload(MData.MeshletBounds.data(), sizeof(FMeshletBounds) * MData.MeshletBounds.size());

            const void*  VertSrc    = bSkinned ? (const void*)MData.MeshletSkinnedVertices.data() : (const void*)MData.MeshletVertices.data();
            const uint64 VertStride = bSkinned ? sizeof(FMeshletSkinnedVertex) : sizeof(FMeshletVertex);
            const uint64 VertCount  = bSkinned ? MData.MeshletSkinnedVertices.size() : MData.MeshletVertices.size();
            MB.MeshletVertexBuffer   = CreateAndUpload(VertSrc, VertCount * VertStride);
            MB.MeshletTriangleBuffer = CreateAndUpload(MData.MeshletTriangles.data(), sizeof(uint32) * MData.MeshletTriangles.size());

            FMeshletHeaderGPU Header;
            Header.MeshletsAddress    = MB.MeshletBuffer;
            Header.BoundsAddress      = MB.MeshletBoundsBuffer;
            Header.VerticesAddress    = MB.MeshletVertexBuffer;
            Header.TrianglesAddress   = MB.MeshletTriangleBuffer;

            MB.MeshletHeaderBuffer = CreateAndUpload(&Header, sizeof(FMeshletHeaderGPU));
        }

        // Reimport, procedural rebuild and dynamic mesh Commit all land here.
        FMeshResolveCache::BumpEpoch();

        // Drop import-time scratch.
        MeshResources->ClearVertices();
        MeshResources->Indices.clear();
        MeshResources->Indices.shrink_to_fit();
    }
}
