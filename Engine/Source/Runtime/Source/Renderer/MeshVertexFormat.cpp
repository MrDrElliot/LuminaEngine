#include "RuntimePCH.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/RenderRelease.h"
#include <meshoptimizer.h>

namespace Lumina
{
    namespace
    {
        FVector3 DecodeLegacyPosition(const FLegacyMeshlet& M, const FLegacyMeshletVertex& V)
        {
            constexpr uint32 kAnchorMask    = 0x00FFFFFFu;
            constexpr uint32 kAnchorSign    = 0x00800000u;
            constexpr uint32 kExponentShift = 24u;

            auto Anchor = [](uint32 Packed) { return (int32)((Packed & kAnchorMask) ^ kAnchorSign) - (int32)kAnchorSign; };
            const float Scale = PositionGridScale((int32)(int8)(uint8)(M.PackedAnchorX >> kExponentShift));

            return FVector3(
                (float)(Anchor(M.PackedAnchorX) + (int32)V.PositionX) * Scale,
                (float)(Anchor(M.PackedAnchorY) + (int32)V.PositionY) * Scale,
                (float)(Anchor(M.PackedAnchorZ) + (int32)V.PositionZ) * Scale);
        }

        uint32 PackLegacyNormal(const FLegacyMeshletVertex& V)
        {
            return PackNormal(FVector3(Math::SNorm16ToFloat(V.NormalX), Math::SNorm16ToFloat(V.NormalY), Math::SNorm16ToFloat(V.NormalZ)));
        }

        // Every field the shared format keeps, decoded, so binary equality means the same vertex.
        struct FLegacyVertexKey
        {
            FVector3 Position;
            uint32   Normal;
            uint32   Tangent;
            uint32   UV;
            uint32   UV1;
            uint32   Color;
        };

        template<typename TVisit>
        void ForEachLegacySlot(const TVector<FLegacyMeshlet>& Meshlets, int32 SlotCount, TVisit&& Visit)
        {
            for (const FLegacyMeshlet& M : Meshlets)
            {
                for (uint32 Local = 0; Local < M.VertexCount && static_cast<int64>(M.VertexOffset) + Local < SlotCount; ++Local)
                {
                    Visit(M, static_cast<int32>(M.VertexOffset + Local));
                }
            }
        }

        void ConvertStaticVertices(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets, const TVector<FLegacyMeshletVertex>& Legacy)
        {
            TVector<FLegacyVertexKey> Keys(Legacy.Num());
            memset(Keys.GetData(), 0, Keys.Num() * sizeof(FLegacyVertexKey));

            ForEachLegacySlot(Meshlets, Legacy.Num(), [&](const FLegacyMeshlet& M, int32 Slot)
            {
                const FLegacyMeshletVertex& V = Legacy[Slot];
                Keys[Slot] = { DecodeLegacyPosition(M, V), PackLegacyNormal(V), V.Tangent, V.UV, V.UV1, V.Color };
            });

            TVector<uint32> Remap(Keys.Num());
            const size_t UniqueCount = meshopt_generateVertexRemap(Remap.GetData(), nullptr, Keys.Num(), Keys.GetData(), Keys.Num(), sizeof(FLegacyVertexKey));

            TVector<FLegacyVertexKey> Unique(IntCastChecked<int32>(UniqueCount));
            for (int32 i = 0; i < Keys.Num(); ++i)
            {
                Unique[Remap[i]] = Keys[i];
            }

            TVector<FVector3> Positions(IntCastChecked<int32>(UniqueCount));
            bool bHasUV1   = false;
            bool bHasColor = false;
            for (int32 i = 0, Limit = IntCastChecked<int32>(UniqueCount); i < Limit; ++i)
            {
                Positions[i] = Unique[i].Position;
                bHasUV1      = bHasUV1   || Unique[i].UV1 != Unique[i].UV;
                bHasColor    = bHasColor || Unique[i].Color != 0xFFFFFFFFu;
            }

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.GetData(), Positions.Num());
            Data.VertexPositions.SetNum(IntCastChecked<int32>(UniqueCount));
            Data.VertexAttributes.SetNum(IntCastChecked<int32>(UniqueCount));
            Data.VertexUV1s.SetNum(IntCastChecked<int32>(bHasUV1 ? UniqueCount : 0u));
            Data.VertexColors.SetNum(IntCastChecked<int32>(bHasColor ? UniqueCount : 0u));
            for (int32 i = 0, Limit = IntCastChecked<int32>(UniqueCount); i < Limit; ++i)
            {
                Data.VertexPositions[i]  = EncodeMeshPosition(Data.PositionGrid, Positions[i]);
                Data.VertexAttributes[i] = { Unique[i].Normal, Unique[i].Tangent, Unique[i].UV };
                if (bHasUV1)
                {
                    Data.VertexUV1s[i] = Unique[i].UV1;
                }
                if (bHasColor)
                {
                    Data.VertexColors[i] = Unique[i].Color;
                }
            }

