#pragma once

#include "RHI.h"
#include "RHITexture.h"
#include "Containers/Vector.h"
#include "Core/Math/AABB.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Utils/NonCopyable.h"
#include "Renderer/MeshDistanceField.h"
#include "Renderer/MeshImpostor.h"
#include "Renderer/Vertex.h"
#include "Shared/SharedConstants.h"

namespace Lumina
{
    namespace MeshGeometryStats
    {
        RUNTIME_API void Add(int64 BlockBytes, int64 VertexBytes);
        RUNTIME_API void Log(const char* Context);
    }

    constexpr uint32 MAX_MESH_LODS = MESHLET_MAX_LODS;

    struct FMeshlet
    {
        // A static meshlet's first word in MeshletVertexRefs, or a skinned meshlet's first vertex copy.
        uint32 VertexOffset;
        uint32 TriangleOffset;
        // Static meshlets add this to every vertex ref; skinned meshlets leave it 0.
        uint32 BaseVertex;

        // Allocated low bit first, which is the order Common.slang unpacks with the MESHLET_* shifts.
        uint32 VertexCount      : MESHLET_COUNT_BITS;
        uint32 TriangleCount    : MESHLET_COUNT_BITS;
        uint32 LODIndex         : MESHLET_LOD_BITS;
        uint32 bShortVertexRefs : 1;

        FORCEINLINE bool HasShortVertexRefs() const { return bShortVertexRefs != 0u; }

        friend FArchive& operator<<(FArchive& Ar, FMeshlet& Data)
        {
            uint32 Words[4];
            memcpy(Words, &Data, sizeof(Words));
            for (uint32& Word : Words)
            {
                Ar << Word;
            }

            memcpy(&Data, Words, sizeof(Words));
            return Ar;
        }

        // Older files packed a three bit LOD, so their short refs flag sits where the fourth LOD bit is now.
        void RepackFromLegacyLayout()
        {
            constexpr uint32 kLegacyLODBits    = 3u;
            constexpr uint32 kLegacyShortShift = MESHLET_LOD_SHIFT + kLegacyLODBits;
            uint32 Words[4];
            memcpy(Words, this, sizeof(Words));
            const uint32 Old    = Words[3];
            const uint32 Counts = Old & ((1u << MESHLET_LOD_SHIFT) - 1u);
            const uint32 LOD    = (Old >> MESHLET_LOD_SHIFT) & ((1u << kLegacyLODBits) - 1u);
            const uint32 Short  = (Old >> kLegacyShortShift) & 1u;
            Words[3] = Counts | (LOD << MESHLET_LOD_SHIFT) | (Short << MESHLET_SHORT_REFS_SHIFT);
            memcpy(this, Words, sizeof(Words));
        }
    };
    // Bulk copied as an array, which is why the layout fix above is a pass over the array rather than part of operator<<.
    static_assert(TCanBulkSerialize<FMeshlet>::value);
    static_assert(sizeof(FMeshlet) == 16, "FMeshlet must stay 16B to match the GPU mirror (Common.slang)");
    static_assert(MESHLET_MAX_VERTICES <= MESHLET_COUNT_MASK && MESHLET_MAX_TRIANGLES <= MESHLET_COUNT_MASK, "Meshlet counts must fit MESHLET_COUNT_BITS");
    static_assert(MESHLET_MAX_LODS - 1 <= MESHLET_LOD_MASK, "LOD indices must fit MESHLET_LOD_BITS");

    // Cluster hierarchy bounds of one meshlet. Self is the group it was simplified from, Parent the group it belongs to.
    struct alignas(16) FMeshletLODBounds
    {
        FVector3 SelfCenter;
        float    SelfRadius;
        FVector3 ParentCenter;
        float    ParentRadius;
        // Mesh-local, 0 for source geometry, and ParentError is MESHLET_LOD_ERROR_INFINITE on a root.
        float    SelfError;
        float    ParentError;
        // Always 0, and a mesh saved with it set carries the old voxel levels and rebuilds on load.
        uint32   Flags;
        // Hierarchy depth, 0 for source geometry.
        uint32   Level;

