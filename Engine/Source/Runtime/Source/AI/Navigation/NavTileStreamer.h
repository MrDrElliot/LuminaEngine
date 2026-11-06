#pragma once

#include "AI/Navigation/NavTypes.h"
#include "Containers/HashTable.h"

namespace Lumina
{
    class FNavMesh;

    // Pages a bounded set of baked tiles into an FNavMesh around focus points, while the baked set stays with its owner.
    class RUNTIME_API FNavTileStreamer
    {
    public:

        struct FSettings
        {
            // Hard ceiling on resident tiles, no more than FNavMesh was initialized with.
            int32 MaxResidentTiles = 1024;

            // Tiles within this of a focus point are wanted, and zero or less keeps every tile resident.
            float LoadRadius = 0.0f;

            // Resident tiles survive out to LoadRadius times this, so a focus point on a tile boundary does not thrash.
            float KeepRadiusScale = 1.25f;

            // Bounds how long one mutation window can hold off queries.
            int32 MaxOpsPerTick = 8;

            bool operator==(const FSettings&) const = default;
        };

        struct FStats
        {
            int32 Resident = 0;
            int32 Wanted   = 0;
            int32 Added    = 0;
            int32 Removed  = 0;

            // Wanted tiles that did not fit the budget, where nonzero means paths can fail at range.
            int32 Starved  = 0;

            // Tiles the mesh already held that this took ownership of, rather than re-adding.
            int32 Adopted  = 0;

            // Wanted tiles the mesh refused to add, which stay out until the baked set changes.
            int32 Rejected = 0;
        };

        void Reset(const FVector3& InOrigin, float InTileWorldSize);
        void Clear();

        bool IsConfigured() const { return TileWorldSize > 0.0f; }

        // Forces the next Update to do a full pass, for when the baked set changed under it.
        void Invalidate() { bSettled = false; Rejected.Reset(); }

        // Tiles is the authoritative baked set, read but never retained.
        FStats Update(FNavMesh& Mesh, const TVector<SNavTileData>& Tiles, const TVector<FVector3>& FocusPoints, const FSettings& Settings);

        int32 GetResidentCount() const { return Resident.Num(); }
        bool IsResident(int32 TX, int32 TY) const { return Resident.Contains(NavTile::PackKey(TX, TY)); }

    private:

        struct FCandidate
        {
            int32 Index = 0;
            int32 X = 0;
            int32 Y = 0;
            float Distance = 0.0f;
        };

        bool CanSkipPass(const TVector<FVector3>& FocusPoints, const FSettings& Settings) const;

        FVector3            Origin = FVector3(0.0f);
        float               TileWorldSize = 0.0f;
        THashSet<uint64>    Resident;

        // Keys whose blob the mesh refused, so they are not copied and re-validated every tick.
        THashSet<uint64>    Rejected;

        TVector<FCandidate> Wanted;
        THashSet<uint64>    WantedKeys;

        // Inputs of the last full pass, which left nothing to page while bSettled holds.
        TVector<FVector3>   SettledFocus;
        FSettings           SettledSettings;
        bool                bSettled = false;

        // Ticks left before the starvation warning may be logged again.
        int32               StarvedLogCooldown = 0;
    };
}
