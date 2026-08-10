#pragma once

#include "RenderResource.h"
#include "RHI.h"
#include "RHICore.h"
#include "RHITexture.h"
#include "Containers/Array.h"
#include "Lumina.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Utils/NonCopyable.h"
#include "Renderer/MeshDistanceField.h"
#include "Renderer/Vertex.h"
#include "Shared/SharedConstants.h"

namespace Lumina
{
    static_assert(uint32(MESHLET_MAX_VERTICES)  == RHI::kMeshMaxOutputVertices,
                  "MESHLET_MAX_VERTICES and RHI::kMeshMaxOutputVertices must agree.");
    static_assert(uint32(MESHLET_MAX_TRIANGLES) == RHI::kMeshMaxOutputPrimitives,
                  "MESHLET_MAX_TRIANGLES and RHI::kMeshMaxOutputPrimitives must agree.");
    static_assert(uint32(MESHLET_CULL_GROUP_SIZE) == RHI::kMeshletCullGroupSize,
                  "MESHLET_CULL_GROUP_SIZE and RHI::kMeshletCullGroupSize must agree.");

    constexpr uint32 MAX_MESH_LODS         = MESHLET_MAX_LODS;
    constexpr uint32 MAX_SHADOW_LOD        = MESHLET_MAX_SHADOW_LOD;
    constexpr uint32 MAX_COARSE_SHADOW_LOD = MESHLET_MAX_COARSE_SHADOW_LOD;

    struct FMeshlet
    {
        uint32 VertexOffset;
        uint32 TriangleOffset;
        uint32 VertexCount;
        uint32 TriangleCount;
        uint32 LODIndex;

        uint32 PackedAnchorX;
        uint32 PackedAnchorY;
        uint32 PackedAnchorZ;

        friend FArchive& operator<<(FArchive& Ar, FMeshlet& Data)
        {
            Ar << Data.VertexOffset;
            Ar << Data.TriangleOffset;
            Ar << Data.VertexCount;
            Ar << Data.TriangleCount;
            Ar << Data.LODIndex;
            Ar << Data.PackedAnchorX;
            Ar << Data.PackedAnchorY;
            Ar << Data.PackedAnchorZ;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshlet) == 32, "FMeshlet must stay 32B to match the GPU mirror (Common.slang)");

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

    // Backface cluster culling. Cutoff = cos(angle / 2); 1.0 means the meshlet has no usable cone.
    struct FMeshletCone
    {
        FVector3 Apex;
        float    Cutoff;
        FVector3 Axis;

        friend FArchive& operator<<(FArchive& Ar, FMeshletCone& Data)
        {
            Ar << Data.Apex;
            Ar << Data.Cutoff;
            Ar << Data.Axis;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshletCone) == 28, "FMeshletCone must match the GPU mirror");

    struct FMeshletData
    {
        TVector<FMeshlet>              Meshlets;
        TVector<FMeshletVertex>        MeshletVertices;
        TVector<FMeshletSkinnedVertex> MeshletSkinnedVertices;
        TVector<uint32>                MeshletTriangles;
        TVector<FMeshletSphere>        MeshletSpheres;
        TVector<FMeshletCone>          MeshletCones;

        FORCEINLINE bool IsEmpty() const { return Meshlets.empty(); }

        FORCEINLINE void Clear()
        {
            Meshlets.clear();
            MeshletVertices.clear();
            MeshletSkinnedVertices.clear();
            MeshletTriangles.clear();
            MeshletSpheres.clear();
            MeshletCones.clear();
        }

        FORCEINLINE void ClearAndShrink()
        {
            auto Drop = [](auto& V) { V.clear(); V.shrink_to_fit(); };
            Drop(Meshlets);
            Drop(MeshletVertices);
            Drop(MeshletSkinnedVertices);
            Drop(MeshletTriangles);
            Drop(MeshletSpheres);
            Drop(MeshletCones);
        }

        friend FArchive& operator<<(FArchive& Ar, FMeshletData& Data)
        {
            Ar << Data.Meshlets;
            Ar << Data.MeshletVertices;
            Ar << Data.MeshletSkinnedVertices;
            Ar << Data.MeshletTriangles;
            Ar << Data.MeshletSpheres;
            Ar << Data.MeshletCones;
            return Ar;
        }
    };

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

        uint32 DistanceFieldIndex;
        uint32 DistanceFieldFlags;                  // EDistanceFieldFlags

        // Mesh-local AABB the volume spans, and the local-space distance the encoded range covers.
        float  DistanceFieldMinX, DistanceFieldMinY, DistanceFieldMinZ;
        float  DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ;
        float  DistanceFieldMaxDistance;

