#include "RuntimePCH.h"
#include "NavClusterGraph.h"

#if defined(LUMINA_HAS_RECAST)
    #include <DetourNavMesh.h>
    #include <DetourNavMeshQuery.h>
#endif

namespace Lumina
{
#if defined(LUMINA_HAS_RECAST)
    namespace
    {
        constexpr uint16 kNoCluster = UINT16_MAX;
        constexpr uint64 kNoKey = UINT64_MAX;
        constexpr int32 kMaxExpansions = 1 << 17;
        constexpr int32 kMaxTilesPerCell = 32;

        uint64 PackKey(uint32 TileIndex, uint16 Cluster)
        {
            return ((uint64)TileIndex << 16) | Cluster;
        }

        uint32 TileOfKey(uint64 Key)
        {
            return (uint32)(Key >> 16);
        }

        uint16 ClusterOfKey(uint64 Key)
        {
            return (uint16)(Key & 0xFFFF);
        }

        FVector3 PolyCenter(const dtMeshTile& Tile, const dtPoly& Poly)
        {
            FVector3 Sum(0.0f);
            for (int32 i = 0; i < Poly.vertCount; ++i)
            {
                const float* V = &Tile.verts[Poly.verts[i] * 3];
                Sum += FVector3(V[0], V[1], V[2]);
            }
            return Sum / (float)Math::Max<int32>(1, Poly.vertCount);
        }

        bool IsBorderPoly(const dtPoly& Poly)
        {
            if (Poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            {
                return true;
            }
            for (int32 i = 0; i < Poly.vertCount; ++i)
            {
                if (Poly.neis[i] & DT_EXT_LINK)
                {
                    return true;
                }
            }
            return false;
        }

        template <typename TIndex>
        TIndex FindRoot(TVector<TIndex>& Parent, TIndex Index)
        {
            while (Parent[Index] != Index)
            {
                Parent[Index] = Parent[Parent[Index]];
                Index = Parent[Index];
            }
            return Index;
        }

        bool PassesFilter(const dtQueryFilter& Filter, uint16 Flags)
        {
            return (Flags & Filter.getIncludeFlags()) != 0 && (Flags & Filter.getExcludeFlags()) == 0;
        }
    }

    void FNavClusterGraph::Reset(int32 MaxTiles)
    {
        Tiles.Reset();
        Tiles.SetNum(Math::Max(0, MaxTiles));
        Labels = FLabels{};
        Labels.FirstId.Init(0, Tiles.Num());
        {
            FScopeLock Lock(PendingLock);
            PendingLabels = FLabels{};
            PendingVersion = 0;
        }
        ++Version;
    }

    void FNavClusterGraph::Rebuild(const dtNavMesh& Mesh)
    {
        Reset(Mesh.getMaxTiles());
        for (int32 i = 0; i < Mesh.getMaxTiles(); ++i)
        {
            if (const dtMeshTile* Tile = Mesh.getTile(i); Tile->header != nullptr)
            {
                BuildClusters(Mesh, *Tile);
            }
        }
        for (int32 i = 0; i < Mesh.getMaxTiles(); ++i)
        {
            if (const dtMeshTile* Tile = Mesh.getTile(i); Tile->header != nullptr)
            {
                BuildEdges(Mesh, *Tile);
            }
        }
        LabelsExactAt = Version;
    }

    void FNavClusterGraph::AddTile(const dtNavMesh& Mesh, const dtMeshTile& Tile)
    {
        AdoptPendingLabels();
        BuildClusters(Mesh, Tile);
        BuildNeighborEdges(Mesh, Tile.header->x, Tile.header->y);

        // A tile arriving only adds connections, which the merges already record exactly.
        const bool bWasExact = LabelsExactAt == Version;
        ++Version;
        LabelsExactAt = bWasExact ? Version : LabelsExactAt;
    }

    void FNavClusterGraph::RemoveTile(const dtNavMesh& Mesh, uint32 TileIndex, int32 TileX, int32 TileY)
    {
        AdoptPendingLabels();
        if (TileIndex < (uint32)Tiles.Num())
        {
            Tiles[TileIndex] = FTileClusters{};
        }
        BuildNeighborEdges(Mesh, TileX, TileY);
        ++Version;
    }

