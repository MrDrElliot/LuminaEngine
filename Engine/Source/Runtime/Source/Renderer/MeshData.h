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

namespace Lumina
{
    // 64 verts / 124 tris = AMD/NV mesh-shader sweet spot, satisfies meshopt
    // limits, and TriangleCount*3 fits the VS-emulation indirect arg count.
    constexpr uint32 MESHLET_MAX_VERTICES       = 64;
    constexpr uint32 MESHLET_MAX_TRIANGLES      = 124;
    constexpr uint32 MESHLET_VERTICES_PER_DRAW  = MESHLET_MAX_TRIANGLES * 3;

    // LOD 0 is full detail. Default ladder uses meshopt_simplify for 0-3 and
    // meshopt_simplifySloppy for 4-5.
    constexpr uint32 MAX_MESH_LODS              = 6;

    // Sloppy LODs (4-5) can produce holes that become shadow light-leaks; cap
    // shadow casters to topology-preserving LODs.
    constexpr uint32 MAX_SHADOW_LOD             = 3;

    // Cap for casters past FSceneRenderSettings::ShadowCoarseLODDistance. Out there one cascade texel is
    // wide enough that a sloppy LOD's holes stay sub-texel, so the leak they would cause never resolves.
    constexpr uint32 MAX_COARSE_SHADOW_LOD      = 5;

    // Positions are full float3 (FMeshletVertex.Position) -- no quantization, gap-free. LODIndex kept for
    // LOD selection. TriangleOffset is in dwords (3 micro-indices per dword).
    // NO alignas: the GPU mirror (Common.slang FMeshlet) is 5 tightly packed uints = 20B; alignas(16) here
    // would pad the upload stride to 32B and desync every meshlet after the first (GPU page fault).
    struct FMeshlet
    {
        uint32     VertexOffset;
        uint32     TriangleOffset;
        uint32     VertexCount;
        uint32     TriangleCount;
        uint32     LODIndex;

        friend FArchive& operator<<(FArchive& Ar, FMeshlet& Data)
        {
            Ar << Data.VertexOffset;
            Ar << Data.TriangleOffset;
            Ar << Data.VertexCount;
            Ar << Data.TriangleCount;
            Ar << Data.LODIndex;
            return Ar;
        }
    };
    static_assert(sizeof(FMeshlet) == 20, "FMeshlet must stay 20B to match the GPU mirror (Common.slang)");

    // Sphere for frustum/occlusion, cone for backface culling.
    struct alignas(16) FMeshletBounds
    {
        FVector3 Center;
        float     Radius;
        FVector3 ConeApex;
        float     ConeCutoff;   // = cos(angle / 2)
        FVector3 ConeAxis;
        float     _Pad0;

        friend FArchive& operator<<(FArchive& Ar, FMeshletBounds& Data)
        {
            Ar << Data.Center;
            Ar << Data.Radius;
            Ar << Data.ConeApex;
            Ar << Data.ConeCutoff;
            Ar << Data.ConeAxis;
            Ar << Data._Pad0;
            return Ar;
        }
    };

    struct FMeshletData
    {
        TVector<FMeshlet>               Meshlets;
        // Exactly one of these is populated, selected by bSkinnedMesh.
        TVector<FMeshletVertex>         MeshletVertices;
        TVector<FMeshletSkinnedVertex>  MeshletSkinnedVertices;
        TVector<uint32>                 MeshletTriangles;
        TVector<FMeshletBounds>         MeshletBounds;

        FORCEINLINE bool IsEmpty() const { return Meshlets.empty(); }

        FORCEINLINE void Clear()
        {
            Meshlets.clear();
            MeshletVertices.clear();
            MeshletSkinnedVertices.clear();
            MeshletTriangles.clear();
            MeshletBounds.clear();
        }

        friend FArchive& operator<<(FArchive& Ar, FMeshletData& Data)
        {
            Ar << Data.Meshlets;
            Ar << Data.MeshletVertices;
            Ar << Data.MeshletSkinnedVertices;
            Ar << Data.MeshletTriangles;
            Ar << Data.MeshletBounds;
            return Ar;
        }
    };

