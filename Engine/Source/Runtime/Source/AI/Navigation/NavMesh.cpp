#include "RuntimePCH.h"
#include "NavMesh.h"

#include "TaskSystem/TaskSystem.h"

// Real Recast/Detour path is gated on LUMINA_HAS_RECAST; otherwise compiles as a no-op shell.
#if defined(LUMINA_HAS_RECAST)
    #include <DetourNavMesh.h>
    #include <DetourNavMeshQuery.h>
    #include <DetourCommon.h>
#include "Log/Log.h"
#endif

namespace Lumina
{
    namespace
    {
        FORCEINLINE void Pack(const FVector3& In, float* Out) { Out[0] = In.x; Out[1] = In.y; Out[2] = In.z; }
        FORCEINLINE FVector3 Unpack(const float* In) { return FVector3(In[0], In[1], In[2]); }

#if defined(LUMINA_HAS_RECAST)
        void ApplyFilter(const FNavQueryFilter& In, dtQueryFilter& Out)
        {
            Out.setIncludeFlags(In.IncludeFlags);
            Out.setExcludeFlags(In.ExcludeFlags);
            for (int32 i = 0; i < 64; ++i)
            {
                Out.setAreaCost(i, In.AreaCost[i]);
            }
        }
#endif
    }

    FNavMesh::FNavMesh() = default;

    FNavMesh::~FNavMesh()
    {
        Shutdown();
    }

    bool FNavMesh::Initialize(const FVector3& InOrigin, float InTileWorldSize, int32 MaxTiles, int32 MaxPolysPerTile, TVector<FNavTileData>&& Tiles)
    {
        Shutdown();

        Origin = InOrigin;
        TileWorldSize = InTileWorldSize;

#if defined(LUMINA_HAS_RECAST)
        NavMesh = dtAllocNavMesh();
        if (!NavMesh)
        {
            LOG_ERROR("FNavMesh::Initialize: dtAllocNavMesh returned null (out of memory).");
            return false;
        }

        if (TileWorldSize <= 0.0f || MaxTiles <= 0)
        {
            LOG_ERROR("FNavMesh::Initialize: invalid layout (TileWorldSize={:.3f}, MaxTiles={}). Refusing to init.", TileWorldSize, MaxTiles);
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            return false;
        }

        dtNavMeshParams Params{};
        Pack(Origin, Params.orig);
        Params.tileWidth  = TileWorldSize;
        Params.tileHeight = TileWorldSize;
        Params.maxTiles   = MaxTiles;
        Params.maxPolys   = MaxPolysPerTile;

        if (dtStatusFailed(NavMesh->init(&Params)))
        {
            LOG_ERROR("FNavMesh::Initialize: dtNavMesh::init failed (TileWorldSize={:.3f}, MaxTiles={}, MaxPolys={}).", TileWorldSize, MaxTiles, MaxPolysPerTile);
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            return false;
        }

        // dtNavMesh::addTile is not thread-safe; the expensive bake work already happened.
        int32 Added = 0;
        int32 Skipped = 0;
        int32 Rejected = 0;
        for (FNavTileData& Tile : Tiles)
        {
            if (Tile.Blob.empty())
            {
                ++Skipped;
                continue;
            }

            const size_t Size = Tile.Blob.size();
            uint8* Owned = (uint8*)dtAlloc((int)Size, DT_ALLOC_PERM);
            if (!Owned)
            {
                ++Rejected;
                continue;
            }
            memcpy(Owned, Tile.Blob.data(), Size);

            dtTileRef Ref = 0;
            const dtStatus Status = NavMesh->addTile(Owned, (int)Size, DT_TILE_FREE_DATA, 0, &Ref);
            if (dtStatusFailed(Status))
            {
                dtFree(Owned);
                ++Rejected;
            }
            else
            {
                ++Added;
            }
        }
        if (Rejected > 0)
        {
            LOG_WARN("FNavMesh::Initialize: dtNavMesh::addTile rejected {} of {} non-empty tiles (likely tile coord collision or maxTiles too small).",
                Rejected, Added + Rejected);
        }
        if (Added == 0 && (int32)Tiles.size() > 0)
        {
            LOG_ERROR("FNavMesh::Initialize: no tiles were added (skipped={}, rejected={}). NavMesh will not be ready.", Skipped, Rejected);
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            return false;
        }

        // Over-provision so contention rarely blocks; each query is a few hundred KB.
        const uint32 PoolSize = (GTaskSystem ? GTaskSystem->GetNumWorkers() : 4u) + 2u;
        QueryPool = TVector<FQuerySlot>(PoolSize);
        uint32 ReadyQueries = 0;
        for (uint32 i = 0; i < PoolSize; ++i)
        {
            dtNavMeshQuery* Query = dtAllocNavMeshQuery();
            if (Query && dtStatusSucceed(Query->init(NavMesh, 2048)))
            {
                QueryPool[i].Query = Query;
                ++ReadyQueries;
            }
            else if (Query)
            {
                dtFreeNavMeshQuery(Query);
            }
        }
        if (ReadyQueries == 0)
        {
            LOG_ERROR("FNavMesh::Initialize: no dtNavMeshQuery instances initialized; pathfinding will always return false.");
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            QueryPool.clear();
            return false;
        }

        bReady = true;
        RefreshTriangleCache();
        return true;
#else
        (void)MaxTiles; (void)MaxPolysPerTile; (void)Tiles;
        LOG_ERROR("FNavMesh::Initialize: Recast/Detour not vendored (LUMINA_HAS_RECAST undefined). NavMesh cannot be initialized.");
        bReady = false;
        return false;
#endif
    }

