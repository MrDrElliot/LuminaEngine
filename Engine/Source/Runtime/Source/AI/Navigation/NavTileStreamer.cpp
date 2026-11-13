#include "RuntimePCH.h"
#include "NavTileStreamer.h"

#include "NavMesh.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // Focus drift below this share of a tile cannot change the wanted set enough to be worth a full pass.
        constexpr float kRefocusTileFraction = 0.05f;

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
        Rejected.Reset();
        StarvedLogCooldown = 0;
        bSettled = false;
    }

    void FNavTileStreamer::Clear()
    {
        Resident.Reset();
        Rejected.Reset();
        TileWorldSize = 0.0f;
        StarvedLogCooldown = 0;
        bSettled = false;
    }

    bool FNavTileStreamer::CanSkipPass(const TVector<FVector3>& FocusPoints, const FSettings& Settings) const
    {
        if (!bSettled || Settings != SettledSettings || FocusPoints.Num() != SettledFocus.Num())
        {
            return false;
        }

        const float MaxDrift = TileWorldSize * kRefocusTileFraction;
        for (int32 i = 0; i < FocusPoints.Num(); ++i)
        {
            if (Math::LengthSquared(FocusPoints[i] - SettledFocus[i]) > MaxDrift * MaxDrift)
            {
                return false;
            }
        }
        return true;
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

        if (CanSkipPass(FocusPoints, Settings))
        {
            Stats.Resident = Resident.Num();
            Stats.Wanted = Wanted.Num();
            Stats.Rejected = Wanted.Num() - Resident.Num();
            return Stats;
        }

        // No focus and a finite radius would evict the world, so treat that as keep everything.
        const bool bStreamAll = Settings.LoadRadius <= 0.0f || FocusPoints.IsEmpty();
        const float KeepRadius = Settings.LoadRadius * Math::Max(1.0f, Settings.KeepRadiusScale);

        Wanted.Reset(Tiles.Num());

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

        // Nearest wins the budget and pages in first, so the clamp does not depend on query history.
        const auto ByDistance = [](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; };
        const int32 Budget = Math::Max(0, Settings.MaxResidentTiles);
        if (Wanted.Num() > Budget)
        {
            Algo::PartialSort(Wanted.begin(), Wanted.begin() + Budget, Wanted.end(), ByDistance);
            Stats.Starved = Wanted.Num() - Budget;
            Wanted.SetNum(Budget);
        }
        else
        {
            Algo::Sort(Wanted, ByDistance);
        }

        WantedKeys.Reset();
        WantedKeys.Reserve(Wanted.Num());
        for (const FCandidate& C : Wanted)
        {
            const uint64 Key = NavTile::PackKey(C.X, C.Y);
            WantedKeys.Add(Key);
            Stats.Rejected += Rejected.Contains(Key) ? 1 : 0;
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

            const uint64 Key = NavTile::PackKey(C.X, C.Y);
            if (Rejected.Contains(Key))
            {
                continue;
            }

            if (Mesh.AddTile(C.X, C.Y, Tiles[C.Index].Blob))
            {
                Resident.Add(Key);
                ++Stats.Added;
            }
            else
            {
                Rejected.Add(Key);
                ++Stats.Rejected;
                LOG_WARN("NavMesh streaming: the navmesh rejected tile ({}, {}), so it stays out until it is rebaked.", C.X, C.Y);
            }
            ++Ops;
        }

        Stats.Resident = Resident.Num();

        // Resident only ever holds wanted tiles once evictions finish, so equal counts mean the sets match.
        bSettled = Ops < Settings.MaxOpsPerTick && Resident.Num() + Stats.Rejected == Wanted.Num();
        if (bSettled)
        {
            SettledFocus = FocusPoints;
            SettledSettings = Settings;
        }

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
