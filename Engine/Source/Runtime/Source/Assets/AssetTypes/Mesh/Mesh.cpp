#include "RuntimePCH.h"
#include "Mesh.h"
#include "MeshBuildBatch.h"
#include "Core/Console/ConsoleVariable.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Config/EngineSettings.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/Package/Package.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHIUpload.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Renderer/MeshQuantization.h"
#include <cfloat>


namespace Lumina
{
    namespace MeshGeometryStats
    {
        namespace
        {
            std::atomic<int64> GBlockBytes{ 0 };
            std::atomic<int64> GVertexBytes{ 0 };
            std::atomic<int64> GMeshCount{ 0 };
        }

        void Add(int64 BlockBytes, int64 VertexBytes)
        {
            GBlockBytes.fetch_add(BlockBytes, std::memory_order_relaxed);
            GVertexBytes.fetch_add(VertexBytes, std::memory_order_relaxed);
            GMeshCount.fetch_add(BlockBytes >= 0 ? 1 : -1, std::memory_order_relaxed);
        }

        void Log(const char* Context)
        {
            constexpr double kMiBDivisor = 1024.0 * 1024.0;
            LOG_DISPLAY("Mesh geometry ({}): {} meshes, {:.1f} MiB total, {:.1f} MiB vertex streams",
                Context, GMeshCount.load(std::memory_order_relaxed),
                (double)GBlockBytes.load(std::memory_order_relaxed) / kMiBDivisor,
                (double)GVertexBytes.load(std::memory_order_relaxed) / kMiBDivisor);
        }

        FAutoConsoleCommand GGeometryStatsCommand(
            "Mesh.GeometryStats",
            "Log the GPU bytes held by mesh geometry and how much of it is vertex streams.",
            [] { Log("console"); });
    }

    void CMesh::Serialize(FArchive& Ar)
    {
        Super::Serialize(Ar);
        
        LUMINA_MEMORY_SCOPE("Meshes");
        if (!MeshResources)
        {
            MeshResources = MakeUnique<FMeshResource>();
        }

        // A save that lands before the staged resource publishes still writes the newest geometry.
        Ar << (Ar.IsWriting() && PendingMeshResources ? *PendingMeshResources : *MeshResources);
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
        if (MeshResources && MeshResources->MeshletData.IsEmpty() && !MeshResources->Indices.IsEmpty())
        {
            Import::Mesh::GenerateMeshlets(*MeshResources);
        }

        GenerateGPUBuffers();
    }

