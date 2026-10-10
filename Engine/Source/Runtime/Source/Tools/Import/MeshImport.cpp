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
            OutStreams[Count++] = { M.Positions.data(), sizeof(FVector3),   sizeof(FVector3) };
            OutStreams[Count++] = { M.Normals.data(),   sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.Tangents.data(),  sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.UVs.data(),       sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.UVs1.data(),      sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.Colors.data(),    sizeof(uint32),      sizeof(uint32) };
            if (M.bSkinnedMesh)
            {
                OutStreams[Count++] = { M.JointIndices.data(), sizeof(FU16Vector4), sizeof(FU16Vector4) };
                OutStreams[Count++] = { M.JointWeights.data(), sizeof(FU8Vector4), sizeof(FU8Vector4) };
            }
            LUMINA_ASSERT(Count <= kMaxVertexStreams, "BuildVertexStreams overflowed the caller's array");
            return Count;
        }

        // Apply a meshopt vertex remap to every active stream into fresh buffers (no overlap).
        void RemapVertexStreams(FMeshResource& M, const uint32* Remap, size_t OldCount, size_t NewCount)
        {
            auto RemapStream = [&](auto& Stream)
            {
                using TElem = typename std::remove_reference_t<decltype(Stream)>::value_type;
                TVector<TElem> Out(NewCount);
                meshopt_remapVertexBuffer(Out.data(), Stream.data(), OldCount, sizeof(TElem), Remap);
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
        const size_t NumVertices = MeshResource.GetNumVertices();
        if (MeshResource.Indices.empty() || NumVertices == 0)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        // meshopt wants unit float3 normals and float2 UVs; both streams live packed.
        if (MeshResource.Normals.size() != NumVertices || MeshResource.UVs.size() != NumVertices)
        {
            return;
        }

        MeshResource.Tangents.resize(NumVertices);

        TVector<FVector3> UnpackedNormals(NumVertices);
        TVector<FVector2> UnpackedUVs(NumVertices);
        {
            LUMINA_PROFILE_SECTION("Unpack Tangent Inputs");
            const uint32* InNormals = MeshResource.Normals.data();
            const uint32* InUVs     = MeshResource.UVs.data();
            FVector3*     OutNormals = UnpackedNormals.data();
            FVector2*     OutUVs     = UnpackedUVs.data();

            Task::ParallelFor((uint32)NumVertices, [=](const Task::FParallelRange& Range)
            {
                const size_t Count = Range.End - Range.Start;
                VertexOps::UnpackNormals(InNormals + Range.Start, OutNormals + Range.Start, Count);
                VertexOps::UnpackHalf2x16s(InUVs + Range.Start, OutUVs + Range.Start, Count);
            }, 4096);
        }

        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.size();
        Task::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
        {
            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            if (Section.IndexCount >= 3)
            {
                const uint32* SurfaceIndices = &MeshResource.Indices[Section.StartIndex];

                TVector<float> CornerTangents((size_t)Section.IndexCount * 4u);
                {
                    LUMINA_PROFILE_SECTION("meshopt_generateTangents");
                    meshopt_generateTangents(
                        CornerTangents.data(),
                        SurfaceIndices, Section.IndexCount,
                        &MeshResource.Positions[0].x, NumVertices, sizeof(FVector3),
                        &UnpackedNormals[0].x, sizeof(FVector3),
                        &UnpackedUVs[0].x, sizeof(FVector2),
                        meshopt_TangentCompatible);
                }

                TVector<uint32> CornerPacked(Section.IndexCount);
                VertexOps::PackTangents(CornerTangents.data(), CornerPacked.data(), Section.IndexCount);

                // Per-corner collapsed to per-vertex last-writer-wins, as the previous per-corner writer did.
                for (uint32 i = 0; i < Section.IndexCount; ++i)
                {
                    MeshResource.Tangents[SurfaceIndices[i]] = CornerPacked[i];
                }
            }

            if (Progress)
            {
                Progress->EnterProgressFrame(StepPerSurface);
            }
        });
    }
    
    void GenerateFallbackTangents(FMeshResource& MeshResource)
    {
        LUMINA_PROFILE_SCOPE();

        const size_t NumVertices = MeshResource.GetNumVertices();
        if (NumVertices == 0 || MeshResource.Normals.size() != NumVertices)
        {
            return;
        }

        MeshResource.Tangents.resize(NumVertices);

        const uint32* Normals  = MeshResource.Normals.data();
        uint32*       Tangents = MeshResource.Tangents.data();

        constexpr uint32 kBatch = 256;
        Task::ParallelFor((uint32)NumVertices, [=](const Task::FParallelRange& Range)
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

        const size_t NumVertices = MeshResource.GetNumVertices();
        const size_t NumIndices  = MeshResource.Indices.size();
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
        const size_t UniqueVerts = meshopt_generateVertexRemapMulti(
            Remap.data(),
            MeshResource.Indices.data(), NumIndices,
            NumVertices, Streams, StreamCount);

        if (UniqueVerts < NumVertices)
        {
            meshopt_remapIndexBuffer(
                MeshResource.Indices.data(),
                MeshResource.Indices.data(), NumIndices,
                Remap.data());

            RemapVertexStreams(MeshResource, Remap.data(), NumVertices, UniqueVerts);
        }
    }

    // Assumes vertices were already deduplicated, which the pass above guarantees.
    void OptimizeNewlyImportedMesh(FMeshResource& MeshResource, FScopedSlowTask* Progress)
    {
        LUMINA_PROFILE_SCOPE();

        const size_t NumVertices = MeshResource.GetNumVertices();
        const size_t NumIndices  = MeshResource.Indices.size();

        if (NumVertices == 0 || NumIndices == 0)
        {
            return;
        }

        // Per-surface reorder; disjoint index slices make the in-place reorder thread-safe.
        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.size();
        if (NumSurfaces > 0)
        {
            if (Progress)
            {
                Progress->UpdateMessage("Optimizing vertex cache & overdraw...");
            }
            const float* VertexPositions = reinterpret_cast<const float*>(MeshResource.Positions.data());

            Task::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
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
            const size_t NewCount = meshopt_optimizeVertexFetchRemap(
                FetchRemap.data(),
                MeshResource.Indices.data(), NumIndices,
                NumVertices);

            meshopt_remapIndexBuffer(
                MeshResource.Indices.data(),
                MeshResource.Indices.data(), NumIndices,
                FetchRemap.data());

            RemapVertexStreams(MeshResource, FetchRemap.data(), NumVertices, NewCount);
        }
    }

    namespace
    {
        // Level i aims at this share of the surface's triangles raised to the power i.
        constexpr float  kLODTriangleRatio    = 0.5f;
        // A level keeping more than this share of the previous level's triangles is not worth a switch.
        constexpr float  kLODMinReduction     = 0.85f;
        // Selection reads the measured error, so this only bounds how far a level may stray.
        constexpr float  kLODMaxRelativeError = 1.0f;
        // ~5 triangles minimum; below this we drop the LOD and fall back to coarser.
        constexpr size_t kLODMinIndices       = 15u;

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

            const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.size();
            const size_t NumVertices = MeshResource.GetNumVertices();
            const bool   bHasNormals = MeshResource.Normals.size() == NumVertices;
            const bool   bHasUVs     = MeshResource.UVs.size() == NumVertices;

            TVector<FSurfaceSimplifyInput> Inputs(NumSurfaces);
            Task::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
            {
                const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
                FSurfaceSimplifyInput&  In      = Inputs[SurfaceIdx];
                if (Section.IndexCount < 3)
                {
                    return;
                }

                const uint32* SurfaceIndices = &MeshResource.Indices[Section.StartIndex];
                In.SourceVertex.assign(SurfaceIndices, SurfaceIndices + Section.IndexCount);
                Algo::Sort(In.SourceVertex);
                In.SourceVertex.erase(Algo::Unique(In.SourceVertex), In.SourceVertex.end());

                In.Indices.resize(Section.IndexCount);
                for (uint32 i = 0; i < Section.IndexCount; ++i)
                {
                    const auto Found = Algo::LowerBound(In.SourceVertex.begin(), In.SourceVertex.end(), SurfaceIndices[i]);
                    In.Indices[i] = (uint32)(Found - In.SourceVertex.begin());
                }

                const size_t LocalCount = In.SourceVertex.size();
                In.Positions.resize(LocalCount);
                In.Attributes.assign(LocalCount * kSimplifyAttributeCount, 0.0f);
                for (size_t v = 0; v < LocalCount; ++v)
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
                for (const FVector3& Position : Inputs[SurfaceIdx].Positions)
                {
                    const auto [It, bInserted] = PositionOwner.try_emplace(PositionKey(Position), SurfaceIdx);
                    if (!bInserted && It->second != SurfaceIdx)
                    {
                        It->second = kSharedPosition;
                    }
                }
            }

            Task::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
            {
                FSurfaceSimplifyInput& In = Inputs[SurfaceIdx];
                In.Locks.assign(In.Positions.size(), (uint8)0u);
                for (size_t v = 0; v < In.Positions.size(); ++v)
                {
                    if (PositionOwner.find(PositionKey(In.Positions[v]))->second == kSharedPosition)
                    {
                        In.Locks[v] = meshopt_SimplifyVertex_Lock;
                    }
                }
            });

            return Inputs;
        }

        // Simplifies from the whole surface rather than the level above, so every error is measured against the source.
        void SimplifySurfaceLOD(const FSurfaceSimplifyInput& In, size_t TargetIndices, bool bDestructive,
                                TVector<uint32>& OutIndices, FSurfaceMeshletResult& Out)
        {
            LUMINA_PROFILE_SCOPE();

            const size_t SourceCount     = In.Indices.size();
            const size_t VertexCount     = In.Positions.size();
            const size_t AttributeStride = kSimplifyAttributeCount * sizeof(float);
            const uint8* Locks           = In.Locks.empty() ? nullptr : In.Locks.data();

            float  RelativeError = 0.0f;
            size_t NewCount      = 0;

            if (bDestructive)
            {
                Out.UpdatedPositions  = In.Positions;
                Out.UpdatedAttributes = In.Attributes;
                OutIndices            = In.Indices;
                NewCount = meshopt_simplifyWithUpdate(
                    OutIndices.data(), SourceCount,
                    &Out.UpdatedPositions[0].x, VertexCount, sizeof(FVector3),
                    Out.UpdatedAttributes.data(), AttributeStride,
                    kSimplifyAttributeWeights, kSimplifyAttributeCount,
                    Locks, TargetIndices, kLODMaxRelativeError, 0u, &RelativeError);
            }
            else
            {
                OutIndices.resize(SourceCount);
                NewCount = meshopt_simplifyWithAttributes(
                    OutIndices.data(), In.Indices.data(), SourceCount,
                    &In.Positions[0].x, VertexCount, sizeof(FVector3),
                    In.Attributes.data(), AttributeStride,
                    kSimplifyAttributeWeights, kSimplifyAttributeCount,
                    Locks, TargetIndices, kLODMaxRelativeError, 0u, &RelativeError);
            }

            // Leaf cards are separate quads whose every edge is a border, so edge collapse cannot touch them and only clustering reduces them.
            if (NewCount > TargetIndices * 2u)
            {
                Out.UpdatedPositions.clear();
                Out.UpdatedAttributes.clear();
                OutIndices.resize(SourceCount);
                NewCount = meshopt_simplifySloppy(
                    OutIndices.data(), In.Indices.data(), SourceCount,
                    &In.Positions[0].x, VertexCount, sizeof(FVector3),
                    Locks, TargetIndices, kLODMaxRelativeError, &RelativeError);
            }

            OutIndices.resize(NewCount);
            Out.Error = RelativeError * In.ErrorScale;
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
                LocalMeshlets.resize(MaxMeshlets);
                Result.Vertices.resize(MaxMeshlets * MaxVertices);
                Result.Triangles.resize(MaxMeshlets * MaxTriangles * 3);
            }

            size_t MeshletCount = 0;
            if (bFastBuild)
            {
                LUMINA_PROFILE_SECTION("meshopt_buildMeshletsScan");
                MeshletCount = meshopt_buildMeshletsScan(
                    LocalMeshlets.data(),
                    Result.Vertices.data(),
                    Result.Triangles.data(),
                    SrcIndices, SrcIndexCount,
                    NumVertices,
                    MaxVertices, MaxTriangles);
            }
            else
            {
                LUMINA_PROFILE_SECTION("meshopt_buildMeshlets");
                MeshletCount = meshopt_buildMeshlets(
                    LocalMeshlets.data(),
                    Result.Vertices.data(),
                    Result.Triangles.data(),
                    SrcIndices, SrcIndexCount,
                    VertexPositions, NumVertices, VertexSize,
                    MaxVertices, MaxTriangles, ConeWeight);
            }

            if (MeshletCount == 0)
            {
                Result.Vertices.clear();
                Result.Triangles.clear();
                return;
            }

            LocalMeshlets.resize(MeshletCount);

            // meshopt pads triangles to a multiple of 4.
            const meshopt_Meshlet& Last = LocalMeshlets.back();
            Result.Vertices.resize(Last.vertex_offset + Last.vertex_count);
            Result.Triangles.resize(Last.triangle_offset + ((Last.triangle_count * 3 + 3) & ~3u));

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

            Result.OutMeshlets.reserve(MeshletCount);
            Result.Spheres.reserve(MeshletCount);
            Result.Cones.reserve(MeshletCount);
            
            {
            LUMINA_PROFILE_SECTION("Meshlet Bounds");
            for (const meshopt_Meshlet& M : LocalMeshlets)
            {
                FMeshlet Out{};
                Out.VertexOffset   = M.vertex_offset;
                Out.TriangleOffset = M.triangle_offset;
                Out.VertexCount    = M.vertex_count;
                Out.TriangleCount  = M.triangle_count;
                Result.OutMeshlets.push_back(Out);

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
                Result.Spheres.push_back(Sphere);
                Result.Cones.push_back(Cone);
            }
            }

            Result.bHasData = !Result.OutMeshlets.empty();
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

            const uint32 VertexShift   = (uint32)Into.Vertices.size();
            const uint32 TriangleShift = (uint32)Into.Triangles.size();
            Into.Vertices.insert(Into.Vertices.end(), From.Vertices.begin(), From.Vertices.end());
            Into.Triangles.insert(Into.Triangles.end(), From.Triangles.begin(), From.Triangles.end());
            for (FMeshlet M : From.OutMeshlets)
            {
                M.VertexOffset   += VertexShift;
                M.TriangleOffset += TriangleShift;
                Into.OutMeshlets.push_back(M);
            }
            Into.Spheres.insert(Into.Spheres.end(), From.Spheres.begin(), From.Spheres.end());
            Into.Cones.insert(Into.Cones.end(), From.Cones.begin(), From.Cones.end());
            Into.bHasData  = true;
        }

        // MUST run before the scratch streams are dropped, since they are never serialized.
        void ComputeSurfaceTexelFactors(FMeshResource& MeshResource)
        {
            LUMINA_PROFILE_SCOPE();

            const size_t NumIndices  = MeshResource.Indices.size();
            const size_t NumVertices = MeshResource.Positions.size();

            // Leaving it at zero is the honest answer, and consumers fall back rather than act on it.
            if (MeshResource.UVs.size() < NumVertices || NumVertices == 0 || NumIndices < 3)
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

                const size_t Start = Surface.StartIndex;
                const size_t End   = Math::Min<size_t>(Start + Surface.IndexCount, NumIndices);
                if (End < Start + 3)
                {
                    continue;
                }

                // A large mesh in centimeters can sum world areas past float precision before running out.
                double WorldArea = 0.0;
                double UVArea    = 0.0;

                for (size_t i = Start; i + 2 < End; i += 3)
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
                                const TVector<TFixedVector<uint32, MAX_MESH_LODS>>& AcceptedPerSurface,
                                const TVector<TFixedVector<float, MAX_MESH_LODS>>& AcceptedErrorPerSurface,
                                uint32 LODCount, uint32 NumSurfaces,
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

            for (uint32 Slot = 0; Slot < LODCount; ++Slot)
            {
                for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
                {
                    const TFixedVector<uint32, MAX_MESH_LODS>& Accepted = AcceptedPerSurface[SurfaceIdx];
                    if (Slot >= Accepted.size())
                    {
                        continue;
                    }

                    FGeometrySurface&            Section = MeshResource.GeometrySurfaces[SurfaceIdx];
                    const FSurfaceMeshletResult& Result  = Results[Accepted[Slot] * NumSurfaces + SurfaceIdx];
                    if (!Result.bHasData)
                    {
                        continue;
                    }

                    Section.LODMeshletOffset[Slot] = (uint32)PackSlots.size();
                    Section.LODMeshletCount[Slot]  = (uint32)Result.OutMeshlets.size();
                    Section.LODError[Slot]         = AcceptedErrorPerSurface[SurfaceIdx][Slot];
                    Section.NumLODs                = Slot + 1u;

                    for (size_t MeshletIdx = 0; MeshletIdx < Result.OutMeshlets.size(); ++MeshletIdx)
                    {
                        const FMeshlet& M = Result.OutMeshlets[MeshletIdx];
                        PackSlots.push_back({ &Result, (uint32)MeshletIdx, Slot, RefCursor, TriangleCursor });
                        RefCursor      += M.VertexCount;
                        TriangleCursor += M.TriangleCount;
                    }
                }
            }

            // A level that kept the source vertices shares their slots, and a destructive level moved its vertices and gets its own.
            TVector<uint32>        UniqueOf(RefCursor);
            TVector<FUniqueSource> UniqueSources;
            {
                LUMINA_PROFILE_SECTION("Assign Shared Vertices");
                TVector<uint32> MeshSlot(MeshResource.GetNumVertices(), ~0u);
                THashMap<const FSurfaceMeshletResult*, TVector<uint32>> MovedSlots;

                for (const FPackSlot& Pack : PackSlots)
                {
                    const FSurfaceMeshletResult& Result = *Pack.Result;
                    const FMeshlet&              M      = Result.OutMeshlets[Pack.MeshletIdx];
                    const bool                   bMoved = !Result.UpdatedPositions.empty();

                    TVector<uint32>* LevelSlots = nullptr;
                    if (bMoved)
                    {
                        LevelSlots = &MovedSlots[&Result];
                        if (LevelSlots->empty())
                        {
                            LevelSlots->assign(Result.UpdatedPositions.size(), ~0u);
                        }
                    }

                    for (uint32 i = 0; i < M.VertexCount; ++i)
                    {
                        const uint32 Ref  = Result.Vertices[M.VertexOffset + i];
                        uint32&      Slot = bMoved ? (*LevelSlots)[Ref]
                                                   : MeshSlot[Result.Source != nullptr ? Result.Source->SourceVertex[Ref] : Ref];
                        if (Slot == ~0u)
                        {
                            Slot = (uint32)UniqueSources.size();
                            UniqueSources.push_back({ &Result, Ref });
                        }
                        UniqueOf[Pack.RefStart + i] = Slot;
                    }
                }
            }

            const uint32  UniqueCount = (uint32)UniqueSources.size();
            FMeshletData& Data        = MeshResource.MeshletData;

            TVector<FVector3> Positions(UniqueCount);
            TVector<uint32>   UV1s(UniqueCount);
            TVector<uint32>   Colors(UniqueCount);
            Data.VertexAttributes.resize(UniqueCount);
            {
                LUMINA_PROFILE_SECTION("Resolve Shared Vertices");
                Task::ParallelFor(UniqueCount, [&](const Task::FParallelRange& Range)
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

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.data(), Positions.size());
            Data.VertexPositions.resize(UniqueCount);
            Task::ParallelFor(UniqueCount, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Data.VertexPositions[i] = EncodeMeshPosition(Data.PositionGrid, Positions[i]);
                }
            }, 4096);

            Data.Meshlets.resize(PackSlots.size());
            Data.MeshletSpheres.resize(PackSlots.size());
            Data.MeshletCones.resize(PackSlots.size());
            Data.MeshletTriangles.resize(TriangleCursor);
            Data.MeshletVertexRefs.reserve(RefCursor);

            {
                LUMINA_PROFILE_SECTION("Pack Static Meshlets");
                for (uint32 PackIdx = 0; PackIdx < (uint32)PackSlots.size(); ++PackIdx)
                {
                    const FPackSlot&             Pack   = PackSlots[PackIdx];
                    const FSurfaceMeshletResult& Result = *Pack.Result;

                    FMeshlet Out = Result.OutMeshlets[Pack.MeshletIdx];
                    Out.LODIndex = Pack.LOD;

                    const uint8* TriSrc = Result.Triangles.data() + Out.TriangleOffset;
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
                Progress->EnterProgressFrame(TangentStep * (float)MeshResource.GeometrySurfaces.size());
            }
        }

        const size_t NumVertices = MeshResource.GetNumVertices();
        const size_t NumIndices  = MeshResource.Indices.size();
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
                Progress->EnterProgressFrame(StepPerSurface * (float)MeshResource.GeometrySurfaces.size());
            }
            return;
        }

        // Before any of the meshlet build below, which drops the streams this reads.
        ComputeSurfaceTexelFactors(MeshResource);

        const float* VertexPositions = reinterpret_cast<const float*>(MeshResource.Positions.data());

        auto ReadPosition = [&](uint32 GlobalIdx) -> FVector3
        {
            return MeshResource.Positions[GlobalIdx];
        };

        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.size();

        // Callers that rebuild often (dynamic meshes) trade distant-draw quality for build time here.
        const uint32 LODCount          = Math::Clamp(MeshResource.MaxLODs, 1u, MAX_MESH_LODS);
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
                    Ranges.push_back(FUIntVector2((uint32)(Section.StartIndex + Begin), (uint32)Count));
                }
            }

            Task::ParallelFor((uint32)Ranges.size(), [&](uint32 RangeIdx)
            {
                uint32* RangeIndices = &MeshResource.Indices[Ranges[RangeIdx].x];
                meshopt_optimizeVertexCache(RangeIndices, RangeIndices, Ranges[RangeIdx].y, NumVertices);
            });
        }

        // A skinned vertex that moved would keep the skin weights of the place it left.
        const bool bDestructiveLODs = MeshResource.bDestructiveLODs && !MeshResource.bSkinnedMesh;

        const TVector<FSurfaceSimplifyInput> SimplifyInputs = LODCount > 1u
            ? BuildSurfaceSimplifyInputs(MeshResource)
            : TVector<FSurfaceSimplifyInput>();

        TVector<FSurfaceMeshletResult> Results(LODCount * NumSurfaces);
        TVector<size_t> CellIndexCount(LODCount * NumSurfaces, 0u);

        Task::ParallelFor(LODCount * NumSurfaces, [&](uint32 Cell)
        {
            LUMINA_PROFILE_SECTION("Process Surfaces and LODs");
            LUMINA_MEMORY_SCOPE("Meshes");

            const uint32 lod        = Cell / NumSurfaces;
            const uint32 SurfaceIdx = Cell % NumSurfaces;

            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            if (Section.IndexCount == 0)
            {
                return;
            }

            const uint32*          SurfaceIndices = &MeshResource.Indices[Section.StartIndex];
            FSurfaceMeshletResult& Out            = Results[Cell];

            if (lod == 0)
            {
                if (ShouldChunkSurface(Section, bFastMeshletBuild))
                {
                    const uint32 NumChunks = (uint32)((Section.IndexCount + kChunkedBuildIndices - 1u) / kChunkedBuildIndices);
                    TVector<FSurfaceMeshletResult> Chunks(NumChunks);
                    Task::ParallelFor(NumChunks, [&](uint32 Chunk)
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
                CellIndexCount[Cell] = Section.IndexCount;
                return;
            }

            // Snap to whole triangles; floor at kLODMinIndices so sloppy LODs don't degenerate.
            size_t TargetIndices = (size_t)((double)Section.IndexCount * Math::Pow((double)kLODTriangleRatio, (double)lod));
            TargetIndices = (TargetIndices / 3u) * 3u;
            if (TargetIndices < kLODMinIndices)
            {
                return;
            }

            const FSurfaceSimplifyInput& Input = SimplifyInputs[SurfaceIdx];
            if (Input.Indices.empty())
            {
                return;
            }

            TVector<uint32> Simplified;
            SimplifySurfaceLOD(Input, TargetIndices, bDestructiveLODs, Simplified, Out);

            const size_t NewCount = Simplified.size();
            CellIndexCount[Cell] = NewCount;
            if (NewCount < kLODMinIndices)
            {
                Out = FSurfaceMeshletResult{};
                return;
            }

            const size_t LocalVertexCount = Input.Positions.size();

            // Restore vertex-cache locality after simplifiers reorder by collapse/cluster priority.
            {
                LUMINA_PROFILE_SECTION("meshopt_optimizeVertexCache");
                meshopt_optimizeVertexCache(
                    Simplified.data(),
                    Simplified.data(),
                    NewCount, LocalVertexCount);
            }

            const FVector3* LevelPositions = Out.UpdatedPositions.empty() ? Input.Positions.data() : Out.UpdatedPositions.data();
            Out.Source = &Input;

            BuildLODMeshletsForRange(
                Simplified.data(), NewCount,
                &LevelPositions[0].x, LocalVertexCount, PositionStride,
                bConeCulling, bOptimizeMeshlets, bFastMeshletBuild,
                [LevelPositions](uint32 LocalIdx) { return LevelPositions[LocalIdx]; }, Out);
        });

        TVector<TFixedVector<uint32, MAX_MESH_LODS>> AcceptedPerSurface(NumSurfaces);
        TVector<TFixedVector<float, MAX_MESH_LODS>>  AcceptedErrorPerSurface(NumSurfaces);

        for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
        {
            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];

            TFixedVector<uint32, MAX_MESH_LODS>& Accepted      = AcceptedPerSurface[SurfaceIdx];
            TFixedVector<float, MAX_MESH_LODS>&  AcceptedError = AcceptedErrorPerSurface[SurfaceIdx];
            if (Results[0 * NumSurfaces + SurfaceIdx].bHasData)
            {
                Accepted.push_back(0u);
                AcceptedError.push_back(0.0f);
            }
            size_t LastIndexCount = Section.IndexCount;
            float  LastError      = 0.0f;

            for (uint32 lod = 1; lod < LODCount; ++lod)
            {
                const uint32                 Cell   = lod * NumSurfaces + SurfaceIdx;
                const FSurfaceMeshletResult& Result = Results[Cell];

                const size_t NewCount = CellIndexCount[Cell];
                if (!Result.bHasData || NewCount < kLODMinIndices)
                {
                    continue;
                }

                // Measured against the last accepted level, so skipping one keeps the bar where it was.
                if ((float)NewCount > (float)LastIndexCount * kLODMinReduction)
                {
                    continue;
                }

                if (!(Result.Error >= 0.0f && Result.Error < FLT_MAX))
                {
                    continue;
                }

                // Every level is measured against the source, so a coarser one can report less; selection needs it non-decreasing.
                LastError      = Math::Max(LastError, Result.Error);
                LastIndexCount = NewCount;
                Accepted.push_back(lod);
                AcceptedError.push_back(LastError);
            }

            // Drop every level that did not make the cut so the pack pass cannot fold one back in.
            for (uint32 lod = 0; lod < LODCount; ++lod)
            {
                if (!Algo::Contains(Accepted, lod))
                {
                    Results[lod * NumSurfaces + SurfaceIdx] = FSurfaceMeshletResult{};
                }
            }

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
            TotalMeshlets += R.OutMeshlets.size();
            TotalVertices += R.Vertices.size();
            for (const FMeshlet& M : R.OutMeshlets)
            {
                TotalTriangles += M.TriangleCount;
            }
        }

        MeshResource.MeshletData.Meshlets.reserve(TotalMeshlets);
        MeshResource.MeshletData.MeshletSpheres.reserve(TotalMeshlets);
        MeshResource.MeshletData.MeshletCones.reserve(TotalMeshlets);
        MeshResource.MeshletData.MeshletTriangles.reserve(TotalTriangles);
        if (MeshResource.bSkinnedMesh)
        {
            MeshResource.MeshletData.MeshletSkinnedVertices.reserve(TotalVertices);
            MeshResource.MeshletData.MeshletBonePalettes.reserve(TotalMeshlets);
            MeshResource.MeshletData.MeshletBoneIndices.reserve(TotalMeshlets * 16);
        }

        for (FGeometrySurface& Section : MeshResource.GeometrySurfaces)
        {
            Section.NumLODs = 0;
            for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
            {
                Section.LODMeshletOffset[i] = 0;
                Section.LODMeshletCount[i]  = 0;
                Section.LODError[i]         = 0.0f;
            }
        }

        auto LevelPosition = [&](const FSurfaceMeshletResult& Result, uint32 Ref) -> FVector3
        {
            if (!Result.UpdatedPositions.empty())
            {
                return Result.UpdatedPositions[Ref];
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

            if (!Result.UpdatedAttributes.empty())
            {
                const float*   Updated   = &Result.UpdatedAttributes[(size_t)Ref * kSimplifyAttributeCount];
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
            PackStaticMeshlets(MeshResource, Results, AcceptedPerSurface, AcceptedErrorPerSurface, LODCount, NumSurfaces,
                               LevelPosition, ResolveLevelVertex);
        }
        else
        {
            // Skinned levels never move a vertex, so the source positions bound every level.
            MeshResource.MeshletData.PositionGrid = ComputeMeshPositionGrid(MeshResource.Positions.data(), MeshResource.Positions.size());
        }

        for (uint32 Slot = 0; MeshResource.bSkinnedMesh && Slot < LODCount; ++Slot)
        {
            LUMINA_PROFILE_SECTION("Serial Pack Skinned LODs");

            for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
            {
                const TFixedVector<uint32, MAX_MESH_LODS>& Accepted = AcceptedPerSurface[SurfaceIdx];
                if (Slot >= Accepted.size())
                {
                    continue;
                }
                const uint32 SourceLOD = Accepted[Slot];

                FGeometrySurface&      Section = MeshResource.GeometrySurfaces[SurfaceIdx];
                FSurfaceMeshletResult& Result  = Results[SourceLOD * NumSurfaces + SurfaceIdx];

                if (!Result.bHasData)
                {
                    continue;
                }

                Section.LODMeshletOffset[Slot] = (uint32)MeshResource.MeshletData.Meshlets.size();
                Section.LODMeshletCount[Slot]  = (uint32)Result.OutMeshlets.size();
                Section.LODError[Slot]         = AcceptedErrorPerSurface[SurfaceIdx][Slot];
                Section.NumLODs                = Slot + 1u;

                FMeshletPaletteScratch Palette;

                for (size_t MeshletIdx = 0; MeshletIdx < Result.OutMeshlets.size(); ++MeshletIdx)
                {
                    FMeshlet Out = Result.OutMeshlets[MeshletIdx];

                    Out.LODIndex = Slot;

                    const uint32 PackedVertexStart = (uint32)MeshResource.MeshletData.MeshletSkinnedVertices.size();

                    Palette.clear();

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
                        MeshResource.MeshletData.MeshletSkinnedVertices.push_back(Packed);
                    }

                    // Indexed in lockstep with Meshlets, which Out is pushed into at the end of this body.
                    AppendMeshletBonePalette(MeshResource.MeshletData, Palette);

                    const uint32 PackedDwordStart = (uint32)MeshResource.MeshletData.MeshletTriangles.size();
                    const uint8* TriSrc           = Result.Triangles.data() + Out.TriangleOffset;
                    for (uint32 t = 0; t < Out.TriangleCount; ++t)
                    {
                        const uint32 Packed =
                              (uint32)TriSrc[t * 3 + 0]
                            | ((uint32)TriSrc[t * 3 + 1] << 8)
                            | ((uint32)TriSrc[t * 3 + 2] << 16);
                        MeshResource.MeshletData.MeshletTriangles.push_back(Packed);
                    }

                    Out.VertexOffset   = PackedVertexStart;
                    Out.TriangleOffset = PackedDwordStart;
                    MeshResource.MeshletData.Meshlets.push_back(Out);
                    MeshResource.MeshletData.MeshletSpheres.push_back(Result.Spheres[MeshletIdx]);
                    MeshResource.MeshletData.MeshletCones.push_back(Result.Cones[MeshletIdx]);
                }
            }
        }

        if (MeshResource.MeshletData.MeshletTriangles.empty())
        {
            MeshResource.MeshletData.MeshletTriangles.push_back(0u);
        }
    }

    void AnalyzeMeshStatistics(FMeshResource& MeshResource, FMeshStatistics& OutMeshStats)
    {
        OutMeshStats.VertexFetchStatics.emplace_back(meshopt_analyzeVertexFetch(MeshResource.Indices.data(), MeshResource.Indices.size(), MeshResource.GetNumVertices(), MeshResource.GetVertexTypeSize()));
        OutMeshStats.OverdrawStatics.emplace_back(meshopt_analyzeOverdraw(MeshResource.Indices.data(), MeshResource.Indices.size(), reinterpret_cast<const float*>(MeshResource.Positions.data()), MeshResource.GetNumVertices(), sizeof(FVector3)));
    }

    namespace
    {
        // Coalesce surfaces by MaterialIndex; rebuilds the index buffer to make per-material slices contiguous.
        void MergeSurfacesByMaterial(FMeshResource& MeshResource)
        {
            const TVector<FGeometrySurface>& OldSurfaces = MeshResource.GeometrySurfaces;
            if (OldSurfaces.size() <= 1 || MeshResource.Indices.empty())
            {
                return;
            }

            // First-seen MaterialIndex order; stable for downstream slot matching.
            THashMap<int16, uint32> MaterialToNewIdx;
            TVector<int16>          MaterialOrder;
            MaterialOrder.reserve(OldSurfaces.size());

            for (const FGeometrySurface& S : OldSurfaces)
            {
                if (MaterialToNewIdx.find(S.MaterialIndex) == MaterialToNewIdx.end())
                {
                    MaterialToNewIdx.emplace(S.MaterialIndex, (uint32)MaterialOrder.size());
                    MaterialOrder.push_back(S.MaterialIndex);
                }
            }

            if (MaterialOrder.size() == OldSurfaces.size())
            {
                return;
            }

            const uint32 NumMerged = (uint32)MaterialOrder.size();

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

            TVector<uint32> NewIndices(MeshResource.Indices.size());
            TVector<uint32> WriteCursor = StartPerMerged;
            for (const FGeometrySurface& S : OldSurfaces)
            {
                if (S.IndexCount == 0)
                {
                    continue;
                }
                const uint32 NewIdx = MaterialToNewIdx[S.MaterialIndex];
                memcpy(NewIndices.data() + WriteCursor[NewIdx],
                       MeshResource.Indices.data() + S.StartIndex,
                       S.IndexCount * sizeof(uint32));
                WriteCursor[NewIdx] += S.IndexCount;
            }

            // Inherit first source surface's ID so re-imports keep stable surface identity.
            TVector<FGeometrySurface> NewSurfaces;
            NewSurfaces.reserve(NumMerged);
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
                NewSurfaces.push_back(NewSurface);
            }

            MeshResource.Indices          = Move(NewIndices);
            MeshResource.GeometrySurfaces = Move(NewSurfaces);
        }

        // Concatenate Src into Dst with index/material rebase; bakes Src.ImportTransform into positions/normals.
        void MergeResourceInto(FMeshResource& Src, FMeshResource& Dst, TVector<int16>& SlotToSource)
        {
            const uint32 BaseVert = (uint32)Dst.GetNumVertices();
            const uint32 BaseIdx  = (uint32)Dst.Indices.size();

            const FMatrix4 PosMatrix    = Src.ImportTransform;
            const FMatrix3 NormalMatrix = Math::Transpose(Math::Inverse(FMatrix3(PosMatrix)));
            const bool bIdentity         = PosMatrix == FMatrix4(1.0f);

            // Append every active stream; joint streams only when both sides are skinned.
            const size_t Start    = Dst.GetNumVertices();
            const size_t SrcCount = Src.GetNumVertices();

            Dst.Positions.insert(Dst.Positions.end(), Src.Positions.begin(), Src.Positions.end());
            Dst.Normals.insert(Dst.Normals.end(),     Src.Normals.begin(),   Src.Normals.end());
            Dst.Tangents.insert(Dst.Tangents.end(),   Src.Tangents.begin(),  Src.Tangents.end());
            Dst.UVs.insert(Dst.UVs.end(),             Src.UVs.begin(),       Src.UVs.end());
            Dst.UVs1.insert(Dst.UVs1.end(),           Src.UVs1.begin(),      Src.UVs1.end());
            Dst.Colors.insert(Dst.Colors.end(),       Src.Colors.begin(),    Src.Colors.end());
            if (Dst.bSkinnedMesh && Src.bSkinnedMesh)
            {
                Dst.JointIndices.insert(Dst.JointIndices.end(), Src.JointIndices.begin(), Src.JointIndices.end());
                Dst.JointWeights.insert(Dst.JointWeights.end(), Src.JointWeights.begin(), Src.JointWeights.end());
            }

            // Bake the source scene-graph transform into the appended positions/normals.
            if (!bIdentity)
            {
                TVector<FVector3> BakedNormals(SrcCount);
                VertexOps::UnpackNormals(Dst.Normals.data() + Start, BakedNormals.data(), SrcCount);
                for (size_t i = 0; i < SrcCount; ++i)
                {
                    Dst.Positions[Start + i] = FVector3(PosMatrix * FVector4(Dst.Positions[Start + i], 1.0f));
                    BakedNormals[i]          = Math::Normalize(NormalMatrix * BakedNormals[i]);
                }
                VertexOps::PackNormals(BakedNormals.data(), Dst.Normals.data() + Start, SrcCount);
            }

            Dst.Indices.reserve(Dst.Indices.size() + Src.Indices.size());
            for (uint32 Idx : Src.Indices)
            {
                Dst.Indices.push_back(Idx + BaseVert);
            }

            // Keyed per piece, so each merged piece keeps its own slot even where pieces share a material.
            THashMap<int16, int16> PieceSlots;
            Dst.GeometrySurfaces.reserve(Dst.GeometrySurfaces.size() + Src.GeometrySurfaces.size());
            for (FGeometrySurface S : Src.GeometrySurfaces)
            {
                S.StartIndex += BaseIdx;
                if (S.MaterialIndex >= 0)
                {
                    auto It = PieceSlots.find(S.MaterialIndex);
                    if (It == PieceSlots.end())
                    {
                        const int16 NewSlot = (int16)SlotToSource.size();
                        SlotToSource.push_back(S.MaterialIndex);
                        PieceSlots.emplace(S.MaterialIndex, NewSlot);
                        S.MaterialIndex = NewSlot;
                    }
                    else
                    {
                        S.MaterialIndex = It->second;
                    }
                }
                Dst.GeometrySurfaces.push_back(S);
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
            Task::ParallelFor((uint32)Data.Resources.size(), [&](uint32 ResIdx)
            {
                TUniquePtr<FMeshResource>& MeshPtr = Data.Resources[ResIdx];
                if (!MeshPtr)
                {
                    return;
                }

                FMeshResource& M = *MeshPtr;
                const size_t NumVerts = M.GetNumVertices();

                for (size_t i = 0; i < NumVerts; ++i)
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
        if (Options.bMergeMeshes && Data.Resources.size() > 1)
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
                NewResources.push_back(std::move(MergedStatic));
            }
            if (MergedSkinned->GetNumVertices() > 0)
            {
                NewResources.push_back(std::move(MergedSkinned));
            }
            Data.Resources = std::move(NewResources);
        }

        // Stats run serially afterwards, so the parallel pass writes only resource-local data.
        Data.MeshStatistics.OverdrawStatics.clear();
        Data.MeshStatistics.VertexFetchStatics.clear();

        if (Progress)
        {
            Progress->UpdateMessage("Merging surfaces...");
        }
        Task::ParallelFor((uint32)Data.Resources.size(), [&](uint32 ResIdx)
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
                TotalSurfaces += MeshPtr->GeometrySurfaces.size();
            }
        }
        const float StepPerSurface = ProgressBudget / (float)std::max<size_t>((size_t)1, TotalSurfaces);

        if (Progress)
        {
            Progress->UpdateMessage("Optimizing geometry...");
        }

        Task::ParallelFor((uint32)Data.Resources.size(), [&](uint32 ResIdx)
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