    void FNavClusterGraph::Union(uint32 A, uint32 B)
    {
        A = FindRoot<uint32>(Labels.Parent, A);
        B = FindRoot<uint32>(Labels.Parent, B);
        if (A == B)
        {
            return;
        }
        if (Labels.Size[A] < Labels.Size[B])
        {
            ElementOps::SwapElements(A, B);
        }
        Labels.Parent[B] = A;
        Labels.Size[A] += Labels.Size[B];
    }

    uint32 FNavClusterGraph::ComponentOf(uint64 Key) const
    {
        uint32 Id = Labels.FirstId[TileOfKey(Key)] + ClusterOfKey(Key);
        while (Labels.Parent[Id] != Id)
        {
            Id = Labels.Parent[Id];
        }
        return Id;
    }

    void FNavClusterGraph::PrepareExactLabels(const std::atomic<uint32>& Abandon) const
    {
        constexpr size_t kAbandonCheckTiles = 64;
        FLabels Fresh;
        Fresh.FirstId.Init(0, Tiles.Num());
        uint32 Count = 0;
        for (int32 t = 0; t < Tiles.Num(); ++t)
        {
            Fresh.FirstId[t] = Count;
            Count += (uint32)Tiles[t].Centers.Num();
        }
        Fresh.Parent.SetNum(Count);
        Algo::Iota(Fresh.Parent.begin(), Fresh.Parent.end(), 0u);
        for (int32 t = 0; t < Tiles.Num(); ++t)
        {
            // A swap waiting would change the graph and throw this result away, so it gets the lock back now.
            if (t % kAbandonCheckTiles == 0 && Abandon.load(std::memory_order_relaxed) != 0)
            {
                return;
            }
            const FTileClusters& Tile = Tiles[t];
            for (int32 c = 0; c < Tile.Centers.Num(); ++c)
            {
                for (uint32 e = Tile.EdgeStart[c]; e < Tile.EdgeStart[c + 1]; ++e)
                {
                    const uint64 Key = Tile.Edges[e].Key;
                    Fresh.Parent[FindRoot<uint32>(Fresh.Parent, Fresh.FirstId[t] + (uint32)c)] = FindRoot<uint32>(Fresh.Parent, Fresh.FirstId[TileOfKey(Key)] + ClusterOfKey(Key));
                }
            }
        }
        Fresh.Size.Init(0, Count);
        for (uint32 i = 0; i < Count; ++i)
        {
            Fresh.Parent[i] = FindRoot<uint32>(Fresh.Parent, i);
            ++Fresh.Size[Fresh.Parent[i]];
        }

        FScopeLock Lock(PendingLock);
        PendingLabels = Move(Fresh);
        PendingVersion = Version;
    }

    bool FNavClusterGraph::HasPendingLabels() const
    {
        FScopeLock Lock(PendingLock);
        return PendingVersion != 0;
    }

    void FNavClusterGraph::AdoptPendingLabels()
    {
        FScopeLock Lock(PendingLock);
        if (PendingVersion != 0 && PendingVersion == Version)
        {
            Labels = Move(PendingLabels);
            LabelsExactAt = Version;
        }
        PendingLabels = FLabels{};
        PendingVersion = 0;
    }