        friend FArchive& operator<<(FArchive& Ar, FMeshletLODBounds& Data)
        {
            Ar << Data.SelfCenter;
            Ar << Data.SelfRadius;
            Ar << Data.ParentCenter;
            Ar << Data.ParentRadius;
            Ar << Data.SelfError;
            Ar << Data.ParentError;
            Ar << Data.Flags;
            Ar << Data.Level;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshletLODBounds) == 48, "FMeshletLODBounds must match the GPU mirror (Common.slang)");

    constexpr float kMeshletLODErrorInfinite = (float)MESHLET_LOD_ERROR_INFINITE;

    // The cull's one unconditional per-meshlet read, kept apart from the cone that far fewer meshlets test.
    struct FMeshletSphere
    {
        FVector3 Center;
        float    Radius;

        friend FArchive& operator<<(FArchive& Ar, FMeshletSphere& Data)
        {
            Ar << Data.Center;
            Ar << Data.Radius;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshletSphere) == 16, "FMeshletSphere must match the GPU mirror");
    // Shaders reach this with loadAligned<16>, which needs every element 16-aligned.
    static_assert(sizeof(FMeshletSphere) % 16 == 0, "FMeshletSphere stride must stay 16-byte aligned for loadAligned<16>");

    // Backface cluster culling, carrying no apex, so readers owe it meshoptimizer's approximate test.
    struct FMeshletCone
    {
        int8 Axis[3];
        // cos(angle / 2) as SNORM8, rounded up by the encoder so a decoded cone rejects less, never more.
        int8 Cutoff;

        friend FArchive& operator<<(FArchive& Ar, FMeshletCone& Data)
        {
            Ar << Data.Axis[0];
            Ar << Data.Axis[1];
            Ar << Data.Axis[2];
            Ar << Data.Cutoff;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshletCone) == 4, "FMeshletCone must match the GPU mirror (Common.slang)");

    // The cone every meshlet without a usable one carries, and what a decode failure must fall back to.
    inline constexpr FMeshletCone kDisabledMeshletCone = { { 0, 0, (int8)MESHLET_CONE_SNORM_SCALE },
                                                           (int8)MESHLET_CONE_DISABLED };

    // A skinned vertex's 8-bit JointIndices address its OWNING MESHLET's slice of MeshletBoneIndices.
    struct FMeshletBonePalette
    {
        uint32 Offset;
        uint32 Count;

        friend FArchive& operator<<(FArchive& Ar, FMeshletBonePalette& Data)
        {
            Ar << Data.Offset;
            Ar << Data.Count;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshletBonePalette) == 8, "FMeshletBonePalette must match the GPU mirror (Common.slang)");

    // One power-of-two grid for every vertex of a static mesh, so a vertex shared by meshlets decodes to identical bits.
    struct FMeshPositionGrid
    {
        int32 AnchorX  = 0;
        int32 AnchorY  = 0;
        int32 AnchorZ  = 0;
        int32 Exponent = 0;

        friend FArchive& operator<<(FArchive& Ar, FMeshPositionGrid& Data)
        {
            Ar << Data.AnchorX;
            Ar << Data.AnchorY;
            Ar << Data.AnchorZ;
            Ar << Data.Exponent;
            return Ar;
        }
    };

    struct FMeshletData
    {
        TVector<FMeshlet>              Meshlets;

        // Both kinds of mesh decode positions against this one grid.
        FMeshPositionGrid              PositionGrid;

        // Static meshes store each vertex once, and a meshlet reaches its vertices through MeshletVertexRefs.
        TVector<FMeshVertexPosition>   VertexPositions;
        TVector<FMeshVertexAttributes> VertexAttributes;
        // Empty unless the source carried a second UV set distinct from the first.
        TVector<uint32>                VertexUV1s;
        // Empty unless the source carried a color other than white.
        TVector<uint32>                VertexColors;
        TVector<uint32>                MeshletVertexRefs;