            for (int32 m = 0; m < Meshlets.Num(); ++m)
            {
                uint32 Vertices[MESHLET_MAX_VERTICES];
                const uint32 Count = Math::Min(Meshlets[m].VertexCount, (uint32)MESHLET_MAX_VERTICES);
                for (uint32 Local = 0; Local < Count; ++Local)
                {
                    const int32 Slot = (size_t)Meshlets[m].VertexOffset + Local;
                    Vertices[Local] = Slot < Remap.Num() ? Remap[Slot] : 0u;
                }
                AppendMeshletVertexRefs(Data, Data.Meshlets[m], Vertices, Count);
            }
        }

        void ConvertSkinnedVertices(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets, const TVector<FLegacyMeshletSkinnedVertex>& Legacy)
        {
            TVector<FVector3> Positions(Legacy.Num(), FVector3(0.0f));
            ForEachLegacySlot(Meshlets, Legacy.Num(), [&](const FLegacyMeshlet& M, int32 Slot)
            {
                Positions[Slot] = DecodeLegacyPosition(M, Legacy[Slot].Base);
            });

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.GetData(), Positions.Num());
            Data.MeshletSkinnedVertices.SetNum(Legacy.Num());
            for (int32 i = 0; i < Legacy.Num(); ++i)
            {
                const FLegacyMeshletVertex& V   = Legacy[i].Base;
                FMeshletSkinnedVertex&      Out = Data.MeshletSkinnedVertices[i];
                Out.Position     = EncodeMeshPosition(Data.PositionGrid, Positions[i]);
                Out.Normal       = PackLegacyNormal(V);
                Out.Tangent      = V.Tangent;
                Out.UV           = V.UV;
                Out.UV1          = V.UV1;
                Out.Color        = V.Color;
                Out.JointIndices = Legacy[i].JointIndices;
                Out.JointWeights = Legacy[i].JointWeights;
            }
        }

        // Mirrors meshopt_quantizeSnorm(v, 8), which rounds away from zero rather than to even.
        int8 QuantizeMeshletConeSnorm(float Value)
        {
            const float Clamped = Math::Clamp(Value, -1.0f, 1.0f);
            return (int8)(int32)(Clamped * (float)MESHLET_CONE_SNORM_SCALE + (Clamped >= 0.0f ? 0.5f : -0.5f));
        }

        // Folds the axis quantization error into the cutoff and rounds up, which is what keeps the test conservative.
        FMeshletCone EncodeMeshletCone(const FVector3& Axis, float Cutoff)
        {
            FMeshletCone Out;
            Out.Axis[0] = QuantizeMeshletConeSnorm(Axis.x);
            Out.Axis[1] = QuantizeMeshletConeSnorm(Axis.y);
            Out.Axis[2] = QuantizeMeshletConeSnorm(Axis.z);

            const float Scale = (float)MESHLET_CONE_SNORM_SCALE;
            const float ErrorX = Math::Abs((float)Out.Axis[0] / Scale - Axis.x);
            const float ErrorY = Math::Abs((float)Out.Axis[1] / Scale - Axis.y);
            const float ErrorZ = Math::Abs((float)Out.Axis[2] / Scale - Axis.z);

            const int32 Quantized = (int32)(Scale * (Cutoff + ErrorX + ErrorY + ErrorZ) + 1.0f);
            Out.Cutoff = (int8)Math::Clamp(Quantized, -MESHLET_CONE_SNORM_SCALE, MESHLET_CONE_DISABLED);
            return Out;
        }

        // Pre-MESHLET_CONE_SNORM8 cones, 28 bytes of float apex, cutoff and axis. The apex is read and dropped.
        void LoadLegacyMeshletCones(FArchive& Ar, TVector<FMeshletCone>& Cones)
        {
            uint64 Count = 0;
            Ar << Count;

            Cones.Reset();

            if (Ar.HasError() || Count == 0)
            {
                return;
            }

            // The same bound TVector's operator applies, which this hand-rolled loop would otherwise skip.
            if (Count > Ar.GetMaxSerializeSize())
            {
                Ar.SetHasError(true);
                LOG_ERROR("Archiver is corrupted, attempted to serialize {} legacy meshlet cones. Max is: {}",
                          Count, Ar.GetMaxSerializeSize());
                return;
            }

            Cones.Reserve(IntCastChecked<int32>(Count));

            for (uint64 i = 0; i < Count; ++i)
            {
                FVector3 Apex;
                float    Cutoff = 1.0f;
                FVector3 Axis;

                Ar << Apex;
                Ar << Cutoff;
                Ar << Axis;

                Cones.Add(Cutoff < 1.0f ? EncodeMeshletCone(Axis, Cutoff) : kDisabledMeshletCone);
            }
        }
    }

    void ConvertLegacyMeshletData(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets,
                                  const TVector<FLegacyMeshletVertex>& StaticVertices,
                                  const TVector<FLegacyMeshletSkinnedVertex>& SkinnedVertices)
    {
        LUMINA_PROFILE_SCOPE();

        Data.Meshlets.SetNum(Meshlets.Num());
        for (int32 m = 0; m < Meshlets.Num(); ++m)
        {
            const FLegacyMeshlet& In  = Meshlets[m];
            FMeshlet&             Out = Data.Meshlets[m];
            Out = {};
            Out.VertexOffset   = In.VertexOffset;
            Out.TriangleOffset = In.TriangleOffset;
            Out.VertexCount    = Math::Min(In.VertexCount, MESHLET_COUNT_MASK);
            Out.TriangleCount  = Math::Min(In.TriangleCount, MESHLET_COUNT_MASK);
            Out.LODIndex       = Math::Min(In.LODIndex, MESHLET_LOD_MASK);
        }

        Data.MeshletVertexRefs.Reset();
        if (!StaticVertices.IsEmpty())
        {
            ConvertStaticVertices(Data, Meshlets, StaticVertices);
        }
        if (!SkinnedVertices.IsEmpty())
        {
            ConvertSkinnedVertices(Data, Meshlets, SkinnedVertices);
        }
    }

    FArchive& operator<<(FArchive& Ar, FMeshletData& Data)
    {
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESHLET_SHARED_VERTICES)
        {
            Ar << Data.Meshlets;
            if (Ar.IsReading() && Ar.GetFileVersion() < (int32)ELuminaEngineVersion::MESH_CLUSTER_LOD)
            {
                for (FMeshlet& Meshlet : Data.Meshlets)
                {
                    Meshlet.RepackFromLegacyLayout();
                }
            }
            Ar << Data.VertexPositions;
            Ar << Data.VertexAttributes;
            Ar << Data.VertexUV1s;
            Ar << Data.VertexColors;
            Ar << Data.MeshletVertexRefs;
            Ar << Data.PositionGrid;
            Ar << Data.MeshletSkinnedVertices;
        }
        else
        {
            TVector<FLegacyMeshlet>              LegacyMeshlets;
            TVector<FLegacyMeshletVertex>        LegacyStatic;
            TVector<FLegacyMeshletSkinnedVertex> LegacySkinned;
            Ar << LegacyMeshlets;
            Ar << LegacyStatic;
            Ar << LegacySkinned;
            if (!Ar.HasError())
            {
                ConvertLegacyMeshletData(Data, LegacyMeshlets, LegacyStatic, LegacySkinned);
            }
        }

        Ar << Data.MeshletTriangles;
        Ar << Data.MeshletSpheres;

        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESHLET_CONE_SNORM8)
        {
            Ar << Data.MeshletCones;
        }
        else
        {
            LoadLegacyMeshletCones(Ar, Data.MeshletCones);
        }

        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_CLUSTER_LOD)
        {
            Ar << Data.MeshletLODBounds;
        }
        else
        {
            Data.MeshletLODBounds.Reset();
        }

        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESHLET_BONE_PALETTES)
        {
            Ar << Data.MeshletBonePalettes;
            Ar << Data.MeshletBoneIndices;
        }
        return Ar;
    }

    FMeshResource::FMeshBuffers::FMeshBuffers(FMeshBuffers&& Other) noexcept
    {
        Steal(Other);
    }

    FMeshResource::FMeshBuffers& FMeshResource::FMeshBuffers::operator=(FMeshBuffers&& Other) noexcept
    {
        if (this != &Other)
        {
            ReleaseAll();
            RHI::Textures::Release(DistanceFieldTexture);
            Steal(Other);
        }
        return *this;
    }

    FMeshResource::FMeshBuffers::~FMeshBuffers()
    {
        ReleaseAll();
        RHI::Textures::Release(DistanceFieldTexture);
    }

    void FMeshResource::FMeshBuffers::ReleaseGeometryBuffers()
    {
        MeshletHeaderSlab::Reset(MeshletHeaderSlot);

        // Not Retire, whose fence cannot see a scene still about to record the old header.
        if (GeometryBlock.Gpu != 0)
        {
            MeshGeometryStats::Add(-(int64)GeometryBlock.Size, -(int64)VertexStreamBytes);
        }
        RHI::RetireAfterExtract(GeometryBlock);

        GeometryBlock         = {};
        VertexStreamBytes     = 0;
        MeshletBuffer         = 0;
        MeshletSphereBuffer   = 0;
        MeshletConeBuffer     = 0;
        MeshletVertexBuffer   = 0;
        MeshletPositionBuffer = 0;
        MeshletVertexRefBuffer = 0;
        VertexUV1Buffer       = 0;
        VertexColorBuffer     = 0;
        MeshletTriangleBuffer = 0;
        MeshletBonePaletteBuffer = 0;
        MeshletBoneIndexBuffer   = 0;
        MeshletLODBoundsBuffer   = 0;
        MeshletCount          = 0;
        UploadBatch           = 0;
    }

    void FMeshResource::FMeshBuffers::ReleaseAll()
    {
        ReleaseGeometryBuffers();
        MeshletHeaderSlab::Release(MeshletHeaderSlot);
        MeshletHeaderSlot = 0;
    }

    void FMeshResource::FMeshBuffers::Steal(FMeshBuffers& Other)
    {
        GeometryBlock         = Other.GeometryBlock;
        MeshletBuffer         = Other.MeshletBuffer;
        MeshletSphereBuffer   = Other.MeshletSphereBuffer;
        MeshletConeBuffer     = Other.MeshletConeBuffer;
        MeshletVertexBuffer   = Other.MeshletVertexBuffer;
        MeshletPositionBuffer = Other.MeshletPositionBuffer;
        MeshletVertexRefBuffer = Other.MeshletVertexRefBuffer;
        VertexUV1Buffer       = Other.VertexUV1Buffer;
        VertexColorBuffer     = Other.VertexColorBuffer;
        MeshletTriangleBuffer = Other.MeshletTriangleBuffer;
        MeshletBonePaletteBuffer = Other.MeshletBonePaletteBuffer;
        MeshletBoneIndexBuffer   = Other.MeshletBoneIndexBuffer;
        MeshletLODBoundsBuffer   = Other.MeshletLODBoundsBuffer;
        MeshletHeaderSlot     = Other.MeshletHeaderSlot;
        MeshletCount          = Other.MeshletCount;
        VertexStreamBytes     = Other.VertexStreamBytes;
        UploadBatch           = Other.UploadBatch;
        DistanceFieldTexture  = Other.DistanceFieldTexture;

        Other.GeometryBlock         = {};
        Other.MeshletBuffer         = 0;
        Other.MeshletSphereBuffer   = 0;
        Other.MeshletConeBuffer     = 0;
        Other.MeshletVertexBuffer   = 0;
        Other.MeshletPositionBuffer = 0;
        Other.MeshletVertexRefBuffer = 0;
        Other.VertexUV1Buffer       = 0;
        Other.VertexColorBuffer     = 0;
        Other.MeshletTriangleBuffer = 0;
        Other.MeshletBonePaletteBuffer = 0;
        Other.MeshletBoneIndexBuffer   = 0;
        Other.MeshletLODBoundsBuffer   = 0;
        Other.MeshletHeaderSlot     = 0;
        Other.MeshletCount          = 0;
        Other.VertexStreamBytes     = 0;
        Other.UploadBatch           = 0;
        Other.DistanceFieldTexture  = RHI::FManagedTexture{};
    }

    FArchive& operator<<(FArchive& Ar, FMeshResource& Data)
    {
        Ar << Data.Name;
        Ar << Data.bSkinnedMesh;
        Ar << Data.GeometrySurfaces;
        Ar << Data.MeshletData;

        const bool bHasLODErrors  = Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_LOD_ERROR;
        const bool bHasClusterLOD = Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_CLUSTER_LOD;
        const uint32 StoredLODs   = bHasClusterLOD ? MAX_MESH_LODS : MESHLET_LEGACY_MAX_LODS;
        bool bDroppedLegacyLODs = false;

        for (FGeometrySurface& Surface : Data.GeometrySurfaces)
        {
            Ar << Surface.NumLODs;
            for (uint32 i = 0; i < StoredLODs; ++i)
            {
                Ar << Surface.LODMeshletOffset[i];
                Ar << Surface.LODMeshletCount[i];
                Ar << Surface.LODError[i];
            }
            if (bHasClusterLOD)
            {
                for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
                {
                    Ar << Surface.LODParentError[i];
                }
                Ar << Surface.LODFlags;
            }
            else
            {
                Surface.LODFlags = 0u;
            }

            if (Ar.IsReading() && !bHasLODErrors)
            {
                bDroppedLegacyLODs = bDroppedLegacyLODs || Surface.NumLODs > 1;
                Surface.NumLODs = Math::Min(Surface.NumLODs, 1u);
                for (float& Error : Surface.LODError)
                {
                    Error = 0.0f;
                }
            }
        }

        if (bDroppedLegacyLODs)
        {
            LOG_WARN("FMeshResource '{}' predates error-based LODs and renders LOD 0 only until it is reimported", Data.Name);
        }

        Ar << Data.DistanceField;

        // Offsets index CPU arrays and GPU buffers unchecked, so a corrupt package loads an empty mesh instead.
        const char* Reason = nullptr;
        if (Ar.IsReading() && !Data.HasConsistentMeshlets(Reason))
        {
            LOG_ERROR("FMeshResource '{}': {} ({} meshlets, {} vertices, {} refs, {} triangles, file version {}); the mesh loads empty",
                      Data.Name, Reason, Data.MeshletData.Meshlets.Num(), Data.MeshletData.VertexPositions.Num(),
                      Data.MeshletData.MeshletVertexRefs.Num(), Data.MeshletData.MeshletTriangles.Num(), Ar.GetFileVersion());
            Ar.SetHasError(true);
            Data.MeshletData.ClearAndShrink();
            Data.GeometrySurfaces.Reset();
        }

        return Ar;
    }

    bool FMeshResource::HasConsistentMeshlets() const
    {
        const char* Reason = nullptr;
        return HasConsistentMeshlets(Reason);
    }

    bool FMeshResource::HasConsistentMeshlets(const char*& OutReason) const
    {
        const FMeshletData& M = MeshletData;
        const uint64 NumMeshlets  = M.Meshlets.Num();
        const uint64 NumVertices  = bSkinnedMesh ? M.MeshletSkinnedVertices.Num() : M.VertexPositions.Num();
        const uint64 NumTriangles = M.MeshletTriangles.Num();
        OutReason = nullptr;

        if (!bSkinnedMesh)
        {
            const bool bUV1sFit    = M.VertexUV1s.IsEmpty()   || static_cast<uint64>(M.VertexUV1s.Num())   == NumVertices;
            const bool bColorsFit  = M.VertexColors.IsEmpty() || static_cast<uint64>(M.VertexColors.Num()) == NumVertices;
            if (static_cast<uint64>(M.VertexAttributes.Num()) != NumVertices || !bUV1sFit || !bColorsFit)
            {
                OutReason = "vertex stream counts differ";
                return false;
            }
        }

        // The cull indexes the bounds by meshlet unchecked, so a partial array is as bad as a wrong offset.
        if (!M.MeshletLODBounds.IsEmpty() && static_cast<uint64>(M.MeshletLODBounds.Num()) != NumMeshlets)
        {
            OutReason = "LOD bounds count differs from the meshlet count";
            return false;
        }

        for (const FGeometrySurface& Surface : GeometrySurfaces)
        {
            if (Surface.NumLODs > MAX_MESH_LODS)
            {
                OutReason = "surface has more LOD slots than exist";
                return false;
            }
            if (Surface.HasClusterLOD() && M.MeshletLODBounds.IsEmpty())
            {
                OutReason = "cluster surface without LOD bounds";
                return false;
            }
            for (uint32 Lod = 0; Lod < Surface.NumLODs; ++Lod)
            {
                if ((uint64)Surface.LODMeshletOffset[Lod] + Surface.LODMeshletCount[Lod] > NumMeshlets)
                {
                    OutReason = "surface LOD range past the meshlets";
                    return false;
                }
            }
        }

        for (const FMeshlet& Meshlet : M.Meshlets)
        {
            if ((uint64)Meshlet.TriangleOffset + Meshlet.TriangleCount > NumTriangles || Meshlet.VertexCount > MESHLET_MAX_VERTICES)
            {
                OutReason = "meshlet triangle range or vertex count out of bounds";
                return false;
            }
            if (bSkinnedMesh)
            {
                if ((uint64)Meshlet.VertexOffset + Meshlet.VertexCount > NumVertices)
                {
                    OutReason = "skinned meshlet vertex range out of bounds";
                    return false;
                }
            }
            else
            {
                const bool   bShort    = Meshlet.HasShortVertexRefs();
                const uint64 RefWords  = bShort ? (Meshlet.VertexCount + 1u) / 2u : Meshlet.VertexCount;
                if ((uint64)Meshlet.VertexOffset + RefWords > static_cast<uint64>(M.MeshletVertexRefs.Num()))
                {
                    OutReason = "meshlet vertex refs past the ref array";
                    return false;
                }
                for (uint32 Local = 0; Local < Meshlet.VertexCount; ++Local)
                {
                    const uint32 Word   = M.MeshletVertexRefs[Meshlet.VertexOffset + (bShort ? Local / 2u : Local)];
                    const uint32 Offset = bShort ? ((Local & 1u) ? (Word >> 16) : (Word & 0xFFFFu)) : Word;
                    if ((uint64)Meshlet.BaseVertex + Offset >= NumVertices)
                    {
                        OutReason = "meshlet vertex ref past the vertex count";
                        return false;
                    }
                }
            }
            for (uint32 Triangle = 0; Triangle < Meshlet.TriangleCount; ++Triangle)
            {
                const uint32 Packed = M.MeshletTriangles[Meshlet.TriangleOffset + Triangle];
                if ((Packed & 0xFFu) >= Meshlet.VertexCount || ((Packed >> 8) & 0xFFu) >= Meshlet.VertexCount
                    || ((Packed >> 16) & 0xFFu) >= Meshlet.VertexCount)
                {
                    OutReason = "meshlet triangle indexes past its vertices";
                    return false;
                }
            }
        }
        return true;
    }
}
