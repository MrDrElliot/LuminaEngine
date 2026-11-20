#include "RuntimePCH.h"
#include <cfloat>
#include "ImportHelpers.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Templates/AsBytes.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Renderer/VertexOps.h"
#include "TaskSystem/TaskSystem.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include <meshoptimizer.h>
#include "Renderer/MeshQuantization.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/Import/ClusterLODBuilder.h"

namespace Lumina::Import::Mesh
{
    FMeshImportData::FMeshImportData() = default;
    FMeshImportData::FMeshImportData(FMeshImportData&&) noexcept = default;
    FMeshImportData& FMeshImportData::operator=(FMeshImportData&&) noexcept = default;

    FMeshImportData::~FMeshImportData()
    {
        // Preview thumbnails are heap textures; release them with the import session.
        for (FSourceImage& Image : Images)
        {
            if (Image.Thumbnail.IsValid())
            {
                RHI::Textures::Release(Image.Thumbnail);
            }
        }
    }

    namespace
    {
        // meshopt_Allocator frees strictly LIFO, asserting blocks[count-1] == ptr, so a bump arena is safe.
        constexpr size_t kMeshoptArenaSize  = 2 * Constants::kMiB;
        constexpr size_t kMeshoptAlignment  = 16u;
        constexpr uint64 kMeshoptHeapMarker = ~0ull;

        struct FMeshoptAllocHeader
        {
            uint64 PrevOffset;
            uint64 Padding;
        };
        static_assert(sizeof(FMeshoptAllocHeader) == kMeshoptAlignment, "Header must preserve payload alignment");

        struct FMeshoptArena
        {
            uint8* Base   = nullptr;
            size_t Offset = 0;

            ~FMeshoptArena() { if (Base != nullptr) { Memory::Free(Base); } }
            FMeshoptArena() = default;
            FMeshoptArena(const FMeshoptArena&) = delete;
            FMeshoptArena& operator=(const FMeshoptArena&) = delete;
            FMeshoptArena(FMeshoptArena&&) = delete;
            FMeshoptArena& operator=(FMeshoptArena&&) = delete;
        };

        FMeshoptArena& GetMeshoptArena()
        {
            thread_local FMeshoptArena GArena;
            return GArena;
        }

        void* MeshoptAlloc(size_t Size)
        {
            LUMINA_MEMORY_SCOPE("MeshOpt");

            FMeshoptArena& Arena = GetMeshoptArena();
            if (Arena.Base == nullptr)
            {
                Arena.Base = static_cast<uint8*>(Memory::Malloc(kMeshoptArenaSize, kMeshoptAlignment));
            }

            const size_t Aligned = (Arena.Offset + kMeshoptAlignment - 1u) & ~(kMeshoptAlignment - 1u);
            const size_t Total   = sizeof(FMeshoptAllocHeader) + Size;

            if (Arena.Base != nullptr && Total <= kMeshoptArenaSize - Aligned)
            {
                FMeshoptAllocHeader* Header = reinterpret_cast<FMeshoptAllocHeader*>(Arena.Base + Aligned);
                Header->PrevOffset = Arena.Offset;
                Arena.Offset       = Aligned + Total;
                return Header + 1;
            }

            // Requests scale with mesh size, so one big mesh must not be able to pin the arena.
            FMeshoptAllocHeader* Header = static_cast<FMeshoptAllocHeader*>(Memory::Malloc(Total, kMeshoptAlignment));
            Header->PrevOffset = kMeshoptHeapMarker;
            return Header + 1;
        }

        void MeshoptFree(void* Ptr)
        {
            if (Ptr == nullptr)
            {
                return;
            }

            FMeshoptAllocHeader* Header = static_cast<FMeshoptAllocHeader*>(Ptr) - 1;
            if (Header->PrevOffset == kMeshoptHeapMarker)
            {
                Memory::Free(Header);
                return;
            }

            GetMeshoptArena().Offset = (size_t)Header->PrevOffset;
        }

        // meshopt_setAllocator stores function pointers only, so static-init is safe.
        const bool GMeshoptAllocatorSet = []{ meshopt_setAllocator(MeshoptAlloc, MeshoptFree); return true; }();
    }

    namespace
    {
        // Callers size their array from this, so adding a stream cannot silently overflow it.
        constexpr uint32 kMaxVertexStreams = 8;

        // Describe every active SoA vertex stream for meshopt's multi-stream remap.
        uint32 BuildVertexStreams(FMeshResource& M, meshopt_Stream* OutStreams)
        {
            uint32 Count = 0;
            OutStreams[Count++] = { M.Positions.GetData(), sizeof(FVector3),   sizeof(FVector3) };
            OutStreams[Count++] = { M.Normals.GetData(),   sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.Tangents.GetData(),  sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.UVs.GetData(),       sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.UVs1.GetData(),      sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.Colors.GetData(),    sizeof(uint32),      sizeof(uint32) };
            if (M.bSkinnedMesh)
            {
                OutStreams[Count++] = { M.JointIndices.GetData(), sizeof(FU16Vector4), sizeof(FU16Vector4) };
                OutStreams[Count++] = { M.JointWeights.GetData(), sizeof(FU8Vector4), sizeof(FU8Vector4) };
            }
            LUMINA_ASSERT(Count <= kMaxVertexStreams, "BuildVertexStreams overflowed the caller's array");
            return Count;
        }

        // Apply a meshopt vertex remap to every active stream into fresh buffers (no overlap).
        void RemapVertexStreams(FMeshResource& M, const uint32* Remap, int32 OldCount, int32 NewCount)
        {
            auto RemapStream = [&](auto& Stream)
            {
                using TElem = typename std::remove_reference_t<decltype(Stream)>::value_type;
                TVector<TElem> Out(NewCount);
                meshopt_remapVertexBuffer(Out.GetData(), Stream.GetData(), OldCount, sizeof(TElem), Remap);
                Stream = Move(Out);
            };
            RemapStream(M.Positions);
            RemapStream(M.Normals);
            RemapStream(M.Tangents);
            RemapStream(M.UVs);
            RemapStream(M.UVs1);
            RemapStream(M.Colors);
            if (M.bSkinnedMesh)
            {
                RemapStream(M.JointIndices);
                RemapStream(M.JointWeights);
            }
        }
    }

    void ComputeTangents(FMeshResource& MeshResource, FScopedSlowTask* Progress = nullptr, float StepPerSurface = 0.0f)
    {
        const int32 NumVertices = MeshResource.GetNumVertices();
        if (MeshResource.Indices.IsEmpty() || NumVertices == 0)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        // meshopt wants unit float3 normals and float2 UVs; both streams live packed.
        if (MeshResource.Normals.Num() != NumVertices || MeshResource.UVs.Num() != NumVertices)
        {
            return;
        }

        MeshResource.Tangents.SetNum(NumVertices);

        TVector<FVector3> UnpackedNormals(NumVertices);
        TVector<FVector2> UnpackedUVs(NumVertices);
        {
            LUMINA_PROFILE_SECTION("Unpack Tangent Inputs");
            const uint32* InNormals = MeshResource.Normals.GetData();
            const uint32* InUVs     = MeshResource.UVs.GetData();
            FVector3*     OutNormals = UnpackedNormals.GetData();
            FVector2*     OutUVs     = UnpackedUVs.GetData();

            Jobs::ParallelFor((uint32)NumVertices, [=](const Jobs::FParallelRange& Range)
            {
                const size_t Count = Range.End - Range.Start;
                VertexOps::UnpackNormals(InNormals + Range.Start, OutNormals + Range.Start, Count);
                VertexOps::UnpackHalf2x16s(InUVs + Range.Start, OutUVs + Range.Start, Count);
            }, 4096);
        }

        // Surfaces share vertices, so each one packs into its own slot and the scatter below runs serially.
        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.Num();
        TVector<TVector<uint32>> SurfaceCornerTangents(NumSurfaces);
        Jobs::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
        {
            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            if (Section.IndexCount >= 3)
            {
                const uint32* SurfaceIndices = &MeshResource.Indices[Section.StartIndex];

                TVector<float> CornerTangents(IntCastChecked<int32>(static_cast<int64>(Section.IndexCount) * 4u));
                {
                    LUMINA_PROFILE_SECTION("meshopt_generateTangents");
                    meshopt_generateTangents(
                        CornerTangents.GetData(),
                        SurfaceIndices, Section.IndexCount,
                        &MeshResource.Positions[0].x, NumVertices, sizeof(FVector3),
                        &UnpackedNormals[0].x, sizeof(FVector3),
                        &UnpackedUVs[0].x, sizeof(FVector2),
                        meshopt_TangentCompatible);
                }

                TVector<uint32>& CornerPacked = SurfaceCornerTangents[SurfaceIdx];
                CornerPacked.SetNum(Section.IndexCount);
                VertexOps::PackTangents(CornerTangents.GetData(), CornerPacked.GetData(), Section.IndexCount);
            }

            if (Progress)
            {
                Progress->EnterProgressFrame(StepPerSurface);
            }
        });