    void FNavMesh::Shutdown()
    {
#if defined(LUMINA_HAS_RECAST)
        for (FQuerySlot& Slot : QueryPool)
        {
            if (Slot.Query) dtFreeNavMeshQuery(Slot.Query);
        }
        if (NavMesh) dtFreeNavMesh(NavMesh);
#endif
        QueryPool.clear();
        NavMesh = nullptr;
        bReady = false;
    }

    FNavMesh::FAcquiredQuery FNavMesh::AcquireQuery() const
    {
        // Linear CAS scan; spin loop is a safety net for pathological contention.
        const uint32 N = (uint32)QueryPool.size();
        if (N == 0) return {};

        for (int32 Attempt = 0; Attempt < 8; ++Attempt)
        {
            for (uint32 i = 0; i < N; ++i)
            {
                if (!QueryPool[i].Query) continue;
                bool Expected = false;
                if (QueryPool[i].Busy.compare_exchange_strong(Expected, true, std::memory_order_acq_rel))
                {
                    return FAcquiredQuery{ &QueryPool[i] };
                }
            }
            std::this_thread::yield();
        }
        return {};
    }

    bool FNavMesh::ProjectPoint(const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out) const
    {
#if defined(LUMINA_HAS_RECAST)
        FAcquiredQuery Q = AcquireQuery();
        if (!Q) return false;

        dtQueryFilter F; ApplyFilter(Filter, F);
        float P[3]; Pack(World, P);
        float E[3]; Pack(Extents, E);
        float Nearest[3];
        dtPolyRef Ref = 0;
        if (dtStatusFailed(Q.Get()->findNearestPoly(P, E, &F, &Ref, Nearest)) || Ref == 0)
        {
            return false;
        }
        Out = Unpack(Nearest);
        return true;
#else
        (void)World; (void)Extents; (void)Filter; (void)Out;
        return false;
#endif
    }

