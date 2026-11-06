#pragma once

#include <atomic>
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Sync.h"

class dtNavMesh;
class dtQueryFilter;
struct dtMeshTile;

namespace Lumina
{
    // The polys of each tile grouped into clusters connected inside it, so a long path plans tile by tile before Detour walks polys.
    class FNavClusterGraph
    {
    public:

        // One cluster on a route, entered through EntryRef, with Distance measured along the route from the start.
        struct FStep
        {
            uint64   Key = 0;
            uint64   EntryRef = 0;
            FVector3 Center = FVector3(0.0f);
            float    Distance = 0.0f;
        };

        enum class ERoute : uint8
        {
            Failed,
            Reached,
            Unreachable,
            Abandoned,
        };

        // Owned by one query slot and cleared between queries, so a long path reuses the search's allocations.
        struct FSearchScratch
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

            TVector<FNode>          Nodes;
            TVector<FOpen>          Open;
            THashMap<uint64, int32> NodeOfKey;
            TVector<FStep>          Route;
        };

        void Reset(int32 MaxTiles);

        // Every resident tile at once, for a mesh that was just filled.
        void Rebuild(const dtNavMesh& Mesh);

        // Called after Detour has linked or unlinked the tile, so the neighbors' edges are read as they now stand.
        void AddTile(const dtNavMesh& Mesh, const dtMeshTile& Tile);
        void RemoveTile(const dtNavMesh& Mesh, uint32 TileIndex, int32 TileX, int32 TileY);

        // Fills Scratch.Route. Unreachable routes to the cluster nearest the goal that the start can reach. Abandoned once Abandon reads nonzero.
        ERoute FindRoute(const dtNavMesh& Mesh, const dtQueryFilter& Filter, uint64 StartRef, uint64 EndRef,
                         const FVector3& StartPos, const FVector3& EndPos, const std::atomic<uint32>& Abandon, FSearchScratch& Scratch) const;

        // The poly of a route's cluster whose center lies nearest Pos.
        uint64 NearestPoly(const dtNavMesh& Mesh, uint64 Key, const FVector3& Pos) const;

        // Swaps only ever merge the island labels, so a tile change that cut a route leaves them saying connected until an exact pass lands.
        bool NeedsExactLabels() const { return LabelsExactAt != Version; }

        // Reads the graph under a shared hold and parks the result for the next writer to adopt, giving up once Abandon reads nonzero.
        void PrepareExactLabels(const std::atomic<uint32>& Abandon) const;
        bool HasPendingLabels() const;

        // Writer only. Takes the parked labels when nothing changed since they were computed.
        void AdoptPendingLabels();

    private:

        struct FEdge
        {
            uint64 Key = 0;
            uint64 EntryRef = 0;
            uint16 Flags = 0;
            uint8  Area = 0;
        };

        // Union find over every cluster ever built, so a swap merges in place and a reader walks it without writing.
        struct FLabels
        {
            TVector<uint32> FirstId;
            TVector<uint32> Parent;
            TVector<uint32> Size;
        };

        struct FTileClusters
        {
            uint32            Salt = 0;
            TVector<uint16>   PolyCluster;
            TVector<FVector3> Centers;
            TVector<FEdge>    Edges;
            TVector<uint32>   EdgeStart;
        };

        const FTileClusters* FindTile(const dtNavMesh& Mesh, uint64 PolyRef) const;
        uint64 KeyOf(const dtNavMesh& Mesh, uint64 PolyRef) const;
        void BuildClusters(const dtNavMesh& Mesh, const dtMeshTile& Tile);
        void BuildEdges(const dtNavMesh& Mesh, const dtMeshTile& Tile);
        void BuildNeighborEdges(const dtNavMesh& Mesh, int32 TileX, int32 TileY);
        uint32 ComponentOf(uint64 Key) const;
        void Union(uint32 A, uint32 B);
        ERoute Search(const dtNavMesh& Mesh, const dtQueryFilter& Filter, uint64 StartKey, uint64 StartRef, uint64 GoalKey,
                      const FVector3& StartPos, const FVector3& GoalPos, const std::atomic<uint32>& Abandon, FSearchScratch& Scratch) const;

        TVector<FTileClusters> Tiles;
        uint64                 Version = 1;
        FLabels                Labels;
        uint64                 LabelsExactAt = 0;

        mutable FMutex         PendingLock;
        mutable FLabels        PendingLabels;
        mutable uint64         PendingVersion = 0;
    };
}
