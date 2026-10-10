#pragma once

#include "RenderResource.h"
#include "RHI.h"
#include "RHICore.h"
#include "RHITexture.h"
#include "RenderRelease.h"
#include "Containers/Vector.h"
#include "Lumina.h"
#include "Core/Math/AABB.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Utils/NonCopyable.h"
#include "Renderer/MeshDistanceField.h"
#include "Renderer/Vertex.h"
#include "Shared/SharedConstants.h"

namespace Lumina
{
    namespace MeshGeometryStats
    {
        RUNTIME_API void Add(int64 BlockBytes, int64 VertexBytes);
        RUNTIME_API void Log(const char* Context);
    }

    constexpr uint32 MAX_MESH_LODS         = MESHLET_MAX_LODS;

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
        uint32 _Pad0            : 32 - 2 * MESHLET_COUNT_BITS - MESHLET_LOD_BITS - 1;

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
    };
    static_assert(sizeof(FMeshlet) == 16, "FMeshlet must stay 16B to match the GPU mirror (Common.slang)");
    static_assert(MESHLET_MAX_VERTICES <= MESHLET_COUNT_MASK && MESHLET_MAX_TRIANGLES <= MESHLET_COUNT_MASK, "Meshlet counts must fit MESHLET_COUNT_BITS");
    static_assert(MESHLET_MAX_LODS - 1 <= MESHLET_LOD_MASK, "LOD indices must fit MESHLET_LOD_BITS");


    // The cull's per-meshlet read, and the only one it always makes. Kept apart from the cone because
    // every meshlet of every view pays this while the cone test needs a much narrower set.
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

    // Mirrors meshopt_quantizeSnorm(v, 8), which rounds away from zero rather than to even.
    FORCEINLINE int8 QuantizeMeshletConeSnorm(float Value)
    {
        const float Clamped = Math::Clamp(Value, -1.0f, 1.0f);
        return (int8)(int32)(Clamped * (float)MESHLET_CONE_SNORM_SCALE + (Clamped >= 0.0f ? 0.5f : -0.5f));
    }

    // Folds the axis quantization error into the cutoff and rounds up, which is what keeps the test conservative.
    inline FMeshletCone EncodeMeshletCone(const FVector3& Axis, float Cutoff)
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
        Out.Cutoff = (int8)Math::Clamp(Quantized, -(int32)MESHLET_CONE_SNORM_SCALE, (int32)MESHLET_CONE_DISABLED);
        return Out;
    }

    // Pre-MESHLET_CONE_SNORM8 cones, 28 bytes of float apex, cutoff and axis. The apex is read and dropped.
    inline void LoadLegacyMeshletCones(FArchive& Ar, TVector<FMeshletCone>& Cones)
    {
        uint64 Count = 0;
        Ar << Count;

        Cones.clear();

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

        Cones.reserve((SIZE_T)Count);

        for (uint64 i = 0; i < Count; ++i)
        {
            FVector3 Apex;
            float    Cutoff = 1.0f;
            FVector3 Axis;

            Ar << Apex;
            Ar << Cutoff;
            Ar << Axis;

            Cones.push_back(Cutoff < 1.0f ? EncodeMeshletCone(Axis, Cutoff) : kDisabledMeshletCone);
        }
    }

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

    // The pre-MESHLET_SHARED_VERTICES meshlet, whose last three words held its quantization anchor.
    struct FLegacyMeshlet
    {
        uint32 VertexOffset;
        uint32 TriangleOffset;
        uint32 VertexCount;
        uint32 TriangleCount;
        uint32 LODIndex;
        uint32 PackedAnchorX;
        uint32 PackedAnchorY;
        uint32 PackedAnchorZ;
    };
    static_assert(sizeof(FLegacyMeshlet) == 32);
    static_assert(TCanBulkSerialize<FLegacyMeshlet>::value);

    // The pre-MESHLET_SHARED_VERTICES static vertex, copied into every meshlet that used it.
    struct FLegacyMeshletVertex
    {
        uint16 PositionX;
        uint16 PositionY;
        uint16 PositionZ;
        int16  NormalX;
        int16  NormalY;
        int16  NormalZ;
        uint32 Tangent;
        uint32 UV;
        uint32 UV1;
        uint32 Color;
    };
    static_assert(sizeof(FLegacyMeshletVertex) == 28);
    static_assert(TCanBulkSerialize<FLegacyMeshletVertex>::value);

    // The pre-MESHLET_SHARED_VERTICES skinned vertex, positioned against its meshlet's anchor.
    struct FLegacyMeshletSkinnedVertex
    {
        FLegacyMeshletVertex Base;
        uint32               JointIndices;
        uint32               JointWeights;
    };
    static_assert(sizeof(FLegacyMeshletSkinnedVertex) == 36);
    static_assert(TCanBulkSerialize<FLegacyMeshletSkinnedVertex>::value);

    struct FMeshletData;

    // Rebuilds old meshlets and their per-meshlet vertex copies in the current format, so an old package needs no reimport.
    RUNTIME_API void ConvertLegacyMeshletData(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets,
                                              const TVector<FLegacyMeshletVertex>& StaticVertices,
                                              const TVector<FLegacyMeshletSkinnedVertex>& SkinnedVertices);

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

        // Skinned meshes only, one palette per meshlet indexing a flat list of skeleton-global bone indices.
        TVector<FMeshletBonePalette>   MeshletBonePalettes;
        TVector<uint32>                MeshletBoneIndices;

        FORCEINLINE bool IsEmpty() const { return Meshlets.empty(); }

        FORCEINLINE void Clear()
        {
            Meshlets.clear();
            VertexPositions.clear();
            VertexAttributes.clear();
            VertexUV1s.clear();
            VertexColors.clear();
            MeshletVertexRefs.clear();
            PositionGrid = {};
            MeshletSkinnedVertices.clear();
            MeshletTriangles.clear();
            MeshletSpheres.clear();
            MeshletCones.clear();
            MeshletBonePalettes.clear();
            MeshletBoneIndices.clear();
        }

        FORCEINLINE void ClearAndShrink()
        {
            auto Drop = [](auto& V) { V.clear(); V.shrink_to_fit(); };
            Drop(Meshlets);
            Drop(VertexPositions);
            Drop(VertexAttributes);
            Drop(VertexUV1s);
            Drop(VertexColors);
            Drop(MeshletVertexRefs);
            PositionGrid = {};
            Drop(MeshletSkinnedVertices);
            Drop(MeshletTriangles);
            Drop(MeshletSpheres);
            Drop(MeshletCones);
            Drop(MeshletBonePalettes);
            Drop(MeshletBoneIndices);
        }

        friend FArchive& operator<<(FArchive& Ar, FMeshletData& Data)
        {
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESHLET_SHARED_VERTICES)
            {
                Ar << Data.Meshlets;
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

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESHLET_BONE_PALETTES)
            {
                Ar << Data.MeshletBonePalettes;
                Ar << Data.MeshletBoneIndices;
            }
            return Ar;
        }
    };

    // 64 vertices x 4 influences is the hard bound on one meshlet's distinct bones.
    constexpr uint32 kMeshletPaletteCapacity = 256;

    using FMeshletPaletteScratch = TFixedVector<uint32, kMeshletPaletteCapacity>;

    // Slot for GlobalBone, appending it when absent. A full palette folds onto slot 0 rather than growing.
    inline uint32 FindOrAddPaletteBone(FMeshletPaletteScratch& Palette, uint32 GlobalBone)
    {
        for (uint32 i = 0; i < (uint32)Palette.size(); ++i)
        {
            if (Palette[i] == GlobalBone)
            {
                return i;
            }
        }

        if (Palette.size() >= kMeshletPaletteCapacity)
        {
            return 0u;
        }

        Palette.push_back(GlobalBone);
        return (uint32)Palette.size() - 1u;
    }

    // Appends Palette as the next meshlet's slice; call once per meshlet, in meshlet order.
    inline void AppendMeshletBonePalette(FMeshletData& Data, FMeshletPaletteScratch& Palette)
    {
        // Slot 0 is where every zero-weight influence lands, so a palette is never empty.
        if (Palette.empty())
        {
            Palette.push_back(0u);
        }

        FMeshletBonePalette& Out = Data.MeshletBonePalettes.emplace_back();
        Out.Offset = (uint32)Data.MeshletBoneIndices.size();
        Out.Count  = (uint32)Palette.size();
        Data.MeshletBoneIndices.insert(Data.MeshletBoneIndices.end(), Palette.begin(), Palette.end());
    }

    /** FMeshletHeaderGPU::DistanceFieldFlags bits. Mirrored in Includes/DistanceField.slang. */
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

        // Entries in Meshlets/Spheres/Cones -- the AUTHORITATIVE bound for every array this header points
        // at, because it ships in the same 128 bytes as the pointers themselves. Everything that indexes
        // them bounds against this rather than against a meshlet count carried on the instance: the two
        // come from different sources and a stale instance paired with a live header is exactly how an
        // in-bounds-looking index walks off the end of a buffer.
        uint32 MeshletCount;

        // Entries in BonePalettes, and the bound every reader of it uses. Zero on a static mesh, which is
        // how "carries no palette" is spelled now that the slab aims the bone pointers at the null page.
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
    };
    static_assert(sizeof(FMeshletHeaderGPU) == 176, "FMeshletHeaderGPU must match FMeshletHeader in Common.slang");

    namespace MeshletHeaderSlab
    {
        // Forward-declared rather than included: MeshletHeaderSlab.h needs FMeshletHeaderGPU from here.
        RUNTIME_API void Release(uint32 Slot);
        RUNTIME_API void Reset(uint32 Slot);
    }

    struct FGeometrySurface final
    {
        FName  ID;
        uint32 IndexCount    = 0;
        uint32 StartIndex    = 0;
        int16  MaterialIndex = -1;

        // Per-LOD meshlet ranges into FMeshletData::Meshlets; NumLODs >= 1.
        uint32 NumLODs                           = 1;
        uint32 LODMeshletOffset[MAX_MESH_LODS]   = {};
        uint32 LODMeshletCount[MAX_MESH_LODS]    = {};
        // Mesh-local distance LOD i strays from the source surface, non-decreasing, and 0 for LOD 0.
        float  LODError[MAX_MESH_LODS]           = {};

        /**
         * Mesh-local world size of ONE full UV tile: sqrt(WorldArea / UVArea) over this surface's
         * triangles. Multiply by the primitive's world scale and you have the world extent that one wrap
         * of the texture covers, which is what turns a distance into a required texture resolution.
         *
         * Texture streaming needs this because screen coverage alone cannot tell a 4K texture stretched
         * once over a wall from the same texture tiled twenty times across it -- those want mip levels
         * four apart at identical on-screen size.
         *
         * 0 means UNKNOWN (no UVs, degenerate unwrap, or a mesh built before this was recorded), and
         * consumers must fall back rather than treat it as "infinitely dense".
         */
        float  TexelFactor = 0.0f;

        friend FArchive& operator << (FArchive& Ar, FGeometrySurface& Data)
        {
            Ar << Data.ID;
            Ar << Data.IndexCount;
            Ar << Data.StartIndex;
            Ar << Data.MaterialIndex;

            // Cannot be rebuilt at load: GenerateMeshlets drops Positions/UVs/Indices once the meshlets are
            // baked, and only the baked result is serialized. So it ships, or it is gone.
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
            uint32      MeshletHeaderSlot     = 0;
            uint32      MeshletCount          = 0;
            uint64      VertexStreamBytes     = 0;

            RHI::FManagedTexture DistanceFieldTexture;

            FMeshBuffers()                               = default;
            FMeshBuffers(const FMeshBuffers&)            = delete;
            FMeshBuffers& operator=(const FMeshBuffers&) = delete;

            FMeshBuffers(FMeshBuffers&& Other) noexcept { Steal(Other); }

            FMeshBuffers& operator=(FMeshBuffers&& Other) noexcept
            {
                if (this != &Other)
                {
                    ReleaseAll();
                    RHI::Textures::Release(DistanceFieldTexture);
                    Steal(Other);
                }
                return *this;
            }

            ~FMeshBuffers()
            {
                ReleaseAll();
                RHI::Textures::Release(DistanceFieldTexture);
            }

            // Keeps the header slot, the mesh's identity, after pointing it at the null header so nothing records the retired geometry.
            void ReleaseGeometryBuffers()
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
                MeshletCount          = 0;
            }

            /** Full teardown, slot included -- destruction only. A REBUILD must keep the slot: it is the
             *  identity cached all over the scene, exactly as the header ADDRESS used to be, and dropping
             *  it would blank every instance of this mesh until each one had re-resolved. */
            void ReleaseAll()
            {
                ReleaseGeometryBuffers();
                MeshletHeaderSlab::Release(MeshletHeaderSlot);
                MeshletHeaderSlot = 0;
            }

        private:

            /** Takes ownership of Other's addresses and leaves it empty, so exactly one object frees. */
            void Steal(FMeshBuffers& Other)
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
                MeshletHeaderSlot     = Other.MeshletHeaderSlot;
                MeshletCount          = Other.MeshletCount;
                VertexStreamBytes     = Other.VertexStreamBytes;
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
                Other.MeshletHeaderSlot     = 0;
                Other.MeshletCount          = 0;
                Other.VertexStreamBytes     = 0;
                Other.DistanceFieldTexture  = RHI::FManagedTexture{};
            }
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

        // Published on the meshlet header, derived rather than serialized; CreateForResource fills it.
        FAABB                     LocalBounds;

        uint32                    RequiredBoneCount = 0;

        //~ Build inputs. None are serialized -- the baked result is what persists.

        // Source scene-graph world transform; baked into vertices at merge time.
        FMatrix4                  ImportTransform = FMatrix4(1.0f);

        uint32                    MaxLODs = MAX_MESH_LODS;

        bool                      bGenerateTangents = true;

        bool                      bMeshletConeCulling = true;

        // Per-meshlet triangle reorder for the hardware vertex cache.
        bool                      bOptimizeMeshlets = true;

        // Linear-scan meshlet build. Only valid with the index pre-pass GenerateMeshlets runs alongside it.
        bool                      bFastMeshletBuild = false;

        // Simplified levels move vertices and rewrite normals and UVs to fit; skinned meshes never do.
        bool                      bDestructiveLODs = true;

        FORCEINLINE size_t GetNumSurfaces() const { return GeometrySurfaces.size(); }
        FORCEINLINE size_t GetNumVertices() const { return Positions.size(); }
        FORCEINLINE size_t GetNumIndices()  const { return Indices.size(); }
        FORCEINLINE NODISCARD bool IsSkinnedMesh() const { return bSkinnedMesh; }

        // Synthetic interleaved vertex size; only meshopt fetch/overdraw analysis needs it.
        FORCEINLINE size_t GetVertexTypeSize() const
        {
            return bSkinnedMesh ? sizeof(FSourceSkinnedVertex) : sizeof(FSourceVertex);
        }

        FORCEINLINE FVector2 GetUVAt(size_t Index) const { return Math::UnpackHalf2x16(UVs[Index]); }
        FORCEINLINE void SetUVAt(size_t Index, FVector2 UV) { UVs[Index] = Math::PackHalf2x16(UV); }

        FORCEINLINE FVector2 GetUV1At(size_t Index) const { return Math::UnpackHalf2x16(UVs1[Index]); }
        FORCEINLINE void SetUV1At(size_t Index, FVector2 UV) { UVs1[Index] = Math::PackHalf2x16(UV); }

        void ResizeVertices(size_t N)
        {
            Positions.resize(N);
            Normals.resize(N);
            Tangents.resize(N);
            UVs.resize(N);
            UVs1.resize(N);
            Colors.resize(N);
            if (bSkinnedMesh)
            {
                JointIndices.resize(N);
                JointWeights.resize(N);
            }
        }

        void ReserveVertices(size_t N)
        {
            Positions.reserve(N);
            Normals.reserve(N);
            Tangents.reserve(N);
            UVs.reserve(N);
            UVs1.reserve(N);
            Colors.reserve(N);
            if (bSkinnedMesh)
            {
                JointIndices.reserve(N);
                JointWeights.reserve(N);
            }
        }

        void ClearVertices()
        {
            auto Drop = [](auto& V) { V.clear(); V.shrink_to_fit(); };
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
            Positions.push_back(V.Position);
            Normals.push_back(V.Normal);
            Tangents.push_back(V.Tangent);
            UVs.push_back(V.UV);
            UVs1.push_back(V.UV1);
            Colors.push_back(V.Color);
        }

        void AppendVertex(const FSourceSkinnedVertex& V)
        {
            AppendVertex(static_cast<const FSourceVertex&>(V));
            JointIndices.push_back(V.JointIndices);
            JointWeights.push_back(V.JointWeights);
        }

        friend FArchive& operator << (FArchive& Ar, FMeshResource& Data)
        {
            Ar << Data.Name;
            Ar << Data.bSkinnedMesh;
            Ar << Data.GeometrySurfaces;
            Ar << Data.MeshletData;

            const bool bHasLODErrors = Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_LOD_ERROR;
            bool bDroppedLegacyLODs = false;

            for (FGeometrySurface& Surface : Data.GeometrySurfaces)
            {
                Ar << Surface.NumLODs;
                for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
                {
                    Ar << Surface.LODMeshletOffset[i];
                    Ar << Surface.LODMeshletCount[i];
                    Ar << Surface.LODError[i];
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
            if (Ar.IsReading() && !Data.HasConsistentMeshlets())
            {
                LOG_ERROR("FMeshResource '{}': meshlet offsets point outside its data; the mesh loads empty", Data.Name);
                Ar.SetHasError(true);
                Data.MeshletData.ClearAndShrink();
                Data.GeometrySurfaces.clear();
            }

            return Ar;
        }

        bool HasConsistentMeshlets() const
        {
            const FMeshletData& M = MeshletData;
            const uint64 NumMeshlets  = M.Meshlets.size();
            const uint64 NumVertices  = bSkinnedMesh ? M.MeshletSkinnedVertices.size() : M.VertexPositions.size();
            const uint64 NumTriangles = M.MeshletTriangles.size();

            if (!bSkinnedMesh)
            {
                const bool bUV1sFit    = M.VertexUV1s.empty()   || M.VertexUV1s.size()   == NumVertices;
                const bool bColorsFit  = M.VertexColors.empty() || M.VertexColors.size() == NumVertices;
                if (M.VertexAttributes.size() != NumVertices || !bUV1sFit || !bColorsFit)
                {
                    return false;
                }
            }

            for (const FGeometrySurface& Surface : GeometrySurfaces)
            {
                if (Surface.NumLODs > MAX_MESH_LODS)
                {
                    return false;
                }
                for (uint32 Lod = 0; Lod < Surface.NumLODs; ++Lod)
                {
                    if ((uint64)Surface.LODMeshletOffset[Lod] + Surface.LODMeshletCount[Lod] > NumMeshlets)
                    {
                        return false;
                    }
                }
            }

            for (const FMeshlet& Meshlet : M.Meshlets)
            {
                if ((uint64)Meshlet.TriangleOffset + Meshlet.TriangleCount > NumTriangles || Meshlet.VertexCount > MESHLET_MAX_VERTICES)
                {
                    return false;
                }
                if (bSkinnedMesh)
                {
                    if ((uint64)Meshlet.VertexOffset + Meshlet.VertexCount > NumVertices)
                    {
                        return false;
                    }
                }
                else
                {
                    const bool   bShort    = Meshlet.HasShortVertexRefs();
                    const uint64 RefWords  = bShort ? (Meshlet.VertexCount + 1u) / 2u : Meshlet.VertexCount;
                    if ((uint64)Meshlet.VertexOffset + RefWords > M.MeshletVertexRefs.size())
                    {
                        return false;
                    }
                    for (uint32 Local = 0; Local < Meshlet.VertexCount; ++Local)
                    {
                        const uint32 Word   = M.MeshletVertexRefs[Meshlet.VertexOffset + (bShort ? Local / 2u : Local)];
                        const uint32 Offset = bShort ? ((Local & 1u) ? (Word >> 16) : (Word & 0xFFFFu)) : Word;
                        if ((uint64)Meshlet.BaseVertex + Offset >= NumVertices)
                        {
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
                        return false;
                    }
                }
            }
            return true;
        }
    };
}
