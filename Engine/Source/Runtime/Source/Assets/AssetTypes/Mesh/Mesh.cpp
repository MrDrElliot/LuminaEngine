#include "RuntimePCH.h"
#include "Mesh.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "assets/assettypes/material/materialinstance.h"
#include "Core/Object/Cast.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Renderer/MeshQuantization.h"


namespace Lumina
{
    void CMesh::Serialize(FArchive& Ar)
    {
        Super::Serialize(Ar);
        
        LUMINA_MEMORY_SCOPE("Meshes");
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

    void CMesh::OnDestroy()
    {
        Super::OnDestroy();

        // The meshlet header address dies with this mesh, but copies of it live on in resolve entries,
        // components and the instance buffer. Without this the next cull dispatch faults the device.
        FMeshResolveCache::InvalidateDependency(this);
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

        // Only the entries that resolved against THIS mesh are wrong. This used to bump the global epoch,
        // which re-resolved every mesh component in every world and re-bound every primitive.
        FMeshResolveCache::InvalidateDependency(this);
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

            // Cooked meshes have no Positions array, so this is the path that actually runs. Driven by
            // the meshlet list because a position is only meaningful against its owning meshlet's anchor.
            for (const FMeshlet& M : MD.Meshlets)
            {
                for (uint32 v = 0; v < M.VertexCount; ++v)
                {
                    const uint32 Index = M.VertexOffset + v;

                    if (Index < MD.MeshletVertices.size())
                    {
                        const FVector3 P = DecodeMeshletPosition(M, MD.MeshletVertices[Index]);
                        BoundingBox.Min = Math::Min(BoundingBox.Min, P);
                        BoundingBox.Max = Math::Max(BoundingBox.Max, P);
                    }

                    if (Index < MD.MeshletSkinnedVertices.size())
                    {
                        const FVector3 P = DecodeMeshletPosition(M, MD.MeshletSkinnedVertices[Index]);
                        BoundingBox.Min = Math::Min(BoundingBox.Min, P);
                        BoundingBox.Max = Math::Max(BoundingBox.Max, P);
                    }
                }
            }

            if (MD.MeshletVertices.empty() && MD.MeshletSkinnedVertices.empty() && !MD.MeshletSpheres.empty())
            {
                // Last resort: each meshlet's bounding sphere squared off. Conservative for culling,
                // wildly loose for anything that is not itself roughly spherical.
                for (const FMeshletSphere& S : MD.MeshletSpheres)
                {
                    BoundingBox.Min = Math::Min(BoundingBox.Min, S.Center - FVector3(S.Radius));
                    BoundingBox.Max = Math::Max(BoundingBox.Max, S.Center + FVector3(S.Radius));
                }
            }
        }
    }

    namespace
    {
        // Replaces any texture already there. Called from CreateForResource, so a mesh whose buffers
        // are rebuilt refreshes the volume with everything else.
        void CreateDistanceFieldTexture(FMeshResource& Resource)
        {
            LUMINA_MEMORY_SCOPE("Meshes");

            RHI::FManagedTexture& Texture = Resource.MeshBuffers.DistanceFieldTexture;

            // Released unconditionally: a rebuild that turned the field OFF has to drop the old volume,
            // and the sentinel written into the header below is what stops shaders reading it.
            RHI::Textures::Release(Texture);

            const FDistanceFieldVolume& Volume = Resource.DistanceField;
            if (!Volume.IsValid())
            {
                return;
            }

            RHI::FTexture3DDesc Desc;
            Desc.Width     = Volume.Dimensions.x;
            Desc.Height    = Volume.Dimensions.y;
            Desc.Depth     = Volume.Dimensions.z;
            Desc.Format    = EFormat::R8_UNORM;
            Desc.DebugName = "Mesh.DistanceField";

            Texture = RHI::Textures::Create(Desc);
            if (!Texture.IsValid())
            {
                LOG_ERROR("Distance field texture creation failed for mesh '{}'; SDF nodes will read as invalid.",
                          Resource.Name.c_str());
                return;
            }

            RHI::Textures::Upload(Texture, 0, Volume.Distances.data(), Volume.Distances.size());
        }

