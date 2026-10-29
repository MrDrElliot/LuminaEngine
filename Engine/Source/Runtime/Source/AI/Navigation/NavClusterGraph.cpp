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
        Tiles.clear();
        Tiles.resize((size_t)Math::Max(0, MaxTiles));
        Labels = FLabels{};
        Labels.FirstId.assign(Tiles.size(), 0);
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
        if (TileIndex < (uint32)Tiles.size())
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
            std::swap(A, B);
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
        Fresh.FirstId.assign(Tiles.size(), 0);
        uint32 Count = 0;
        for (size_t t = 0; t < Tiles.size(); ++t)
        {
            Fresh.FirstId[t] = Count;
            Count += (uint32)Tiles[t].Centers.size();
        }
        Fresh.Parent.resize(Count);
        Algo::Iota(Fresh.Parent.begin(), Fresh.Parent.end(), 0u);
        for (size_t t = 0; t < Tiles.size(); ++t)
        {
            // A swap waiting would change the graph and throw this result away, so it gets the lock back now.
            if (t % kAbandonCheckTiles == 0 && Abandon.load(std::memory_order_relaxed) != 0)
            {
                return;
            }
            const FTileClusters& Tile = Tiles[t];
            for (size_t c = 0; c < Tile.Centers.size(); ++c)
            {
                for (uint32 e = Tile.EdgeStart[c]; e < Tile.EdgeStart[c + 1]; ++e)
                {
                    const uint64 Key = Tile.Edges[e].Key;
                    Fresh.Parent[FindRoot<uint32>(Fresh.Parent, Fresh.FirstId[t] + (uint32)c)] = FindRoot<uint32>(Fresh.Parent, Fresh.FirstId[TileOfKey(Key)] + ClusterOfKey(Key));
                }
            }
        }
        Fresh.Size.assign(Count, 0);
        for (uint32 i = 0; i < Count; ++i)
        {
            Fresh.Parent[i] = FindRoot<uint32>(Fresh.Parent, i);
            ++Fresh.Size[Fresh.Parent[i]];
        }

        FScopeLock Lock(PendingLock);
        PendingLabels = std::move(Fresh);
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
            Labels = std::move(PendingLabels);
            LabelsExactAt = Version;
        }
        PendingLabels = FLabels{};
        PendingVersion = 0;
    }

    void FNavClusterGraph::BuildClusters(const dtNavMesh& Mesh, const dtMeshTile& Tile)
    {
        const uint32 TileIndex = Mesh.decodePolyIdTile(Mesh.getPolyRefBase(&Tile));
        if (TileIndex >= (uint32)Tiles.size() || Tile.header == nullptr)
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
        Out.PolyCluster.assign((size_t)PolyCount, kNoCluster);

        TVector<uint16> ClusterOfRoot((size_t)PolyCount, kNoCluster);
        TVector<int32> PolysInCluster;
        for (int32 i = 0; i < PolyCount; ++i)
        {
            uint16& Cluster = ClusterOfRoot[FindRoot<uint16>(Parent, (uint16)i)];
            if (Cluster == kNoCluster)
            {
                Cluster = (uint16)Out.Centers.size();
                Out.Centers.push_back(FVector3(0.0f));
                PolysInCluster.push_back(0);
            }
            Out.PolyCluster[i] = Cluster;
            Out.Centers[Cluster] += PolyCenter(Tile, Tile.polys[i]);
            ++PolysInCluster[Cluster];
        }
        for (size_t c = 0; c < Out.Centers.size(); ++c)
        {
            Out.Centers[c] /= (float)PolysInCluster[c];
        }
        Out.EdgeStart.assign(Out.Centers.size() + 1, 0);

        Labels.FirstId[TileIndex] = (uint32)Labels.Parent.size();
        for (size_t c = 0; c < Out.Centers.size(); ++c)
        {
            Labels.Parent.push_back((uint32)Labels.Parent.size());
            Labels.Size.push_back(1);
        }
    }

    void FNavClusterGraph::BuildEdges(const dtNavMesh& Mesh, const dtMeshTile& Tile)
    {
        const uint32 TileIndex = Mesh.decodePolyIdTile(Mesh.getPolyRefBase(&Tile));
        if (TileIndex >= (uint32)Tiles.size() || Tiles[TileIndex].Salt != Tile.salt)
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
                if (NeighborTile == TileIndex || NeighborTile >= (uint32)Tiles.size() || Tiles[NeighborTile].Salt != Salt)
                {
                    continue;
                }
                const dtMeshTile* Other = nullptr;
                const dtPoly* Poly = nullptr;
                Mesh.getTileAndPolyByRefUnsafe(Ref, &Other, &Poly);
                const uint64 Key = PackKey(NeighborTile, Tiles[NeighborTile].PolyCluster[NeighborPoly]);
                Found.push_back(FClusterEdge{ Out.PolyCluster[i], FEdge{ Key, Ref, Poly->flags, Poly->getArea() } });
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

        Out.Edges.clear();
        Out.EdgeStart.assign(Out.Centers.size() + 1, 0);
        for (size_t i = 0; i < Found.size(); ++i)
        {
            if (i > 0 && !Order(Found[i - 1], Found[i]))
            {
                continue;
            }
            Out.Edges.push_back(Found[i].Edge);
            ++Out.EdgeStart[Found[i].Cluster + 1];
            Union(Labels.FirstId[TileIndex] + Found[i].Cluster, Labels.FirstId[TileOfKey(Found[i].Edge.Key)] + ClusterOfKey(Found[i].Edge.Key));
        }
        for (size_t c = 1; c < Out.EdgeStart.size(); ++c)
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
        if (TileIndex >= (uint32)Tiles.size() || Tiles[TileIndex].Salt != Salt || PolyIndex >= (uint32)Tiles[TileIndex].PolyCluster.size())
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
        for (size_t i = 0; i < PolyCluster.size(); ++i)
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
                                                         const FVector3& StartPos, const FVector3& EndPos, const std::atomic<uint32>& Abandon, TVector<FStep>& Out) const
    {
        Out.clear();
        const uint64 StartKey = KeyOf(Mesh, StartRef);
        const uint64 EndKey = KeyOf(Mesh, EndRef);
        if (StartKey == kNoKey || EndKey == kNoKey)
        {
            return ERoute::Failed;
        }

        const uint32 StartComponent = ComponentOf(StartKey);
        if (StartComponent == ComponentOf(EndKey))
        {
            return Search(Mesh, Filter, StartKey, StartRef, EndKey, StartPos, EndPos, Abandon, Out);
        }

        // A different island, so the route heads for the part of the start's island nearest the goal instead.
        uint64 NearestKey = StartKey;
        float NearestDistance = TNumericLimits<float>::Max();
        for (size_t t = 0; t < Tiles.size(); ++t)
        {
            if (t % 1024 == 0 && Abandon.load(std::memory_order_relaxed) != 0)
            {
                return ERoute::Abandoned;
            }
            for (size_t c = 0; c < Tiles[t].Centers.size(); ++c)
            {
                const float Distance = ComponentOf(PackKey((uint32)t, (uint16)c)) == StartComponent ? Math::LengthSquared(Tiles[t].Centers[c] - EndPos) : TNumericLimits<float>::Max();
                if (Distance < NearestDistance)
                {
                    NearestDistance = Distance;
                    NearestKey = PackKey((uint32)t, (uint16)c);
                }
            }
        }
        const FVector3& NearestCenter = Tiles[TileOfKey(NearestKey)].Centers[ClusterOfKey(NearestKey)];
        const ERoute Toward = Search(Mesh, Filter, StartKey, StartRef, NearestKey, StartPos, NearestCenter, Abandon, Out);
        return Toward == ERoute::Failed || Toward == ERoute::Abandoned ? Toward : ERoute::Unreachable;
    }

    FNavClusterGraph::ERoute FNavClusterGraph::Search(const dtNavMesh& Mesh, const dtQueryFilter& Filter, uint64 StartKey, uint64 StartRef, uint64 GoalKey,
                                                      const FVector3& StartPos, const FVector3& GoalPos, const std::atomic<uint32>& Abandon, TVector<FStep>& Out) const
    {
        struct FNode
        {
            uint64   Key;
            uint64   EntryRef;
            FVector3 Center;
            float    Cost;
            int32    Parent;
            bool     bClosed;
        };
        struct FOpen
        {
            float Estimate;
            int32 Node;
        };
        auto Later = [](const FOpen& A, const FOpen& B) { return A.Estimate > B.Estimate; };

        TVector<FNode> Nodes;
        TVector<FOpen> Open;
        THashMap<uint64, int32> NodeOfKey;
        Nodes.push_back(FNode{ StartKey, StartRef, StartPos, 0.0f, -1, false });
        NodeOfKey.emplace(StartKey, 0);
        Open.push_back(FOpen{ Math::Length(StartPos - GoalPos), 0 });

        int32 Goal = -1;
        int32 Closest = 0;
        float ClosestDistance = TNumericLimits<float>::Max();
        int32 Expansions = 0;
        while (!Open.empty() && Expansions < kMaxExpansions)
        {
            Algo::PopHeap(Open.begin(), Open.end(), Later);
            const int32 Current = Open.back().Node;
            Open.pop_back();
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

                auto [It, bAdded] = NodeOfKey.try_emplace(Edge.Key, (int32)Nodes.size());
                if (bAdded)
                {
                    Nodes.push_back(FNode{ Edge.Key, Edge.EntryRef, NeighborCenter, Cost, Current, false });
                }
                else
                {
                    FNode& Existing = Nodes[It->second];
                    if (Existing.bClosed || Cost >= Existing.Cost)
                    {
                        continue;
                    }
                    Existing.Cost = Cost;
                    Existing.Parent = Current;
                    Existing.EntryRef = Edge.EntryRef;
                }
                Open.push_back(FOpen{ Cost + Math::Length(NeighborCenter - GoalPos), It->second });
                Algo::PushHeap(Open.begin(), Open.end(), Later);
            }
        }

        const ERoute Result = Goal >= 0 ? ERoute::Reached : Open.empty() ? ERoute::Unreachable : ERoute::Failed;
        if (Result == ERoute::Failed)
        {
            return Result;
        }
        Out.clear();
        for (int32 Node = Goal >= 0 ? Goal : Closest; Node >= 0; Node = Nodes[Node].Parent)
        {
            Out.push_back(FStep{ Nodes[Node].Key, Nodes[Node].EntryRef, Nodes[Node].Center, 0.0f });
        }
        Algo::Reverse(Out.begin(), Out.end());
        for (size_t i = 1; i < Out.size(); ++i)
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
    FNavClusterGraph::ERoute FNavClusterGraph::FindRoute(const dtNavMesh&, const dtQueryFilter&, uint64, uint64, const FVector3&, const FVector3&, const std::atomic<uint32>&, TVector<FStep>&) const { return ERoute::Failed; }
#endif
}