    void FNavClusterGraph::BuildClusters(const dtNavMesh& Mesh, const dtMeshTile& Tile)
    {
        const uint32 TileIndex = Mesh.decodePolyIdTile(Mesh.getPolyRefBase(&Tile));
        if (TileIndex >= (uint32)Tiles.Num() || Tile.header == nullptr)
        {
            return;
        }

        const int32 PolyCount = Tile.header->polyCount;
        TVector<uint16> Parent((size_t)PolyCount);
        Algo::Iota(Parent.begin(), Parent.end(), (uint16)0);
        for (int32 i = 0; i < PolyCount; ++i)
        {
            for (uint32 Link = Tile.polys[i].firstLink; Link != DT_NULL_LINK; Link = Tile.links[Link].next)
            {
                const dtPolyRef Ref = Tile.links[Link].ref;
                if (Mesh.decodePolyIdTile(Ref) == TileIndex)
                {
                    Parent[FindRoot<uint16>(Parent, (uint16)i)] = FindRoot<uint16>(Parent, (uint16)Mesh.decodePolyIdPoly(Ref));
                }
            }
        }

        FTileClusters& Out = Tiles[TileIndex];
        Out = FTileClusters{};
        Out.Salt = Tile.salt;
        Out.PolyCluster.Init(kNoCluster, (size_t)PolyCount);

        TVector<uint16> ClusterOfRoot((size_t)PolyCount, kNoCluster);
        TVector<int32> PolysInCluster;
        for (int32 i = 0; i < PolyCount; ++i)
        {
            uint16& Cluster = ClusterOfRoot[FindRoot<uint16>(Parent, (uint16)i)];
            if (Cluster == kNoCluster)
            {
                Cluster = (uint16)Out.Centers.Num();
                Out.Centers.Emplace(0.0f);
                PolysInCluster.Add(0);
            }
            Out.PolyCluster[i] = Cluster;
            Out.Centers[Cluster] += PolyCenter(Tile, Tile.polys[i]);
            ++PolysInCluster[Cluster];
        }
        for (int32 c = 0; c < Out.Centers.Num(); ++c)
        {
            Out.Centers[c] /= (float)PolysInCluster[c];
        }
        Out.EdgeStart.Init(0, Out.Centers.Num() + 1);

        Labels.FirstId[TileIndex] = (uint32)Labels.Parent.Num();
        for (int32 c = 0; c < Out.Centers.Num(); ++c)
        {
            Labels.Parent.Add((uint32)Labels.Parent.Num());
            Labels.Size.Add(1);
        }
    }

    void FNavClusterGraph::BuildEdges(const dtNavMesh& Mesh, const dtMeshTile& Tile)
    {
        const uint32 TileIndex = Mesh.decodePolyIdTile(Mesh.getPolyRefBase(&Tile));
        if (TileIndex >= (uint32)Tiles.Num() || Tiles[TileIndex].Salt != Tile.salt)
        {
            return;
        }
        FTileClusters& Out = Tiles[TileIndex];

        struct FClusterEdge
        {
            uint16 Cluster;
            FEdge  Edge;
        };
        TVector<FClusterEdge> Found;
        for (int32 i = 0; i < Tile.header->polyCount; ++i)
        {
            if (!IsBorderPoly(Tile.polys[i]))
            {
                continue;
            }
            for (uint32 Link = Tile.polys[i].firstLink; Link != DT_NULL_LINK; Link = Tile.links[Link].next)
            {
                const dtPolyRef Ref = Tile.links[Link].ref;
                uint32 Salt = 0, NeighborTile = 0, NeighborPoly = 0;
                Mesh.decodePolyId(Ref, Salt, NeighborTile, NeighborPoly);
                if (NeighborTile == TileIndex || NeighborTile >= (uint32)Tiles.Num() || Tiles[NeighborTile].Salt != Salt)
                {
                    continue;
                }
                const dtMeshTile* Other = nullptr;
                const dtPoly* Poly = nullptr;
                Mesh.getTileAndPolyByRefUnsafe(Ref, &Other, &Poly);
                const uint64 Key = PackKey(NeighborTile, Tiles[NeighborTile].PolyCluster[NeighborPoly]);
                Found.Add(FClusterEdge{ Out.PolyCluster[i], FEdge{ Key, Ref, Poly->flags, Poly->getArea() } });
            }
        }

        // One edge per neighbor cluster and filter signature, however many polys share that border.
        auto Order = [](const FClusterEdge& A, const FClusterEdge& B)
        {
            if (A.Cluster != B.Cluster) return A.Cluster < B.Cluster;
            if (A.Edge.Key != B.Edge.Key) return A.Edge.Key < B.Edge.Key;
            if (A.Edge.Flags != B.Edge.Flags) return A.Edge.Flags < B.Edge.Flags;
            return A.Edge.Area < B.Edge.Area;
        };
        Algo::Sort(Found.begin(), Found.end(), Order);

        Out.Edges.Reset();
        Out.EdgeStart.Init(0, Out.Centers.Num() + 1);
        for (int32 i = 0; i < Found.Num(); ++i)
        {
            if (i > 0 && !Order(Found[i - 1], Found[i]))
            {
                continue;
            }
            Out.Edges.Add(Found[i].Edge);
            ++Out.EdgeStart[Found[i].Cluster + 1];
            Union(Labels.FirstId[TileIndex] + Found[i].Cluster, Labels.FirstId[TileOfKey(Found[i].Edge.Key)] + ClusterOfKey(Found[i].Edge.Key));
        }
        for (int32 c = 1; c < Out.EdgeStart.Num(); ++c)
        {
            Out.EdgeStart[c] += Out.EdgeStart[c - 1];
        }
    }

