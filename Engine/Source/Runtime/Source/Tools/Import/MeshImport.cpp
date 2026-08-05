#include "RuntimePCH.h"
#include "ImportHelpers.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Templates/AsBytes.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/TaskSystem.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include <mikktspace.h>
#include <meshoptimizer.h>

namespace Lumina::Import::Mesh
{
    FMeshImportData::FMeshImportData() = default;
    FMeshImportData::FMeshImportData(FMeshImportData&&) noexcept = default;
    FMeshImportData& FMeshImportData::operator=(FMeshImportData&&) noexcept = default;

    FMeshImportData::~FMeshImportData()
    {
        // Preview thumbnails are heap textures; release them with the import session.
        for (const FMeshImportImage& Img : Textures)
        {
            RHI::FManagedTexture Tmp = Img.DisplayImage;
            if (Tmp.IsValid())
            {
                RHI::Textures::Release(Tmp);
            }
        }
    }

    namespace
    {
        // meshopt_setAllocator stores function pointers only, so static-init is safe.
        void* MeshoptAlloc(size_t Size) { LUMINA_MEMORY_SCOPE("MeshOpt"); return Memory::Malloc(Size); }
        void  MeshoptFree(void* Ptr)    { if (Ptr) { Memory::Free(Ptr); } }
        const bool GMeshoptAllocatorSet = []{ meshopt_setAllocator(MeshoptAlloc, MeshoptFree); return true; }();
    }

    // MikkTSpace tangent gen: matches authored normal-map convention so baked normals round-trip.
    namespace
    {
        // One MikkTSpace context per surface so the (single-threaded) generator can run
        // concurrently across a mesh's disjoint index ranges.
        struct FMikkSurfaceContext
        {
            FMeshResource* Mesh;
            uint32         StartIndex;
            uint32         IndexCount;
        };

        FORCEINLINE uint32 MikkVertexIndex(const SMikkTSpaceContext* Ctx, int iFace, int iVert)
        {
            const FMikkSurfaceContext* S = static_cast<const FMikkSurfaceContext*>(Ctx->m_pUserData);
            return S->Mesh->Indices[(size_t)S->StartIndex + (size_t)iFace * 3u + (size_t)iVert];
        }

        int Mikk_GetNumFaces(const SMikkTSpaceContext* Ctx)
        {
            const FMikkSurfaceContext* S = static_cast<const FMikkSurfaceContext*>(Ctx->m_pUserData);
            return (int)(S->IndexCount / 3u);
        }

        int Mikk_GetNumVerticesOfFace(const SMikkTSpaceContext*, int)
        {
            return 3;
        }

        void Mikk_GetPosition(const SMikkTSpaceContext* Ctx, float Out[], int iFace, int iVert)
        {
            const FMikkSurfaceContext* S = static_cast<const FMikkSurfaceContext*>(Ctx->m_pUserData);
            const FVector3 P = S->Mesh->Positions[MikkVertexIndex(Ctx, iFace, iVert)];
            Out[0] = P.x; Out[1] = P.y; Out[2] = P.z;
        }

        void Mikk_GetNormal(const SMikkTSpaceContext* Ctx, float Out[], int iFace, int iVert)
        {
            const FMikkSurfaceContext* S = static_cast<const FMikkSurfaceContext*>(Ctx->m_pUserData);
            const FVector3 N = UnpackNormal(S->Mesh->Normals[MikkVertexIndex(Ctx, iFace, iVert)]);
            Out[0] = N.x; Out[1] = N.y; Out[2] = N.z;
        }

        void Mikk_GetTexCoord(const SMikkTSpaceContext* Ctx, float Out[], int iFace, int iVert)
        {
            const FMikkSurfaceContext* S = static_cast<const FMikkSurfaceContext*>(Ctx->m_pUserData);
            const FVector2 UV = S->Mesh->GetUVAt(MikkVertexIndex(Ctx, iFace, iVert));
            Out[0] = UV.x; Out[1] = UV.y;
        }

        void Mikk_SetTSpaceBasic(const SMikkTSpaceContext* Ctx, const float Tangent[], float Sign, int iFace, int iVert)
        {
            const FMikkSurfaceContext* S = static_cast<const FMikkSurfaceContext*>(Ctx->m_pUserData);
            // Disjoint surfaces rarely share a vertex post-dedup; when they do, both write a
            // valid tangent and the aligned uint32 store is atomic, so it is last-writer-wins.
            S->Mesh->Tangents[MikkVertexIndex(Ctx, iFace, iVert)] = PackTangent(FVector3(Tangent[0], Tangent[1], Tangent[2]), Sign);
        }

