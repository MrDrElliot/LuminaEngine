#include "RuntimePCH.h"
#include "Mesh.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Core/Object/Cast.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/MeshletHeaderSlab.h"
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

        // A cooked mesh has no Positions, so regenerating would replace the baked box with a lossy decode.
        if (!BoundingBox.IsValid())
        {
            GenerateBoundingBox();
        }

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

        // Copies of the header address live on in resolve entries, so the next cull would fault the device.
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

        // This used to bump the global epoch, re-resolving every mesh component in every world.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMesh::SetMeshResource(TUniquePtr<FMeshResource>&& NewResource)
    {
        MeshResources = std::move(NewResource);
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
        if (MeshResources == nullptr || MeshResources->MeshBuffers.MeshletHeaderSlot == MeshletHeaderSlab::kNullSlot)
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

            // A cooked mesh has no Positions array, and a position is only meaningful against its meshlet anchor.
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
                // Conservative for culling but wildly loose for anything not roughly spherical.
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
        // Called from CreateForResource, so a mesh whose buffers rebuild refreshes the volume too.
        void CreateDistanceFieldTexture(FMeshResource& Resource)
        {
            LUMINA_MEMORY_SCOPE("Meshes");

            RHI::FManagedTexture& Texture = Resource.MeshBuffers.DistanceFieldTexture;

            // A rebuild that turned the field off must drop the old volume, and the sentinel stops shaders reading it.
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

        // Meshes built by the importer already carry palettes and are left alone.
        void BuildBonePalettes(FMeshletData& MData)
        {
            if (MData.MeshletBonePalettes.size() == MData.Meshlets.size())
            {
                return;
            }

            LUMINA_PROFILE_SCOPE();

            MData.MeshletBonePalettes.clear();
            MData.MeshletBonePalettes.reserve(MData.Meshlets.size());
            MData.MeshletBoneIndices.clear();
            MData.MeshletBoneIndices.reserve(MData.Meshlets.size() * 16);

            const uint32 VertexTotal = (uint32)MData.MeshletSkinnedVertices.size();

            FMeshletPaletteScratch Palette;

            for (const FMeshlet& Meshlet : MData.Meshlets)
            {
                // Vertices past MESHLET_MAX_VERTICES are skipped because no shader path reads them.
                const uint32 Begin = Meshlet.VertexOffset;
                const uint32 End   = Math::Min(Begin + Math::Min(Meshlet.VertexCount, (uint32)MESHLET_MAX_VERTICES),
                                               VertexTotal);

                Palette.clear();

                for (uint32 v = Begin; v < End; ++v)
                {
                    FMeshletSkinnedVertex& Vertex = MData.MeshletSkinnedVertices[v];

                    uint32 Local = 0;
                    for (uint32 b = 0; b < 4u; ++b)
                    {
                        const uint32 Shift  = b * 8u;
                        const uint32 Weight = (Vertex.JointWeights >> Shift) & 0xFFu;

                        // A zero-weight influence contributes nothing, so it costs no palette entry.
                        const uint32 Slot = (Weight != 0u)
                            ? FindOrAddPaletteBone(Palette, (Vertex.JointIndices >> Shift) & 0xFFu)
                            : 0u;
                        Local |= Slot << Shift;
                    }
                    Vertex.JointIndices = Local;
                }

                AppendMeshletBonePalette(MData, Palette);
            }
        }

        // Split out so the distance-field refresh can rewrite the header without rebuilding anything else.
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
            Header.ConesAddress             = MB.MeshletConeBuffer;
            Header.BonePalettesAddress      = MB.MeshletBonePaletteBuffer;
            Header.BoneIndicesAddress       = MB.MeshletBoneIndexBuffer;
            Header.MeshletCount             = MB.MeshletCount;
            return Header;
        }
    }

    void MeshBuffers::RefreshDistanceField(FMeshResource& Resource)
    {
        LUMINA_MEMORY_SCOPE("Meshes");

        // The mesh never got its buffers, so the next CreateForResource picks the volume up anyway.
        if (Resource.MeshBuffers.MeshletHeaderSlot == MeshletHeaderSlab::kNullSlot)
        {
            return;
        }

        CreateDistanceFieldTexture(Resource);

        // Rewritten into the same SLOT, and the replaced volume is frame-deferred so the old index resolves.
        MeshletHeaderSlab::Write(Resource.MeshBuffers.MeshletHeaderSlot, MakeMeshletHeader(Resource));
    }

    void MeshBuffers::CreateForResource(FMeshResource& Resource)
    {
        LUMINA_MEMORY_SCOPE("Meshes");

        if (Resource.MeshletData.IsEmpty())
        {
            return;
        }
        
        LUMINA_PROFILE_SCOPE();

        const bool bSkinned = Resource.bSkinnedMesh;
        if (bSkinned)
        {
            BuildBonePalettes(Resource.MeshletData);
        }

        const FMeshletData& MData = Resource.MeshletData;
        FMeshResource::FMeshBuffers& MB = Resource.MeshBuffers;

        // The GPU bone fetch is unbounded, so a mesh outrunning its skeleton reads garbage matrices.
        if (bSkinned)
        {
            uint32 MaxJoint = 0;
            for (uint32 GlobalBone : MData.MeshletBoneIndices)
            {
                MaxJoint = Math::Max(MaxJoint, GlobalBone + 1u);
            }
            Resource.RequiredBoneCount = MaxJoint;
        }

        const void*  VertSrc    = bSkinned ? (const void*)MData.MeshletSkinnedVertices.data() : (const void*)MData.MeshletVertices.data();
        const uint64 VertStride = bSkinned ? sizeof(FMeshletSkinnedVertex) : sizeof(FMeshletVertex);
        const uint64 VertCount  = bSkinned ? MData.MeshletSkinnedVertices.size() : MData.MeshletVertices.size();

        const uint64 MeshletBytes  = sizeof(FMeshlet)       * MData.Meshlets.size();
        const uint64 SphereBytes   = sizeof(FMeshletSphere) * MData.MeshletSpheres.size();
        const uint64 ConeBytes     = sizeof(FMeshletCone)   * MData.MeshletCones.size();
        const uint64 VertexBytes   = VertCount              * VertStride;
        const uint64 TriangleBytes = sizeof(uint32)         * MData.MeshletTriangles.size();
        const uint64 PaletteBytes  = sizeof(FMeshletBonePalette) * MData.MeshletBonePalettes.size();
        const uint64 BoneIdxBytes  = sizeof(uint32)         * MData.MeshletBoneIndices.size();
        
        if (MeshletBytes == 0 || SphereBytes == 0 || ConeBytes == 0 || VertexBytes == 0 || TriangleBytes == 0)
        {
            LOG_ERROR("Mesh rebuild failed: {} meshlets with an empty geometry stream "
                      "(meshlets {}, spheres {}, cones {}, vertices {}, triangles {} bytes). Previous geometry kept.",
                      MData.Meshlets.size(), MeshletBytes, SphereBytes, ConeBytes, VertexBytes, TriangleBytes);
            return;
        }
        
        uint64 Cursor = 0;
        auto Reserve = [&Cursor](uint64 Bytes)
        {
            const uint64 Offset = Math::AlignUp(Cursor, (uint64)RHI::kDefaultAlign);
            Cursor = Offset + Bytes;
            return Offset;
        };

        const uint64 MeshletOffset  = Reserve(MeshletBytes);
        const uint64 SphereOffset   = Reserve(SphereBytes);
        const uint64 ConeOffset     = Reserve(ConeBytes);
        const uint64 VertexOffset   = Reserve(VertexBytes);
        const uint64 TriangleOffset = Reserve(TriangleBytes);
        const uint64 PaletteOffset  = Reserve(PaletteBytes);
        const uint64 BoneIdxOffset  = Reserve(BoneIdxBytes);
        
        const RHI::GPUPtr Block = RHI::Malloc(Cursor, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
        
        if (Block == 0)
        {
            LOG_ERROR("Mesh rebuild failed: {} KiB GPU allocation for {} meshlets. Previous geometry kept.",
                      Cursor / 1024, MData.Meshlets.size());
            return;
        }
        
        RHI::SetDebugName(Block, bSkinned ? "Mesh.SkinnedGeometry" : "Mesh.Geometry");

        RHI::UploadBuffer(Block + MeshletOffset,  MData.Meshlets.data(),         MeshletBytes);
        RHI::UploadBuffer(Block + SphereOffset,   MData.MeshletSpheres.data(),   SphereBytes);
        RHI::UploadBuffer(Block + ConeOffset,     MData.MeshletCones.data(),     ConeBytes);
        RHI::UploadBuffer(Block + VertexOffset,   VertSrc,                       VertexBytes);
        RHI::UploadBuffer(Block + TriangleOffset, MData.MeshletTriangles.data(), TriangleBytes);

        if (PaletteBytes != 0 && BoneIdxBytes != 0)
        {
            RHI::UploadBuffer(Block + PaletteOffset, MData.MeshletBonePalettes.data(), PaletteBytes);
            RHI::UploadBuffer(Block + BoneIdxOffset, MData.MeshletBoneIndices.data(),  BoneIdxBytes);
        }
        
        MB.ReleaseGeometryBuffers();

        MB.GeometryBlock         = Block;
        MB.MeshletBuffer         = Block + MeshletOffset;
        MB.MeshletSphereBuffer   = Block + SphereOffset;
        MB.MeshletConeBuffer     = Block + ConeOffset;
        MB.MeshletVertexBuffer   = Block + VertexOffset;
        MB.MeshletTriangleBuffer = Block + TriangleOffset;

        // Null for a static mesh; SkinVertex reads that as bind pose rather than misreading the indices.
        MB.MeshletBonePaletteBuffer = (PaletteBytes != 0 && BoneIdxBytes != 0) ? Block + PaletteOffset : 0;
        MB.MeshletBoneIndexBuffer   = (PaletteBytes != 0 && BoneIdxBytes != 0) ? Block + BoneIdxOffset : 0;
        MB.MeshletCount          = (uint32)MData.Meshlets.size();

        // The header publishes the heap slot, and a failed allocation leaves the sentinel every SDF path gates on.
        CreateDistanceFieldTexture(Resource);
        
        if (MB.MeshletHeaderSlot == MeshletHeaderSlab::kNullSlot)
        {
            MB.MeshletHeaderSlot = MeshletHeaderSlab::Acquire();
        }
        MeshletHeaderSlab::Write(MB.MeshletHeaderSlot, MakeMeshletHeader(Resource));
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

        // Build clears its output on every failure path, which would destroy a good field on a failed rebuild.
        FDistanceFieldVolume NewVolume;
        const bool bBuilt = DistanceField::Build(*MeshResources, DistanceFieldSettings, NewVolume);

        // A disabled setting is a real instruction to drop the field rather than a failure.
        if (!bBuilt && DistanceFieldSettings.bEnabled && MeshResources->DistanceField.IsValid())
        {
            return;
        }

        MeshResources->DistanceField = Move(NewVolume);

        // GenerateGPUBuffers would rebuild the whole meshlet set to publish a volume the header can carry.
        MeshBuffers::RefreshDistanceField(*MeshResources);
    }

    void CMesh::GenerateGPUBuffers()
    {
        MeshBuffers::CreateForResource(*MeshResources);

        // Bumping the global epoch here made loading one mesh re-resolve every component in the world.
        FMeshResolveCache::InvalidateDependency(this);

        // Drop import-time scratch.
        MeshResources->ClearVertices();
        MeshResources->Indices.clear();
        MeshResources->Indices.shrink_to_fit();
    }
}
