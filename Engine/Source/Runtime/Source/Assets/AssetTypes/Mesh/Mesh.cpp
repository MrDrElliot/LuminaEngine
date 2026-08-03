#include "RuntimePCH.h"
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

        // Zero when GPU buffer creation failed; the header address is fetched through unguarded.
        if (MeshResources == nullptr || MeshResources->MeshBuffers.MeshletHeaderBuffer == 0)
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

            // Vertex positions first. A cooked mesh has no Positions array, so this is the path that
            // actually runs for anything imported, and it is exact.
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

            if (MD.MeshletVertices.empty() && MD.MeshletSkinnedVertices.empty() && !MD.MeshletBounds.empty())
            {
                // Last resort only. Each meshlet contributes a cube of side 2*Radius around its
                // centre, which is its bounding SPHERE squared off: conservative, never wrong for
                // culling, and wildly loose for anything that is not itself roughly spherical. A tall
                // thin mesh came out looking like a cube several times its real size.
                for (const FMeshletBounds& B : MD.MeshletBounds)
                {
                    BoundingBox.Min = Math::Min(BoundingBox.Min, B.Center - FVector3(B.Radius));
                    BoundingBox.Max = Math::Max(BoundingBox.Max, B.Center + FVector3(B.Radius));
                }
            }
        }
    }

    void MeshBuffers::CreateForResource(FMeshResource& Resource)
    {
        if (Resource.MeshletData.IsEmpty())
        {
            return;
        }
        
        LUMINA_PROFILE_SCOPE();

        const FMeshletData& MData = Resource.MeshletData;
        const bool bSkinned       = Resource.bSkinnedMesh;
        FMeshResource::FMeshBuffers& MB = Resource.MeshBuffers;

        bool bAllocationFailed = false;
        auto CreateAndUpload = [&bAllocationFailed](const void* Data, uint64 Size) -> RHI::GPUPtr
        {
            const RHI::GPUPtr Memory = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            if (Memory == 0)
            {
                bAllocationFailed = true;
                return 0;
            }
            RHI::UploadBuffer(Memory, Data, Size);
            return Memory;
        };

        MB.MeshletBuffer       = CreateAndUpload(MData.Meshlets.data(), sizeof(FMeshlet) * MData.Meshlets.size());
        MB.MeshletBoundsBuffer = CreateAndUpload(MData.MeshletBounds.data(), sizeof(FMeshletBounds) * MData.MeshletBounds.size());

        // Highest joint index the packed vertices actually reference. Checked at resolve against the
        // skeleton's bone count -- the GPU bone fetch is unbounded, so a mesh that outruns its skeleton
        // reads garbage matrices for its leaf bones.
        if (bSkinned)
        {
            uint32 MaxJoint = 0;
            for (const FMeshletSkinnedVertex& V : MData.MeshletSkinnedVertices)
            {
                const uint32 Packed = V.JointIndices;
                for (uint32 b = 0; b < 4u; ++b)
                {
                    // Only influences with a non-zero weight can actually be fetched.
                    const uint32 Weight = (V.JointWeights >> (b * 8u)) & 0xFFu;
                    if (Weight != 0u)
                    {
                        MaxJoint = Math::Max(MaxJoint, ((Packed >> (b * 8u)) & 0xFFu) + 1u);
                    }
                }
            }
            Resource.RequiredBoneCount = MaxJoint;
        }

        const void*  VertSrc    = bSkinned ? (const void*)MData.MeshletSkinnedVertices.data() : (const void*)MData.MeshletVertices.data();
        const uint64 VertStride = bSkinned ? sizeof(FMeshletSkinnedVertex) : sizeof(FMeshletVertex);
        const uint64 VertCount  = bSkinned ? MData.MeshletSkinnedVertices.size() : MData.MeshletVertices.size();
        MB.MeshletVertexBuffer   = CreateAndUpload(VertSrc, VertCount * VertStride);
        MB.MeshletTriangleBuffer = CreateAndUpload(MData.MeshletTriangles.data(), sizeof(uint32) * MData.MeshletTriangles.size());

        // Nothing downstream null-checks the addresses the header carries, so a mesh that only
        // partly allocated has to drop out entirely rather than hand the GPU a null base to fetch
        // vertices through. IsReadyForRender gates on MeshletHeaderBuffer.
        if (bAllocationFailed)
        {
            LOG_ERROR("Mesh left unrenderable: GPU buffer allocation failed for {} meshlets.", MData.Meshlets.size());

            RHI::Core::DeferredFree(MB.MeshletBuffer);
            RHI::Core::DeferredFree(MB.MeshletBoundsBuffer);
            RHI::Core::DeferredFree(MB.MeshletVertexBuffer);
            RHI::Core::DeferredFree(MB.MeshletTriangleBuffer);

            MB.MeshletBuffer         = 0;
            MB.MeshletBoundsBuffer   = 0;
            MB.MeshletVertexBuffer   = 0;
            MB.MeshletTriangleBuffer = 0;
            MB.MeshletHeaderBuffer   = 0;
            return;
        }

        FMeshletHeaderGPU Header;
        Header.MeshletsAddress    = MB.MeshletBuffer;
        Header.BoundsAddress      = MB.MeshletBoundsBuffer;
        Header.VerticesAddress    = MB.MeshletVertexBuffer;
        Header.TrianglesAddress   = MB.MeshletTriangleBuffer;

        MB.MeshletHeaderBuffer = CreateAndUpload(&Header, sizeof(FMeshletHeaderGPU));
    }

    void CMesh::GenerateGPUBuffers()
    {
        MeshBuffers::CreateForResource(*MeshResources);
        
        FMeshResolveCache::BumpEpoch();

        // Drop import-time scratch.
        MeshResources->ClearVertices();
        MeshResources->Indices.clear();
        MeshResources->Indices.shrink_to_fit();
    }
}