        // Per-corner collapsed to per-vertex, where the last surface in order wins on a shared vertex.
        for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
        {
            const TVector<uint32>& CornerPacked = SurfaceCornerTangents[SurfaceIdx];
            if (CornerPacked.IsEmpty())
            {
                continue;
            }
            const uint32* SurfaceIndices = &MeshResource.Indices[MeshResource.GeometrySurfaces[SurfaceIdx].StartIndex];
            for (int32 i = 0; i < CornerPacked.Num(); ++i)
            {
                MeshResource.Tangents[SurfaceIndices[i]] = CornerPacked[i];
            }
        }
    }
    
    void GenerateFallbackTangents(FMeshResource& MeshResource)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumVertices = MeshResource.GetNumVertices();
        if (NumVertices == 0 || MeshResource.Normals.Num() != NumVertices)
        {
            return;
        }

        MeshResource.Tangents.SetNum(NumVertices);

        const uint32* Normals  = MeshResource.Normals.GetData();
        uint32*       Tangents = MeshResource.Tangents.GetData();

        constexpr uint32 kBatch = 256;
        Jobs::ParallelFor((uint32)NumVertices, [=](const Jobs::FParallelRange& Range)
        {
            FVector3 N[kBatch];
            float TangentsXYZW[kBatch * 4];
            for (uint32 Start = Range.Start; Start < Range.End; Start += kBatch)
            {
                const uint32 Count = Math::Min(kBatch, Range.End - Start);
                VertexOps::UnpackNormals(Normals + Start, N, Count);
                for (uint32 i = 0; i < Count; ++i)
                {
                    // Cross against whichever axis is least parallel to N, so the result never degenerates.
                    const FVector3 Axis = (Math::Abs(N[i].z) < 0.9f) ? FVector3(0.0f, 0.0f, 1.0f)
                                                                     : FVector3(1.0f, 0.0f, 0.0f);
                    const FVector3 T = Math::Normalize(Math::Cross(Axis, N[i]));
                    TangentsXYZW[i * 4 + 0] = T.x;
                    TangentsXYZW[i * 4 + 1] = T.y;
                    TangentsXYZW[i * 4 + 2] = T.z;
                    TangentsXYZW[i * 4 + 3] = 1.0f;
                }
                VertexOps::PackTangents(TangentsXYZW, Tangents + Start, Count);
            }
        }, 4096);
    }

    // meshopt_simplify allocates per vertex, so un-deduplicated input can request multi-GB.
    void DeduplicateMeshVertices(FMeshResource& MeshResource, FScopedSlowTask* Progress)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumVertices = MeshResource.GetNumVertices();
        const int32 NumIndices  = MeshResource.Indices.Num();
        if (NumVertices == 0 || NumIndices == 0)
        {
            return;
        }

        // Dedup exact-duplicate vertices across every SoA stream at once.
        if (Progress)
        {
            Progress->UpdateMessage("Removing duplicate vertices...");
        }

        meshopt_Stream Streams[kMaxVertexStreams];
        const uint32 StreamCount = BuildVertexStreams(MeshResource, Streams);

        TVector<uint32> Remap(NumVertices);
        const int32 UniqueVerts = IntCastChecked<int32>(meshopt_generateVertexRemapMulti(
            Remap.GetData(),
            MeshResource.Indices.GetData(), NumIndices,
            NumVertices, Streams, StreamCount));

        if (UniqueVerts < NumVertices)
        {
            meshopt_remapIndexBuffer(
                MeshResource.Indices.GetData(),
                MeshResource.Indices.GetData(), NumIndices,
                Remap.GetData());

            RemapVertexStreams(MeshResource, Remap.GetData(), NumVertices, UniqueVerts);
        }
    }

    // Assumes vertices were already deduplicated, which the pass above guarantees.
    void OptimizeNewlyImportedMesh(FMeshResource& MeshResource, FScopedSlowTask* Progress)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumVertices = MeshResource.GetNumVertices();
        const int32 NumIndices  = MeshResource.Indices.Num();

        if (NumVertices == 0 || NumIndices == 0)
        {
            return;
        }

        // Per-surface reorder; disjoint index slices make the in-place reorder thread-safe.
        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.Num();
        if (NumSurfaces > 0)
        {
            if (Progress)
            {
                Progress->UpdateMessage("Optimizing vertex cache & overdraw...");
            }
            const float* VertexPositions = reinterpret_cast<const float*>(MeshResource.Positions.GetData());

            Jobs::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
            {
                FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
                if (Section.IndexCount == 0)
                {
                    return;
                }

                meshopt_optimizeVertexCache(
                    &MeshResource.Indices[Section.StartIndex],
                    &MeshResource.Indices[Section.StartIndex],
                    Section.IndexCount, NumVertices);

                constexpr float Threshold = 1.05f;
                meshopt_optimizeOverdraw(
                    &MeshResource.Indices[Section.StartIndex],
                    &MeshResource.Indices[Section.StartIndex],
                    Section.IndexCount,
                    VertexPositions,
                    NumVertices, sizeof(FVector3), Threshold);
            });
        }

        // Vertex-fetch reorder must be last; depends on final index order.
        if (Progress)
        {
            Progress->UpdateMessage("Optimizing vertex fetch...");
        }
        {
            TVector<uint32> FetchRemap(NumVertices);
            const int32 NewCount = IntCastChecked<int32>(meshopt_optimizeVertexFetchRemap(
                FetchRemap.GetData(),
                MeshResource.Indices.GetData(), NumIndices,
                NumVertices));

            meshopt_remapIndexBuffer(
                MeshResource.Indices.GetData(),
                MeshResource.Indices.GetData(), NumIndices,
                FetchRemap.GetData());

            RemapVertexStreams(MeshResource, FetchRemap.GetData(), NumVertices, NewCount);
        }
    }

    namespace
    {
        // Normal xyz then UV xy, which the simplifier weighs and a destructive level rewrites.
        constexpr uint32 kSimplifyAttributeCount = 5u;
        constexpr float  kSimplifyAttributeWeights[kSimplifyAttributeCount] = { 1.0f, 1.0f, 1.0f, 0.5f, 0.5f };

        // One surface in its own compact vertex space, which every level of it simplifies from.
        struct FSurfaceSimplifyInput
        {
            TVector<uint32>   Indices;
            TVector<FVector3> Positions;
            TVector<float>    Attributes;
            TVector<uint8>    Locks;
            TVector<uint32>   SourceVertex;
            // meshopt_simplifyScale of Positions, which turns a relative error into a mesh-local distance.
            float             ErrorScale = 0.0f;
            // Positions are the vertices' own rather than copies of the mesh's, as when a hierarchy grew some of them.
            bool              bOwnsPositions = false;
        };

        struct FSurfaceMeshletResult
        {
            TVector<uint32>          Vertices;
            TVector<uint8>           Triangles;
            TVector<FMeshlet>        OutMeshlets;
            TVector<FMeshletSphere>  Spheres;
            TVector<FMeshletCone>    Cones;
            bool                     bHasData  = false;

            // Null when Vertices index the mesh's own streams, as LOD 0 does.
            const FSurfaceSimplifyInput* Source = nullptr;
            // A destructive level's moved positions and rewritten attributes, in Source's vertex space.
            TVector<FVector3>        UpdatedPositions;
            TVector<float>           UpdatedAttributes;
            float                    Error = 0.0f;

            // One per meshlet on a cluster hierarchy level, empty on a level that is its own root.
            TVector<FMeshletLODBounds> LODBounds;
        };

        // One surface's LOD slots as the pack writes them, the result cell each draws from and what each tells the cull.
        struct FSurfaceLODTable
        {
            TInlineVector<uint32, MAX_MESH_LODS> Accepted;
            TInlineVector<float, MAX_MESH_LODS>  Error;
            TInlineVector<float, MAX_MESH_LODS>  ParentError;
            uint32                               Flags = 0;

            void Add(uint32 Cell, float InError, float InParentError)
            {
                Accepted.Add(Cell);
                Error.Add(InError);
                ParentError.Add(InParentError);
            }
        };

        uint64 PositionKey(const FVector3& Position)
        {
            uint32 Bits[3];
            memcpy(Bits, &Position.x, sizeof(Bits));
            uint64 Key = Bits[0];
            Key = Key * 0x100000001B3ull ^ Bits[1];
            Key = Key * 0x100000001B3ull ^ Bits[2];
            return Key;
        }

        // A vertex sharing its position with another surface is locked, so neighboring surfaces stay sealed at every level.
        TVector<FSurfaceSimplifyInput> BuildSurfaceSimplifyInputs(const FMeshResource& MeshResource)
        {
            LUMINA_PROFILE_SCOPE();

            const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.Num();
            const int32 NumVertices = MeshResource.GetNumVertices();
            const bool   bHasNormals = MeshResource.Normals.Num() == NumVertices;
            const bool   bHasUVs     = MeshResource.UVs.Num() == NumVertices;

            TVector<FSurfaceSimplifyInput> Inputs(NumSurfaces);
            Jobs::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
            {
                const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
                FSurfaceSimplifyInput&  In      = Inputs[SurfaceIdx];
                if (Section.IndexCount < 3)
                {
                    return;
                }

                const uint32* SurfaceIndices = &MeshResource.Indices[Section.StartIndex];
                In.SourceVertex.Reset();
                In.SourceVertex.Append(SurfaceIndices, SurfaceIndices + Section.IndexCount);
                Algo::Sort(In.SourceVertex);
                const uint32* const UniqueEnd = Algo::Unique(In.SourceVertex);
                In.SourceVertex.SetNum(static_cast<int32>(UniqueEnd - In.SourceVertex.GetData()));

                In.Indices.SetNum(Section.IndexCount);
                for (uint32 i = 0; i < Section.IndexCount; ++i)
                {
                    const auto Found = Algo::LowerBound(In.SourceVertex.begin(), In.SourceVertex.end(), SurfaceIndices[i]);
                    In.Indices[i] = (uint32)(Found - In.SourceVertex.begin());
                }

                const int32 LocalCount = In.SourceVertex.Num();
                In.Positions.SetNum(LocalCount);
                In.Attributes.Init(0.0f, LocalCount * kSimplifyAttributeCount);
                for (int32 v = 0; v < LocalCount; ++v)
                {
                    const uint32 MeshVertex = In.SourceVertex[v];
                    In.Positions[v] = MeshResource.Positions[MeshVertex];

                    float* Attributes = &In.Attributes[v * kSimplifyAttributeCount];
                    if (bHasNormals)
                    {
                        const FVector3 Normal = UnpackNormal(MeshResource.Normals[MeshVertex]);
                        Attributes[0] = Normal.x;
                        Attributes[1] = Normal.y;
                        Attributes[2] = Normal.z;
                    }
                    if (bHasUVs)
                    {
                        const FVector2 UV = Math::UnpackHalf2x16(MeshResource.UVs[MeshVertex]);
                        Attributes[3] = UV.x;
                        Attributes[4] = UV.y;
                    }
                }

                In.ErrorScale = meshopt_simplifyScale(&In.Positions[0].x, LocalCount, sizeof(FVector3));
            });

            if (NumSurfaces < 2)
            {
                return Inputs;
            }

            constexpr uint32 kSharedPosition = ~0u;
            THashMap<uint64, uint32> PositionOwner;
            for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
            {
                const uint32 Surface = SurfaceIdx;
                for (const FVector3& Position : Inputs[SurfaceIdx].Positions)
                {
                    uint32& Owner = PositionOwner.FindOrAdd(PositionKey(Position), Surface);
                    if (Owner != Surface)
                    {
                        Owner = kSharedPosition;
                    }
                }
            }

            Jobs::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
            {
                FSurfaceSimplifyInput& In = Inputs[SurfaceIdx];
                In.Locks.Init((uint8)0u, In.Positions.Num());
                for (int32 v = 0; v < In.Positions.Num(); ++v)
                {
                    if (PositionOwner.FindChecked(PositionKey(In.Positions[v])) == kSharedPosition)
                    {
                        In.Locks[v] = meshopt_SimplifyVertex_Lock;
                    }
                }
            });

            return Inputs;
        }

        // Build meshlets for one (LOD, Surface) cell; quantization deferred to the serial pack pass.
        template<typename TReadPos>
        void BuildLODMeshletsForRange(
            const uint32* SrcIndices, size_t SrcIndexCount,
            const float*  VertexPositions, size_t NumVertices, size_t VertexSize,
            bool bConeCulling, bool bOptimizeMeshlets, bool bFastBuild,
            TReadPos&&    ReadPosition,
            FSurfaceMeshletResult& Result)
        {
            LUMINA_PROFILE_SECTION("Build LOD Meshlets For Range");
            
            constexpr size_t MaxVertices  = MESHLET_MAX_VERTICES;
            constexpr size_t MaxTriangles = MESHLET_MAX_TRIANGLES;
            
            const float ConeWeight = bConeCulling ? 0.25f : 0.0f;

            if (SrcIndexCount < 3)
            {
                return;
            }
            
            const size_t MaxMeshlets = meshopt_buildMeshletsBound(SrcIndexCount, MaxVertices, MaxTriangles);

            TVector<meshopt_Meshlet> LocalMeshlets;
            {
                LUMINA_PROFILE_SECTION("Meshlet Scratch Alloc");
                LocalMeshlets.SetNum(IntCastChecked<int32>(MaxMeshlets));
                Result.Vertices.SetNum(IntCastChecked<int32>(MaxMeshlets * MaxVertices));
                Result.Triangles.SetNum(IntCastChecked<int32>(MaxMeshlets * MaxTriangles * 3));
            }

            size_t MeshletCount = 0;
            if (bFastBuild)
            {
                LUMINA_PROFILE_SECTION("meshopt_buildMeshletsScan");
                MeshletCount = meshopt_buildMeshletsScan(
                    LocalMeshlets.GetData(),
                    Result.Vertices.GetData(),
                    Result.Triangles.GetData(),
                    SrcIndices, SrcIndexCount,
                    NumVertices,
                    MaxVertices, MaxTriangles);
            }
            else
            {
                LUMINA_PROFILE_SECTION("meshopt_buildMeshlets");
                MeshletCount = meshopt_buildMeshlets(
                    LocalMeshlets.GetData(),
                    Result.Vertices.GetData(),
                    Result.Triangles.GetData(),
                    SrcIndices, SrcIndexCount,
                    VertexPositions, NumVertices, VertexSize,
                    MaxVertices, MaxTriangles, ConeWeight);
            }

            if (MeshletCount == 0)
            {
                Result.Vertices.Reset();
                Result.Triangles.Reset();
                return;
            }

            LocalMeshlets.SetNum(IntCastChecked<int32>(MeshletCount));

            // meshopt pads triangles to a multiple of 4.
            const meshopt_Meshlet& Last = LocalMeshlets.Last();
            Result.Vertices.SetNum(Last.vertex_offset + Last.vertex_count);
            Result.Triangles.SetNum(Last.triangle_offset + ((Last.triangle_count * 3 + 3) & ~3u));

            if (bOptimizeMeshlets)
            {
                LUMINA_PROFILE_SECTION("meshopt_optimizeMeshlet");
                for (const meshopt_Meshlet& M : LocalMeshlets)
                {
                    meshopt_optimizeMeshlet(
                        &Result.Vertices[M.vertex_offset],
                        &Result.Triangles[M.triangle_offset],
                        M.triangle_count, M.vertex_count);
                }
            }

            Result.OutMeshlets.Reserve(IntCastChecked<int32>(MeshletCount));
            Result.Spheres.Reserve(IntCastChecked<int32>(MeshletCount));
            Result.Cones.Reserve(IntCastChecked<int32>(MeshletCount));
            
            {
            LUMINA_PROFILE_SECTION("Meshlet Bounds");
            for (const meshopt_Meshlet& M : LocalMeshlets)
            {
                FMeshlet Out{};
                Out.VertexOffset   = M.vertex_offset;
                Out.TriangleOffset = M.triangle_offset;
                Out.VertexCount    = M.vertex_count;
                Out.TriangleCount  = M.triangle_count;
                Result.OutMeshlets.Add(Out);

                FMeshletSphere Sphere{};
                FMeshletCone   Cone = kDisabledMeshletCone;
                if (bConeCulling)
                {
                    const meshopt_Bounds B = meshopt_computeMeshletBounds(
                        &Result.Vertices[M.vertex_offset],
                        &Result.Triangles[M.triangle_offset],
                        M.triangle_count,
                        VertexPositions, NumVertices, VertexSize);

                    Sphere.Center = FVector3(B.center[0], B.center[1], B.center[2]);
                    Sphere.Radius = B.radius;

                    // meshopt already quantized these conservatively, so re-encoding would only lose ground.
                    Cone.Axis[0] = (int8)B.cone_axis_s8[0];
                    Cone.Axis[1] = (int8)B.cone_axis_s8[1];
                    Cone.Axis[2] = (int8)B.cone_axis_s8[2];
                    Cone.Cutoff  = (int8)B.cone_cutoff_s8;
                }
                else
                {
                    FVector3 MeshletPositions[MESHLET_MAX_VERTICES];
                    for (uint32 i = 0; i < M.vertex_count; ++i)
                    {
                        MeshletPositions[i] = ReadPosition(Result.Vertices[M.vertex_offset + i]);
                    }

                    // Same solver the cone path gets, so dropping cones no longer also loosens the sphere.
                    const meshopt_Bounds B = meshopt_computeSphereBounds(
                        &MeshletPositions[0].x, M.vertex_count, sizeof(FVector3),
                        nullptr, 0);

                    Sphere.Center = FVector3(B.center[0], B.center[1], B.center[2]);
                    Sphere.Radius = B.radius;
                }
                Result.Spheres.Add(Sphere);
                Result.Cones.Add(Cone);
            }
            }

            Result.bHasData = !Result.OutMeshlets.IsEmpty();
        }

        // A fast build splits a surface this large into ranges built in parallel, since one surface is otherwise one serial job.
        constexpr size_t kChunkedBuildIndices = 8192u * 3u;

        bool ShouldChunkSurface(const FGeometrySurface& Section, bool bFastMeshletBuild)
        {
            return bFastMeshletBuild && Section.IndexCount > kChunkedBuildIndices * 2u;
        }

        // Appends From's meshlets after Into's, shifting their offsets into the merged vertex and triangle lists.
        void AppendMeshletResult(FSurfaceMeshletResult& Into, FSurfaceMeshletResult& From)
        {
            if (!From.bHasData)
            {
                return;
            }

            const uint32 VertexShift   = (uint32)Into.Vertices.Num();
            const uint32 TriangleShift = (uint32)Into.Triangles.Num();
            Into.Vertices.Append(From.Vertices);
            Into.Triangles.Append(From.Triangles);
            for (FMeshlet M : From.OutMeshlets)
            {
                M.VertexOffset   += VertexShift;
                M.TriangleOffset += TriangleShift;
                Into.OutMeshlets.Add(M);
            }
            Into.Spheres.Append(From.Spheres);
            Into.Cones.Append(From.Cones);
            Into.bHasData  = true;
        }

        // MUST run before the scratch streams are dropped, since they are never serialized.
        void ComputeSurfaceTexelFactors(FMeshResource& MeshResource)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 NumIndices  = MeshResource.Indices.Num();
            const uint32 NumVertices = static_cast<uint32>(MeshResource.Positions.Num());

            // Leaving it at zero is the honest answer, and consumers fall back rather than act on it.
            if (static_cast<uint32>(MeshResource.UVs.Num()) < NumVertices || NumVertices == 0 || NumIndices < 3)
            {
                for (FGeometrySurface& Surface : MeshResource.GeometrySurfaces)
                {
                    Surface.TexelFactor = 0.0f;
                }
                return;
            }

            for (FGeometrySurface& Surface : MeshResource.GeometrySurfaces)
            {
                Surface.TexelFactor = 0.0f;

                const int64 Start = Surface.StartIndex;
                const int64 End   = Math::Min<int64>(Start + Surface.IndexCount, NumIndices);
                if (End < Start + 3)
                {
                    continue;
                }

                // A large mesh in centimeters can sum world areas past float precision before running out.
                double WorldArea = 0.0;
                double UVArea    = 0.0;

                for (int32 i = static_cast<int32>(Start); i + 2 < End; i += 3)
                {
                    const uint32 I0 = MeshResource.Indices[i];
                    const uint32 I1 = MeshResource.Indices[i + 1];
                    const uint32 I2 = MeshResource.Indices[i + 2];

                    if (I0 >= NumVertices || I1 >= NumVertices || I2 >= NumVertices)
                    {
                        continue;
                    }

                    const FVector3& P0 = MeshResource.Positions[I0];
                    const FVector3& P1 = MeshResource.Positions[I1];
                    const FVector3& P2 = MeshResource.Positions[I2];

                    const FVector3 E1 = P1 - P0;
                    const FVector3 E2 = P2 - P0;
                    WorldArea += 0.5 * (double)Math::Length(Math::Cross(E1, E2));

                    const FVector2 UV0 = Math::UnpackHalf2x16(MeshResource.UVs[I0]);
                    const FVector2 UV1 = Math::UnpackHalf2x16(MeshResource.UVs[I1]);
                    const FVector2 UV2 = Math::UnpackHalf2x16(MeshResource.UVs[I2]);

                    const FVector2 T1 = UV1 - UV0;
                    const FVector2 T2 = UV2 - UV0;
                    UVArea += 0.5 * Math::Abs((double)T1.x * (double)T2.y - (double)T2.x * (double)T1.y);
                }

                // Dividing would be infinite, which downstream reads as needing infinite resolution.
                if (UVArea > 1e-12 && WorldArea > 0.0)
                {
                    Surface.TexelFactor = (float)Math::Sqrt(WorldArea / UVArea);
                }
            }
        }
    } // namespace

    namespace
    {
        // A level without bounds of its own is always its own finest and coarsest, so the cut keeps it.
        FMeshletLODBounds GetPackedLODBounds(const FSurfaceMeshletResult& Result, uint32 MeshletIdx)
        {
            if (!Result.LODBounds.IsEmpty())
            {
                return Result.LODBounds[MeshletIdx];
            }
            FMeshletLODBounds Bounds{};
            Bounds.ParentError = kMeshletLODErrorInfinite;
            return Bounds;
        }

        struct FLevelVertex
        {
            uint32 MeshVertex;
            uint32 Normal;
            uint32 Tangent;
            uint32 UV;
        };

        // Each distinct vertex gets one slot, in order of first use, so a meshlet's refs stay close and usually fit 16 bits.
        template<typename TLevelPosition, typename TResolveLevelVertex>
        void PackStaticMeshlets(FMeshResource& MeshResource, const TVector<FSurfaceMeshletResult>& Results,
                                const TVector<FSurfaceLODTable>& Tables, uint32 NumSurfaces,
                                TLevelPosition&& LevelPosition, TResolveLevelVertex&& ResolveLevelVertex)
        {
            LUMINA_PROFILE_SCOPE();

            struct FPackSlot
            {
                const FSurfaceMeshletResult* Result;
                uint32                       MeshletIdx;
                uint32                       LOD;
                uint32                       RefStart;
                uint32                       TriangleStart;
            };

            struct FUniqueSource
            {
                const FSurfaceMeshletResult* Result;
                uint32                       Ref;
            };

            TVector<FPackSlot> PackSlots;
            uint32 RefCursor      = 0;
            uint32 TriangleCursor = 0;

            // Surface-major, so a cluster surface's slots form one contiguous run a view can take a slice of.
            for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
            {
                const FSurfaceLODTable& Table   = Tables[SurfaceIdx];
                FGeometrySurface&       Section = MeshResource.GeometrySurfaces[SurfaceIdx];
                Section.LODFlags = Table.Flags;

                for (uint32 Slot = 0; Slot < static_cast<uint32>(Table.Accepted.Num()); ++Slot)
                {
                    const FSurfaceMeshletResult& Result = Results[Table.Accepted[Slot] * NumSurfaces + SurfaceIdx];
                    if (!Result.bHasData)
                    {
                        continue;
                    }

                    Section.LODMeshletOffset[Slot] = (uint32)PackSlots.Num();
                    Section.LODMeshletCount[Slot]  = (uint32)Result.OutMeshlets.Num();
                    Section.LODError[Slot]         = Table.Error[Slot];
                    Section.LODParentError[Slot]   = Table.ParentError[Slot];
                    Section.NumLODs                = Slot + 1u;

                    for (int32 MeshletIdx = 0; MeshletIdx < Result.OutMeshlets.Num(); ++MeshletIdx)
                    {
                        const FMeshlet& M = Result.OutMeshlets[MeshletIdx];
                        PackSlots.Emplace(&Result, (uint32)MeshletIdx, Slot, RefCursor, TriangleCursor);
                        RefCursor      += M.VertexCount;
                        TriangleCursor += M.TriangleCount;
                    }
                }
            }

            // A level that kept the source vertices shares their slots, a destructive level moved its vertices and gets its own, and a hierarchy's levels share its vertex space.
            TVector<uint32>        UniqueOf(RefCursor);
            TVector<FUniqueSource> UniqueSources;
            {
                LUMINA_PROFILE_SECTION("Assign Shared Vertices");
                TVector<uint32> MeshSlot(MeshResource.GetNumVertices(), ~0u);
                THashMap<const void*, TVector<uint32>> OwnSlots;

                for (const FPackSlot& Pack : PackSlots)
                {
                    const FSurfaceMeshletResult& Result   = *Pack.Result;
                    const FMeshlet&              M        = Result.OutMeshlets[Pack.MeshletIdx];
                    const bool                   bMoved   = !Result.UpdatedPositions.IsEmpty();
                    const bool                   bOwnSpace = bMoved || (Result.Source != nullptr && Result.Source->bOwnsPositions);

                    TVector<uint32>* LevelSlots = nullptr;
                    if (bOwnSpace)
                    {
                        LevelSlots = &OwnSlots[bMoved ? static_cast<const void*>(&Result) : static_cast<const void*>(Result.Source)];
                        if (LevelSlots->IsEmpty())
                        {
                            LevelSlots->Init(~0u, bMoved ? Result.UpdatedPositions.Num() : Result.Source->Positions.Num());
                        }
                    }

                    for (uint32 i = 0; i < M.VertexCount; ++i)
                    {
                        const uint32 Ref  = Result.Vertices[M.VertexOffset + i];
                        uint32&      Slot = bOwnSpace ? (*LevelSlots)[Ref]
                                                      : MeshSlot[Result.Source != nullptr ? Result.Source->SourceVertex[Ref] : Ref];
                        if (Slot == ~0u)
                        {
                            Slot = (uint32)UniqueSources.Num();
                            UniqueSources.Emplace(&Result, Ref);
                        }
                        UniqueOf[Pack.RefStart + i] = Slot;
                    }
                }
            }

            const uint32  UniqueCount = (uint32)UniqueSources.Num();
            FMeshletData& Data        = MeshResource.MeshletData;

            TVector<FVector3> Positions(UniqueCount);
            TVector<uint32>   UV1s(UniqueCount);
            TVector<uint32>   Colors(UniqueCount);
            Data.VertexAttributes.SetNum(UniqueCount);
            {
                LUMINA_PROFILE_SECTION("Resolve Shared Vertices");
                Jobs::ParallelFor(UniqueCount, [&](const Jobs::FParallelRange& Range)
                {
                    for (uint32 i = Range.Start; i < Range.End; ++i)
                    {
                        const FUniqueSource& Source = UniqueSources[i];
                        const FLevelVertex   Level  = ResolveLevelVertex(*Source.Result, Source.Ref);
                        Positions[i]             = LevelPosition(*Source.Result, Source.Ref);
                        Data.VertexAttributes[i] = { Level.Normal, Level.Tangent, Level.UV };
                        UV1s[i]                  = MeshResource.UVs1[Level.MeshVertex];
                        Colors[i]                = MeshResource.Colors[Level.MeshVertex];
                    }
                }, 1024);
            }

            bool bHasUV1   = false;
            bool bHasColor = false;
            for (uint32 i = 0; i < UniqueCount; ++i)
            {
                bHasUV1   = bHasUV1   || UV1s[i] != Data.VertexAttributes[i].UV;
                bHasColor = bHasColor || Colors[i] != 0xFFFFFFFFu;
            }
            Data.VertexUV1s   = bHasUV1   ? Move(UV1s)   : TVector<uint32>();
            Data.VertexColors = bHasColor ? Move(Colors) : TVector<uint32>();

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.GetData(), Positions.Num());
            Data.VertexPositions.SetNum(UniqueCount);
            Jobs::ParallelFor(UniqueCount, [&](const Jobs::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Data.VertexPositions[i] = EncodeMeshPosition(Data.PositionGrid, Positions[i]);
                }
            }, 4096);

            Data.Meshlets.SetNum(PackSlots.Num());
            Data.MeshletSpheres.SetNum(PackSlots.Num());
            Data.MeshletCones.SetNum(PackSlots.Num());
            Data.MeshletTriangles.SetNum(TriangleCursor);
            Data.MeshletVertexRefs.Reserve(RefCursor);
            // Every static mesh is a hierarchy, a level of its own when built without one.
            Data.MeshletLODBounds.SetNum(PackSlots.Num());

            {
                LUMINA_PROFILE_SECTION("Pack Static Meshlets");
                for (int32 PackIdx = 0; PackIdx < PackSlots.Num(); ++PackIdx)
                {
                    const FPackSlot&             Pack   = PackSlots[PackIdx];
                    const FSurfaceMeshletResult& Result = *Pack.Result;

                    FMeshlet Out = Result.OutMeshlets[Pack.MeshletIdx];
                    Out.LODIndex = Math::Min(Pack.LOD, (uint32)MESHLET_LOD_MASK);

                    Data.MeshletLODBounds[PackIdx] = GetPackedLODBounds(Result, Pack.MeshletIdx);

                    const uint8* TriSrc = Result.Triangles.GetData() + Out.TriangleOffset;
                    for (uint32 t = 0; t < Out.TriangleCount; ++t)
                    {
                        Data.MeshletTriangles[Pack.TriangleStart + t] =
                              (uint32)TriSrc[t * 3 + 0]
                            | ((uint32)TriSrc[t * 3 + 1] << 8)
                            | ((uint32)TriSrc[t * 3 + 2] << 16);
                    }

                    AppendMeshletVertexRefs(Data, Out, &UniqueOf[Pack.RefStart], Out.VertexCount);
                    Out.TriangleOffset = Pack.TriangleStart;

                    Data.Meshlets[PackIdx]       = Out;
                    Data.MeshletSpheres[PackIdx] = Result.Spheres[Pack.MeshletIdx];
                    Data.MeshletCones[PackIdx]   = Result.Cones[Pack.MeshletIdx];
                }
            }
        }
    }

    void GenerateMeshlets(FMeshResource& MeshResource, FScopedSlowTask* Progress, float StepPerSurface)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        MeshResource.MeshletData.Clear();

        const float TangentStep = StepPerSurface * 0.35f;
        const float MeshletStep = StepPerSurface * 0.65f;

        if (Progress)
        {
            Progress->UpdateMessage("Generating tangents...");
        }
        if (MeshResource.bGenerateTangents)
        {
            ComputeTangents(MeshResource, Progress, TangentStep);
        }
        else
        {
            GenerateFallbackTangents(MeshResource);
            if (Progress)
            {
                Progress->EnterProgressFrame(TangentStep * (float)MeshResource.GeometrySurfaces.Num());
            }
        }

        const int32 NumVertices = MeshResource.GetNumVertices();
        const int32 NumIndices  = MeshResource.Indices.Num();
        constexpr size_t PositionStride = sizeof(FVector3);

        if (NumVertices == 0 || NumIndices == 0)
        {
            for (FGeometrySurface& Section : MeshResource.GeometrySurfaces)
            {
                Section.NumLODs = 1;
                for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
                {
                    Section.LODMeshletOffset[i] = 0;
                    Section.LODMeshletCount[i]  = 0;
                    Section.LODError[i]         = 0.0f;
                }
            }
            if (Progress)
            {
                Progress->EnterProgressFrame(StepPerSurface * (float)MeshResource.GeometrySurfaces.Num());
            }
            return;
        }

        // Before any of the meshlet build below, which drops the streams this reads.
        ComputeSurfaceTexelFactors(MeshResource);

        const float* VertexPositions = reinterpret_cast<const float*>(MeshResource.Positions.GetData());

        auto ReadPosition = [&](uint32 GlobalIdx) -> FVector3
        {
            return MeshResource.Positions[GlobalIdx];
        };

        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.Num();

        const bool   bConeCulling      = MeshResource.bMeshletConeCulling;
        const bool   bOptimizeMeshlets = MeshResource.bOptimizeMeshlets;
        const bool   bFastMeshletBuild = MeshResource.bFastMeshletBuild;

        if (Progress)
        {
            Progress->UpdateMessage("Building meshlets & LODs...");
        }

        // The scan builder cuts on index order alone, so it needs a locality pass the greedy one does for itself.
        if (bFastMeshletBuild)
        {
            LUMINA_PROFILE_SECTION("Scan Build Index Pre-Pass");

            // Per chunk on a chunked surface, matching the ranges the build below cuts it into.
            TVector<FUIntVector2> Ranges;
            for (const FGeometrySurface& Section : MeshResource.GeometrySurfaces)
            {
                const size_t Step = ShouldChunkSurface(Section, true) ? kChunkedBuildIndices : (size_t)Section.IndexCount;
                for (size_t Begin = 0; Begin < Section.IndexCount; Begin += Step)
                {
                    const size_t Count = Math::Min(Step, (size_t)Section.IndexCount - Begin);
                    Ranges.Emplace((uint32)(Section.StartIndex + Begin), (uint32)Count);
                }
            }

            Jobs::ParallelFor((uint32)Ranges.Num(), [&](uint32 RangeIdx)
            {
                uint32* RangeIndices = &MeshResource.Indices[Ranges[RangeIdx].x];
                meshopt_optimizeVertexCache(RangeIndices, RangeIndices, Ranges[RangeIdx].y, NumVertices);
            });
        }

        // Callers that rebuild often (dynamic meshes) keep only the source level, a hierarchy of one.
        const bool bBuildHierarchy = MeshResource.bLODHierarchy;

        const TVector<FSurfaceSimplifyInput> SimplifyInputs = bBuildHierarchy ? BuildSurfaceSimplifyInputs(MeshResource) : TVector<FSurfaceSimplifyInput>();

        // Cells are slot-major, every slot a mesh can hold, since a cluster surface fills them from the hierarchy.
        TVector<FSurfaceMeshletResult> Results(MAX_MESH_LODS * NumSurfaces);

        TVector<FSurfaceLODTable> Tables(NumSurfaces);

        // A hierarchy's levels index its own vertex space, the surface's vertices plus those area preservation grew.
        TVector<FSurfaceSimplifyInput> DagSources(bBuildHierarchy ? NumSurfaces : 0u);

        if (bBuildHierarchy)
        {
            Jobs::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
            {
                LUMINA_PROFILE_SECTION("Build Cluster Hierarchy");
                LUMINA_MEMORY_SCOPE("Meshes");

                const FSurfaceSimplifyInput& Input = SimplifyInputs[SurfaceIdx];
                if (Input.Indices.IsEmpty())
                {
                    return;
                }

                ClusterLOD::FBuildInput In;
                In.Indices           = TSpan<const uint32>(Input.Indices.GetData(), Input.Indices.Num());
                In.Positions         = TSpan<const FVector3>(Input.Positions.GetData(), Input.Positions.Num());
                In.Attributes        = TSpan<const float>(Input.Attributes.GetData(), Input.Attributes.Num());
                In.AttributeWeights  = TSpan<const float>(kSimplifyAttributeWeights, kSimplifyAttributeCount);
                In.AttributeCount    = kSimplifyAttributeCount;
                In.Locks             = TSpan<const uint8>(Input.Locks.GetData(), Input.Locks.Num());
                In.ErrorScale        = Input.ErrorScale;
                In.bConeCulling      = bConeCulling;
                In.bOptimizeMeshlets = bOptimizeMeshlets;

                ClusterLOD::FBuildOutput Dag;
                if (!ClusterLOD::Build(In, Dag))
                {
                    return;
                }

                FSurfaceSimplifyInput& DagSource = DagSources[SurfaceIdx];
                DagSource.bOwnsPositions = true;
                DagSource.ErrorScale     = Input.ErrorScale;
                DagSource.Positions      = Move(Dag.Positions);
                DagSource.SourceVertex.SetNum(Dag.SourceVertex.Num());
                for (int32 v = 0; v < Dag.SourceVertex.Num(); ++v)
                {
                    DagSource.SourceVertex[v] = Input.SourceVertex[Dag.SourceVertex[v]];
                }

                // The hierarchy's levels share the slots, finest first.
                const uint32 DagSlots = Math::Clamp(MAX_MESH_LODS, 1u, Dag.NumLevels);

                FSurfaceLODTable& Table = Tables[SurfaceIdx];
                Table.Flags |= SURFACE_DESC_CLUSTER_LOD;

                for (uint32 Slot = 0; Slot < DagSlots; ++Slot)
                {
                    const uint32 FirstLevel = Slot * Dag.NumLevels / DagSlots;
                    const uint32 EndLevel   = (Slot + 1u) * Dag.NumLevels / DagSlots;
                    const uint32 Begin      = Dag.LevelBegin(FirstLevel);
                    const uint32 End        = Dag.LevelBegin(EndLevel);

                    FSurfaceMeshletResult& Out = Results[Slot * NumSurfaces + SurfaceIdx];
                    Out.Source = &DagSource;

                    float MinSelf   = FLT_MAX;
                    float MaxParent = 0.0f;
                    for (uint32 m = Begin; m < End; ++m)
                    {
                        const FMeshlet&          M      = Dag.Meshlets[m];
                        const FMeshletLODBounds& Bounds = Dag.LODBounds[m];

                        FMeshlet Local = M;
                        Local.VertexOffset   = (uint32)Out.Vertices.Num();
                        Local.TriangleOffset = (uint32)Out.Triangles.Num();
                        Out.Vertices.Append(&Dag.Vertices[M.VertexOffset], M.VertexCount);
                        Out.Triangles.Append(&Dag.Triangles[M.TriangleOffset], M.TriangleCount * 3u);
                        Out.OutMeshlets.Add(Local);
                        Out.Spheres.Add(Dag.Spheres[m]);
                        Out.Cones.Add(Dag.Cones[m]);
                        Out.LODBounds.Add(Bounds);

                        MinSelf   = Math::Min(MinSelf, Bounds.SelfError);
                        MaxParent = Math::Max(MaxParent, Bounds.ParentError);
                    }
                    Out.bHasData = !Out.OutMeshlets.IsEmpty();

                    Table.Add(Slot, MinSelf < FLT_MAX ? MinSelf : 0.0f, MaxParent);
                }
            });

            if (Progress)
            {
                Progress->EnterProgressFrame(MeshletStep * (float)NumSurfaces);
            }
        }

        // Only the source level, for a mesh built without a hierarchy.
        Jobs::ParallelFor(bBuildHierarchy ? 0u : NumSurfaces, [&](uint32 Cell)
        {
            LUMINA_PROFILE_SECTION("Process Surfaces");
            LUMINA_MEMORY_SCOPE("Meshes");

            const uint32 SurfaceIdx = Cell;

            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            if (Section.IndexCount == 0)
            {
                return;
            }

            const uint32*          SurfaceIndices = &MeshResource.Indices[Section.StartIndex];
            FSurfaceMeshletResult& Out            = Results[Cell];

            if (ShouldChunkSurface(Section, bFastMeshletBuild))
            {
                const uint32 NumChunks = (uint32)((Section.IndexCount + kChunkedBuildIndices - 1u) / kChunkedBuildIndices);
                TVector<FSurfaceMeshletResult> Chunks(NumChunks);
                Jobs::ParallelFor(NumChunks, [&](uint32 Chunk)
                {
                    const size_t Begin = (size_t)Chunk * kChunkedBuildIndices;
                    const size_t Count = Math::Min(kChunkedBuildIndices, (size_t)Section.IndexCount - Begin);
                    BuildLODMeshletsForRange(
                        SurfaceIndices + Begin, Count,
                        VertexPositions, NumVertices, PositionStride,
                        bConeCulling, bOptimizeMeshlets, bFastMeshletBuild,
                        ReadPosition, Chunks[Chunk]);
                });
                for (FSurfaceMeshletResult& Chunk : Chunks)
                {
                    AppendMeshletResult(Out, Chunk);
                }
            }
            else
            {
                BuildLODMeshletsForRange(
                    SurfaceIndices, Section.IndexCount,
                    VertexPositions, NumVertices, PositionStride,
                    bConeCulling, bOptimizeMeshlets, bFastMeshletBuild,
                    ReadPosition, Out);
            }
        });

        // The source level alone, which still flags as a hierarchy so the cull treats every mesh alike.
        for (uint32 SurfaceIdx = 0; !bBuildHierarchy && SurfaceIdx < NumSurfaces; ++SurfaceIdx)
        {
            FSurfaceLODTable& Table = Tables[SurfaceIdx];
            if (Results[SurfaceIdx].bHasData)
            {
                Table.Add(0u, 0.0f, kMeshletLODErrorInfinite);
            }
            Table.Flags |= SURFACE_DESC_CLUSTER_LOD;

            if (Progress)
            {
                Progress->EnterProgressFrame(MeshletStep);
            }
        }

        size_t TotalMeshlets  = 0;
        size_t TotalVertices  = 0;
        size_t TotalTriangles = 0;
        for (const FSurfaceMeshletResult& R : Results)
        {
            if (!R.bHasData)
            {
                continue;
            }
            TotalMeshlets += R.OutMeshlets.Num();
            TotalVertices += R.Vertices.Num();
            for (const FMeshlet& M : R.OutMeshlets)
            {
                TotalTriangles += M.TriangleCount;
            }
        }

        MeshResource.MeshletData.Meshlets.Reserve(IntCastChecked<int32>(TotalMeshlets));
        MeshResource.MeshletData.MeshletSpheres.Reserve(IntCastChecked<int32>(TotalMeshlets));
        MeshResource.MeshletData.MeshletCones.Reserve(IntCastChecked<int32>(TotalMeshlets));
        MeshResource.MeshletData.MeshletTriangles.Reserve(IntCastChecked<int32>(TotalTriangles));
        if (MeshResource.bSkinnedMesh)
        {
            MeshResource.MeshletData.MeshletSkinnedVertices.Reserve(IntCastChecked<int32>(TotalVertices));
            MeshResource.MeshletData.MeshletBonePalettes.Reserve(IntCastChecked<int32>(TotalMeshlets));
            MeshResource.MeshletData.MeshletBoneIndices.Reserve(IntCastChecked<int32>(TotalMeshlets * 16));
        }

        for (FGeometrySurface& Section : MeshResource.GeometrySurfaces)
        {
            Section.NumLODs  = 0;
            Section.LODFlags = 0;
            for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
            {
                Section.LODMeshletOffset[i] = 0;
                Section.LODMeshletCount[i]  = 0;
                Section.LODError[i]         = 0.0f;
                Section.LODParentError[i]   = 0.0f;
            }
        }

        auto LevelPosition = [&](const FSurfaceMeshletResult& Result, uint32 Ref) -> FVector3
        {
            if (!Result.UpdatedPositions.IsEmpty())
            {
                return Result.UpdatedPositions[Ref];
            }
            if (Result.Source != nullptr && Result.Source->bOwnsPositions)
            {
                return Result.Source->Positions[Ref];
            }
            return MeshResource.Positions[Result.Source != nullptr ? Result.Source->SourceVertex[Ref] : Ref];
        };

        // The mesh vertex Ref came from still owns every stream the simplifier left alone.
        auto ResolveLevelVertex = [&](const FSurfaceMeshletResult& Result, uint32 Ref) -> FLevelVertex
        {
            FLevelVertex Out;
            Out.MeshVertex = Result.Source != nullptr ? Result.Source->SourceVertex[Ref] : Ref;
            Out.Normal     = MeshResource.Normals[Out.MeshVertex];
            Out.Tangent    = MeshResource.Tangents[Out.MeshVertex];
            Out.UV         = MeshResource.UVs[Out.MeshVertex];

            if (!Result.UpdatedAttributes.IsEmpty())
            {
                const float*   Updated   = &Result.UpdatedAttributes[Ref * kSimplifyAttributeCount];
                const FVector3 RawNormal = FVector3(Updated[0], Updated[1], Updated[2]);
                if (Math::Dot(RawNormal, RawNormal) > 1e-12f)
                {
                    const FVector3 UpdatedNormal = Math::Normalize(RawNormal);
                    Out.Normal = PackNormal(UpdatedNormal);

                    // Squared back up against the moved normal, since the tangent itself was not simplified.
                    const FVector4 SourceTangent = UnpackTangent(Out.Tangent);
                    const FVector3 TangentXYZ    = FVector3(SourceTangent);
                    const FVector3 Orthogonal    = TangentXYZ - UpdatedNormal * Math::Dot(UpdatedNormal, TangentXYZ);
                    if (Math::Dot(Orthogonal, Orthogonal) > 1e-12f)
                    {
                        Out.Tangent = PackTangent(Math::Normalize(Orthogonal), SourceTangent.w);
                    }
                }
                Out.UV = Math::PackHalf2x16(FVector2(Updated[3], Updated[4]));
            }
            return Out;
        };

        if (!MeshResource.bSkinnedMesh)
        {
            PackStaticMeshlets(MeshResource, Results, Tables, NumSurfaces, LevelPosition, ResolveLevelVertex);
        }
        else
        {
            // Area preservation can move a hierarchy vertex past the source, so the grid takes every level's positions.
            TVector<FVector3> GridPositions = MeshResource.Positions;
            for (const FSurfaceSimplifyInput& DagSource : DagSources)
            {
                GridPositions.Append(DagSource.Positions.GetData(), DagSource.Positions.Num());
            }
            MeshResource.MeshletData.PositionGrid = ComputeMeshPositionGrid(GridPositions.GetData(), GridPositions.Num());
        }

        // Surface-major, so a surface's slots form one contiguous run the camera's slice can take.
        for (uint32 SurfaceIdx = 0; MeshResource.bSkinnedMesh && SurfaceIdx < NumSurfaces; ++SurfaceIdx)
        {
            LUMINA_PROFILE_SECTION("Serial Pack Skinned Surfaces");

            const FSurfaceLODTable& Table   = Tables[SurfaceIdx];
            FGeometrySurface&       Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            Section.LODFlags = Table.Flags;

            for (uint32 Slot = 0; Slot < static_cast<uint32>(Table.Accepted.Num()); ++Slot)
            {
                FSurfaceMeshletResult& Result = Results[Table.Accepted[Slot] * NumSurfaces + SurfaceIdx];
                if (!Result.bHasData)
                {
                    continue;
                }

                Section.LODMeshletOffset[Slot] = (uint32)MeshResource.MeshletData.Meshlets.Num();
                Section.LODMeshletCount[Slot]  = (uint32)Result.OutMeshlets.Num();
                Section.LODError[Slot]         = Table.Error[Slot];
                Section.LODParentError[Slot]   = Table.ParentError[Slot];
                Section.NumLODs                = Slot + 1u;

                FMeshletPaletteScratch Palette;

                for (int32 MeshletIdx = 0; MeshletIdx < Result.OutMeshlets.Num(); ++MeshletIdx)
                {
                    FMeshlet Out = Result.OutMeshlets[MeshletIdx];

                    Out.LODIndex = Slot;

                    const uint32 PackedVertexStart = (uint32)MeshResource.MeshletData.MeshletSkinnedVertices.Num();

                    Palette.Reset();

                    for (uint32 i = 0; i < Out.VertexCount; ++i)
                    {
                        const uint32       Ref       = Result.Vertices[Out.VertexOffset + i];
                        const FLevelVertex Level     = ResolveLevelVertex(Result, Ref);
                        const uint32       GlobalIdx = Level.MeshVertex;

                        FMeshletSkinnedVertex Packed;
                        Packed.Position = EncodeMeshPosition(MeshResource.MeshletData.PositionGrid, LevelPosition(Result, Ref));
                        Packed.Normal  = Level.Normal;
                        Packed.Tangent = Level.Tangent;
                        Packed.UV      = Level.UV;
                        Packed.UV1     = MeshResource.UVs1[GlobalIdx];
                        Packed.Color   = MeshResource.Colors[GlobalIdx];

                        // The source index is 16-bit and the packed one 8-bit, so it can only be a palette slot.
                        const FU16Vector4& SourceJoints  = MeshResource.JointIndices[GlobalIdx];
                        const FU8Vector4&  SourceWeights = MeshResource.JointWeights[GlobalIdx];

                        uint32 LocalJoints = 0;
                        for (uint32 b = 0; b < 4u; ++b)
                        {
                            // A zero-weight influence contributes nothing, so it costs no palette entry.
                            const uint32 PaletteSlot = (SourceWeights[b] != 0)
                                ? FindOrAddPaletteBone(Palette, SourceJoints[b])
                                : 0u;
                            LocalJoints |= PaletteSlot << (b * 8u);
                        }

                        Packed.JointIndices = LocalJoints;
                        memcpy(&Packed.JointWeights, &MeshResource.JointWeights[GlobalIdx], sizeof(uint32));
                        MeshResource.MeshletData.MeshletSkinnedVertices.Add(Packed);
                    }

                    // Indexed in lockstep with Meshlets, which Out is pushed into at the end of this body.
                    AppendMeshletBonePalette(MeshResource.MeshletData, Palette);

                    const uint32 PackedDwordStart = (uint32)MeshResource.MeshletData.MeshletTriangles.Num();
                    const uint8* TriSrc           = Result.Triangles.GetData() + Out.TriangleOffset;
                    for (uint32 t = 0; t < Out.TriangleCount; ++t)
                    {
                        const uint32 Packed =
                              (uint32)TriSrc[t * 3 + 0]
                            | ((uint32)TriSrc[t * 3 + 1] << 8)
                            | ((uint32)TriSrc[t * 3 + 2] << 16);
                        MeshResource.MeshletData.MeshletTriangles.Add(Packed);
                    }

                    Out.VertexOffset   = PackedVertexStart;
                    Out.TriangleOffset = PackedDwordStart;
                    MeshResource.MeshletData.Meshlets.Add(Out);
                    MeshResource.MeshletData.MeshletSpheres.Add(Result.Spheres[MeshletIdx]);
                    MeshResource.MeshletData.MeshletCones.Add(Result.Cones[MeshletIdx]);

                    MeshResource.MeshletData.MeshletLODBounds.Add(GetPackedLODBounds(Result, MeshletIdx));
                }
            }
        }

        if (MeshResource.MeshletData.MeshletTriangles.IsEmpty())
        {
            MeshResource.MeshletData.MeshletTriangles.Add(0u);
        }
    }

    void AnalyzeMeshStatistics(FMeshResource& MeshResource, FMeshStatistics& OutMeshStats)
    {
        OutMeshStats.VertexFetchStatics.Emplace(meshopt_analyzeVertexFetch(MeshResource.Indices.GetData(), MeshResource.Indices.Num(), MeshResource.GetNumVertices(), MeshResource.GetVertexTypeSize()));
        OutMeshStats.OverdrawStatics.Emplace(meshopt_analyzeOverdraw(MeshResource.Indices.GetData(), MeshResource.Indices.Num(), reinterpret_cast<const float*>(MeshResource.Positions.GetData()), MeshResource.GetNumVertices(), sizeof(FVector3)));
    }

    int16 FindOrAddMergedMaterialSlot(TVector<int16>& SlotToSource, int16 SourceMaterial)
    {
        int32 Slot = SlotToSource.Find(SourceMaterial);
        if (Slot == Constants::kIndexNone)
        {
            Slot = SlotToSource.Num();
            SlotToSource.Add(SourceMaterial);
        }
        return (int16)Slot;
    }

    namespace
    {
        // Coalesce surfaces by MaterialIndex; rebuilds the index buffer to make per-material slices contiguous.
        void MergeSurfacesByMaterial(FMeshResource& MeshResource)
        {
            const TVector<FGeometrySurface>& OldSurfaces = MeshResource.GeometrySurfaces;
            if (OldSurfaces.Num() <= 1 || MeshResource.Indices.IsEmpty())
            {
                return;
            }

            // First-seen MaterialIndex order; stable for downstream slot matching.
            THashMap<int16, uint32> MaterialToNewIdx;
            TVector<int16>          MaterialOrder;
            MaterialOrder.Reserve(OldSurfaces.Num());

            for (const FGeometrySurface& S : OldSurfaces)
            {
                if (!MaterialToNewIdx.Contains(S.MaterialIndex))
                {
                    MaterialToNewIdx.TryAdd(S.MaterialIndex, (uint32)MaterialOrder.Num());
                    MaterialOrder.Add(S.MaterialIndex);
                }
            }

            if (MaterialOrder.Num() == OldSurfaces.Num())
            {
                return;
            }

            const uint32 NumMerged = (uint32)MaterialOrder.Num();

            TVector<uint32> CountPerMerged(NumMerged, 0u);
            for (const FGeometrySurface& S : OldSurfaces)
            {
                CountPerMerged[MaterialToNewIdx[S.MaterialIndex]] += S.IndexCount;
            }

            TVector<uint32> StartPerMerged(NumMerged, 0u);
            uint32 Running = 0;
            for (uint32 i = 0; i < NumMerged; ++i)
            {
                StartPerMerged[i] = Running;
                Running += CountPerMerged[i];
            }

            TVector<uint32> NewIndices(MeshResource.Indices.Num());
            TVector<uint32> WriteCursor = StartPerMerged;
            for (const FGeometrySurface& S : OldSurfaces)
            {
                if (S.IndexCount == 0)
                {
                    continue;
                }
                const uint32 NewIdx = MaterialToNewIdx[S.MaterialIndex];
                memcpy(NewIndices.GetData() + WriteCursor[NewIdx],
                       MeshResource.Indices.GetData() + S.StartIndex,
                       S.IndexCount * sizeof(uint32));
                WriteCursor[NewIdx] += S.IndexCount;
            }

            // Inherit first source surface's ID so re-imports keep stable surface identity.
            TVector<FGeometrySurface> NewSurfaces;
            NewSurfaces.Reserve(NumMerged);
            for (uint32 i = 0; i < NumMerged; ++i)
            {
                FGeometrySurface NewSurface;
                for (const FGeometrySurface& Old : OldSurfaces)
                {
                    if (Old.MaterialIndex == MaterialOrder[i])
                    {
                        NewSurface.ID = Old.ID;
                        break;
                    }
                }
                NewSurface.IndexCount    = CountPerMerged[i];
                NewSurface.StartIndex    = StartPerMerged[i];
                NewSurface.MaterialIndex = MaterialOrder[i];
                NewSurfaces.Add(NewSurface);
            }

            MeshResource.Indices          = Move(NewIndices);
            MeshResource.GeometrySurfaces = Move(NewSurfaces);
        }

        // Concatenate Src into Dst with index/material rebase; bakes Src.ImportTransform into positions/normals.
        void MergeResourceInto(FMeshResource& Src, FMeshResource& Dst, TVector<int16>& SlotToSource)
        {
            const uint32 BaseVert = (uint32)Dst.GetNumVertices();
            const uint32 BaseIdx  = (uint32)Dst.Indices.Num();

            const FMatrix4 PosMatrix    = Src.ImportTransform;
            const FMatrix3 NormalMatrix = Math::Transpose(Math::Inverse(FMatrix3(PosMatrix)));
            const bool bIdentity         = PosMatrix == FMatrix4(1.0f);

            // Append every active stream; joint streams only when both sides are skinned.
            const int32 Start    = Dst.GetNumVertices();
            const int32 SrcCount = Src.GetNumVertices();

            Dst.Positions.Append(Src.Positions);
            Dst.Normals.Append(Src.Normals);
            Dst.Tangents.Append(Src.Tangents);
            Dst.UVs.Append(Src.UVs);
            Dst.UVs1.Append(Src.UVs1);
            Dst.Colors.Append(Src.Colors);
            if (Dst.bSkinnedMesh && Src.bSkinnedMesh)
            {
                Dst.JointIndices.Append(Src.JointIndices);
                Dst.JointWeights.Append(Src.JointWeights);
            }

            // Bake the source scene-graph transform into the appended positions/normals.
            if (!bIdentity)
            {
                TVector<FVector3> BakedNormals(SrcCount);
                VertexOps::UnpackNormals(Dst.Normals.GetData() + Start, BakedNormals.GetData(), SrcCount);
                for (int32 i = 0; i < SrcCount; ++i)
                {
                    Dst.Positions[Start + i] = FVector3(PosMatrix * FVector4(Dst.Positions[Start + i], 1.0f));
                    BakedNormals[i]          = Math::Normalize(NormalMatrix * BakedNormals[i]);
                }
                VertexOps::PackNormals(BakedNormals.GetData(), Dst.Normals.GetData() + Start, SrcCount);
            }

            Dst.Indices.Reserve(Dst.Indices.Num() + Src.Indices.Num());
            for (uint32 Idx : Src.Indices)
            {
                Dst.Indices.Add(Idx + BaseVert);
            }

            Dst.GeometrySurfaces.Reserve(Dst.GeometrySurfaces.Num() + Src.GeometrySurfaces.Num());
            for (FGeometrySurface S : Src.GeometrySurfaces)
            {
                S.StartIndex += BaseIdx;
                if (S.MaterialIndex >= 0)
                {
                    S.MaterialIndex = FindOrAddMergedMaterialSlot(SlotToSource, S.MaterialIndex);
                }
                Dst.GeometrySurfaces.Add(S);
            }
        }
    }

    void FinalizeMeshImportData(FMeshImportData& Data, const FMeshImportOptions& Options, FScopedSlowTask* Progress, float ProgressBudget)
    {
        const float Scale          = Options.Scale;
        const bool  bScaleEnabled  = (Scale != 1.0f);
        const bool  bFlipUVs       = Options.bFlipUVs;
        const bool  bFlipU         = Options.bFlipU;
        const bool  bFlipNormals   = Options.bFlipNormals;

        // Per-mesh transforms (each resource owns its own vertex buffer).
        if (bScaleEnabled || bFlipUVs || bFlipU || bFlipNormals)
        {
            Jobs::ParallelFor((uint32)Data.Resources.Num(), [&](uint32 ResIdx)
            {
                TUniquePtr<FMeshResource>& MeshPtr = Data.Resources[ResIdx];
                if (!MeshPtr)
                {
                    return;
                }

                FMeshResource& M = *MeshPtr;
                const int32 NumVerts = M.GetNumVertices();

                for (int32 i = 0; i < NumVerts; ++i)
                {
                    if (bScaleEnabled)
                    {
                        M.Positions[i] *= Scale;
                    }
                    if (bFlipUVs || bFlipU)
                    {
                        auto Flip = [&](FVector2 UV)
                        {
                            if (bFlipUVs) { UV.y = 1.0f - UV.y; }
                            if (bFlipU)   { UV.x = 1.0f - UV.x; }
                            return UV;
                        };
                        // A flip is a property of the source's UV convention, so applying it to one set would tear.
                        M.SetUVAt(i, Flip(M.GetUVAt(i)));
                        M.SetUV1At(i, Flip(M.GetUV1At(i)));
                    }
                    if (bFlipNormals)
                    {
                        M.Normals[i] = PackNormal(-UnpackNormal(M.Normals[i]));
                    }
                }
            });
        }

        // Rotation is untouched, since a uniform scale commutes with the transpose.
        if (bScaleEnabled)
        {
            for (TUniquePtr<FSkeletonResource>& SkelPtr : Data.Skeletons)
            {
                if (!SkelPtr)
                {
                    continue;
                }
                for (FSkeletonResource::FBoneInfo& B : SkelPtr->Bones)
                {
                    B.LocalTransform[3][0] *= Scale;
                    B.LocalTransform[3][1] *= Scale;
                    B.LocalTransform[3][2] *= Scale;
                    B.InvBindMatrix[3][0]  *= Scale;
                    B.InvBindMatrix[3][1]  *= Scale;
                    B.InvBindMatrix[3][2]  *= Scale;
                }
            }
        }

        // Animation translation channels (rotation/scale are unitless).
        if (bScaleEnabled)
        {
            for (TUniquePtr<FAnimationResource>& AnimPtr : Data.Animations)
            {
                if (!AnimPtr)
                {
                    continue;
                }
                for (FAnimationChannel& Ch : AnimPtr->Channels)
                {
                    if (Ch.TargetPath == FAnimationChannel::ETargetPath::Translation)
                    {
                        for (FVector3& T : Ch.Translations)
                        {
                            T *= Scale;
                        }
                    }
                }
            }
        }

        // Merged before optimize and meshlets, so the heavy passes run on the merged geometry.
        if (Options.bMergeMeshes && Data.Resources.Num() > 1)
        {
            TUniquePtr<FMeshResource> MergedStatic = MakeUnique<FMeshResource>();

            TUniquePtr<FMeshResource> MergedSkinned = MakeUnique<FMeshResource>();
            MergedSkinned->bSkinnedMesh = true;

            // Inherit name from first matching source.
            for (const TUniquePtr<FMeshResource>& Res : Data.Resources)
            {
                if (!Res) continue;
                if (!Res->bSkinnedMesh && MergedStatic->Name.IsNone())
                {
                    MergedStatic->Name = Res->Name;
                }
                if (Res->bSkinnedMesh && MergedSkinned->Name.IsNone())
                {
                    MergedSkinned->Name = Res->Name;
                }
            }

            // Shared by both halves, since two lists would give slot 0 a different meaning in each.
            TVector<int16> SlotToSource;

            for (TUniquePtr<FMeshResource>& Res : Data.Resources)
            {
                if (!Res)
                {
                    continue;
                }
                if (Res->bSkinnedMesh)
                {
                    MergeResourceInto(*Res, *MergedSkinned, SlotToSource);
                }
                else
                {
                    MergeResourceInto(*Res, *MergedStatic, SlotToSource);
                }
            }

            // Without it the importer falls back to treating the slot as the source index.
            Data.MergedMaterialSlotToSource = Move(SlotToSource);

            TVector<TUniquePtr<FMeshResource>> NewResources;
            if (MergedStatic->GetNumVertices() > 0)
            {
                NewResources.Add(Move(MergedStatic));
            }
            if (MergedSkinned->GetNumVertices() > 0)
            {
                NewResources.Add(Move(MergedSkinned));
            }
            Data.Resources = Move(NewResources);
        }

        // Stats run serially afterwards, so the parallel pass writes only resource-local data.
        Data.MeshStatistics.OverdrawStatics.Reset();
        Data.MeshStatistics.VertexFetchStatics.Reset();

        if (Progress)
        {
            Progress->UpdateMessage("Merging surfaces...");
        }
        Jobs::ParallelFor((uint32)Data.Resources.Num(), [&](uint32 ResIdx)
        {
            if (Data.Resources[ResIdx])
            {
                MergeSurfacesByMaterial(*Data.Resources[ResIdx]);
            }
        });

        // So the bar moves smoothly even when a merge collapses everything to one resource.
        size_t TotalSurfaces = 0;
        for (const TUniquePtr<FMeshResource>& MeshPtr : Data.Resources)
        {
            if (MeshPtr)
            {
                TotalSurfaces += MeshPtr->GeometrySurfaces.Num();
            }
        }
        const float StepPerSurface = ProgressBudget / (float)Math::Max<size_t>(1, TotalSurfaces);

        if (Progress)
        {
            Progress->UpdateMessage("Optimizing geometry...");
        }

        Jobs::ParallelFor((uint32)Data.Resources.Num(), [&](uint32 ResIdx)
        {
            TUniquePtr<FMeshResource>& MeshPtr = Data.Resources[ResIdx];
            if (!MeshPtr)
            {
                return;
            }
            FMeshResource& M = *MeshPtr;
            // Dedup ALWAYS for correctness, and only the reordering is the optional optimize pass.
            DeduplicateMeshVertices(M, Progress);
            if (Options.bOptimize)
            {
                OptimizeNewlyImportedMesh(M, Progress);
            }
            // It runs tangent generation internally and advances progress per surface it meshletizes.
            GenerateMeshlets(M, Progress, StepPerSurface);
        });

        for (TUniquePtr<FMeshResource>& MeshPtr : Data.Resources)
        {
            if (!MeshPtr)
            {
                continue;
            }
            AnalyzeMeshStatistics(*MeshPtr, Data.MeshStatistics);
        }

        // The builder parallelizes over its own slices, so nesting it here would nest two levels.
        if (Options.DistanceField.bEnabled)
        {
            if (Progress)
            {
                Progress->UpdateMessage("Building distance fields...");
            }

            for (TUniquePtr<FMeshResource>& MeshPtr : Data.Resources)
            {
                if (MeshPtr)
                {
                    DistanceField::Build(*MeshPtr, Options.DistanceField, MeshPtr->DistanceField, Progress);
                }
            }
        }
    }
}
