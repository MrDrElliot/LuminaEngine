#include "RuntimePCH.h"
#include "NavMesh.h"

#include "Config/NavigationSettings.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"

// Real Recast/Detour path is gated on LUMINA_HAS_RECAST; otherwise compiles as a no-op shell.
#if defined(LUMINA_HAS_RECAST)
    #include <DetourNavMesh.h>
    #include <DetourNavMeshQuery.h>
    #include <DetourCommon.h>

// Compiling without the define gives a narrower dtPolyRef than Detour's sources, and the mismatch corrupts memory rather than failing to link.
static_assert(sizeof(dtPolyRef) == 8, "Recast must be built with DT_POLYREF64; see Recast.Build.cs.");
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

        // Mirrors the block walk in dtCreateNavMeshData and dtNavMesh::addTile, which widen the header and vert blocks to 8 under DT_POLYREF64.
        int32 TileLinkOffset(const dtMeshHeader* H)
        {
            return Math::AlignUp((int32)sizeof(dtMeshHeader), 8)
                 + Math::AlignUp((int32)sizeof(float) * 3 * H->vertCount, 8)
                 + Math::AlignUp((int32)sizeof(dtPoly) * H->polyCount, 4);
        }

        int32 ExpectedTileBlobSize(const dtMeshHeader* H)
        {
            return TileLinkOffset(H)
                 + Math::AlignUp((int32)sizeof(dtLink)               * H->maxLinkCount, 4)
                 + Math::AlignUp((int32)sizeof(dtPolyDetail)         * H->detailMeshCount, 4)
                 + Math::AlignUp((int32)sizeof(float) * 3            * H->detailVertCount, 4)
                 + Math::AlignUp(4                                   * H->detailTriCount, 4)
                 + Math::AlignUp((int32)sizeof(dtBVNode)             * H->bvNodeCount, 4)
                 + Math::AlignUp((int32)sizeof(dtOffMeshConnection)  * H->offMeshConCount, 4);
        }

        float NextSlotRandom01(void* SlotRng)
        {
            return static_cast<FRandomStream*>(SlotRng)->NextFloat();
        }

        // Detour frees a tile's data itself, so a blob is copied into its allocator before it is handed over.
        uint8* CopyForDetour(const TVector<uint8>& Blob)
        {
            uint8* Owned = (uint8*)dtAlloc(Blob.Num(), DT_ALLOC_PERM);
            if (Owned != nullptr)
            {
                Memory::Memcpy(Owned, Blob.GetData(), Blob.Num());
            }
            return Owned;
        }

        // addTile only checks magic and version, which don't change with ref width or block alignment, so a stale blob would be read at the wrong offsets.
        bool ValidateTileBlob(const TVector<uint8>& Blob, int32 TileX, int32 TileY)
        {
            if (static_cast<size_t>(Blob.Num()) < sizeof(dtMeshHeader))
            {
                LOG_ERROR("NavMesh tile ({}, {}): blob is {} bytes, smaller than a tile header. Re-bake required.",
                    TileX, TileY, Blob.Num());
                return false;
            }

            const dtMeshHeader* H = reinterpret_cast<const dtMeshHeader*>(Blob.GetData());
            if (H->magic != DT_NAVMESH_MAGIC || H->version != DT_NAVMESH_VERSION)
            {
                LOG_ERROR("NavMesh tile ({}, {}): bad magic/version. Re-bake required.", TileX, TileY);
                return false;
            }

            const int32 Expected = ExpectedTileBlobSize(H);
            if (Blob.Num() != Expected)
            {
                LOG_ERROR("NavMesh tile ({}, {}): blob is {} bytes but its header describes {}. This is navmesh data "
                          "baked against a different Detour layout (poly ref width or block alignment). Re-bake required.",
                    TileX, TileY, Blob.Num(), Expected);
                return false;
            }
            return true;
        }

        void PolyCenter(const dtNavMesh& Mesh, dtPolyRef Ref, float* Out)
        {
            const dtMeshTile* Tile = nullptr;
            const dtPoly* Poly = nullptr;
            Mesh.getTileAndPolyByRefUnsafe(Ref, &Tile, &Poly);
            dtVset(Out, 0.0f, 0.0f, 0.0f);
            for (int32 i = 0; i < Poly->vertCount; ++i)
            {
                dtVadd(Out, Out, &Tile->verts[Poly->verts[i] * 3]);
            }
            dtVscale(Out, Out, 1.0f / (float)Math::Max<int32>(1, Poly->vertCount));
        }

        // False once the corridor is full. A poly already near the tail cuts the loop back to it, where a segment doubled back.
        bool AppendCorridor(dtPolyRef* Path, int32& PathLen, int32 MaxPath, const dtPolyRef* Segment, int32 Count)
        {
            constexpr int32 kLoopLookback = 64;
            for (int32 i = 0; i < Count; ++i)
            {
                int32 Seen = -1;
                for (int32 j = PathLen - 1; j >= Math::Max(0, PathLen - kLoopLookback) && Seen < 0; --j)
                {
                    Seen = Path[j] == Segment[i] ? j : -1;
                }
                if (Seen >= 0)
                {
                    PathLen = Seen + 1;
                    continue;
                }
                if (PathLen == MaxPath)
                {
                    return false;
                }
                Path[PathLen++] = Segment[i];
            }
            return true;
        }

        enum class ECorridor : uint8
        {
            Fallback,
            Done,
            Restart,
        };

        // Plans over tile clusters, then has Detour search two segments ahead and keeps one, so its node pool never sees the whole distance.
        template <typename TYield>
        ECorridor FindLongCorridor(const dtNavMesh& Mesh, const FNavClusterGraph& Clusters, dtNavMeshQuery& Query, FNavClusterGraph::FSearchScratch& Scratch,
                                   const dtQueryFilter& Filter, dtPolyRef StartRef, dtPolyRef EndRef, const float* StartPos, const float* EndPos, float SegmentLength,
                                   dtPolyRef* Path, int32& PathLen, int32 MaxPath, dtStatus& OutStatus, const std::atomic<uint32>& Abandon, TYield&& YieldToSwap)
        {
            const TVector<FNavClusterGraph::FStep>& Route = Scratch.Route;
            const FNavClusterGraph::ERoute Planned = Clusters.FindRoute(Mesh, Filter, StartRef, EndRef, Unpack(StartPos), Unpack(EndPos), Abandon, Scratch);
            if (Planned == FNavClusterGraph::ERoute::Abandoned)
            {
                return ECorridor::Restart;
            }
            if (Planned == FNavClusterGraph::ERoute::Failed)
            {
                return ECorridor::Fallback;
            }

            // An unreachable goal is swapped for the nearest point the start can reach, which Detour then walks to in full.
            const bool bReachable = Planned == FNavClusterGraph::ERoute::Reached;
            float GoalPos[3];
            dtVcopy(GoalPos, EndPos);
            const dtPolyRef GoalRef = bReachable ? EndRef : Clusters.NearestPoly(Mesh, Route.Last().Key, Unpack(EndPos));
            if (!bReachable)
            {
                PolyCenter(Mesh, GoalRef, GoalPos);
            }
            const dtStatus Arrived = bReachable ? DT_SUCCESS : (DT_SUCCESS | DT_PARTIAL_RESULT);

            constexpr int32 kSegmentPolys = 1024;
            dtPolyRef Segment[kSegmentPolys];
            dtPolyRef CurrentRef = StartRef;
            float CurrentPos[3];
            dtVcopy(CurrentPos, StartPos);
            int32 Progress = 0;
            PathLen = 0;

            const int32 MaxSegments = 2 * (int32)Route.Num() + 8;
            for (int32 Iteration = 0; Iteration < MaxSegments; ++Iteration)
            {
                if (YieldToSwap())
                {
                    return ECorridor::Restart;
                }

                int32 Target = Progress;
                while (Target + 1 < (int32)Route.Num() && Route[Target].Distance - Route[Progress].Distance < 2.0f * SegmentLength)
                {
                    ++Target;
                }
                const bool bFinal = Target + 1 >= (int32)Route.Num();
                const dtPolyRef TargetRef = bFinal ? GoalRef : Route[Target].EntryRef;
                float TargetPos[3];
                if (bFinal)
                {
                    dtVcopy(TargetPos, GoalPos);
                }
                else
                {
                    PolyCenter(Mesh, TargetRef, TargetPos);
                }

                int32 Count = 0;
                const dtStatus Status = Query.findPath(CurrentRef, TargetRef, CurrentPos, TargetPos, &Filter, Segment, &Count, kSegmentPolys);
                if (dtStatusFailed(Status) || Count == 0)
                {
                    return ECorridor::Fallback;
                }
                const bool bReached = Segment[Count - 1] == TargetRef;
                // Short of a target the clusters say is connected, and not for want of nodes, means the filter disagrees with the plan.
                if (!bReached && (Status & DT_OUT_OF_NODES) == 0)
                {
                    return ECorridor::Fallback;
                }
                if (bFinal && bReached)
                {
                    OutStatus = AppendCorridor(Path, PathLen, MaxPath, Segment, Count) ? Arrived : (DT_SUCCESS | DT_PARTIAL_RESULT | DT_BUFFER_TOO_SMALL);
                    return ECorridor::Done;
                }

                int32 Keep = 1;
                float Covered = 0.0f;
                float Previous[3];
                dtVcopy(Previous, CurrentPos);
                while (Keep < Count && Covered < SegmentLength)
                {
                    float Center[3];
                    PolyCenter(Mesh, Segment[Keep], Center);
                    Covered += dtVdist(Previous, Center);
                    dtVcopy(Previous, Center);
                    ++Keep;
                }
                if (!AppendCorridor(Path, PathLen, MaxPath, Segment, Keep))
                {
                    OutStatus = DT_SUCCESS | DT_PARTIAL_RESULT | DT_BUFFER_TOO_SMALL;
                    return ECorridor::Done;
                }
                CurrentRef = Path[PathLen - 1];
                PolyCenter(Mesh, CurrentRef, CurrentPos);

                int32 Nearest = Progress;
                float NearestDistance = FLT_MAX;
                for (int32 i = Progress; i <= Target; ++i)
                {
                    const float Distance = dtVdist(&Route[i].Center.x, CurrentPos);
                    if (Distance < NearestDistance)
                    {
                        NearestDistance = Distance;
                        Nearest = i;
                    }
                }
                Progress = Math::Min(Math::Max(Nearest, Progress + 1), (int32)Route.Num() - 1);
            }
            return ECorridor::Fallback;
        }