        TVector<FMeshletSkinnedVertex> MeshletSkinnedVertices;
        TVector<uint32>                MeshletTriangles;
        TVector<FMeshletSphere>        MeshletSpheres;
        TVector<FMeshletCone>          MeshletCones;

        // One per meshlet, empty only on a mesh saved before every mesh carried a hierarchy, which loading rebuilds.
        TVector<FMeshletLODBounds>     MeshletLODBounds;

        // Skinned meshes only, one palette per meshlet indexing a flat list of skeleton-global bone indices.
        TVector<FMeshletBonePalette>   MeshletBonePalettes;
        TVector<uint32>                MeshletBoneIndices;

        FORCEINLINE bool IsEmpty() const { return Meshlets.IsEmpty(); }

        void Clear()
        {
            ForEachStream([](auto& Stream) { Stream.Reset(); });
            PositionGrid = {};
        }

        void ClearAndShrink()
        {
            ForEachStream([](auto& Stream) { Stream.Reset(); Stream.Shrink(); });
            PositionGrid = {};
        }

        template<typename TFunc>
        void ForEachStream(TFunc&& Func)
        {
            Func(Meshlets);
            Func(VertexPositions);
            Func(VertexAttributes);
            Func(VertexUV1s);
            Func(VertexColors);
            Func(MeshletVertexRefs);
            Func(MeshletSkinnedVertices);
            Func(MeshletTriangles);
            Func(MeshletSpheres);
            Func(MeshletCones);
            Func(MeshletLODBounds);
            Func(MeshletBonePalettes);
            Func(MeshletBoneIndices);
        }

        friend RUNTIME_API FArchive& operator<<(FArchive& Ar, FMeshletData& Data);
    };

    // 64 vertices x 4 influences is the hard bound on one meshlet's distinct bones.
    constexpr uint32 kMeshletPaletteCapacity = 256;

    using FMeshletPaletteScratch = TInlineVector<uint32, kMeshletPaletteCapacity>;

    // Slot for GlobalBone, appending it when absent. A full palette folds onto slot 0 rather than growing.
    inline uint32 FindOrAddPaletteBone(FMeshletPaletteScratch& Palette, uint32 GlobalBone)
    {
        for (int32 i = 0; i < Palette.Num(); ++i)
        {
            if (Palette[i] == GlobalBone)
            {
                return i;
            }
        }

        if (static_cast<uint32>(Palette.Num()) >= kMeshletPaletteCapacity)
        {
            return 0u;
        }

        Palette.Add(GlobalBone);
        return (uint32)Palette.Num() - 1u;
    }

    // Appends Palette as the next meshlet's slice; call once per meshlet, in meshlet order.
    inline void AppendMeshletBonePalette(FMeshletData& Data, FMeshletPaletteScratch& Palette)
    {
        // Slot 0 is where every zero-weight influence lands, so a palette is never empty.
        if (Palette.IsEmpty())
        {
            Palette.Add(0u);
        }

        FMeshletBonePalette& Out = Data.MeshletBonePalettes.EmplaceGetRef();
        Out.Offset = (uint32)Data.MeshletBoneIndices.Num();
        Out.Count  = (uint32)Palette.Num();
        Data.MeshletBoneIndices.Append(Palette);
    }

    // FMeshletHeaderGPU::DistanceFieldFlags bits, mirrored in Includes/DistanceField.slang.
    enum class EDistanceFieldFlags : uint32
    {
        None     = 0,
        TwoSided = BIT(0),
    };

    struct alignas(16) FMeshletHeaderGPU
    {
        uint64 MeshletsAddress;                     // FMeshlet*
        uint64 SpheresAddress;                      // FMeshletSphere*
        uint64 VerticesAddress;                     // uint32*
        uint64 TrianglesAddress;                    // uint32*
        uint64 ConesAddress;                        // FMeshletCone*
        uint64 BonePalettesAddress;                 // FMeshletBonePalette*
        uint64 BoneIndicesAddress;                  // uint32*