    bool FNavMesh::FindPath(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out) const
    {
        Out = {};

#if defined(LUMINA_HAS_RECAST)
        FAcquiredQuery Q = AcquireQuery();
        if (!Q) return false;

        dtQueryFilter F; ApplyFilter(Filter, F);

        float SP[3]; Pack(Start, SP);
        float EP[3]; Pack(End,   EP);
        float Extents[3]; Pack(Filter.QueryExtents, Extents);

        dtPolyRef SRef = 0, ERef = 0;
        float SNear[3], ENear[3];
        if (dtStatusFailed(Q.Get()->findNearestPoly(SP, Extents, &F, &SRef, SNear)) || SRef == 0) return false;
        if (dtStatusFailed(Q.Get()->findNearestPoly(EP, Extents, &F, &ERef, ENear)) || ERef == 0) return false;

        constexpr int32 MaxPolys = 256;
        dtPolyRef Path[MaxPolys];
        int32 PathLen = 0;
        const dtStatus PathStatus = Q.Get()->findPath(SRef, ERef, SNear, ENear, &F, Path, &PathLen, MaxPolys);
        if (dtStatusFailed(PathStatus) || PathLen == 0) return false;

        Out.bPartial = (PathStatus & DT_PARTIAL_RESULT) != 0;

        constexpr int32 MaxStraight = 256;
        float StraightPath[MaxStraight * 3];
        uint8 StraightFlags[MaxStraight];
        dtPolyRef StraightRefs[MaxStraight];
        int32 StraightCount = 0;
        if (dtStatusFailed(Q.Get()->findStraightPath(SNear, ENear, Path, PathLen, StraightPath, StraightFlags, StraightRefs, &StraightCount, MaxStraight)))
        {
            return false;
        }

        Out.Corners.reserve(StraightCount);
        for (int32 i = 0; i < StraightCount; ++i)
        {
            Out.Corners.push_back(Unpack(&StraightPath[i * 3]));
        }
        Out.bValid = true;
        return true;
#else
        (void)Start; (void)End; (void)Filter;
        return false;
#endif
    }