    void FNavClusterGraph::BuildNeighborEdges(const dtNavMesh& Mesh, int32 TileX, int32 TileY)
    {
        for (int32 Dy = -1; Dy <= 1; ++Dy)
        {
            for (int32 Dx = -1; Dx <= 1; ++Dx)
            {
                const dtMeshTile* Found[kMaxTilesPerCell];
                const int32 Count = Mesh.getTilesAt(TileX + Dx, TileY + Dy, Found, kMaxTilesPerCell);
                for (int32 i = 0; i < Count; ++i)
                {
                    BuildEdges(Mesh, *Found[i]);
                }
            }
        }
    }

    const FNavClusterGraph::FTileClusters* FNavClusterGraph::FindTile(const dtNavMesh& Mesh, uint64 PolyRef) const
    {
        uint32 Salt = 0, TileIndex = 0, PolyIndex = 0;
        Mesh.decodePolyId(PolyRef, Salt, TileIndex, PolyIndex);
        if (TileIndex >= (uint32)Tiles.Num() || Tiles[TileIndex].Salt != Salt || PolyIndex >= (uint32)Tiles[TileIndex].PolyCluster.Num())
        {
            return nullptr;
        }
        return &Tiles[TileIndex];
    }

    uint64 FNavClusterGraph::KeyOf(const dtNavMesh& Mesh, uint64 PolyRef) const
    {
        const FTileClusters* Tile = FindTile(Mesh, PolyRef);
        return Tile ? PackKey(Mesh.decodePolyIdTile(PolyRef), Tile->PolyCluster[Mesh.decodePolyIdPoly(PolyRef)]) : kNoKey;
    }

    uint64 FNavClusterGraph::NearestPoly(const dtNavMesh& Mesh, uint64 Key, const FVector3& Pos) const
    {
        const dtMeshTile* Tile = Mesh.getTile((int)TileOfKey(Key));
        const dtPolyRef Base = Mesh.getPolyRefBase(Tile);
        const TVector<uint16>& PolyCluster = Tiles[TileOfKey(Key)].PolyCluster;
        uint64 Nearest = 0;
        float NearestDistance = TNumericLimits<float>::Max();
        for (int32 i = 0; i < PolyCluster.Num(); ++i)
        {
            const float Distance = PolyCluster[i] == ClusterOfKey(Key) ? Math::LengthSquared(PolyCenter(*Tile, Tile->polys[i]) - Pos) : TNumericLimits<float>::Max();
            if (Distance < NearestDistance)
            {
                NearestDistance = Distance;
                Nearest = Base | (dtPolyRef)i;
            }
        }
        return Nearest;
    }