    // Per-mesh GPU header. Reached through FGPUInstance's MeshletHeader BDA.
    // Positions are full float3 in the vertex buffer -- no per-LOD grid needed.
    //
    // Slang reads this through a pointer under SCALAR layout, where every member sits at its own
    // natural alignment with no vector padding. The distance-field block is therefore declared as loose
    // floats rather than a float3: a float3 aligns to 4 under scalar layout but to 16 in C++ (FVector3
    // is not over-aligned today, but nothing stops it becoming so), and the two would silently disagree.
    // Keep this member-for-member identical to FMeshletHeader in Includes/Common.slang.
    struct alignas(16) FMeshletHeaderGPU
    {
        uint64    MeshletsAddress;                  // FMeshlet*
        uint64    BoundsAddress;                    // FMeshletBounds*
        uint64    VerticesAddress;                  // uint32*
        uint64    TrianglesAddress;                 // uint32*

        // gTextures3D[] slot of this mesh's distance field, or DistanceField::kInvalidIndex when the
        // mesh has none. Every SDF material node gates on this, so it must be written on ALL paths.
        uint32    DistanceFieldIndex;
        // bit0 = two-sided (unsigned) encoding.
        uint32    DistanceFieldFlags;

        // Mesh-local AABB the volume spans, and the local-space distance the encoded range covers.
        float     DistanceFieldMinX, DistanceFieldMinY, DistanceFieldMinZ;
        float     DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ;
        float     DistanceFieldMaxDistance;

        uint32    _Pad0;
        uint32    _Pad1;
        uint32    _Pad2;
    };
    static_assert(sizeof(FMeshletHeaderGPU) == 80, "FMeshletHeaderGPU must match FMeshletHeader in Common.slang");

    /** FMeshletHeaderGPU::DistanceFieldFlags bits. Mirrored in Includes/DistanceField.slang. */
    enum class EDistanceFieldFlags : uint32
    {
        None      = 0,
        TwoSided  = BIT(0),
    };

    struct FGeometrySurface final
    {
        FName   ID;
        uint32  IndexCount = 0;
        uint32  StartIndex = 0;
        int16   MaterialIndex = -1;

        // Per-LOD meshlet ranges into MeshletData.Meshlets; NumLODs >= 1.
        uint32  NumLODs                              = 1;
        uint32  LODMeshletOffset[MAX_MESH_LODS]      = {};
        uint32  LODMeshletCount[MAX_MESH_LODS]       = {};
        // distance/radius threshold at which LOD i becomes active (monotonic, [0] unused).
        float   LODScreenThreshold[MAX_MESH_LODS]    = {};

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
        // Device-local meshlet streams; GPUPtr doubles as the shader-visible BDA.
        // Frame-deferred frees keep in-flight frames safe when a mesh dies.
        struct FMeshBuffers
        {
            RHI::GPUPtr MeshletBuffer = 0;
            RHI::GPUPtr MeshletBoundsBuffer = 0;
            RHI::GPUPtr MeshletVertexBuffer = 0;
            RHI::GPUPtr MeshletTriangleBuffer = 0;
            RHI::GPUPtr MeshletHeaderBuffer = 0;

            // Extra frames beyond the GPU pipeline depth.
            static constexpr uint32 kResolveLagFrames = 2;

            // The mesh's distance field volume (Tex3D). Its heap slot is published in the meshlet
            // header, so it dies with the same set. Release is frame-deferred by kFramesInFlight and
            // repoints the freed slot at the magenta placeholder, so the worst a frame recorded against
            // the old index can do is sample a constant -- not fault.
            RHI::FManagedTexture DistanceFieldTexture;

            FMeshBuffers()                              = default;
            FMeshBuffers(const FMeshBuffers&)            = delete;
            FMeshBuffers& operator=(const FMeshBuffers&) = delete;

            FMeshBuffers(FMeshBuffers&& Other) noexcept { Steal(Other); }

            FMeshBuffers& operator=(FMeshBuffers&& Other) noexcept
            {
                if (this != &Other)
                {
                    ReleaseBuffers();
                    RHI::Textures::Release(DistanceFieldTexture);
                    Steal(Other);
                }
                return *this;
            }

            ~FMeshBuffers()
            {
                ReleaseBuffers();
                RHI::Textures::Release(DistanceFieldTexture);
            }