        uint32 _Pad0;
    };
    static_assert(sizeof(FMeshletHeaderGPU) == 80, "FMeshletHeaderGPU must match FMeshletHeader in Common.slang");

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
        // Distance/radius threshold at which LOD i becomes active (monotonic, [0] unused).
        float  LODScreenThreshold[MAX_MESH_LODS] = {};

        friend FArchive& operator << (FArchive& Ar, FGeometrySurface& Data)
        {
            Ar << Data.ID;
            Ar << Data.IndexCount;
            Ar << Data.StartIndex;
            Ar << Data.MaterialIndex;
            return Ar;
        }
    };

    struct RUNTIME_API FMeshResource : INonCopyable
    {
        struct FMeshBuffers
        {
            RHI::GPUPtr MeshletBuffer         = 0;
            RHI::GPUPtr MeshletSphereBuffer   = 0;
            RHI::GPUPtr MeshletConeBuffer     = 0;
            RHI::GPUPtr MeshletVertexBuffer   = 0;
            RHI::GPUPtr MeshletTriangleBuffer = 0;
            RHI::GPUPtr MeshletHeaderBuffer   = 0;

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

            /** Retire the five geometry buffers, leaving the header ALLOCATION in place: its address is identity
             *  that components and the uploaded instance buffer hold, and none is told synchronously of a rebuild. */
            void ReleaseGeometryBuffers()
            {
                RHI::Core::Retire(MeshletBuffer);
                RHI::Core::Retire(MeshletSphereBuffer);
                RHI::Core::Retire(MeshletConeBuffer);
                RHI::Core::Retire(MeshletVertexBuffer);
                RHI::Core::Retire(MeshletTriangleBuffer);

                MeshletBuffer         = 0;
                MeshletSphereBuffer   = 0;
                MeshletConeBuffer     = 0;
                MeshletVertexBuffer   = 0;
                MeshletTriangleBuffer = 0;
            }

            /** Full teardown, header included. Destruction only -- a REBUILD must never move the header. */
            void ReleaseAll()
            {
                ReleaseGeometryBuffers();
                RHI::Core::Retire(MeshletHeaderBuffer);
                MeshletHeaderBuffer = 0;
            }

        private:

            /** Takes ownership of Other's addresses and leaves it empty, so exactly one object frees. */
            void Steal(FMeshBuffers& Other)
            {
                MeshletBuffer         = Other.MeshletBuffer;
                MeshletSphereBuffer   = Other.MeshletSphereBuffer;
                MeshletConeBuffer     = Other.MeshletConeBuffer;
                MeshletVertexBuffer   = Other.MeshletVertexBuffer;
                MeshletTriangleBuffer = Other.MeshletTriangleBuffer;
                MeshletHeaderBuffer   = Other.MeshletHeaderBuffer;
                DistanceFieldTexture  = Other.DistanceFieldTexture;

                Other.MeshletBuffer         = 0;
                Other.MeshletSphereBuffer   = 0;
                Other.MeshletConeBuffer     = 0;
                Other.MeshletVertexBuffer   = 0;
                Other.MeshletTriangleBuffer = 0;
                Other.MeshletHeaderBuffer   = 0;
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
        TVector<FU8Vector4>       JointIndices;
        TVector<FU8Vector4>       JointWeights;
        TVector<uint32>           Indices;

        TVector<FGeometrySurface> GeometrySurfaces;
        FMeshletData              MeshletData;
        FMeshBuffers              MeshBuffers;
        bool                      bSkinnedMesh = false;

        FDistanceFieldVolume      DistanceField;

        uint32                    RequiredBoneCount = 0;

        //~ Build inputs. None are serialized -- the baked result is what persists.

        // Source scene-graph world transform; baked into vertices at merge time.
        FMatrix4                  ImportTransform = FMatrix4(1.0f);

        uint32                    MaxLODs = MAX_MESH_LODS;

        bool                      bGenerateTangents = true;

        bool                      bMeshletConeCulling = true;

        // Per-meshlet triangle reorder for the hardware vertex cache.
        bool                      bOptimizeMeshlets = true;

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

            for (FGeometrySurface& Surface : Data.GeometrySurfaces)
            {
                Ar << Surface.NumLODs;
                for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
                {
                    Ar << Surface.LODMeshletOffset[i];
                    Ar << Surface.LODMeshletCount[i];
                    Ar << Surface.LODScreenThreshold[i];
                }
            }

            Ar << Data.DistanceField;

            return Ar;
        }
    };
}