    FNavClusterGraph::ERoute FNavClusterGraph::FindRoute(const dtNavMesh& Mesh, const dtQueryFilter& Filter, uint64 StartRef, uint64 EndRef,
                                                         const FVector3& StartPos, const FVector3& EndPos, const std::atomic<uint32>& Abandon, FSearchScratch& Scratch) const
    {
        Scratch.Route.Reset();
        const uint64 StartKey = KeyOf(Mesh, StartRef);
        const uint64 EndKey = KeyOf(Mesh, EndRef);
        if (StartKey == kNoKey || EndKey == kNoKey)
        {
            return ERoute::Failed;
        }

        const uint32 StartComponent = ComponentOf(StartKey);
        if (StartComponent == ComponentOf(EndKey))
        {
            return Search(Mesh, Filter, StartKey, StartRef, EndKey, StartPos, EndPos, Abandon, Scratch);
        }

        // A different island, so the route heads for the part of the start's island nearest the goal, searched in tile rings around the goal.
        uint64 NearestKey = StartKey;
        float NearestDistance = Math::LengthSquared(Tiles[TileOfKey(StartKey)].Centers[ClusterOfKey(StartKey)] - EndPos);
        const float RingWidth = Math::Min(Mesh.getParams()->tileWidth, Mesh.getParams()->tileHeight);
        int32 GoalX = 0, GoalY = 0;
        Mesh.calcTileLoc(&EndPos.x, &GoalX, &GoalY);
        for (int32 Ring = 0;; ++Ring)
        {
            const float RingGap = (float)Math::Max(0, Ring - 1) * RingWidth;
            if (RingGap * RingGap > NearestDistance)
            {
                break;
            }
            if (Abandon.load(std::memory_order_relaxed) != 0)
            {
                return ERoute::Abandoned;
            }
            for (int32 Dy = -Ring; Dy <= Ring; ++Dy)
            {
                const int32 Step = Math::Abs(Dy) == Ring ? 1 : 2 * Ring;
                for (int32 Dx = -Ring; Dx <= Ring; Dx += Math::Max(1, Step))
                {
                    const dtMeshTile* Found[kMaxTilesPerCell];
                    const int32 Count = Mesh.getTilesAt(GoalX + Dx, GoalY + Dy, Found, kMaxTilesPerCell);
                    for (int32 i = 0; i < Count; ++i)
                    {
                        const uint32 TileIndex = Mesh.decodePolyIdTile(Mesh.getTileRef(Found[i]));
                        if (TileIndex >= (uint32)Tiles.Num() || Tiles[TileIndex].Salt != Found[i]->salt)
                        {
                            continue;
                        }
                        const TVector<FVector3>& Centers = Tiles[TileIndex].Centers;
                        for (int32 c = 0; c < Centers.Num(); ++c)
                        {
                            const uint64 Key = PackKey(TileIndex, (uint16)c);
                            const float Distance = Math::LengthSquared(Centers[c] - EndPos);
                            if ((Distance < NearestDistance || (Distance == NearestDistance && Key < NearestKey)) && ComponentOf(Key) == StartComponent)
                            {
                                NearestDistance = Distance;
                                NearestKey = Key;
                            }
                        }
                    }
                }
            }
        }
        const FVector3& NearestCenter = Tiles[TileOfKey(NearestKey)].Centers[ClusterOfKey(NearestKey)];
        const ERoute Toward = Search(Mesh, Filter, StartKey, StartRef, NearestKey, StartPos, NearestCenter, Abandon, Scratch);
        return Toward == ERoute::Failed || Toward == ERoute::Abandoned ? Toward : ERoute::Unreachable;
    }