            /** Frees the five meshlet streams and clears them, so the set is safe to rebuild into.
             *  Deliberately does NOT touch DistanceFieldTexture: CreateDistanceFieldTexture owns that
             *  one and releases it itself, and releasing it twice would retire the same image twice. */
            void ReleaseBuffers()
            {
                RHI::Core::DeferredFree(MeshletBuffer,         kResolveLagFrames);
                RHI::Core::DeferredFree(MeshletBoundsBuffer,   kResolveLagFrames);
                RHI::Core::DeferredFree(MeshletVertexBuffer,   kResolveLagFrames);
                RHI::Core::DeferredFree(MeshletTriangleBuffer, kResolveLagFrames);
                RHI::Core::DeferredFree(MeshletHeaderBuffer,   kResolveLagFrames);

                MeshletBuffer         = 0;
                MeshletBoundsBuffer   = 0;
                MeshletVertexBuffer   = 0;
                MeshletTriangleBuffer = 0;
                MeshletHeaderBuffer   = 0;
            }

        private:

            /** Takes ownership of Other's addresses and leaves it empty, so exactly one object frees. */
            void Steal(FMeshBuffers& Other)
            {
                MeshletBuffer         = Other.MeshletBuffer;
                MeshletBoundsBuffer   = Other.MeshletBoundsBuffer;
                MeshletVertexBuffer   = Other.MeshletVertexBuffer;
                MeshletTriangleBuffer = Other.MeshletTriangleBuffer;
                MeshletHeaderBuffer   = Other.MeshletHeaderBuffer;
                DistanceFieldTexture  = Other.DistanceFieldTexture;

                Other.MeshletBuffer         = 0;
                Other.MeshletBoundsBuffer   = 0;
                Other.MeshletVertexBuffer   = 0;
                Other.MeshletTriangleBuffer = 0;
                Other.MeshletHeaderBuffer   = 0;
                Other.DistanceFieldTexture  = RHI::FManagedTexture{};
            }
        };

        FName                       Name;

        // Import-time scratch SoA streams; dropped after GenerateMeshlets. Active streams stay
        // parallel and equal length; joint streams populated only when bSkinnedMesh.
        TVector<FVector3>          Positions;
        TVector<uint32>             Normals;        // octahedral pack (PackNormal)
        TVector<uint32>             Tangents;       // octahedral + handedness (PackTangent)
        TVector<uint32>             UVs;            // packHalf2x16
        TVector<uint32>             Colors;         // RGBA8 (PackColor)
        TVector<FU8Vector4>        JointIndices;
        TVector<FU8Vector4>        JointWeights;

        TVector<uint32>             Indices;
        TVector<FGeometrySurface>   GeometrySurfaces;
        FMeshletData                MeshletData;
        FMeshBuffers                MeshBuffers;
        bool                        bSkinnedMesh = false;

        // Baked signed distance field, empty when the mesh has none. Kept on the CPU after upload: it is
        // small (a 48^3 field is 108 KB), and the editor's field inspector and any future CPU query path
        // both need it. Survives ClearVertices, like MeshletData does.
        FDistanceFieldVolume        DistanceField;

        // Highest joint index any skinned vertex references, +1 (0 when not skinned). Computed from the
        // packed meshlet vertices at GPU-buffer creation, before the import scratch is dropped.
        //
        // Runtime-only and deliberately NOT serialized: it exists to be checked against the SKELETON'S bone
        // count, which is a separate asset that can change independently of the mesh. Nothing bounds
        // `Bones()[BoneOffset + JointIndices.x]` on the GPU, so a mesh whose baked indices outrun its
        // skeleton reads past its bone slice into whatever follows -- and since a rig's highest indices are
        // its leaf bones (fingers, toes), that surfaces as wildly displaced vertices exactly there.
        uint32                      RequiredBoneCount = 0;

        // Source scene-graph world transform; baked into vertices at merge time.
        FMatrix4                   ImportTransform = FMatrix4(1.0f);

        // How many LOD levels GenerateMeshlets should build, clamped to [1, MAX_MESH_LODS]. A build input
        // like ImportTransform, so it is deliberately not serialized -- the baked result is what persists.
        // Runtime-generated meshes usually want far fewer than the import default; every extra level is
        // another full meshopt_simplify pass over the source index range.
        uint32                      MaxLODs = MAX_MESH_LODS;