#endif
    }

    FNavMesh::FNavMesh() = default;

    FNavMesh::~FNavMesh()
    {
        Shutdown();
    }

    bool FNavMesh::Initialize(const FVector3& InOrigin, float InTileWorldSize, int32 MaxResidentTiles, int32 MaxPolysPerTile)
    {
        TVector<SNavTileData> None;
        return Initialize(InOrigin, InTileWorldSize, MaxResidentTiles, MaxPolysPerTile, Move(None));
    }

    bool FNavMesh::Initialize(const FVector3& InOrigin, float InTileWorldSize, int32 MaxResidentTiles, int32 MaxPolysPerTile, TVector<SNavTileData>&& Tiles)
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

        if (TileWorldSize <= 0.0f || MaxResidentTiles <= 0)
        {
            LOG_ERROR("FNavMesh::Initialize: invalid layout (TileWorldSize={:.3f}, MaxResidentTiles={}). Refusing to init.", TileWorldSize, MaxResidentTiles);
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            return false;
        }

        dtNavMeshParams Params{};
        Pack(Origin, Params.orig);
        Params.tileWidth  = TileWorldSize;
        Params.tileHeight = TileWorldSize;
        Params.maxTiles   = MaxResidentTiles;
        Params.maxPolys   = MaxPolysPerTile;

        if (dtStatusFailed(NavMesh->init(&Params)))
        {
            LOG_ERROR("FNavMesh::Initialize: dtNavMesh::init failed (TileWorldSize={:.3f}, MaxResidentTiles={}, MaxPolys={}).", TileWorldSize, MaxResidentTiles, MaxPolysPerTile);
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            return false;
        }

        // dtNavMesh::addTile is not thread-safe; the expensive bake work already happened.
        int32 Added = 0;
        int32 Skipped = 0;
        int32 Rejected = 0;
        int32 Stale = 0;
        for (SNavTileData& Tile : Tiles)
        {
            if (Tile.Blob.IsEmpty())
            {
                ++Skipped;
                continue;
            }

            if (!ValidateTileBlob(Tile.Blob, Tile.X, Tile.Y))
            {
                ++Stale;
                continue;
            }

            const int32 Size = Tile.Blob.Num();
            uint8* Owned = CopyForDetour(Tile.Blob);
            if (!Owned)
            {
                ++Rejected;
                continue;
            }

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
        Clusters.Rebuild(*NavMesh);

        if (Stale > 0)
        {
            LOG_ERROR("FNavMesh::Initialize: {} of {} non-empty tiles were baked against a different Detour layout. Re-bake this navmesh.",
                Stale, Added + Rejected + Stale);
        }
        if (Rejected > 0)
        {
            LOG_WARN("FNavMesh::Initialize: dtNavMesh::addTile rejected {} of {} non-empty tiles (likely tile coord collision or maxTiles too small).",
                Rejected, Added + Rejected);
        }
        if (Added == 0 && Skipped + Rejected + Stale > 0)
        {
            LOG_ERROR("FNavMesh::Initialize: no tiles were added (skipped={}, rejected={}, stale={}). NavMesh will not be ready.", Skipped, Rejected, Stale);
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
            return false;
        }

        // Over-provision so contention rarely blocks; each query is a few hundred KB.
        const CNavigationSettings& Settings = *GetDefault<CNavigationSettings>();
        const uint32 PoolSize = (Jobs::IsRuntimeRunning() ? Jobs::GetNumWorkers() : 4u) + (uint32)Math::Max(0, Settings.QueryPoolSlack);
        QueryPool = TVector<FQuerySlot>(PoolSize);
        uint32 ReadyQueries = 0;
        for (uint32 i = 0; i < PoolSize; ++i)
        {
            dtNavMeshQuery* Query = dtAllocNavMeshQuery();
            if (Query && dtStatusSucceed(Query->init(NavMesh, Math::Max(64, Settings.QueryNodePoolSize))))
            {
                QueryPool[i].Query = Query;
                QueryPool[i].Rng.Seed(0, i);
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
            QueryPool.Reset();
            return false;
        }

        bReady = true;
        bDebugCacheDirty = true;
        return true;
#else
        (void)MaxResidentTiles; (void)MaxPolysPerTile; (void)Tiles;
        LOG_ERROR("FNavMesh::Initialize: Recast/Detour not vendored (LUMINA_HAS_RECAST undefined). NavMesh cannot be initialized.");
        bReady = false;
        return false;
#endif
    }

    bool FNavMesh::TryBeginClusterRelabel()
    {
        {
            FQueryScope Topology(*this);
            if (!bReady || !Clusters.NeedsExactLabels() || Clusters.HasPendingLabels())
            {
                return false;
            }
        }
        uint32 Idle = 0;
        return RelabelsInFlight.compare_exchange_strong(Idle, 1, std::memory_order_acq_rel);
    }

    void FNavMesh::RelabelClusters()
    {
        // Normal priority while it holds the topology, or a swap could wait on a thread the scheduler is starving.
        Threading::SetCurrentThreadPriority(EThreadPriority::Normal);
        {
            FQueryScope Topology(*this);
            Clusters.PrepareExactLabels(PendingSwaps);
        }
        Threading::SetCurrentThreadPriority(EThreadPriority::Background);
        RelabelsInFlight.store(0, std::memory_order_release);
    }

    void FNavMesh::AdoptClusterLabels()
    {
        if (Clusters.HasPendingLabels())
        {
            FSwapScope Topology(*this);
            Clusters.AdoptPendingLabels();
        }
    }

    void FNavMesh::Shutdown()
    {
        while (RelabelsInFlight.load(std::memory_order_acquire) != 0)
        {
            Threading::ThreadYield();
        }

#if defined(LUMINA_HAS_RECAST)
        for (FQuerySlot& Slot : QueryPool)
        {
            if (Slot.Query) dtFreeNavMeshQuery(Slot.Query);
        }
        if (NavMesh) dtFreeNavMesh(NavMesh);
#endif
        QueryPool.Reset();
        Clusters.Reset(0);
        NavMesh = nullptr;
        bReady = false;
    }

    void FNavMesh::WaitForSwaps() const
    {
        const volatile uint32* Word = reinterpret_cast<const volatile uint32*>(&PendingSwaps);
        for (uint32 Pending = PendingSwaps.load(std::memory_order_acquire); Pending != 0; Pending = PendingSwaps.load(std::memory_order_acquire))
        {
            Threading::WaitOnAddress32(Word, Pending, 1);
        }
    }

    FNavMesh::FQueryScope::FQueryScope(const FNavMesh& InMesh)
        : Mesh(InMesh)
    {
        Mesh.WaitForSwaps();
        Mesh.TopologyLock.LockShared();
    }

    FNavMesh::FQueryScope::~FQueryScope()
    {
        Mesh.TopologyLock.UnlockShared();
    }

    bool FNavMesh::FQueryScope::YieldToSwap(uint64 Epoch) const
    {
        if (Mesh.PendingSwaps.load(std::memory_order_acquire) == 0)
        {
            return false;
        }
        Mesh.TopologyLock.UnlockShared();
        Mesh.WaitForSwaps();
        Mesh.TopologyLock.LockShared();
        return Mesh.TopologyEpoch.load(std::memory_order_acquire) != Epoch;
    }

    FNavMesh::FSwapScope::FSwapScope(FNavMesh& InMesh)
        : Mesh(InMesh)
    {
        Mesh.PendingSwaps.fetch_add(1, std::memory_order_acq_rel);
        Mesh.TopologyLock.Lock();
    }

    FNavMesh::FSwapScope::~FSwapScope()
    {
        Mesh.TopologyLock.Unlock();
        if (Mesh.PendingSwaps.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            Threading::WakeAllOnAddress32(reinterpret_cast<const volatile uint32*>(&Mesh.PendingSwaps));
        }
    }

    FNavMesh::FAcquiredQuery FNavMesh::AcquireQuery() const
    {
        // Linear CAS scan; spin loop is a safety net for pathological contention.
        const uint32 N = (uint32)QueryPool.Num();
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
            PlatformTime::YieldThread();
        }
        return {};
    }

    bool FNavMesh::ProjectPoint(const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out) const
    {
#if defined(LUMINA_HAS_RECAST)
        FQueryScope Topology(*this);
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
        FQueryScope Topology(*this);
        FAcquiredQuery Q = AcquireQuery();
        if (!Q)
        {
            Out.bQueryUnavailable = true;
            Out.Result = ENavPathResult::QueryUnavailable;
            return false;
        }

        dtQueryFilter F; ApplyFilter(Filter, F);

        float SP[3]; Pack(Start, SP);
        float EP[3]; Pack(End,   EP);
        float Extents[3]; Pack(Filter.QueryExtents, Extents);

        // The settings are clamped to these ceilings rather than sized dynamically, keeping the query heap free.
        constexpr int32 MaxPolysCeiling = 4096;
        constexpr int32 MaxStraightCeiling = 512;
        constexpr float kSegmentTiles = 1.5f;
        constexpr int32 kMaxRestarts = 3;
        const CNavigationSettings& NavSettings = *GetDefault<CNavigationSettings>();
        const int32 MaxPolys = Math::Clamp(NavSettings.MaxPathPolys, 16, MaxPolysCeiling);

        dtPolyRef SRef = 0, ERef = 0;
        float SNear[3], ENear[3];
        dtPolyRef Path[MaxPolysCeiling];
        int32 PathLen = 0;
        dtStatus PathStatus = 0;
        const std::atomic<uint32> NeverAbandon{ 0 };
        for (int32 Attempt = 0; ; ++Attempt)
        {
            Out.Epoch = TopologyEpoch.load(std::memory_order_acquire);
            if (dtStatusFailed(Q.Get()->findNearestPoly(SP, Extents, &F, &SRef, SNear)) || SRef == 0)
            {
                Out.Result = ENavPathResult::StartOffNavMesh;
                return false;
            }
            if (dtStatusFailed(Q.Get()->findNearestPoly(EP, Extents, &F, &ERef, ENear)) || ERef == 0)
            {
                Out.Result = ENavPathResult::EndOffNavMesh;
                return false;
            }

            // A swap landing while a long query stands aside invalidates its refs, so it starts over, a few times at most.
            ECorridor Long = ECorridor::Fallback;
            if (NavSettings.HierarchicalPathDistance > 0.0f && dtVdist2D(SNear, ENear) >= NavSettings.HierarchicalPathDistance)
            {
                Long = FindLongCorridor(*NavMesh, Clusters, *Q.Get(), Q.Slot->ClusterScratch, F, SRef, ERef, SNear, ENear, TileWorldSize * kSegmentTiles, Path, PathLen, MaxPolys, PathStatus,
                    Attempt < kMaxRestarts ? PendingSwaps : NeverAbandon, [&] { return Attempt < kMaxRestarts && Topology.YieldToSwap(Out.Epoch); });
            }
            if (Long == ECorridor::Restart)
            {
                Topology.YieldToSwap(Out.Epoch);
                continue;
            }
            if (Long == ECorridor::Fallback)
            {
                PathStatus = Q.Get()->findPath(SRef, ERef, SNear, ENear, &F, Path, &PathLen, MaxPolys);
            }
            break;
        }

        if (dtStatusFailed(PathStatus) || PathLen == 0)
        {
            Out.Result = ENavPathResult::NoRoute;
            return false;
        }

        // A corridor not ending on the goal poly stopped short, or the caller reads the last corner as the goal.
        Out.bPartial   = (PathStatus & DT_PARTIAL_RESULT) != 0 || Path[PathLen - 1] != ERef;
        Out.bTruncated = (PathStatus & (DT_BUFFER_TOO_SMALL | DT_OUT_OF_NODES)) != 0;

        // Detour gets the cap the caller can store, or the overflow comes back looking like arrival.
        const int32 RequestedCorners = Filter.MaxCorners > 0 ? Filter.MaxCorners : NavSettings.MaxPathCorners;
        const int32 MaxStraight = Math::Clamp(RequestedCorners, 8, MaxStraightCeiling);
        float StraightPath[MaxStraightCeiling * 3];
        uint8 StraightFlags[MaxStraightCeiling];
        dtPolyRef StraightRefs[MaxStraightCeiling];
        int32 StraightCount = 0;
        const dtStatus StraightStatus = Q.Get()->findStraightPath(SNear, ENear, Path, PathLen, StraightPath, StraightFlags, StraightRefs, &StraightCount, MaxStraight);
        if (dtStatusFailed(StraightStatus))
        {
            Out.Result = ENavPathResult::CornersUnavailable;
            return false;
        }
        // Detour reports a filled corner buffer as success, so the straight path silently ends mid-corridor.
        if (StraightStatus & DT_BUFFER_TOO_SMALL)
        {
            Out.bPartial   = true;
            Out.bTruncated = true;
        }

        Out.Corners.Reserve(StraightCount);
        Out.CornerFlags.Reserve(StraightCount);
        for (int32 i = 0; i < StraightCount; ++i)
        {
            Out.Corners.Add(Unpack(&StraightPath[i * 3]));

            uint8 Flags = (uint8)ENavCornerFlag::None;
            if (StraightFlags[i] & DT_STRAIGHTPATH_START)                Flags |= (uint8)ENavCornerFlag::PathStart;
            if (StraightFlags[i] & DT_STRAIGHTPATH_END)                  Flags |= (uint8)ENavCornerFlag::PathEnd;
            if (StraightFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)   Flags |= (uint8)ENavCornerFlag::OffMeshLink;
            Out.CornerFlags.Add(Flags);
        }
        Out.bValid = true;

        // Truncated outranks partial, since a limit cut the route rather than the goal being unreachable.
        Out.Result = Out.bTruncated ? ENavPathResult::Truncated
                   : Out.bPartial   ? ENavPathResult::Partial
                                    : ENavPathResult::Success;
        return true;
#else
        (void)Start; (void)End; (void)Filter;
        Out.Result = ENavPathResult::NavigationCompiledOut;
        return false;
#endif
    }

    void FNavMesh::InvalidateDebugCache()
    {
        bDebugCacheDirty.store(true, std::memory_order_release);
    }

    void FNavMesh::EnsureDebugCache() const
    {
        if (!bDebugCacheDirty.load(std::memory_order_acquire))
        {
            return;
        }

        // Its own lock, because the rebuild writes the caches while the topology lock is only shared.
        TScopeLock<FMutex> Rebuild(DebugCacheLock);
        if (!bDebugCacheDirty.load(std::memory_order_acquire))
        {
            return;
        }

        FQueryScope Topology(*this);
        bDebugCacheDirty.store(false, std::memory_order_release);

        CachedTriVerts.Reset();
        CachedTriAreas.Reset();
        CachedBoundaryVerts.Reset();
        CachedBoundaryAreas.Reset();
        CachedOffMeshVerts.Reset();
        CachedTileBounds.Reset();

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
        CachedTriVerts.Reserve(IntCastChecked<int32>(TriEstimate * 3));
        CachedTriAreas.Reserve(IntCastChecked<int32>(TriEstimate));
        CachedBoundaryVerts.Reserve(IntCastChecked<int32>(EdgeEstimate * 2));
        CachedBoundaryAreas.Reserve(IntCastChecked<int32>(EdgeEstimate));
        CachedOffMeshVerts.Reserve(IntCastChecked<int32>(LinkEstimate * 2));
        CachedTileBounds.Reserve(MaxTiles);

        for (int32 i = 0; i < MaxTiles; ++i)
        {
            const dtMeshTile* Tile = CMesh->getTile(i);
            if (!Tile || !Tile->header) continue;

            FNavTileBounds Tb;
            Tb.Min = FVector3(Tile->header->bmin[0], Tile->header->bmin[1], Tile->header->bmin[2]);
            Tb.Max = FVector3(Tile->header->bmax[0], Tile->header->bmax[1], Tile->header->bmax[2]);
            Tb.X   = Tile->header->x;
            Tb.Y   = Tile->header->y;
            CachedTileBounds.Add(Tb);

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
                    CachedBoundaryVerts.Emplace(A[0], A[1], A[2]);
                    CachedBoundaryVerts.Emplace(B[0], B[1], B[2]);
                    CachedBoundaryAreas.Add(Area);
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
                        CachedTriVerts.Emplace(P[0], P[1], P[2]);
                    }
                    CachedTriAreas.Add(Area);
                }
            }

            for (int c = 0; c < Tile->header->offMeshConCount; ++c)
            {
                const dtOffMeshConnection& Con = Tile->offMeshCons[c];
                CachedOffMeshVerts.Emplace(Con.pos[0], Con.pos[1], Con.pos[2]);
                CachedOffMeshVerts.Emplace(Con.pos[3], Con.pos[4], Con.pos[5]);
            }
        }