    void CMesh::OnReferencesReplaced()
    {
        // A slot material that was nulled is still what every resolve entry for this mesh draws with.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMesh::OnDestroy()
    {
        Super::OnDestroy();

        // Copies of the header address live on in resolve entries, so the next cull would fault the device.
        FMeshResolveCache::InvalidateDependency(this);
    }

    CMaterialInterface* CMesh::GetMaterialAtSlot(int32 Slot) const
    {
        return Materials.IsValidIndex(Slot) ? Materials[Slot].Get() : nullptr;
    }

    void CMesh::SetMaterialAtSlot(int32 Slot, CMaterialInterface* NewMaterial)
    {
        if (Materials.Num() <= Slot)
        {
            Materials.Add(NewMaterial);
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
        // ThumbnailManager's primitive meshes arrive without baked meshlets.
        if (NewResource && NewResource->MeshletData.IsEmpty() && !NewResource->Indices.IsEmpty())
        {
            Import::Mesh::GenerateMeshlets(*NewResource);
        }

        StageMeshResource(Move(NewResource));

        // With no renderer nothing is drawing the current resource, so the staged one publishes at once.
        FRenderManager* RenderManager = TryRender();
        if (RenderManager != nullptr && PendingMeshResources != nullptr)
        {
            RenderManager->WatchMeshPublish(this);
        }
        else
        {
            PublishPendingMeshResource();
        }
    }

    void CMesh::SetMeshResourceBatched(TUniquePtr<FMeshResource>&& NewResource)
    {
        if (!FMeshBuildBatchScope::IsOpen())
        {
            SetMeshResource(Move(NewResource));
            return;
        }

        // A first resource replaces nothing that draws, so code later in the scope can read it at once.
        if (MeshResources == nullptr)
        {
            ReplaceMeshResources(Move(NewResource));
            GenerateBoundingBox();
        }
        else
        {
            PendingMeshResources = Move(NewResource);
        }
        FMeshBuildBatchScope::DeferMeshlets(this);
    }

    void CMesh::BuildDeferredMeshlets()
    {
        FMeshResource* Resource = PendingMeshResources ? PendingMeshResources.get() : MeshResources.get();
        if (Resource && Resource->MeshletData.IsEmpty() && !Resource->Indices.IsEmpty())
        {
            Import::Mesh::GenerateMeshlets(*Resource);
        }
    }

    void CMesh::UploadDeferredBuffers()
    {
        if (PendingMeshResources)
        {
            TUniquePtr<FMeshResource> Resource = Move(PendingMeshResources);
            SetMeshResource(Move(Resource));
        }
        else if (MeshResources)
        {
            GenerateGPUBuffers();
        }
    }

    bool CMesh::IsGeometryResident() const
    {
        return !HasAnyFlag(OF_NeedsLoad) && MeshResources != nullptr && MeshResources->MeshBuffers.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot;
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

        const bool bMaterialsReady = Algo::AllOf(Materials, [](const TStrongObjectPtr<CMaterialInterface>& Material)
        {
            return Material != nullptr && Material->IsReadyForRender();
        });

        return bMaterialsReady && !Materials.IsEmpty();
    }

    namespace
    {
        // A cooked mesh has dropped its Positions, so the meshlet streams and then the spheres stand in.
        SAABB ComputeMeshLocalBounds(const FMeshResource& Resource)
        {
            SAABB Bounds;
            Bounds.Min = { FLT_MAX, FLT_MAX, FLT_MAX };
            Bounds.Max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

            if (Resource.GetNumVertices() > 0)
            {
                for (const FVector3& P : Resource.Positions)
                {
                    Bounds.Min = Math::Min(Bounds.Min, P);
                    Bounds.Max = Math::Max(Bounds.Max, P);
                }
                return Bounds;
            }

            if (Resource.MeshletData.IsEmpty())
            {
                return Bounds;
            }

            const FMeshletData& MD = Resource.MeshletData;

            for (const FMeshVertexPosition& Packed : MD.VertexPositions)
            {
                const FVector3 P = DecodeMeshPosition(MD.PositionGrid, Packed);
                Bounds.Min = Math::Min(Bounds.Min, P);
                Bounds.Max = Math::Max(Bounds.Max, P);
            }

            for (const FMeshletSkinnedVertex& Skinned : MD.MeshletSkinnedVertices)
            {
                const FVector3 P = DecodeMeshPosition(MD.PositionGrid, Skinned.Position);
                Bounds.Min = Math::Min(Bounds.Min, P);
                Bounds.Max = Math::Max(Bounds.Max, P);
            }

            if (MD.VertexPositions.IsEmpty() && MD.MeshletSkinnedVertices.IsEmpty() && !MD.MeshletSpheres.IsEmpty())
            {
                // Conservative for culling but wildly loose for anything not roughly spherical.
                for (const FMeshletSphere& S : MD.MeshletSpheres)
                {
                    Bounds.Min = Math::Min(Bounds.Min, S.Center - FVector3(S.Radius));
                    Bounds.Max = Math::Max(Bounds.Max, S.Center + FVector3(S.Radius));
                }
            }

            return Bounds;
        }
    }

    void CMesh::GenerateBoundingBox()
    {
        BoundingBox = MeshResources ? ComputeMeshLocalBounds(*MeshResources) : SAABB();
    }

    void CMesh::StageMeshResource(TUniquePtr<FMeshResource>&& NewResource)
    {
        PendingMeshResources = Move(NewResource);
        if (PendingMeshResources)
        {
            // Measured while the raw positions are still around, since the upload drops them.
            PendingMeshResources->LocalBounds = ComputeMeshLocalBounds(*PendingMeshResources);
            if (!Global::GIsHeadless)
            {
                UploadGeometry(*PendingMeshResources);
            }
        }

        // Nothing is drawing the current resource, so there is no previous geometry to keep live.
        if (MeshResources == nullptr || Global::GIsHeadless)
        {
            PublishPendingMeshResource();
        }
    }

    void CMesh::ReplaceMeshResources(TUniquePtr<FMeshResource>&& NewResource)
    {
        // The first geometry a mesh gets replaces nothing, so it keeps the version anything built from a loaded mesh saw.
        if (MeshResources != nullptr)
        {
            ++GeometryVersion;
        }
        MeshResources = Move(NewResource);
    }

    bool CMesh::PublishPendingMeshResource()
    {
        if (PendingMeshResources == nullptr)
        {
            return true;
        }
        if (!RHI::Upload::IsBatchComplete(PendingMeshResources->MeshBuffers.UploadBatch))
        {
            return false;
        }

        // The replaced resource's destructor retires its block and slot behind the frames still recording them.
        ReplaceMeshResources(Move(PendingMeshResources));
        BoundingBox   = MeshResources->LocalBounds;

        // The surfaces' LOD tables and header slot changed, which every instance of this mesh reads through its resolve entry.
        FMeshResolveCache::InvalidateDependency(this);
        return true;
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
                          *Resource.Name);
                return;
            }

            RHI::Textures::Upload(Texture, 0, Volume.Distances.GetData(), Volume.Distances.Num());
        }