        // False replaces the MikkTSpace pass with a cheap arbitrary per-vertex tangent basis. Also a build
        // input, also not serialized. Worth it only for geometry whose materials never sample a normal map,
        // since the substitute basis is valid but arbitrarily oriented.
        bool                        bGenerateTangents = true;

        // Per-meshlet normal cones, which cost twice: cone-weighted clustering inside meshopt_buildMeshlets
        // (the expensive clustering mode) plus a meshopt_computeMeshletBounds call per meshlet. Off derives
        // the culling sphere from the per-meshlet AABB pass that already runs and writes ConeCutoff = 1.0,
        // which is the shader's own "this meshlet has no cone" gate (CullMeshlets.slang) -- so backface
        // cluster culling is skipped rather than fed garbage, and frustum/occlusion culling still work off a
        // slightly more conservative (AABB-circumscribed) sphere. Build input, not serialized.
        //
        // Assets build once and keep it; geometry rebuilt every frame generally should not, because the
        // build cost lands on the frame that rebuilds it while the cull saving is spread over the frames
        // that draw it. See SDynamicMeshComponent.
        bool                        bMeshletConeCulling = true;

        // Per-meshlet triangle reorder for the hardware vertex cache. Build input, not serialized.
        bool                        bOptimizeMeshlets = true;

        FORCEINLINE size_t GetNumSurfaces() const { return GeometrySurfaces.size(); }

        FORCEINLINE bool IsSurfaceIndexValid(size_t Slot) const
        {
            return Slot < GetNumSurfaces();
        }

        FORCEINLINE const FGeometrySurface& GetSurface(size_t Slot) const
        {
            return GeometrySurfaces[Slot];
        }

        FORCEINLINE size_t GetNumVertices() const { return Positions.size(); }
        FORCEINLINE size_t GetNumIndices()  const { return Indices.size(); }
        FORCEINLINE size_t GetNumTriangles() const { return Indices.size() / 3; }
        FORCEINLINE NODISCARD bool IsSkinnedMesh() const { return bSkinnedMesh; }

        // Synthetic interleaved vertex size; only meshopt fetch/overdraw analysis needs it.
        FORCEINLINE size_t GetVertexTypeSize() const
        {
            return bSkinnedMesh ? sizeof(FSkinnedVertex) : sizeof(FVertex);
        }

        void ResizeVertices(size_t N)
        {
            Positions.resize(N);
            Normals.resize(N);
            Tangents.resize(N);
            UVs.resize(N);
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
            Drop(Colors);
            Drop(JointIndices);
            Drop(JointWeights);
        }

        void AppendVertex(const FVertex& V)
        {
            Positions.push_back(V.Position);
            Normals.push_back(V.Normal);
            Tangents.push_back(V.Tangent);
            UVs.push_back(V.UV);
            Colors.push_back(V.Color);
        }

        void AppendVertex(const FSkinnedVertex& V)
        {
            AppendVertex(static_cast<const FVertex&>(V));
            JointIndices.push_back(V.JointIndices);
            JointWeights.push_back(V.JointWeights);
        }

        FORCEINLINE FVector3 GetPositionAt(size_t Index) const { return Positions[Index]; }
        FORCEINLINE void SetPositionAt(size_t Index, FVector3 Position) { Positions[Index] = Position; }

        FORCEINLINE uint32 GetNormalAt(size_t Index) const { return Normals[Index]; }
        FORCEINLINE void SetNormalAt(size_t Index, uint32 Normal) { Normals[Index] = Normal; }

        FORCEINLINE uint32 GetTangentAt(size_t Index) const { return Tangents[Index]; }
        FORCEINLINE void SetTangentAt(size_t Index, uint32 Tangent) { Tangents[Index] = Tangent; }

        FORCEINLINE FVector2 GetUVAt(size_t Index) const { return Math::UnpackHalf2x16(UVs[Index]); }
        FORCEINLINE void SetUVAt(size_t Index, FVector2 UV) { UVs[Index] = Math::PackHalf2x16(UV); }

        FORCEINLINE uint32 GetColorAt(size_t Index) const { return Colors[Index]; }
        FORCEINLINE void SetColorAt(size_t Index, uint32 Color) { Colors[Index] = Color; }

        FORCEINLINE FU8Vector4 GetJointIndicesAt(size_t Index) const { return JointIndices[Index]; }
        FORCEINLINE void SetJointIndicesAt(size_t Index, FU8Vector4 InIndices) { JointIndices[Index] = InIndices; }