#endif
    }

    void FNavMesh::ForEachBoundaryEdge(const FBoundaryEdgeVisitor& Visitor) const
    {
        EnsureDebugCache();
        const int32 N = CachedBoundaryAreas.Num();
        for (int32 i = 0; i < N; ++i)
        {
            Visitor(CachedBoundaryVerts[i * 2 + 0], CachedBoundaryVerts[i * 2 + 1], CachedBoundaryAreas[i]);
        }
    }

    void FNavMesh::ForEachOffMeshLink(const FOffMeshLinkVisitor& Visitor) const
    {
        EnsureDebugCache();
        const int32 N = CachedOffMeshVerts.Num() / 2;
        for (int32 i = 0; i < N; ++i)
        {
            Visitor(CachedOffMeshVerts[i * 2 + 0], CachedOffMeshVerts[i * 2 + 1]);
        }
    }

    void FNavMesh::ForEachLoadedTile(const FTileBoundsVisitor& Visitor) const
    {
        EnsureDebugCache();
        for (const FNavTileBounds& T : CachedTileBounds)
        {
            Visitor(T);
        }
    }

    FNavDebugStats FNavMesh::GetDebugStats() const
    {
        EnsureDebugCache();
        FNavDebugStats S;
        S.LoadedTiles   = CachedTileBounds.Num();
        S.Triangles     = CachedTriAreas.Num();
        S.BoundaryEdges = CachedBoundaryAreas.Num();
        S.OffMeshLinks  = CachedOffMeshVerts.Num() / 2;
        return S;
    }

    void FNavMesh::ForEachTriangle(FTriangleVisitor Visitor) const
    {
        EnsureDebugCache();
        const int32 NumTris = CachedTriAreas.Num();
        for (int32 i = 0; i < NumTris; ++i)
        {
            Visitor(CachedTriVerts[i * 3 + 0], CachedTriVerts[i * 3 + 1], CachedTriVerts[i * 3 + 2], CachedTriAreas[i]);
        }
    }

    void FNavMesh::ParallelForEachTriangle(const FParallelTriangleVisitor& Visitor) const
    {
        EnsureDebugCache();
        const int32 NumTris = CachedTriAreas.Num();
        if (NumTris == 0) return;

        Jobs::ParallelFor((uint32)NumTris, [this, &Visitor](uint32 i)
        {
            Visitor(CachedTriVerts[i * 3 + 0], CachedTriVerts[i * 3 + 1], CachedTriVerts[i * 3 + 2], CachedTriAreas[i]);
        });
    }

    bool FNavMesh::AddTileLocked(uint8* Owned, int32 Size)
    {
#if defined(LUMINA_HAS_RECAST)
        dtTileRef NewRef = 0;
        if (dtStatusFailed(NavMesh->addTile(Owned, Size, DT_TILE_FREE_DATA, 0, &NewRef)))
        {
            dtFree(Owned);
            return false;
        }
        Clusters.AddTile(*NavMesh, *NavMesh->getTileByRef(NewRef));
        return true;
#else
        (void)Owned; (void)Size;
        return false;
#endif
    }

    bool FNavMesh::RemoveTileLocked(int32 TileX, int32 TileY)
    {
#if defined(LUMINA_HAS_RECAST)
        if (!NavMesh)
        {
            return false;
        }
        const dtTileRef OldRef = NavMesh->getTileRefAt(TileX, TileY, 0);
        if (OldRef != 0)
        {
            NavMesh->removeTile(OldRef, nullptr, nullptr);
            Clusters.RemoveTile(*NavMesh, NavMesh->decodePolyIdTile(OldRef), TileX, TileY);
        }
        return true;
#else
        (void)TileX; (void)TileY;
        return false;
#endif
    }

    void FNavMesh::RecordTileChangeLocked(int32 TileX, int32 TileY)
    {
        const uint64 Epoch = TopologyEpoch.fetch_add(1, std::memory_order_release) + 1;
        FTileChange& Slot = ChangeRing[Epoch % (uint64)ChangeRingSize];
        Slot.Epoch = Epoch;
        Slot.X = TileX;
        Slot.Y = TileY;
    }

    bool FNavMesh::HasTileChangedSince(uint64 SinceEpoch, const FVector3& Min, const FVector3& Max) const
    {
        FQueryScope Topology(*this);

        const uint64 Current = TopologyEpoch.load(std::memory_order_acquire);
        if (SinceEpoch >= Current)
        {
            return false;
        }
        if (Current - SinceEpoch > (uint64)ChangeRingSize || TileWorldSize <= 0.0f)
        {
            return true;
        }

        for (uint64 E = SinceEpoch + 1; E <= Current; ++E)
        {
            const FTileChange& Slot = ChangeRing[E % (uint64)ChangeRingSize];
            if (Slot.Epoch != E)
            {
                return true;
            }

            const float TileMinX = Origin.x + (float)Slot.X * TileWorldSize;
            const float TileMinZ = Origin.z + (float)Slot.Y * TileWorldSize;
            if (Max.x >= TileMinX && Min.x <= TileMinX + TileWorldSize
             && Max.z >= TileMinZ && Min.z <= TileMinZ + TileWorldSize)
            {
                return true;
            }
        }
        return false;
    }

    bool FNavMesh::RebuildTile(int32 TileX, int32 TileY, TVector<uint8>&& NewBlob)
    {
        return RebuildTiles({ FTileSwap{ TileX, TileY, &NewBlob } }) == 1;
    }

    int32 FNavMesh::RebuildTiles(const TVector<FTileSwap>& Swaps)
    {
        LUMINA_PROFILE_SCOPE();

#if defined(LUMINA_HAS_RECAST)
        if (!NavMesh || Swaps.IsEmpty())
        {
            return 0;
        }

        // Copied into Detour's allocator before the hold, so readers wait only for the relinking, and a rejected blob keeps its old tile.
        TVector<uint8*> Owned(Swaps.Num(), nullptr);
        TVector<uint8> Accepted(Swaps.Num(), 1);
        for (int32 i = 0; i < Swaps.Num(); ++i)
        {
            const TVector<uint8>* Blob = Swaps[i].Blob;
            if (Blob == nullptr || Blob->IsEmpty())
            {
                continue;
            }
            Owned[i] = ValidateTileBlob(*Blob, Swaps[i].X, Swaps[i].Y) ? CopyForDetour(*Blob) : nullptr;
            Accepted[i] = Owned[i] != nullptr;
        }

        int32 Landed = 0;
        {
            FSwapScope Topology(*this);
            for (int32 i = 0; i < Swaps.Num(); ++i)
            {
                if (!Accepted[i])
                {
                    continue;
                }
                RemoveTileLocked(Swaps[i].X, Swaps[i].Y);
                if (Owned[i] == nullptr || AddTileLocked(Owned[i], Swaps[i].Blob->Num()))
                {
                    ++Landed;
                }
                else
                {
                    LOG_ERROR("FNavMesh::RebuildTiles: tile ({}, {}) was removed but Detour rejected its rebuilt blob; the area is now unpathable until it is retried.",
                        Swaps[i].X, Swaps[i].Y);
                }
                RecordTileChangeLocked(Swaps[i].X, Swaps[i].Y);
            }
        }

        InvalidateDebugCache();
        return Landed;
#else
        (void)Swaps;
        return 0;
#endif
    }

    bool FNavMesh::AddTile(int32 TileX, int32 TileY, const TVector<uint8>& Blob)
    {
        LUMINA_PROFILE_SCOPE();

#if defined(LUMINA_HAS_RECAST)
        if (!NavMesh || Blob.IsEmpty() || !ValidateTileBlob(Blob, TileX, TileY))
        {
            return false;
        }
        uint8* Owned = CopyForDetour(Blob);
        if (Owned == nullptr)
        {
            return false;
        }
#else
        uint8* Owned = nullptr;
#endif

        bool bResult = false;
        {
            FSwapScope Topology(*this);
            bResult = AddTileLocked(Owned, Blob.Num());
            if (bResult)
            {
                RecordTileChangeLocked(TileX, TileY);
            }
        }

        if (bResult)
        {
            InvalidateDebugCache();
        }
        return bResult;
    }

    bool FNavMesh::RemoveTile(int32 TileX, int32 TileY)
    {
        LUMINA_PROFILE_SCOPE();

        bool bResult = false;
        {
            FSwapScope Topology(*this);
            bResult = RemoveTileLocked(TileX, TileY);
            RecordTileChangeLocked(TileX, TileY);
        }

        InvalidateDebugCache();
        return bResult;
    }

    bool FNavMesh::HasTile(int32 TileX, int32 TileY) const
    {
#if defined(LUMINA_HAS_RECAST)
        FQueryScope Topology(*this);
        return NavMesh && NavMesh->getTileRefAt(TileX, TileY, 0) != 0;
#else
        (void)TileX; (void)TileY;
        return false;
#endif
    }

    int32 FNavMesh::GetResidentTileCount() const
    {
#if defined(LUMINA_HAS_RECAST)
        FQueryScope Topology(*this);
        if (!NavMesh)
        {
            return 0;
        }
        int32 Count = 0;
        const dtNavMesh* CMesh = NavMesh;
        for (int32 i = 0, N = CMesh->getMaxTiles(); i < N; ++i)
        {
            const dtMeshTile* Tile = CMesh->getTile(i);
            if (Tile && Tile->header)
            {
                ++Count;
            }
        }
        return Count;
#else
        return 0;
#endif
    }

    bool FNavMesh::FindRandomPoint(const FVector3& Center, float Radius, const FNavQueryFilter& Filter, FVector3& Out) const
    {
#if defined(LUMINA_HAS_RECAST)
        FQueryScope Topology(*this);
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

        dtPolyRef RandomRef = 0;
        float RandomPt[3];
        if (dtStatusFailed(Q.Get()->findRandomPointAroundCircle(StartRef, Snapped, Radius, &F, NextSlotRandom01, &Q.Slot->Rng, &RandomRef, RandomPt)) || RandomRef == 0)
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

    bool FNavMesh::Raycast(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, SNavRaycastResult& Out) const
    {
        Out = {};
        Out.Point = End;

#if defined(LUMINA_HAS_RECAST)
        FQueryScope Topology(*this);
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

        // Detour reports an unobstructed walk as T = FLT_MAX.
        Out.bHit   = T < 1.0f;
        Out.T      = Out.bHit ? T : 1.0f;
        Out.Point  = Out.bHit ? Math::Mix(Start, End, T) : End;
        Out.Normal = Out.bHit ? Unpack(Normal) : FVector3(0.0f);
        return true;
#else
        (void)Start; (void)End; (void)Filter;
        return false;
#endif
    }

    namespace NavMeshTesting
    {
        bool TileLinksAreAligned(const TVector<uint8>& Blob)
        {
#if defined(LUMINA_HAS_RECAST)
            if (static_cast<size_t>(Blob.Num()) < sizeof(dtMeshHeader))
            {
                return false;
            }
            const dtMeshHeader* H = reinterpret_cast<const dtMeshHeader*>(Blob.GetData());
            const int32 Expected = ExpectedTileBlobSize(H);
            if (Blob.Num() != Expected)
            {
                return false;
            }

            return (TileLinkOffset(H) % (int32)alignof(dtLink)) == 0;
#else
            (void)Blob;
            return true;
#endif
        }
    }
}