        uint32 DistanceFieldIndex;
        uint32 DistanceFieldFlags;                  // EDistanceFieldFlags

        // Mesh-local AABB the volume spans, and the local-space distance the encoded range covers.
        float  DistanceFieldMinX, DistanceFieldMinY, DistanceFieldMinZ;
        float  DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ;
        float  DistanceFieldMaxDistance;

        // What the LocalBounds material node reads. Zero is a degenerate box the shader substitutes for.
        float  LocalMinX, LocalMinY, LocalMinZ;
        float  LocalMaxX, LocalMaxY, LocalMaxZ;

        // Bound every reader of Meshlets, Spheres and Cones must use, since a stale instance count can walk off a live header's arrays.
        uint32 MeshletCount;

        // Bound for BonePalettes readers, and zero on a static mesh because its bone pointers aim at the null page.
        uint32 BonePaletteCount;

        // MESH_VERTEX_STREAM_* bits for the optional static streams below.
        uint32 VertexStreamFlags;

        // A static mesh's FMeshVertexPosition per vertex, so a position-only pass fetches 8 bytes.
        uint64 PositionsAddress;                    // uint2*
        uint64 VertexRefsAddress;                   // uint32*
        uint64 VertexUV1sAddress;                   // uint32*
        uint64 VertexColorsAddress;                 // uint32*

        int32  PositionGridAnchorX;
        int32  PositionGridAnchorY;
        int32  PositionGridAnchorZ;
        int32  PositionGridExponent;

        // FMeshletLODBounds per meshlet, read only while Flags carries MESH_HEADER_FLAG_CLUSTER_LOD.
        uint64 LODBoundsAddress;
        // MESH_HEADER_FLAG_* bits.
        uint32 Flags;

        // Bindless slots of the impostor atlas, whose views number ImpostorFrames along a side, 0 with no impostor.
        uint32 ImpostorBaseColorIndex;
        uint32 ImpostorNormalDepthIndex;
        uint32 ImpostorFrames;
        float  ImpostorCenterX, ImpostorCenterY, ImpostorCenterZ;
        float  ImpostorRadius;
        uint32 ImpostorFrameSize;
        uint32 _Pad0;
    };
    static_assert(sizeof(FMeshletHeaderGPU) == 224, "FMeshletHeaderGPU must match FMeshletHeader in Common.slang");

    struct FGeometrySurface final
    {
        FName  ID;
        uint32 IndexCount    = 0;
        uint32 StartIndex    = 0;
        int16  MaterialIndex = -1;

        // Mesh-local size of one UV tile, sqrt(WorldArea / UVArea); 0 means unknown and consumers must fall back, not treat it as infinitely dense.
        float  TexelFactor = 0.0f;

        // Hierarchy slots as meshlet ranges into FMeshletData::Meshlets, each a run of whole levels, finest first and contiguous.
        uint32 NumLODs                           = 1;
        uint32 LODMeshletOffset[MAX_MESH_LODS]   = {};
        uint32 LODMeshletCount[MAX_MESH_LODS]    = {};
        // A slot's smallest self error, mesh-local, non-decreasing with the slot and 0 for the source level.
        float  LODError[MAX_MESH_LODS]           = {};
        // A cluster slot's largest parent error, so a view can skip a whole level its parents already cover.
        float  LODParentError[MAX_MESH_LODS]     = {};
        // SURFACE_DESC_* bits.
        uint32 LODFlags                          = 0;

        FORCEINLINE bool HasClusterLOD() const { return (LODFlags & SURFACE_DESC_CLUSTER_LOD) != 0u; }

        // The source geometry's meshlets, which open slot 0 ahead of any coarser hierarchy levels sharing it.
        FORCEINLINE uint32 FinestMeshletCount(const FMeshletData& Data) const
        {
            const uint32 Begin = LODMeshletOffset[0];
            const uint32 Count = LODMeshletCount[0];
            if (Data.MeshletLODBounds.IsEmpty())
            {
                return Count;
            }
            uint32 Finest = 0;
            while (Finest < Count && Begin + Finest < (uint32)Data.MeshletLODBounds.Num() && Data.MeshletLODBounds[Begin + Finest].Level == 0u)
            {
                ++Finest;
            }
            return Finest;
        }