        FORCEINLINE FU8Vector4 GetJointWeightsAt(size_t Index) const { return JointWeights[Index]; }
        FORCEINLINE void SetJointWeightsAt(size_t Index, FU8Vector4 Weights) { JointWeights[Index] = Weights; }

        friend FArchive& operator << (FArchive& Ar, FMeshResource& Data)
        {
            Ar << Data.Name;
            Ar << Data.bSkinnedMesh;
            Ar << Data.GeometrySurfaces;
            Ar << Data.MeshletData;

            // Per-surface LOD payload; pre-LOD assets must be re-imported.
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

            // Assets saved before the field existed simply have none; a re-import (or the mesh editor's
            // Build Distance Field) is what gives them one.
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_DISTANCE_FIELD)
            {
                Ar << Data.DistanceField;
            }

            return Ar;
        }
    };
    
    
    struct RUNTIME_API FSkeletonResource : INonCopyable
    {
        struct FBoneInfo
        {
            FName Name;
            int32 ParentIndex;           // -1 for root bone
            FMatrix4 InvBindMatrix;     // Inverse bind pose matrix
            FMatrix4 LocalTransform;    // Local transform (relative to parent)
        
            friend FArchive& operator << (FArchive& Ar, FBoneInfo& Data)
            {
                Ar << Data.Name;
                Ar << Data.ParentIndex;
                Ar << Data.InvBindMatrix;
                Ar << Data.LocalTransform;
                return Ar;
            }
        };
        
        FName Name;
        TVector<FBoneInfo> Bones;
        THashMap<FName, int32> BoneNameToIndex;
        
        TVector<FVector3> BindLocalTranslations;
        TVector<FQuat>    BindLocalRotations;
        TVector<FVector3> BindLocalScales;
        uint32 BindPoseGeneration = 0;

        // Transient import-dialog flag; not serialized.
        bool bShouldImport = true;
        
        FORCEINLINE int32 GetNumBones() const
        {
            return (int32)Bones.size();
        }

        FORCEINLINE bool HasBindPoseCache() const
        {
            return !Bones.empty() && BindLocalRotations.size() == Bones.size();
        }

        // Decomposes every bone's LocalTransform into the SoA bind cache. Defined in Pose.cpp.
        void BuildBindPoseCache();
    
        FORCEINLINE int32 FindBoneIndex(const FName& BoneName) const
        {
            auto It = BoneNameToIndex.find(BoneName);
            return It != BoneNameToIndex.end() ? It->second : INDEX_NONE;
        }
    
        FORCEINLINE bool IsBoneIndexValid(int32 BoneIndex) const
        {
            return BoneIndex >= 0 && BoneIndex < GetNumBones();
        }
    
        FORCEINLINE const FBoneInfo& GetBone(int32 BoneIndex) const
        {
            return Bones[BoneIndex];
        }
        
        FORCEINLINE FBoneInfo& GetBone(int32 BoneIndex)
        {
            return Bones[BoneIndex];
        }
    
        FORCEINLINE const FBoneInfo* GetParentBone(int32 BoneIndex) const
        {
            if (!IsBoneIndexValid(BoneIndex))
            {
                return nullptr;
            }

            int32 ParentIdx = Bones[BoneIndex].ParentIndex;
            if (ParentIdx < 0)
            {
                return nullptr;
            }

            return &Bones[ParentIdx];
        }
    
        TVector<int32> GetChildBones(int32 BoneIndex) const
        {
            TVector<int32> Children;
            for (int32 i = 0; i < GetNumBones(); ++i)
            {
                if (Bones[i].ParentIndex == BoneIndex)
                {
                    Children.push_back(i);
                }
            }
            return Children;
        }
    
        TVector<int32> GetRootBones() const
        {
            TVector<int32> Roots;
            for (int32 i = 0; i < GetNumBones(); ++i)
            {
                if (Bones[i].ParentIndex < 0)
                {
                    Roots.push_back(i);
                }
            }
            return Roots;
        }
        
        friend FArchive& operator << (FArchive& Ar, FSkeletonResource& Data)
        {
            Ar << Data.Name;
            Ar << Data.Bones;
            Ar << Data.BoneNameToIndex;

            Data.BuildBindPoseCache();

            return Ar;
        }
    };
}