        // Describe every active SoA vertex stream for meshopt's multi-stream remap.
        uint32 BuildVertexStreams(FMeshResource& M, meshopt_Stream* OutStreams)
        {
            uint32 Count = 0;
            OutStreams[Count++] = { M.Positions.data(), sizeof(FVector3),   sizeof(FVector3) };
            OutStreams[Count++] = { M.Normals.data(),   sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.Tangents.data(),  sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.UVs.data(),       sizeof(uint32),      sizeof(uint32) };
            OutStreams[Count++] = { M.Colors.data(),    sizeof(uint32),      sizeof(uint32) };
            if (M.bSkinnedMesh)
            {
                OutStreams[Count++] = { M.JointIndices.data(), sizeof(FU8Vector4), sizeof(FU8Vector4) };
                OutStreams[Count++] = { M.JointWeights.data(), sizeof(FU8Vector4), sizeof(FU8Vector4) };
            }
            return Count;
        }

        // Apply a meshopt vertex remap to every active stream into fresh buffers (no overlap).
        void RemapVertexStreams(FMeshResource& M, const uint32* Remap, size_t OldCount, size_t NewCount)
        {
            auto RemapStream = [&](auto& Stream)
            {
                using TElem = typename eastl::remove_reference_t<decltype(Stream)>::value_type;
                TVector<TElem> Out(NewCount);
                meshopt_remapVertexBuffer(Out.data(), Stream.data(), OldCount, sizeof(TElem), Remap);
                Stream = Move(Out);
            };
            RemapStream(M.Positions);
            RemapStream(M.Normals);
            RemapStream(M.Tangents);
            RemapStream(M.UVs);
            RemapStream(M.Colors);
            if (M.bSkinnedMesh)
            {
                RemapStream(M.JointIndices);
                RemapStream(M.JointWeights);
            }
        }
    }

