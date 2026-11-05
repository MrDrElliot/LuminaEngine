#pragma once

#include "AI/Navigation/NavTypes.h"
#include "Containers/HashTable.h"

namespace Lumina
{
    class FNavMesh;

    /** Keeps a bounded set of baked tiles resident in an FNavMesh, following focus points.
     *  The baked set stays with its owner; this only decides what is paged in. */
    class RUNTIME_API FNavTileStreamer
    {
    public:

        struct FSettings
        {
            /** Hard ceiling on resident tiles. Must not exceed what FNavMesh was initialized with. */
            int32 MaxResidentTiles = 1024;

            /** Tiles within this of a focus point are wanted. Zero or less keeps every tile resident. */
            float LoadRadius = 0.0f;

            /** Resident tiles survive out to LoadRadius times this, so a focus point sitting on a tile
             *  boundary does not page the same tile in and out every frame. */
            float KeepRadiusScale = 1.25f;

            /** Bounds how long one mutation window can hold off queries. */
            int32 MaxOpsPerTick = 8;
        };

        struct FStats
        {
            int32 Resident = 0;
            int32 Wanted   = 0;
            int32 Added    = 0;
            int32 Removed  = 0;

            /** Wanted tiles that did not fit the budget. Nonzero means paths can fail at range. */
            int32 Starved  = 0;

            // Tiles the mesh already held that this took ownership of, rather than re-adding.
            int32 Adopted  = 0;
        };

        void Reset(const FVector3& InOrigin, float InTileWorldSize);
        void Clear();

        bool IsConfigured() const { return TileWorldSize > 0.0f; }

        /** Tiles is the authoritative baked set, read but never retained. */
        FStats Update(FNavMesh& Mesh, const TVector<SNavTileData>& Tiles, const TVector<FVector3>& FocusPoints, const FSettings& Settings);

        int32 GetResidentCount() const { return Resident.Num(); }
        bool IsResident(int32 TX, int32 TY) const { return Resident.Contains(NavTile::PackKey(TX, TY)); }

    private:

        FVector3            Origin = FVector3(0.0f);
        float               TileWorldSize = 0.0f;
        THashSet<uint64>    Resident;

        // Ticks left before the starvation warning may be logged again.
        int32               StarvedLogCooldown = 0;
    };
}