    void FNavMesh::RefreshTriangleCache()
    {
        CachedTriVerts.clear();
        CachedTriAreas.clear();
        CachedBoundaryVerts.clear();
        CachedBoundaryAreas.clear();
        CachedOffMeshVerts.clear();
        CachedTileBounds.clear();

#if defined(LUMINA_HAS_RECAST)
        if (!NavMesh) return;

        const dtNavMesh* CMesh = NavMesh;
        const int32 MaxTiles = CMesh->getMaxTiles();

        size_t TriEstimate = 0;
        size_t EdgeEstimate = 0;
        size_t LinkEstimate = 0;
        for (int32 i = 0; i < MaxTiles; ++i)
        {
            const dtMeshTile* Tile = CMesh->getTile(i);
            if (!Tile || !Tile->header) continue;
            TriEstimate  += (size_t)Tile->header->detailTriCount;
            EdgeEstimate += (size_t)Tile->header->polyCount * 4;
            LinkEstimate += (size_t)Tile->header->offMeshConCount;
        }
        CachedTriVerts.reserve(TriEstimate * 3);
        CachedTriAreas.reserve(TriEstimate);
        CachedBoundaryVerts.reserve(EdgeEstimate * 2);
        CachedBoundaryAreas.reserve(EdgeEstimate);
        CachedOffMeshVerts.reserve(LinkEstimate * 2);
        CachedTileBounds.reserve(MaxTiles);

        for (int32 i = 0; i < MaxTiles; ++i)
        {
            const dtMeshTile* Tile = CMesh->getTile(i);
            if (!Tile || !Tile->header) continue;

            FNavTileBounds Tb;
            Tb.Min = FVector3(Tile->header->bmin[0], Tile->header->bmin[1], Tile->header->bmin[2]);
            Tb.Max = FVector3(Tile->header->bmax[0], Tile->header->bmax[1], Tile->header->bmax[2]);
            Tb.X   = Tile->header->x;
            Tb.Y   = Tile->header->y;
            CachedTileBounds.push_back(Tb);

            for (int p = 0; p < Tile->header->polyCount; ++p)
            {
                const dtPoly& Poly = Tile->polys[p];
                if (Poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;

                const uint8 Area = Poly.getArea();

                // Outer perimeter only. neis[j]==0 -> hard boundary; nonzero (incl. DT_EXT_LINK) -> internal/cross-tile.
                for (int j = 0; j < Poly.vertCount; ++j)
                {
                    if (Poly.neis[j] != 0) continue;
                    const float* A = &Tile->verts[Poly.verts[j] * 3];
                    const float* B = &Tile->verts[Poly.verts[(j + 1) % Poly.vertCount] * 3];
                    CachedBoundaryVerts.emplace_back(A[0], A[1], A[2]);
                    CachedBoundaryVerts.emplace_back(B[0], B[1], B[2]);
                    CachedBoundaryAreas.push_back(Area);
                }

                const dtPolyDetail& Detail = Tile->detailMeshes[p];
                for (int t = 0; t < Detail.triCount; ++t)
                {
                    const uint8* Tri = &Tile->detailTris[(Detail.triBase + t) * 4];
                    for (int k = 0; k < 3; ++k)
                    {
                        const uint8 Idx = Tri[k];
                        const float* P = (Idx < Poly.vertCount)
                            ? &Tile->verts[Poly.verts[Idx] * 3]
                            : &Tile->detailVerts[(Detail.vertBase + (Idx - Poly.vertCount)) * 3];
                        CachedTriVerts.emplace_back(P[0], P[1], P[2]);
                    }
                    CachedTriAreas.push_back(Area);
                }
            }

            for (int c = 0; c < Tile->header->offMeshConCount; ++c)
            {
                const dtOffMeshConnection& Con = Tile->offMeshCons[c];
                CachedOffMeshVerts.emplace_back(Con.pos[0], Con.pos[1], Con.pos[2]);
                CachedOffMeshVerts.emplace_back(Con.pos[3], Con.pos[4], Con.pos[5]);
            }
        }
#endif
    }

    void FNavMesh::ForEachBoundaryEdge(const FBoundaryEdgeVisitor& Visitor) const
    {
        const size_t N = CachedBoundaryAreas.size();
        for (size_t i = 0; i < N; ++i)
        {
            Visitor(CachedBoundaryVerts[i * 2 + 0], CachedBoundaryVerts[i * 2 + 1], CachedBoundaryAreas[i]);
        }
    }

    void FNavMesh::ForEachOffMeshLink(const FOffMeshLinkVisitor& Visitor) const
    {
        const size_t N = CachedOffMeshVerts.size() / 2;
        for (size_t i = 0; i < N; ++i)
        {
            Visitor(CachedOffMeshVerts[i * 2 + 0], CachedOffMeshVerts[i * 2 + 1]);
        }
    }

    void FNavMesh::ForEachLoadedTile(const FTileBoundsVisitor& Visitor) const
    {
        for (const FNavTileBounds& T : CachedTileBounds)
        {
            Visitor(T);
        }
    }

    FNavDebugStats FNavMesh::GetDebugStats() const
    {
        FNavDebugStats S;
        S.LoadedTiles   = (int32)CachedTileBounds.size();
        S.Triangles     = (int32)CachedTriAreas.size();
        S.BoundaryEdges = (int32)CachedBoundaryAreas.size();
        S.OffMeshLinks  = (int32)(CachedOffMeshVerts.size() / 2);
        return S;
    }

    void FNavMesh::ForEachTriangle(FTriangleVisitor Visitor) const
    {
        const size_t NumTris = CachedTriAreas.size();
        for (size_t i = 0; i < NumTris; ++i)
        {
            Visitor(CachedTriVerts[i * 3 + 0], CachedTriVerts[i * 3 + 1], CachedTriVerts[i * 3 + 2], CachedTriAreas[i]);
        }
    }

    void FNavMesh::ParallelForEachTriangle(const FParallelTriangleVisitor& Visitor) const
    {
        const size_t NumTris = CachedTriAreas.size();
        if (NumTris == 0) return;

        Task::ParallelFor((uint32)NumTris, [this, &Visitor](uint32 i)
        {
            Visitor(CachedTriVerts[i * 3 + 0], CachedTriVerts[i * 3 + 1], CachedTriVerts[i * 3 + 2], CachedTriAreas[i]);
        });
    }

    bool FNavMesh::RebuildTile(int32 TileX, int32 TileY, TVector<uint8>&& NewBlob)
    {
        LUMINA_PROFILE_SCOPE();
        
#if defined(LUMINA_HAS_RECAST)
        if (!NavMesh)
        {
            return false;
        }

        const dtTileRef OldRef = NavMesh->getTileRefAt(TileX, TileY, 0);
        if (OldRef != 0)
        {
            NavMesh->removeTile(OldRef, nullptr, nullptr);
        }

        if (NewBlob.empty())
        {
            return true; // intentionally empty
        }

        const size_t Size = NewBlob.size();
        uint8* Owned = (uint8*)dtAlloc((int)Size, DT_ALLOC_PERM);
        if (!Owned)
        {
            return false;
        }
        memcpy(Owned, NewBlob.data(), Size);

        dtTileRef NewRef = 0;
        if (dtStatusFailed(NavMesh->addTile(Owned, (int)Size, DT_TILE_FREE_DATA, 0, &NewRef)))
        {
            dtFree(Owned);
            return false;
        }
        // Full refresh; per-tile patching isn't worth the complexity.
        RefreshTriangleCache();
        return true;
#else
        (void)TileX; (void)TileY; (void)NewBlob;
        return false;
#endif
    }

    bool FNavMesh::FindRandomPoint(const FVector3& Center, float Radius, const FNavQueryFilter& Filter, FVector3& Out) const
    {
#if defined(LUMINA_HAS_RECAST)
        FAcquiredQuery Q = AcquireQuery();
        if (!Q) return false;

        dtQueryFilter F; ApplyFilter(Filter, F);

        float CP[3]; Pack(Center, CP);
        float Extents[3]; Pack(Filter.QueryExtents, Extents);

        dtPolyRef StartRef = 0;
        float Snapped[3];
        if (dtStatusFailed(Q.Get()->findNearestPoly(CP, Extents, &F, &StartRef, Snapped)) || StartRef == 0)
        {
            return false;
        }

        // Thread-local xorshift; cheap RNG for Detour's sampling callback.
        static thread_local uint32 RngState = 0xDEADBEEF;
        auto Rand01 = []() -> float
        {
            uint32 X = RngState ? RngState : 1u;
            X ^= X << 13; X ^= X >> 17; X ^= X << 5;
            RngState = X;
            return (X & 0x00FFFFFFu) * (1.0f / 16777216.0f);
        };

        dtPolyRef RandomRef = 0;
        float RandomPt[3];
        if (dtStatusFailed(Q.Get()->findRandomPointAroundCircle(StartRef, Snapped, Radius, &F, Rand01, &RandomRef, RandomPt)) || RandomRef == 0)
        {
            return false;
        }
        Out = Unpack(RandomPt);
        return true;
#else
        (void)Center; (void)Radius; (void)Filter; (void)Out;
        return false;
#endif
    }

    bool FNavMesh::Raycast(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FVector3& HitOut) const
    {
#if defined(LUMINA_HAS_RECAST)
        FAcquiredQuery Q = AcquireQuery();
        if (!Q) return false;

        dtQueryFilter F; ApplyFilter(Filter, F);
        float SP[3]; Pack(Start, SP);
        float EP[3]; Pack(End,   EP);
        float Extents[3]; Pack(Filter.QueryExtents, Extents);

        dtPolyRef SRef = 0;
        float SNear[3];
        if (dtStatusFailed(Q.Get()->findNearestPoly(SP, Extents, &F, &SRef, SNear)) || SRef == 0) return false;

        float T = 0.0f;
        float Normal[3];
        dtPolyRef PathRefs[64];
        int32 PathCount = 0;
        if (dtStatusFailed(Q.Get()->raycast(SRef, SNear, EP, &F, &T, Normal, PathRefs, &PathCount, 64)))
        {
            return false;
        }
        HitOut = (T >= 1.0f) ? End : Math::Mix(Start, End, T);
        return true;
#else
        (void)Start; (void)End; (void)Filter; (void)HitOut;
        return false;
#endif
    }
}