        // Fills the header from the buffer set and volume currently on Resource. Split out so the
        // distance-field refresh path can rewrite the header in place without rebuilding anything else.
        FMeshletHeaderGPU MakeMeshletHeader(const FMeshResource& Resource)
        {
            const FMeshResource::FMeshBuffers& MB = Resource.MeshBuffers;
            const FDistanceFieldVolume& Volume    = Resource.DistanceField;

            const bool bHasField = MB.DistanceFieldTexture.IsValid()
                                && MB.DistanceFieldTexture.SampledSlot != RHI::kInvalidHeapSlot;

            FMeshletHeaderGPU Header;
            Header.MeshletsAddress          = MB.MeshletBuffer;
            Header.SpheresAddress           = MB.MeshletSphereBuffer;
            Header.VerticesAddress          = MB.MeshletVertexBuffer;
            Header.TrianglesAddress         = MB.MeshletTriangleBuffer;
            Header.DistanceFieldIndex       = bHasField ? MB.DistanceFieldTexture.SampledSlot : DistanceField::kInvalidIndex;
            Header.DistanceFieldFlags       = Volume.bTwoSided ? (uint32)EDistanceFieldFlags::TwoSided : 0u;
            Header.DistanceFieldMinX        = Volume.VolumeMin.x;
            Header.DistanceFieldMinY        = Volume.VolumeMin.y;
            Header.DistanceFieldMinZ        = Volume.VolumeMin.z;
            Header.DistanceFieldSizeX       = Volume.VolumeSize.x;
            Header.DistanceFieldSizeY       = Volume.VolumeSize.y;
            Header.DistanceFieldSizeZ       = Volume.VolumeSize.z;
            Header.DistanceFieldMaxDistance = Volume.MaxDistance;
            Header.ConesAddress = MB.MeshletConeBuffer;
            Header._Pad0 = 0;
            return Header;
        }
    }

    void MeshBuffers::RefreshDistanceField(FMeshResource& Resource)
    {
        LUMINA_MEMORY_SCOPE("Meshes");

        // Nothing to publish through: the mesh never got its buffers, so the next CreateForResource
        // picks the volume up anyway.
        if (Resource.MeshBuffers.MeshletHeaderBuffer == 0)
        {
            return;
        }

        CreateDistanceFieldTexture(Resource);

        // Rewritten in place so the header ADDRESS is unchanged and every cached copy stays correct.
        // Frames in flight are safe: the replaced volume is frame-deferred, so the old index still resolves.
        const FMeshletHeaderGPU Header = MakeMeshletHeader(Resource);
        RHI::UploadBuffer(Resource.MeshBuffers.MeshletHeaderBuffer, &Header, sizeof(FMeshletHeaderGPU));
    }