        // Meshes built by the importer already carry palettes and are left alone.
        void BuildBonePalettes(FMeshletData& MData)
        {
            if (MData.MeshletBonePalettes.Num() == MData.Meshlets.Num())
            {
                return;
            }

            LUMINA_PROFILE_SCOPE();

            MData.MeshletBonePalettes.Reset();
            MData.MeshletBonePalettes.Reserve(MData.Meshlets.Num());
            MData.MeshletBoneIndices.Reset();
            MData.MeshletBoneIndices.Reserve(MData.Meshlets.Num() * 16);

            const uint32 VertexTotal = static_cast<uint32>(MData.MeshletSkinnedVertices.Num());

            FMeshletPaletteScratch Palette;

            for (const FMeshlet& Meshlet : MData.Meshlets)
            {
                // Vertices past MESHLET_MAX_VERTICES are skipped because no shader path reads them.
                const uint32 Begin = Meshlet.VertexOffset;
                const uint32 End   = Math::Min(Begin + Math::Min(Meshlet.VertexCount, (uint32)MESHLET_MAX_VERTICES),
                                               VertexTotal);

                Palette.Reset();

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

            FMeshletHeaderGPU Header = {};
            Header.MeshletsAddress          = MB.MeshletBuffer;
            Header.SpheresAddress           = MB.MeshletSphereBuffer;
            Header.VerticesAddress          = MB.MeshletVertexBuffer;
            Header.PositionsAddress         = MB.MeshletPositionBuffer;
            Header.VertexRefsAddress        = MB.MeshletVertexRefBuffer;
            Header.VertexUV1sAddress        = MB.VertexUV1Buffer;
            Header.VertexColorsAddress      = MB.VertexColorBuffer;
            Header.VertexStreamFlags        = (MB.VertexUV1Buffer   != 0 ? MESH_VERTEX_STREAM_UV1   : 0u)
                                            | (MB.VertexColorBuffer != 0 ? MESH_VERTEX_STREAM_COLOR : 0u);
            Header.PositionGridAnchorX      = Resource.MeshletData.PositionGrid.AnchorX;
            Header.PositionGridAnchorY      = Resource.MeshletData.PositionGrid.AnchorY;
            Header.PositionGridAnchorZ      = Resource.MeshletData.PositionGrid.AnchorZ;
            Header.PositionGridExponent     = Resource.MeshletData.PositionGrid.Exponent;
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
            Header.LODBoundsAddress         = MB.MeshletLODBoundsBuffer;
            Header.Flags                    = MB.MeshletLODBoundsBuffer != 0 ? MESH_HEADER_FLAG_CLUSTER_LOD : 0u;
            Header.BonePalettesAddress      = MB.MeshletBonePaletteBuffer;
            Header.BoneIndicesAddress       = MB.MeshletBoneIndexBuffer;
            Header.MeshletCount             = MB.MeshletCount;
            Header.BonePaletteCount         = (MB.MeshletBonePaletteBuffer != 0 && MB.MeshletBoneIndexBuffer != 0)
                                            ? MB.MeshletCount : 0u;

            // Zero when the bounds could not be derived; the shader reads that as a degenerate box.
            const SAABB& Local = Resource.LocalBounds;
            Header.LocalMinX                = Local.Min.x;
            Header.LocalMinY                = Local.Min.y;
            Header.LocalMinZ                = Local.Min.z;
            Header.LocalMaxX                = Local.Max.x;
            Header.LocalMaxY                = Local.Max.y;
            Header.LocalMaxZ                = Local.Max.z;
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

        // CMesh hands its own box down; a procedural resource has none, so derive one for the header.
        if (!Resource.LocalBounds.IsValid())
        {
            Resource.LocalBounds = ComputeMeshLocalBounds(Resource);
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

        const void*  VertSrc    = bSkinned ? (const void*)MData.MeshletSkinnedVertices.GetData() : (const void*)MData.VertexAttributes.GetData();
        const uint64 VertStride = bSkinned ? sizeof(FMeshletSkinnedVertex) : sizeof(FMeshVertexAttributes);
        const uint64 VertCount  = bSkinned ? MData.MeshletSkinnedVertices.Num() : MData.VertexAttributes.Num();

        const uint64 MeshletBytes  = sizeof(FMeshlet)       * MData.Meshlets.Num();
        const uint64 SphereBytes   = sizeof(FMeshletSphere) * MData.MeshletSpheres.Num();
        const uint64 ConeBytes     = sizeof(FMeshletCone)   * MData.MeshletCones.Num();
        const uint64 VertexBytes   = VertCount              * VertStride;
        const uint64 PositionBytes = sizeof(FMeshVertexPosition) * MData.VertexPositions.Num();
        const uint64 RefBytes      = sizeof(uint32)         * MData.MeshletVertexRefs.Num();
        const uint64 UV1Bytes      = sizeof(uint32)         * MData.VertexUV1s.Num();
        const uint64 ColorBytes    = sizeof(uint32)         * MData.VertexColors.Num();
        const uint64 TriangleBytes = sizeof(uint32)         * MData.MeshletTriangles.Num();
        const uint64 PaletteBytes  = sizeof(FMeshletBonePalette) * MData.MeshletBonePalettes.Num();
        const uint64 BoneIdxBytes  = sizeof(uint32)         * MData.MeshletBoneIndices.Num();
        // Only a complete array is published; a partial one would be indexed past its end by the cull.
        const uint64 LODBoundsBytes = MData.MeshletLODBounds.Num() == MData.Meshlets.Num()
                                    ? sizeof(FMeshletLODBounds) * MData.MeshletLODBounds.Num() : 0u;

        const bool bMissingStaticStream = !bSkinned && (PositionBytes == 0 || RefBytes == 0);
        if (MeshletBytes == 0 || SphereBytes == 0 || ConeBytes == 0 || VertexBytes == 0 || TriangleBytes == 0 || bMissingStaticStream)
        {
            LOG_ERROR("Mesh rebuild failed: {} meshlets with an empty geometry stream "
                      "(meshlets {}, spheres {}, cones {}, vertices {}, positions {}, refs {}, triangles {} bytes). Previous geometry kept.",
                      MData.Meshlets.Num(), MeshletBytes, SphereBytes, ConeBytes, VertexBytes, PositionBytes, RefBytes, TriangleBytes);
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
        const uint64 PositionOffset = Reserve(PositionBytes);
        const uint64 RefOffset      = Reserve(RefBytes);
        const uint64 UV1Offset      = Reserve(UV1Bytes);
        const uint64 ColorOffset    = Reserve(ColorBytes);
        const uint64 TriangleOffset = Reserve(TriangleBytes);
        const uint64 PaletteOffset  = Reserve(PaletteBytes);
        const uint64 BoneIdxOffset  = Reserve(BoneIdxBytes);
        const uint64 LODBoundsOffset = Reserve(LODBoundsBytes);

        const RHI::FGPUAllocation Block = RHI::Malloc(Cursor, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);

        if (Block.Gpu == 0)
        {
            LOG_ERROR("Mesh rebuild failed: {} KiB GPU allocation for {} meshlets. Previous geometry kept.",
                      Cursor / 1024, MData.Meshlets.Num());
            return;
        }

        RHI::SetDebugName(Block.Gpu, bSkinned ? "Mesh.SkinnedGeometry" : "Mesh.Geometry");

        // A stream the staging ring could not take leaves raw Malloc bytes behind the header, so none may be skipped.
        bool bQueued = true;
        auto UploadIfPresent = [&Block, &bQueued](const void* Source, uint64 Bytes, uint64 Offset)
        {
            if (Bytes != 0)
            {
                bQueued &= RHI::UploadBuffer(Block, Source, Bytes, Offset);
            }
        };
        UploadIfPresent(MData.Meshlets.GetData(),          MeshletBytes,  MeshletOffset);
        UploadIfPresent(MData.MeshletSpheres.GetData(),    SphereBytes,   SphereOffset);
        UploadIfPresent(MData.MeshletCones.GetData(),      ConeBytes,     ConeOffset);
        UploadIfPresent(VertSrc,                           VertexBytes,   VertexOffset);
        UploadIfPresent(MData.MeshletTriangles.GetData(),  TriangleBytes, TriangleOffset);
        UploadIfPresent(MData.VertexPositions.GetData(),   PositionBytes, PositionOffset);
        UploadIfPresent(MData.MeshletVertexRefs.GetData(), RefBytes,      RefOffset);
        UploadIfPresent(MData.VertexUV1s.GetData(),        UV1Bytes,      UV1Offset);
        UploadIfPresent(MData.VertexColors.GetData(),      ColorBytes,    ColorOffset);
        UploadIfPresent(MData.MeshletLODBounds.GetData(),  LODBoundsBytes, LODBoundsOffset);

        if (PaletteBytes != 0 && BoneIdxBytes != 0)
        {
            UploadIfPresent(MData.MeshletBonePalettes.GetData(), PaletteBytes, PaletteOffset);
            UploadIfPresent(MData.MeshletBoneIndices.GetData(),  BoneIdxBytes, BoneIdxOffset);
        }

        if (!bQueued)
        {
            LOG_ERROR("Mesh rebuild failed: a geometry upload of {} meshlets was dropped. Previous geometry kept.",
                      MData.Meshlets.Num());
            RHI::Retire(Block);
            return;
        }

        MB.ReleaseGeometryBuffers();

        MB.GeometryBlock         = Block;
        MB.VertexStreamBytes     = VertexBytes + PositionBytes + RefBytes + UV1Bytes + ColorBytes;
        MeshGeometryStats::Add((int64)Block.Size, (int64)MB.VertexStreamBytes);
        MB.MeshletBuffer         = Block.Gpu + MeshletOffset;
        MB.MeshletSphereBuffer   = Block.Gpu + SphereOffset;
        MB.MeshletConeBuffer     = Block.Gpu + ConeOffset;
        MB.MeshletVertexBuffer   = Block.Gpu + VertexOffset;
        MB.MeshletPositionBuffer  = PositionBytes != 0 ? Block.Gpu + PositionOffset : 0;
        MB.MeshletVertexRefBuffer = RefBytes      != 0 ? Block.Gpu + RefOffset      : 0;
        MB.VertexUV1Buffer        = UV1Bytes      != 0 ? Block.Gpu + UV1Offset      : 0;
        MB.VertexColorBuffer      = ColorBytes    != 0 ? Block.Gpu + ColorOffset    : 0;
        MB.MeshletTriangleBuffer  = Block.Gpu + TriangleOffset;
        MB.MeshletLODBoundsBuffer = LODBoundsBytes != 0 ? Block.Gpu + LODBoundsOffset : 0;

        // Null for a static mesh; SkinVertex reads that as bind pose rather than misreading the indices.
        MB.MeshletBonePaletteBuffer = (PaletteBytes != 0 && BoneIdxBytes != 0) ? Block.Gpu + PaletteOffset : 0;
        MB.MeshletBoneIndexBuffer   = (PaletteBytes != 0 && BoneIdxBytes != 0) ? Block.Gpu + BoneIdxOffset : 0;
        MB.MeshletCount          = (uint32)MData.Meshlets.Num();

        // The header publishes the heap slot, and a failed allocation leaves the sentinel every SDF path gates on.
        CreateDistanceFieldTexture(Resource);
        
        if (MB.MeshletHeaderSlot == MeshletHeaderSlab::kNullSlot)
        {
            MB.MeshletHeaderSlot = MeshletHeaderSlab::Acquire();
        }
        MeshletHeaderSlab::Write(MB.MeshletHeaderSlot, MakeMeshletHeader(Resource));

        // Read after the header write is queued, so it names that write's batch or a later one.
        MB.UploadBatch = RHI::Upload::BatchForQueuedOps();
    }

    bool CMesh::HasDistanceField() const
    {
        return MeshResources != nullptr && MeshResources->DistanceField.IsValid();
    }

    bool CMesh::HasClusterLOD() const
    {
        return MeshResources != nullptr && MeshResources->MeshletData.HasClusterLOD();
    }

    namespace
    {
        // The finest slot of every surface holds the source geometry, so the build streams can be read back out of it into Resource.
        bool RestoreSourceStreamsFromMeshlets(const FMeshResource& Source, FMeshResource& Resource)
        {
            const FMeshletData& MD = Source.MeshletData;
            if (Source.bSkinnedMesh || MD.IsEmpty() || MD.VertexPositions.IsEmpty())
            {
                return false;
            }

            // Surfaces and the field carry over, and the build inputs the importer set keep steering the rebuild.
            Resource.Name                = Source.Name;
            Resource.GeometrySurfaces    = Source.GeometrySurfaces;
            Resource.DistanceField       = Source.DistanceField;
            Resource.MaxLODs             = Source.MaxLODs;
            Resource.bMeshletConeCulling = Source.bMeshletConeCulling;
            Resource.bOptimizeMeshlets   = Source.bOptimizeMeshlets;
            Resource.bFastMeshletBuild   = Source.bFastMeshletBuild;
            Resource.bDestructiveLODs    = Source.bDestructiveLODs;

            const int32 NumVertices = MD.VertexPositions.Num();
            if (MD.VertexAttributes.Num() != NumVertices)
            {
                return false;
            }

            // Only the finest slot's vertices come back, since coarser levels and splats hold vertices the source never had.
            constexpr uint32 kUnused = ~0u;
            TVector<uint32>  Compact(NumVertices, kUnused);
            TVector<uint32>  Indices;
            uint32           Used = 0;
            auto Remap = [&](uint32 Vertex)
            {
                uint32& Slot = Compact[Vertex];
                if (Slot == kUnused)
                {
                    Slot = Used++;
                }
                Indices.Add(Slot);
            };

            for (FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                Surface.StartIndex = (uint32)Indices.Num();
                const uint32 Begin = Surface.LODMeshletOffset[0];
                const uint32 End   = Math::Min(Begin + Surface.LODMeshletCount[0], (uint32)MD.Meshlets.Num());
                for (uint32 m = Begin; m < End; ++m)
                {
                    const FMeshlet& M = MD.Meshlets[m];
                    for (uint32 t = 0; t < M.TriangleCount; ++t)
                    {
                        const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                        Remap(GetStaticVertexIndex(MD, M, Packed & 0xFFu));
                        Remap(GetStaticVertexIndex(MD, M, (Packed >> 8) & 0xFFu));
                        Remap(GetStaticVertexIndex(MD, M, (Packed >> 16) & 0xFFu));
                    }
                }
                Surface.IndexCount = (uint32)Indices.Num() - Surface.StartIndex;
            }

            Resource.ResizeVertices(Used);
            Resource.Indices = Move(Indices);

            const bool bHasUV1   = MD.VertexUV1s.Num()   == NumVertices;
            const bool bHasColor = MD.VertexColors.Num() == NumVertices;
            for (int32 v = 0; v < NumVertices; ++v)
            {
                const uint32 Slot = Compact[v];
                if (Slot == kUnused)
                {
                    continue;
                }
                Resource.Positions[Slot] = DecodeMeshPosition(MD.PositionGrid, MD.VertexPositions[v]);
                Resource.Normals[Slot]   = MD.VertexAttributes[v].Normal;
                Resource.Tangents[Slot]  = MD.VertexAttributes[v].Tangent;
                Resource.UVs[Slot]       = MD.VertexAttributes[v].UV;
                Resource.UVs1[Slot]      = bHasUV1 ? MD.VertexUV1s[v] : MD.VertexAttributes[v].UV;
                Resource.Colors[Slot]    = bHasColor ? MD.VertexColors[v] : 0xFFFFFFFFu;
            }
            return !Resource.Indices.IsEmpty();
        }
    }

    namespace
    {
        // Retrofits content imported before virtual geometry existed, which otherwise needs each mesh reimported by hand.
        void RebuildAllVirtualGeometry()
        {
            const CRendererSettings* Settings = GetDefault<CRendererSettings>();
            if (Settings == nullptr)
            {
                return;
            }

            // Collected first, since a rebuild creates GPU objects while the object array is walked.
            TVector<CMesh*> Meshes;
            ForEachObject([&Meshes](CObjectBase* Object, int32)
            {
                CMesh* Mesh = Object != nullptr ? Cast<CMesh>(static_cast<CObject*>(Object)) : nullptr;
                if (Mesh != nullptr && !Mesh->IsSkinned() && !Mesh->HasAnyFlag(OF_NeedsLoad) && Mesh->GetPackage() != nullptr
                    && !Mesh->GetPackage()->IsTransientPackage())
                {
                    Meshes.Add(Mesh);
                }
            });

            uint32 Rebuilt = 0;
            for (CMesh* Mesh : Meshes)
            {
                Mesh->VirtualGeometrySettings.bClusterLOD             = Settings->bClusterLODByDefault;
                Mesh->VirtualGeometrySettings.bVoxelFarRepresentation = Settings->bVoxelFarByDefault;
                Mesh->VirtualGeometrySettings.VoxelResolution         = (uint32)Math::Clamp(Settings->DefaultVoxelResolution, 4, 128);
                if (Mesh->RebuildVirtualGeometry())
                {
                    Mesh->GetPackage()->MarkDirty();
                    ++Rebuilt;
                }
            }
            LOG_INFO("Mesh.RebuildVirtualGeometry: rebuilt {} of {} loaded static meshes with the project defaults. Save to keep them.", Rebuilt, Meshes.Num());
        }

        FAutoConsoleCommand GRebuildVirtualGeometryCommand(
            "Mesh.RebuildVirtualGeometry",
            "Rebuild every loaded static mesh's meshlets with the project's cluster hierarchy and voxel defaults, leaving their packages dirty.",
            &RebuildAllVirtualGeometry);

        void DumpVirtualGeometry()
        {
            ForEachObject([](CObjectBase* Object, int32)
            {
                CMesh* Mesh = Object != nullptr ? Cast<CMesh>(static_cast<CObject*>(Object)) : nullptr;
                const FMeshResource* Resource = Mesh != nullptr && !Mesh->HasAnyFlag(OF_NeedsLoad) ? Mesh->TryGetMeshResource() : nullptr;
                if (Resource == nullptr || Resource->GeometrySurfaces.IsEmpty())
                {
                    return;
                }

                const SAABB& Box = Mesh->GetAABB();
                LOG_INFO("VG '{}': {} meshlets, {} LOD bounds, LOD distance scale {:.2f}, extent ({:.2f} {:.2f} {:.2f})", Mesh->GetName().ToString(),
                    Resource->MeshletData.Meshlets.Num(), Resource->MeshletData.MeshletLODBounds.Num(), Mesh->LODDistanceScale,
                    Box.Max.x - Box.Min.x, Box.Max.y - Box.Min.y, Box.Max.z - Box.Min.z);

                for (int32 SurfaceIdx = 0; SurfaceIdx < Resource->GeometrySurfaces.Num(); ++SurfaceIdx)
                {
                    const FGeometrySurface& Surface = Resource->GeometrySurfaces[SurfaceIdx];
                    LOG_INFO("  surface {} '{}' cluster={} flags={:#x} slots={}", SurfaceIdx, Surface.ID.ToString(), Surface.HasClusterLOD(), Surface.LODFlags, Surface.NumLODs);
                    for (uint32 Slot = 0; Slot < Surface.NumLODs; ++Slot)
                    {
                        LOG_INFO("    slot {}{} meshlets={} error={:.4f} parent={:.4f}", Slot, Surface.IsVoxelLOD(Slot) ? " voxel" : "",
                            Surface.LODMeshletCount[Slot], Surface.LODError[Slot], Surface.LODParentError[Slot]);

                        const uint32 First = Surface.LODMeshletOffset[Slot];
                        if (Surface.LODMeshletCount[Slot] > 0u && First < (uint32)Resource->MeshletData.MeshletLODBounds.Num())
                        {
                            const FMeshletLODBounds& B = Resource->MeshletData.MeshletLODBounds[First];
                            LOG_INFO("      first meshlet self=({:.2f} {:.2f} {:.2f}) r={:.2f} e={:.4f} parent=({:.2f} {:.2f} {:.2f}) r={:.2f} e={:.4f} flags={}",
                                B.SelfCenter.x, B.SelfCenter.y, B.SelfCenter.z, B.SelfRadius, B.SelfError,
                                B.ParentCenter.x, B.ParentCenter.y, B.ParentCenter.z, B.ParentRadius, B.ParentError, B.Flags);
                        }
                    }
                }
            });
        }

        FAutoConsoleCommand GDumpVirtualGeometryCommand(
            "Mesh.DumpVirtualGeometry",
            "Log every loaded mesh's per-surface LOD slots, their errors, parent errors and which slots hold voxels.",
            &DumpVirtualGeometry);
    }

    bool CMesh::RebuildVirtualGeometry()
    {
        if (MeshResources == nullptr || MeshResources->bSkinnedMesh)
        {
            return false;
        }

        LUMINA_MEMORY_SCOPE("Meshes");

        // Built beside the live resource from its finest baked level, so instances keep drawing it until the rebuild has uploaded.
        TUniquePtr<FMeshResource> Rebuilt = MakeUnique<FMeshResource>();
        if (!RestoreSourceStreamsFromMeshlets(*MeshResources, *Rebuilt))
        {
            LOG_WARN("Virtual geometry rebuild skipped for '{}', it has no finest level to rebuild from.", GetName().ToString());
            return false;
        }

        // Normals and UVs survived the bake, and MikkTSpace rebuilds tangents from those.
        Rebuilt->bGenerateTangents = true;
        Import::Mesh::ApplyVirtualGeometrySettings(*Rebuilt, VirtualGeometrySettings);
        Import::Mesh::GenerateMeshlets(*Rebuilt);

        SetMeshResource(Move(Rebuilt));
        return true;
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

        // Instances carry whether their mesh has a field, so they re-resolve to pick the change up.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMesh::UploadGeometry(FMeshResource& Resource)
    {
        MeshBuffers::CreateForResource(Resource);

        // Drop import-time scratch.
        Resource.ClearVertices();
        Resource.Indices.Reset();
        Resource.Indices.Shrink();
    }

    void CMesh::GenerateGPUBuffers()
    {
        // A headless process has no RHI, and it keeps the CPU streams a mesh collider still reads.
        if (Global::GIsHeadless)
        {
            return;
        }

        // The box is already computed by PostLoad, and the header publishes it for the LocalBounds node.
        MeshResources->LocalBounds = BoundingBox;

        // Same meshlet layout as before, and the header rides the same upload batch as the block it names.
        UploadGeometry(*MeshResources);

        // Bumping the global epoch here made loading one mesh re-resolve every component in the world.
        FMeshResolveCache::InvalidateDependency(this);
    }
}