        friend FArchive& operator << (FArchive& Ar, FGeometrySurface& Data)
        {
            Ar << Data.ID;
            Ar << Data.IndexCount;
            Ar << Data.StartIndex;
            Ar << Data.MaterialIndex;

            // Serialized because GenerateMeshlets drops the positions, UVs and indices needed to rebuild it at load.
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_SURFACE_TEXEL_FACTOR)
            {
                Ar << Data.TexelFactor;
            }
            return Ar;
        }
    };

    struct RUNTIME_API FMeshResource : INonCopyable
    {
        struct FMeshBuffers
        {
            // Every stream below is an offset into this one allocation, which is the only thing freed.
            RHI::FGPUAllocation GeometryBlock = {};

            RHI::GPUPtr MeshletBuffer         = 0;
            RHI::GPUPtr MeshletSphereBuffer   = 0;
            RHI::GPUPtr MeshletConeBuffer     = 0;
            RHI::GPUPtr MeshletVertexBuffer   = 0;
            RHI::GPUPtr MeshletPositionBuffer = 0;
            RHI::GPUPtr MeshletVertexRefBuffer = 0;
            RHI::GPUPtr VertexUV1Buffer       = 0;
            RHI::GPUPtr VertexColorBuffer     = 0;
            RHI::GPUPtr MeshletTriangleBuffer = 0;
            RHI::GPUPtr MeshletBonePaletteBuffer = 0;
            RHI::GPUPtr MeshletBoneIndexBuffer   = 0;
            RHI::GPUPtr MeshletLODBoundsBuffer   = 0;
            uint32      MeshletHeaderSlot     = 0;
            uint32      MeshletCount          = 0;
            uint64      VertexStreamBytes     = 0;

            // The upload batch carrying the geometry and header, which a reader waits on before drawing this slot.
            uint64      UploadBatch           = 0;

            RHI::FManagedTexture DistanceFieldTexture;
            RHI::FManagedTexture ImpostorBaseColorTexture;
            RHI::FManagedTexture ImpostorNormalDepthTexture;

            FMeshBuffers()                               = default;
            FMeshBuffers(const FMeshBuffers&)            = delete;
            FMeshBuffers& operator=(const FMeshBuffers&) = delete;

            RUNTIME_API FMeshBuffers(FMeshBuffers&& Other) noexcept;
            RUNTIME_API FMeshBuffers& operator=(FMeshBuffers&& Other) noexcept;
            RUNTIME_API ~FMeshBuffers();

            // Keeps the header slot, the mesh's identity, after pointing it at the null header so nothing records the retired geometry.
            RUNTIME_API void ReleaseGeometryBuffers();

            // Full teardown, slot included, so only destruction calls it; a rebuild keeps the slot every cached instance points at.
            RUNTIME_API void ReleaseAll();

            void ReleaseTextures();

        private:

            void Steal(FMeshBuffers& Other);
        };

        FName                     Name;

        TVector<FVector3>         Positions;
        TVector<uint32>           Normals;     // octahedral pack (PackNormal)
        TVector<uint32>           Tangents;    // octahedral + handedness (PackTangent)
        TVector<uint32>           UVs;         // packHalf2x16, TEXCOORD_0
        TVector<uint32>           UVs1;        // packHalf2x16, TEXCOORD_1 (mirrors UVs for single-set sources)
        TVector<uint32>           Colors;      // RGBA8 (PackColor)
        TVector<FU16Vector4>      JointIndices;
        TVector<FU8Vector4>       JointWeights;
        TVector<uint32>           Indices;

        TVector<FGeometrySurface> GeometrySurfaces;
        FMeshletData              MeshletData;
        FMeshBuffers              MeshBuffers;
        bool                      bSkinnedMesh = false;

        FDistanceFieldVolume      DistanceField;
        FMeshImpostor             Impostor;

        // Published on the meshlet header, derived rather than serialized; CreateForResource fills it.
        SAABB                     LocalBounds;

        uint32                    RequiredBoneCount = 0;

        //~ Build inputs, never serialized since only the baked result persists.

        // Source scene-graph world transform; baked into vertices at merge time.
        FMatrix4                  ImportTransform = FMatrix4(1.0f);

        bool                      bGenerateTangents = true;

        bool                      bMeshletConeCulling = true;

        // Per-meshlet triangle reorder for the hardware vertex cache.
        bool                      bOptimizeMeshlets = true;

        // Linear-scan meshlet build. Only valid with the index pre-pass GenerateMeshlets runs alongside it.
        bool                      bFastMeshletBuild = false;

        // Builds the cluster hierarchy past the source level; off keeps a mesh rebuilt too often to simplify at its source level.
        bool                      bLODHierarchy = true;

        FORCEINLINE int32 GetNumSurfaces() const { return GeometrySurfaces.Num(); }
        FORCEINLINE int32 GetNumVertices() const { return Positions.Num(); }
        FORCEINLINE int32 GetNumIndices()  const { return Indices.Num(); }

        // Synthetic interleaved vertex size; only meshopt fetch/overdraw analysis needs it.
        FORCEINLINE size_t GetVertexTypeSize() const
        {
            return bSkinnedMesh ? sizeof(FSourceSkinnedVertex) : sizeof(FSourceVertex);
        }

        FORCEINLINE FVector2 GetUVAt(int32 Index) const { return Math::UnpackHalf2x16(UVs[Index]); }
        FORCEINLINE void SetUVAt(int32 Index, FVector2 UV) { UVs[Index] = Math::PackHalf2x16(UV); }

        FORCEINLINE FVector2 GetUV1At(int32 Index) const { return Math::UnpackHalf2x16(UVs1[Index]); }
        FORCEINLINE void SetUV1At(int32 Index, FVector2 UV) { UVs1[Index] = Math::PackHalf2x16(UV); }

        void ResizeVertices(int32 N)
        {
            Positions.SetNum(N);
            Normals.SetNum(N);
            Tangents.SetNum(N);
            UVs.SetNum(N);
            UVs1.SetNum(N);
            Colors.SetNum(N);
            if (bSkinnedMesh)
            {
                JointIndices.SetNum(N);
                JointWeights.SetNum(N);
            }
        }

        void ReserveVertices(int32 N)
        {
            Positions.Reserve(N);
            Normals.Reserve(N);
            Tangents.Reserve(N);
            UVs.Reserve(N);
            UVs1.Reserve(N);
            Colors.Reserve(N);
            if (bSkinnedMesh)
            {
                JointIndices.Reserve(N);
                JointWeights.Reserve(N);
            }
        }

        void ClearVertices()
        {
            auto Drop = [](auto& V) { V.Reset(); V.Shrink(); };
            Drop(Positions);
            Drop(Normals);
            Drop(Tangents);
            Drop(UVs);
            Drop(UVs1);
            Drop(Colors);
            Drop(JointIndices);
            Drop(JointWeights);
        }

        void AppendVertex(const FSourceVertex& V)
        {
            Positions.Add(V.Position);
            Normals.Add(V.Normal);
            Tangents.Add(V.Tangent);
            UVs.Add(V.UV);
            UVs1.Add(V.UV1);
            Colors.Add(V.Color);
        }

        void AppendVertex(const FSourceSkinnedVertex& V)
        {
            AppendVertex(static_cast<const FSourceVertex&>(V));
            JointIndices.Add(V.JointIndices);
            JointWeights.Add(V.JointWeights);
        }

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FMeshResource& Data);

        bool HasConsistentMeshlets() const;

        // OutReason names the first check that failed, so a package that fails says what is wrong with it.
        bool HasConsistentMeshlets(const char*& OutReason) const;
    };
}