    void MeshBuffers::CreateForResource(FMeshResource& Resource)
    {
        LUMINA_MEMORY_SCOPE("Meshes");

        if (Resource.MeshletData.IsEmpty())
        {
            return;
        }
        
        LUMINA_PROFILE_SCOPE();

        const FMeshletData& MData = Resource.MeshletData;
        const bool bSkinned       = Resource.bSkinnedMesh;
        FMeshResource::FMeshBuffers& MB = Resource.MeshBuffers;

        bool bAllocationFailed = false;

        // The name is what a GPU crash report resolves a faulting address to. These five are reached
        // from the shader through a raw device address, so they are the ones worth naming.
        auto CreateAndUpload = [&bAllocationFailed](const void* Data, uint64 Size, const char* DebugName) -> RHI::GPUPtr
        {
            const RHI::GPUPtr Memory = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            if (Memory == 0)
            {
                bAllocationFailed = true;
                return 0;
            }
            RHI::SetDebugName(Memory, DebugName);
            RHI::UploadBuffer(Memory, Data, Size);
            return Memory;
        };

        // Built into locals and swapped in only once the whole set exists, so a rebuild that runs out of
        // memory keeps rendering what it had instead of losing its geometry.
        const RHI::GPUPtr NewMeshlets = CreateAndUpload(MData.Meshlets.data(), sizeof(FMeshlet) * MData.Meshlets.size(), "Mesh.Meshlets");
        const RHI::GPUPtr NewSpheres  = CreateAndUpload(MData.MeshletSpheres.data(), sizeof(FMeshletSphere) * MData.MeshletSpheres.size(), "Mesh.MeshletSpheres");
        const RHI::GPUPtr NewCones    = CreateAndUpload(MData.MeshletCones.data(), sizeof(FMeshletCone) * MData.MeshletCones.size(), "Mesh.MeshletCones");

        // Checked at resolve against the skeleton's bone count: the GPU bone fetch is unbounded, so a
        // mesh that outruns its skeleton reads garbage matrices for its leaf bones.
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
        const RHI::GPUPtr NewVertices  = CreateAndUpload(VertSrc, VertCount * VertStride, bSkinned ? "Mesh.SkinnedVertices" : "Mesh.Vertices");
        const RHI::GPUPtr NewTriangles = CreateAndUpload(MData.MeshletTriangles.data(), sizeof(uint32) * MData.MeshletTriangles.size(), "Mesh.MeshletTriangles");

        // Nothing downstream null-checks the addresses the header carries, so a partly allocated set can
        // never be published -- it would hand the GPU a null base to fetch vertices through.
        if (bAllocationFailed)
        {
            LOG_ERROR("Mesh rebuild failed: GPU buffer allocation for {} meshlets. Previous geometry kept.", MData.Meshlets.size());

            RHI::Core::Retire(NewMeshlets);
            RHI::Core::Retire(NewSpheres);
            RHI::Core::Retire(NewCones);
            RHI::Core::Retire(NewVertices);
            RHI::Core::Retire(NewTriangles);
            return;
        }

        MB.ReleaseGeometryBuffers();
        MB.MeshletBuffer         = NewMeshlets;
        MB.MeshletSphereBuffer   = NewSpheres;
        MB.MeshletConeBuffer     = NewCones;
        MB.MeshletVertexBuffer   = NewVertices;
        MB.MeshletTriangleBuffer = NewTriangles;

        // Volume upload before the header, because the header publishes its heap slot. A field that
        // failed to allocate leaves the sentinel in place, which is what every SDF shader path gates on.
        CreateDistanceFieldTexture(Resource);

        // Allocated ONCE and rewritten in place forever after: its address is the identity every cached
        // copy holds, so moving it means invalidating the resolve cache and re-uploading every instance.
        const FMeshletHeaderGPU Header = MakeMeshletHeader(Resource);
        if (MB.MeshletHeaderBuffer == 0)
        {
            MB.MeshletHeaderBuffer = CreateAndUpload(&Header, sizeof(FMeshletHeaderGPU), "Mesh.MeshletHeader");
        }
        else
        {
            RHI::UploadBuffer(MB.MeshletHeaderBuffer, &Header, sizeof(FMeshletHeaderGPU));
        }
    }

    bool CMesh::HasDistanceField() const
    {
        return MeshResources != nullptr && MeshResources->DistanceField.IsValid();
    }

    void CMesh::BuildDistanceField()
    {
        if (MeshResources == nullptr)
        {
            return;
        }

        // Build into a scratch volume first. Build() clears its output on every failure path, which
        // would silently destroy a good field whenever a rebuild could not run.
        FDistanceFieldVolume NewVolume;
        const bool bBuilt = DistanceField::Build(*MeshResources, DistanceFieldSettings, NewVolume);

        // A disabled setting is a real instruction to drop the field, not a failure, so the two are
        // distinguished here rather than inside Build.
        if (!bBuilt && DistanceFieldSettings.bEnabled && MeshResources->DistanceField.IsValid())
        {
            return;
        }

        MeshResources->DistanceField = Move(NewVolume);

        // Deliberately NOT GenerateGPUBuffers: that rebuilds the whole meshlet set to publish a volume
        // the existing header can carry on its own.
        MeshBuffers::RefreshDistanceField(*MeshResources);
    }

    void CMesh::GenerateGPUBuffers()
    {
        MeshBuffers::CreateForResource(*MeshResources);

        // Only entries holding THIS mesh need to re-read the header. Bumping the global epoch here made
        // loading one mesh re-resolve every component in the world.
        FMeshResolveCache::InvalidateDependency(this);

        // Drop import-time scratch.
        MeshResources->ClearVertices();
        MeshResources->Indices.clear();
        MeshResources->Indices.shrink_to_fit();
    }
}