    FNavClusterGraph::ERoute FNavClusterGraph::Search(const dtNavMesh& Mesh, const dtQueryFilter& Filter, uint64 StartKey, uint64 StartRef, uint64 GoalKey,
                                                      const FVector3& StartPos, const FVector3& GoalPos, const std::atomic<uint32>& Abandon, FSearchScratch& Scratch) const
    {
        using FNode = FSearchScratch::FNode;
        using FOpen = FSearchScratch::FOpen;
        auto Later = [](const FOpen& A, const FOpen& B) { return A.Estimate > B.Estimate; };

        TVector<FNode>& Nodes = Scratch.Nodes;
        TVector<FOpen>& Open = Scratch.Open;
        THashMap<uint64, int32>& NodeOfKey = Scratch.NodeOfKey;
        TVector<FStep>& Out = Scratch.Route;
        Nodes.Reset();
        Open.Reset();
        NodeOfKey.Reset();
        Nodes.Emplace(StartKey, StartRef, StartPos, 0.0f, -1, false);
        NodeOfKey.TryAdd(StartKey, 0);
        Open.Emplace(Math::Length(StartPos - GoalPos), 0);

        int32 Goal = -1;
        int32 Closest = 0;
        float ClosestDistance = TNumericLimits<float>::Max();
        int32 Expansions = 0;
        while (!Open.IsEmpty() && Expansions < kMaxExpansions)
        {
            Algo::PopHeap(Open.begin(), Open.end(), Later);
            const int32 Current = Open.Last().Node;
            Open.Pop();
            if (Nodes[Current].bClosed)
            {
                continue;
            }
            Nodes[Current].bClosed = true;
            if ((Expansions & 255) == 0 && Abandon.load(std::memory_order_relaxed) != 0)
            {
                return ERoute::Abandoned;
            }
            if (Nodes[Current].Key == GoalKey)
            {
                Goal = Current;
                break;
            }
            const float ToGoal = Math::LengthSquared(Nodes[Current].Center - GoalPos);
            if (ToGoal < ClosestDistance)
            {
                ClosestDistance = ToGoal;
                Closest = Current;
            }
            ++Expansions;

            const FTileClusters& Tile = Tiles[TileOfKey(Nodes[Current].Key)];
            const uint16 Cluster = ClusterOfKey(Nodes[Current].Key);
            for (uint32 e = Tile.EdgeStart[Cluster]; e < Tile.EdgeStart[Cluster + 1]; ++e)
            {
                const FEdge& Edge = Tile.Edges[e];
                if (!PassesFilter(Filter, Edge.Flags))
                {
                    continue;
                }
                const FVector3& NeighborCenter = Edge.Key == GoalKey ? GoalPos : Tiles[TileOfKey(Edge.Key)].Centers[ClusterOfKey(Edge.Key)];
                const float Cost = Nodes[Current].Cost + Math::Length(Nodes[Current].Center - NeighborCenter) * Filter.getAreaCost(Edge.Area);

                const int32 NewNode = Nodes.Num();
                const int32 Node = NodeOfKey.FindOrAdd(Edge.Key, NewNode);
                if (Node == NewNode)
                {
                    Nodes.Emplace(Edge.Key, Edge.EntryRef, NeighborCenter, Cost, Current, false);
                }
                else
                {
                    FNode& Existing = Nodes[Node];
                    if (Existing.bClosed || Cost >= Existing.Cost)
                    {
                        continue;
                    }
                    Existing.Cost = Cost;
                    Existing.Parent = Current;
                    Existing.EntryRef = Edge.EntryRef;
                }
                Open.Emplace(Cost + Math::Length(NeighborCenter - GoalPos), Node);
                Algo::PushHeap(Open.begin(), Open.end(), Later);
            }
        }

        const ERoute Result = Goal >= 0 ? ERoute::Reached : Open.IsEmpty() ? ERoute::Unreachable : ERoute::Failed;
        if (Result == ERoute::Failed)
        {
            return Result;
        }
        Out.Reset();
        for (int32 Node = Goal >= 0 ? Goal : Closest; Node >= 0; Node = Nodes[Node].Parent)
        {
            Out.Emplace(Nodes[Node].Key, Nodes[Node].EntryRef, Nodes[Node].Center, 0.0f);
        }
        Algo::Reverse(Out.begin(), Out.end());
        for (int32 i = 1; i < Out.Num(); ++i)
        {
            Out[i].Distance = Out[i - 1].Distance + Math::Length(Out[i - 1].Center - Out[i].Center);
        }
        return Result;
    }
#else
    void FNavClusterGraph::Reset(int32) {}
    void FNavClusterGraph::Rebuild(const dtNavMesh&) {}
    void FNavClusterGraph::AddTile(const dtNavMesh&, const dtMeshTile&) {}
    void FNavClusterGraph::RemoveTile(const dtNavMesh&, uint32, int32, int32) {}
    void FNavClusterGraph::PrepareExactLabels(const std::atomic<uint32>&) const {}
    bool FNavClusterGraph::HasPendingLabels() const { return false; }
    void FNavClusterGraph::AdoptPendingLabels() {}
    uint64 FNavClusterGraph::NearestPoly(const dtNavMesh&, uint64, const FVector3&) const { return 0; }
    FNavClusterGraph::ERoute FNavClusterGraph::FindRoute(const dtNavMesh&, const dtQueryFilter&, uint64, uint64, const FVector3&, const FVector3&, const std::atomic<uint32>&, FSearchScratch&) const { return ERoute::Failed; }
#endif
}