    void ComputeMikkTSpaceTangents(FMeshResource& MeshResource, FScopedSlowTask* Progress = nullptr, float StepPerSurface = 0.0f)
    {
        if (MeshResource.Indices.empty() || MeshResource.GetNumVertices() == 0)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();
        
        SMikkTSpaceInterface Interface = {};
        Interface.m_getNumFaces          = Mikk_GetNumFaces;
        Interface.m_getNumVerticesOfFace = Mikk_GetNumVerticesOfFace;
        Interface.m_getPosition          = Mikk_GetPosition;
        Interface.m_getNormal            = Mikk_GetNormal;
        Interface.m_getTexCoord          = Mikk_GetTexCoord;
        Interface.m_setTSpaceBasic       = Mikk_SetTSpaceBasic;

        // MikkTSpace is single-threaded internally; surfaces are disjoint index ranges,
        // so run one generator context per surface in parallel.
        const uint32 NumSurfaces = (uint32)MeshResource.GeometrySurfaces.size();
        Task::ParallelFor(NumSurfaces, [&](uint32 SurfaceIdx)
        {
            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            if (Section.IndexCount >= 3)
            {
                FMikkSurfaceContext UserData{ &MeshResource, Section.StartIndex, Section.IndexCount };
                SMikkTSpaceContext Ctx = {};
                Ctx.m_pInterface = &Interface;
                Ctx.m_pUserData  = &UserData;
                genTangSpaceDefault(&Ctx);
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

        Task::ParallelFor((uint32)NumVertices, [=](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                const FVector3 N = UnpackNormal(Normals[i]);

                // Cross against whichever axis is least parallel to N, so the result never degenerates.
                const FVector3 Axis = (Math::Abs(N.z) < 0.9f) ? FVector3(0.0f, 0.0f, 1.0f)
                                                              : FVector3(1.0f, 0.0f, 0.0f);
                const FVector3 T = Math::Normalize(Math::Cross(Axis, N));
                Tangents[i] = PackTangent(T, 1.0f);
            }
        }, 4096);
    }

    // MUST run before meshlet/LOD generation, regardless of the "optimize" option: meshopt_simplify allocates
    // O(vertex_count) internal buffers, so an un-deduplicated array (FBX commonly emits 3 unique verts per
    // triangle) can request multi-GB allocations and OOM-crash. Dedup is a correctness prerequisite, not an optimization.
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

        meshopt_Stream Streams[7];
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

    // Vertex cache / overdraw / fetch reordering -- a pure optimization gated on the import option.
    // Assumes vertices were already deduplicated (DeduplicateMeshVertices ran first).
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
        struct FLODSettings
        {
            float Ratio;        // target index count fraction of LOD 0
            float Threshold;    // distance/radius ratio at which this LOD activates
            float TargetError;  // simplifier target error (model-AABB-relative)
            bool  bSloppy;      // true = clustering simplify; false = edge-collapse
        };

        // 50/25/12.5% topology-preserving ramp; 3%/0.5% sloppy for distant near-billboards.
        constexpr FLODSettings kLODs[MAX_MESH_LODS] =
        {
            { 1.0f,     0.0f,  0.05f, false },  // LOD 0 -- full detail
            { 0.5f,     8.0f,  0.05f, false },  // LOD 1
            { 0.25f,   16.0f,  0.05f, false },  // LOD 2
            { 0.125f,  32.0f,  0.05f, false },  // LOD 3
            { 0.03f,   64.0f,  0.50f, true  },  // LOD 4 (sloppy, distant)
            { 0.005f, 128.0f,  1.00f, true  },  // LOD 5 (sloppy, near-quad)
        };

        // ~5 triangles minimum; below this we drop the LOD and fall back to coarser.
        constexpr size_t kLODMinIndices = 15u;

        struct FSurfaceMeshletResult
        {
            TVector<uint32>          Vertices;
            TVector<uint8>           Triangles;
            TVector<FMeshlet>        OutMeshlets;
            TVector<FMeshletBounds>  Bounds;
            TVector<FVector3>       MeshletLo;
            FVector3                MaxExtent = FVector3(0.0f);
            bool                     bHasData  = false;
        };

        // Build meshlets for one (LOD, Surface) cell; quantization deferred to the serial pack pass.
        template<typename TReadPos>
        void BuildLODMeshletsForRange(
            const uint32* SrcIndices, size_t SrcIndexCount,
            const float*  VertexPositions, size_t NumVertices, size_t VertexSize,
            bool bConeCulling, bool bOptimizeMeshlets,
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
            Result.Bounds.reserve(MeshletCount);
            Result.MeshletLo.reserve(MeshletCount);
            
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

                // Hoisted above the bounds: without cones this pass IS the bounds, so it must run first.
                FVector3 Lo( FLT_MAX);
                FVector3 Hi(-FLT_MAX);
                for (uint32 i = 0; i < M.vertex_count; ++i)
                {
                    const FVector3 P = ReadPosition(Result.Vertices[M.vertex_offset + i]);
                    Lo = Math::Min(Lo, P);
                    Hi = Math::Max(Hi, P);
                }

                FMeshletBounds Bounds{};
                if (bConeCulling)
                {
                    const meshopt_Bounds B = meshopt_computeMeshletBounds(
                        &Result.Vertices[M.vertex_offset],
                        &Result.Triangles[M.triangle_offset],
                        M.triangle_count,
                        VertexPositions, NumVertices, VertexSize);

                    Bounds.Center     = FVector3(B.center[0],    B.center[1],    B.center[2]);
                    Bounds.Radius     = B.radius;
                    Bounds.ConeApex   = FVector3(B.cone_apex[0], B.cone_apex[1], B.cone_apex[2]);
                    Bounds.ConeAxis   = FVector3(B.cone_axis[0], B.cone_axis[1], B.cone_axis[2]);
                    Bounds.ConeCutoff = B.cone_cutoff;
                }
                else
                {
                    Bounds.Center     = (Lo + Hi) * 0.5f;
                    Bounds.Radius     = Math::Length(Hi - Bounds.Center);
                    Bounds.ConeAxis   = FVector3(0.0f, 0.0f, 1.0f);
                    Bounds.ConeCutoff = 1.0f;
                }
                Result.Bounds.push_back(Bounds);

                Result.MeshletLo.push_back(Lo);
                Result.MaxExtent = Math::Max(Result.MaxExtent, Hi - Lo);
            }
            }

            Result.bHasData = !Result.OutMeshlets.empty();
        }
    } // namespace

    void GenerateMeshlets(FMeshResource& MeshResource, FScopedSlowTask* Progress, float StepPerSurface)
    {
        LUMINA_PROFILE_SCOPE();
        
        MeshResource.MeshletData.Clear();

        const float TangentStep = StepPerSurface * 0.35f;
        const float MeshletStep = StepPerSurface * 0.65f;

        if (Progress)
        {
            Progress->UpdateMessage("Generating tangents...");
        }
        if (MeshResource.bGenerateTangents)
        {
            ComputeMikkTSpaceTangents(MeshResource, Progress, TangentStep);
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
                    Section.LODMeshletOffset[i]   = 0;
                    Section.LODMeshletCount[i]    = 0;
                    Section.LODScreenThreshold[i] = i == 0 ? 0.0f : FLT_MAX;
                }
            }
            if (Progress)
            {
                Progress->EnterProgressFrame(StepPerSurface * (float)MeshResource.GeometrySurfaces.size());
            }
            return;
        }

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

        if (Progress)
        {
            Progress->UpdateMessage("Building meshlets & LODs...");
        }
        
        TVector<FSurfaceMeshletResult> Results(LODCount * NumSurfaces);
        TVector<size_t> CellIndexCount(LODCount * NumSurfaces, 0u);

        Task::ParallelFor(LODCount * NumSurfaces, [&](uint32 Cell)
        {
            LUMINA_PROFILE_SECTION("Process Surfaces and LODs");
            
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
                BuildLODMeshletsForRange(
                    SurfaceIndices, Section.IndexCount,
                    VertexPositions, NumVertices, PositionStride,
                    bConeCulling, bOptimizeMeshlets,
                    ReadPosition, Out);
                CellIndexCount[Cell] = Section.IndexCount;
                return;
            }

            const FLODSettings& Cfg = kLODs[lod];

            // Snap to whole triangles; floor at kLODMinIndices so sloppy LODs don't degenerate.
            size_t TargetIndices = (size_t)((float)Section.IndexCount * Cfg.Ratio);
            TargetIndices = (TargetIndices / 3u) * 3u;
            if (TargetIndices < kLODMinIndices)
            {
                return;
            }

            // Scratch is per cell now that cells run concurrently. Sized to the FULL source range and
            // value-initialized by resize, i.e. 4 bytes memset per source index per LOD cell, on top of
            // the meshlet scratch inside BuildLODMeshletsForRange.
            TVector<uint32> Simplified;
            {
                LUMINA_PROFILE_SECTION("Simplify Scratch Alloc");
                Simplified.resize(Section.IndexCount);
            }

            float  ResultError = 0.0f;
            size_t NewCount    = 0;
            {
                // Note this simplifies from the FULL LOD-0 range every level, not from LOD N-1, so each
                // cell's input is the whole surface regardless of how coarse the target is.
                LUMINA_PROFILE_SECTION("meshopt_simplify");
                NewCount = Cfg.bSloppy
                    ? meshopt_simplifySloppy(
                        Simplified.data(),
                        SurfaceIndices, Section.IndexCount,
                        VertexPositions, NumVertices, PositionStride,
                        nullptr,
                        TargetIndices, Cfg.TargetError,
                        &ResultError)
                    : meshopt_simplify(
                        Simplified.data(),
                        SurfaceIndices, Section.IndexCount,
                        VertexPositions, NumVertices, PositionStride,
                        TargetIndices, Cfg.TargetError,
                        meshopt_SimplifyLockBorder,
                        &ResultError);
            }

            CellIndexCount[Cell] = NewCount;
            if (NewCount < kLODMinIndices)
            {
                return;
            }

            // Restore vertex-cache locality after simplifiers reorder by collapse/cluster priority.
            {
                LUMINA_PROFILE_SECTION("meshopt_optimizeVertexCache");
                meshopt_optimizeVertexCache(
                    Simplified.data(),
                    Simplified.data(),
                    NewCount, NumVertices);
            }

            BuildLODMeshletsForRange(
                Simplified.data(), NewCount,
                VertexPositions, NumVertices, PositionStride,
                bConeCulling, bOptimizeMeshlets,
                ReadPosition, Out);
        });
        
        TVector<TFixedVector<uint32, MAX_MESH_LODS>> AcceptedPerSurface(NumSurfaces);

        for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
        {
            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];

            TFixedVector<uint32, MAX_MESH_LODS>& Accepted = AcceptedPerSurface[SurfaceIdx];
            if (Results[0 * NumSurfaces + SurfaceIdx].bHasData)
            {
                Accepted.push_back(0u);
            }
            size_t LastIndexCount = Section.IndexCount;

            for (uint32 lod = 1; lod < LODCount; ++lod)
            {
                const uint32        Cell = lod * NumSurfaces + SurfaceIdx;
                const FLODSettings& Cfg  = kLODs[lod];
                
                size_t TargetIndices = (size_t)((float)Section.IndexCount * Cfg.Ratio);
                TargetIndices = (TargetIndices / 3u) * 3u;
                if (TargetIndices < kLODMinIndices)
                {
                    continue;
                }

                const size_t NewCount = CellIndexCount[Cell];
                if (NewCount < kLODMinIndices)
                {
                    continue;
                }

                // 5% progress floor for topology-preserving simplify; sloppy always hits target. Measured
                // against the last ACCEPTED level, so skipping one keeps the bar where it was.
                if (!Cfg.bSloppy && (float)NewCount > (float)LastIndexCount * 0.95f)
                {
                    continue;
                }

                if (!Results[Cell].bHasData)
                {
                    continue;
                }

                LastIndexCount = NewCount;
                Accepted.push_back(lod);
            }

            // Drop every level that did not make the cut so the pack pass cannot fold one back in.
            for (uint32 lod = 0; lod < LODCount; ++lod)
            {
                if (eastl::find(Accepted.begin(), Accepted.end(), lod) == Accepted.end())
                {
                    Results[lod * NumSurfaces + SurfaceIdx] = FSurfaceMeshletResult{};
                }
            }

            if (Progress)
            {
                Progress->EnterProgressFrame(MeshletStep);
            }
        }

        // Positions are stored as full mesh-local float3 in the vertex buffer -- no quantization grid,
        // so no per-meshlet/per-LOD precision tradeoff and no boundary cracks (shared verts are bit-identical).

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
        MeshResource.MeshletData.MeshletBounds.reserve(TotalMeshlets);
        MeshResource.MeshletData.MeshletTriangles.reserve(TotalTriangles);
        if (MeshResource.bSkinnedMesh)
        {
            MeshResource.MeshletData.MeshletSkinnedVertices.reserve(TotalVertices);
        }
        else
        {
            MeshResource.MeshletData.MeshletVertices.reserve(TotalVertices);
        }

        // Unused LOD slots keep FLT_MAX threshold so first-miss-wins selection can't pick them.
        for (FGeometrySurface& Section : MeshResource.GeometrySurfaces)
        {
            Section.NumLODs = 0;
            for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
            {
                Section.LODMeshletOffset[i]   = 0;
                Section.LODMeshletCount[i]    = 0;
                Section.LODScreenThreshold[i] = i == 0 ? 0.0f : FLT_MAX;
            }
        }

        // Slot, NOT source level: accepted levels compact down so LODMeshletOffset[0..NumLODs) is dense,
        // which is what both selectors require (they walk slots 0..NumLODs-1 and index the arrays with the
        // result). Each slot keeps its SOURCE level's screen threshold, so a compacted level still
        // activates at the distance it was authored for, and the sequence stays ascending.
        for (uint32 Slot = 0; Slot < LODCount; ++Slot)
        {
            LUMINA_PROFILE_SECTION("Serial Pack LODs");

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

                Section.LODMeshletOffset[Slot]   = (uint32)MeshResource.MeshletData.Meshlets.size();
                Section.LODMeshletCount[Slot]    = (uint32)Result.OutMeshlets.size();
                Section.LODScreenThreshold[Slot] = kLODs[SourceLOD].Threshold;
                Section.NumLODs                  = Slot + 1u;

                for (size_t MeshletIdx = 0; MeshletIdx < Result.OutMeshlets.size(); ++MeshletIdx)
                {
                    FMeshlet Out = Result.OutMeshlets[MeshletIdx];

                    Out.LODIndex = Slot;

                    const uint32 PackedVertexStart = MeshResource.bSkinnedMesh
                        ? (uint32)MeshResource.MeshletData.MeshletSkinnedVertices.size()
                        : (uint32)MeshResource.MeshletData.MeshletVertices.size();

                    if (MeshResource.bSkinnedMesh)
                    {
                        for (uint32 i = 0; i < Out.VertexCount; ++i)
                        {
                            const uint32 GlobalIdx = Result.Vertices[Out.VertexOffset + i];

                            FMeshletSkinnedVertex Packed;
                            Packed.Position = MeshResource.Positions[GlobalIdx];
                            Packed.Normal   = MeshResource.Normals[GlobalIdx];
                            Packed.Tangent  = MeshResource.Tangents[GlobalIdx];
                            Packed.UV       = MeshResource.UVs[GlobalIdx];
                            Packed.Color    = MeshResource.Colors[GlobalIdx];
                            memcpy(&Packed.JointIndices, &MeshResource.JointIndices[GlobalIdx], sizeof(uint32));
                            memcpy(&Packed.JointWeights, &MeshResource.JointWeights[GlobalIdx], sizeof(uint32));
                            MeshResource.MeshletData.MeshletSkinnedVertices.push_back(Packed);
                        }
                    }
                    else
                    {
                        for (uint32 i = 0; i < Out.VertexCount; ++i)
                        {
                            const uint32 GlobalIdx = Result.Vertices[Out.VertexOffset + i];

                            FMeshletVertex Packed;
                            Packed.Position = MeshResource.Positions[GlobalIdx];
                            Packed.Normal   = MeshResource.Normals[GlobalIdx];
                            Packed.Tangent  = MeshResource.Tangents[GlobalIdx];
                            Packed.UV       = MeshResource.UVs[GlobalIdx];
                            Packed.Color    = MeshResource.Colors[GlobalIdx];
                            MeshResource.MeshletData.MeshletVertices.push_back(Packed);
                        }
                    }

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
                    MeshResource.MeshletData.MeshletBounds.push_back(Result.Bounds[MeshletIdx]);
                }
            }
        }

        if (MeshResource.MeshletData.MeshletTriangles.empty())
        {
            MeshResource.MeshletData.MeshletTriangles.push_back(0u);
        }

        // Per-surface LOD outcome. LOD selection clamps to each surface's OWN NumLODs, so a surface that
        // built one level silently ignores every LOD pick -- indistinguishable in the viewport from a
        // broken selector, which is exactly how the foliage-vs-trunk case presented. Logged per surface so
        // "this material slot has no LODs" is readable instead of inferred.
        for (uint32 SurfaceIdx = 0; SurfaceIdx < NumSurfaces; ++SurfaceIdx)
        {
            const FGeometrySurface& Section = MeshResource.GeometrySurfaces[SurfaceIdx];
            if (Section.NumLODs > 1u || Section.IndexCount == 0)
            {
                continue;
            }

            LOG_WARN("Mesh '{}' surface {} (material slot {}, {} tris) built only LOD 0 -- every LOD "
                     "selection on it resolves to 0. Simplification could not reduce it: geometry made of "
                     "disconnected shells (foliage cards) is all border, which meshopt_SimplifyLockBorder "
                     "pins in place.",
                     MeshResource.Name.c_str(), SurfaceIdx, Section.MaterialIndex, Section.IndexCount / 3u);
        }
    }

    float GetDefaultLODScreenThreshold(uint32 Index)
    {
        // Reads the same table GenerateMeshlets bakes from, so the editor's reset and the importer can
        // never drift apart.
        return Index < MAX_MESH_LODS ? kLODs[Index].Threshold : FLT_MAX;
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
        void MergeResourceInto(FMeshResource& Src, FMeshResource& Dst, THashMap<int16, int16>& MaterialRemap)
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
            Dst.Colors.insert(Dst.Colors.end(),       Src.Colors.begin(),    Src.Colors.end());
            if (Dst.bSkinnedMesh && Src.bSkinnedMesh)
            {
                Dst.JointIndices.insert(Dst.JointIndices.end(), Src.JointIndices.begin(), Src.JointIndices.end());
                Dst.JointWeights.insert(Dst.JointWeights.end(), Src.JointWeights.begin(), Src.JointWeights.end());
            }

            // Bake the source scene-graph transform into the appended positions/normals.
            if (!bIdentity)
            {
                for (size_t i = Start; i < Start + SrcCount; ++i)
                {
                    Dst.Positions[i] = FVector3(PosMatrix * FVector4(Dst.Positions[i], 1.0f));
                    Dst.Normals[i]   = PackNormal(Math::Normalize(NormalMatrix * UnpackNormal(Dst.Normals[i])));
                }
            }

            Dst.Indices.reserve(Dst.Indices.size() + Src.Indices.size());
            for (uint32 Idx : Src.Indices)
            {
                Dst.Indices.push_back(Idx + BaseVert);
            }

            Dst.GeometrySurfaces.reserve(Dst.GeometrySurfaces.size() + Src.GeometrySurfaces.size());
            for (FGeometrySurface S : Src.GeometrySurfaces)
            {
                S.StartIndex += BaseIdx;
                if (S.MaterialIndex >= 0)
                {
                    auto It = MaterialRemap.find(S.MaterialIndex);
                    if (It == MaterialRemap.end())
                    {
                        const int16 NewSlot = (int16)MaterialRemap.size();
                        MaterialRemap.emplace(S.MaterialIndex, NewSlot);
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
                        M.SetPositionAt(i, M.GetPositionAt(i) * Scale);
                    }
                    if (bFlipUVs || bFlipU)
                    {
                        FVector2 UV = M.GetUVAt(i);
                        if (bFlipUVs) { UV.y = 1.0f - UV.y; }
                        if (bFlipU)   { UV.x = 1.0f - UV.x; }
                        M.SetUVAt(i, UV);
                    }
                    if (bFlipNormals)
                    {
                        FVector3 N = UnpackNormal(M.GetNormalAt(i));
                        M.SetNormalAt(i, PackNormal(-N));
                    }
                }
            });
        }

        // Skeleton bind/local translations scale like positions; rotation
        // is untouched because uniform scale commutes with R^T.
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

        // Merge into static + skinned pair before optimize/meshlets so the
        // heavy passes run on the merged geometry.
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

            THashMap<int16, int16> StaticMatRemap;
            THashMap<int16, int16> SkinnedMatRemap;

            for (TUniquePtr<FMeshResource>& Res : Data.Resources)
            {
                if (!Res)
                {
                    continue;
                }
                if (Res->bSkinnedMesh)
                {
                    MergeResourceInto(*Res, *MergedSkinned, SkinnedMatRemap);
                }
                else
                {
                    MergeResourceInto(*Res, *MergedStatic, StaticMatRemap);
                }
            }

            TVector<TUniquePtr<FMeshResource>> NewResources;
            if (MergedStatic->GetNumVertices() > 0)
            {
                NewResources.push_back(eastl::move(MergedStatic));
            }
            if (MergedSkinned->GetNumVertices() > 0)
            {
                NewResources.push_back(eastl::move(MergedSkinned));
            }
            Data.Resources = eastl::move(NewResources);
        }

        // Heavy CPU finalize. Stats run serially afterwards so the parallel
        // pass writes only resource-local data.
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

        // Spread the finalize progress budget evenly across every surface in every
        // resource, so the bar moves smoothly even when merge collapses to one resource.
        size_t TotalSurfaces = 0;
        for (const TUniquePtr<FMeshResource>& MeshPtr : Data.Resources)
        {
            if (MeshPtr)
            {
                TotalSurfaces += MeshPtr->GeometrySurfaces.size();
            }
        }
        const float StepPerSurface = ProgressBudget / (float)eastl::max<size_t>((size_t)1, TotalSurfaces);

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
            // Dedup ALWAYS (correctness: meshlet/simplify need a sane vertex count or they OOM); only the
            // cache/overdraw/fetch reordering is the optional "optimize" pass.
            DeduplicateMeshVertices(M, Progress);
            if (Options.bOptimize)
            {
                OptimizeNewlyImportedMesh(M, Progress);
            }
            // GenerateMeshlets internally runs ComputeMikkTSpaceTangents before packing,
            // and advances StepPerSurface of progress for each surface it meshletizes.
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

        // Distance fields last, and serially over resources: the builder parallelises over its own Z
        // slices, so putting it inside the per-resource ParallelFor above would nest one ParallelFor
        // inside another. It also has to run AFTER GenerateMeshlets, because it voxelises the baked
        // meshlets rather than the import scratch (that is what lets a rebuild work on a loaded asset).
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
