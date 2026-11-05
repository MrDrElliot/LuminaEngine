#include "RuntimePCH.h"
#include "NavTileStreamer.h"

#include "NavMesh.h"
#include "Log/Log.h"

#include <algorithm>

namespace Lumina
{
    namespace
    {
        struct FCandidate
        {
            int32 Index = 0;
            int32 X = 0;
            int32 Y = 0;
            float Distance = 0.0f;
        };

        float NearestFocusDistance(const TVector<FVector3>& FocusPoints, const FVector3& Origin, float TileWorldSize, int32 TX, int32 TY)
        {
            float Best = FLT_MAX;
            for (const FVector3& Focus : FocusPoints)
            {
                Best = Math::Min(Best, NavTile::DistanceToTile(Focus, Origin, TileWorldSize, TX, TY));
            }
            return Best;
        }
    }

    void FNavTileStreamer::Reset(const FVector3& InOrigin, float InTileWorldSize)
    {
        Origin = InOrigin;
        TileWorldSize = InTileWorldSize;
        Resident.Reset();
        StarvedLogCooldown = 0;
    }

    void FNavTileStreamer::Clear()
    {
        Resident.Reset();
        TileWorldSize = 0.0f;
        StarvedLogCooldown = 0;
    }

    FNavTileStreamer::FStats FNavTileStreamer::Update(FNavMesh& Mesh, const TVector<SNavTileData>& Tiles,
        const TVector<FVector3>& FocusPoints, const FSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        FStats Stats;
        if (TileWorldSize <= 0.0f)
        {
            return Stats;
        }

        // No focus and a finite radius would evict the world, so treat that as keep everything.
        const bool bStreamAll = Settings.LoadRadius <= 0.0f || FocusPoints.IsEmpty();
        const float KeepRadius = Settings.LoadRadius * Math::Max(1.0f, Settings.KeepRadiusScale);

        TVector<FCandidate> Wanted;
        Wanted.Reserve(Tiles.Num());

        for (int32 i = 0; i < Tiles.Num(); ++i)
        {
            const SNavTileData& Tile = Tiles[i];
            if (Tile.Blob.IsEmpty())
            {
                continue;
            }

            const float Distance = bStreamAll ? 0.0f : NearestFocusDistance(FocusPoints, Origin, TileWorldSize, Tile.X, Tile.Y);
            const bool bResident = IsResident(Tile.X, Tile.Y);

            // Hysteresis: a tile already in keeps its place out to the wider radius.
            const float Threshold = bResident ? KeepRadius : Settings.LoadRadius;
            if (bStreamAll || Distance <= Threshold)
            {
                Wanted.Emplace(i, Tile.X, Tile.Y, Distance);
            }
        }

        Stats.Wanted = Wanted.Num();

        const int32 Budget = Math::Max(0, Settings.MaxResidentTiles);
        if (Wanted.Num() > Budget)
        {
            // Nearest wins, so the clamp is predictable rather than dependent on query history.
            std::nth_element(Wanted.begin(), Wanted.begin() + Budget, Wanted.end(),
                [](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });
            Stats.Starved = Wanted.Num() - Budget;
            Wanted.SetNum(Budget);
        }

        THashSet<uint64> WantedKeys;
        WantedKeys.Reserve(Wanted.Num());
        for (const FCandidate& C : Wanted)
        {
            WantedKeys.Add(NavTile::PackKey(C.X, C.Y));
        }

        // Evictions first: a full pool would otherwise reject an admission that does fit on net.
        int32 Ops = 0;
        for (auto It = Resident.begin(); It != Resident.end() && Ops < Settings.MaxOpsPerTick; )
        {
            if (WantedKeys.Contains(*It))
            {
                ++It;
                continue;
            }

            int32 TX, TY;
            NavTile::UnpackKey(*It, TX, TY);
            Mesh.RemoveTile(TX, TY);
            It = Resident.Remove(It);
            ++Stats.Removed;
            ++Ops;
        }

        // Nearest first, so a budget-limited frame pages in what matters most.
        std::sort(Wanted.begin(), Wanted.end(),
            [](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

        for (const FCandidate& C : Wanted)
        {
            if (Ops >= Settings.MaxOpsPerTick)
            {
                break;
            }
            if (IsResident(C.X, C.Y))
            {
                continue;
            }
            if (Resident.Num() >= Budget)
            {
                break;
            }

            // Re-adding a tile the mesh already holds fails, and copies the whole blob to do so.
            if (Mesh.HasTile(C.X, C.Y))
            {
                Resident.Add(NavTile::PackKey(C.X, C.Y));
                ++Stats.Adopted;
                continue;
            }

            if (Mesh.AddTile(C.X, C.Y, Tiles[C.Index].Blob))
            {
                Resident.Add(NavTile::PackKey(C.X, C.Y));
                ++Stats.Added;
                ++Ops;
            }
        }

        Stats.Resident = Resident.Num();

        if (Stats.Starved > 0)
        {
            if (StarvedLogCooldown <= 0)
            {
                LOG_WARN("NavMesh streaming: {} tiles wanted but the budget is {}. Paths beyond the resident set "
                         "will fail; raise Navigation settings ResidentTileBudget or lower StreamLoadRadius.", Stats.Wanted, Budget);
                StarvedLogCooldown = 600;
            }
            --StarvedLogCooldown;
        }
        else
        {
            StarvedLogCooldown = 0;
        }

        return Stats;
    }
}
