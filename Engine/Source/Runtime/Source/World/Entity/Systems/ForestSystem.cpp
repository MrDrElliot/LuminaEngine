#include "RuntimePCH.h"
#include "ForestSystem.h"

#include "SystemContext.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Config/EngineSettings.h"
#include "Containers/Algorithm.h"
#include "Containers/HashPrimitives.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/ForestComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/CameraSubsystem.h"

namespace Lumina
{
    namespace ForestSystemInternal
    {
        // New entities per frame, since each one binds a tile of foliage.
        constexpr uint32 kMaxAppliesPerFrame    = 6;
        // A far tile binds only points, a fraction of what a tile of slots costs.
        constexpr uint32 kMaxPointAppliesPerFrame = 32;
        // Updates a new tile must already draw in before the one it replaces goes, so the swap never shows a hole.
        constexpr uint16 kDrawnSettleFrames     = 3;
        // A tile that cannot draw, such as one whose mesh never resolves, stops holding the old one after this.
        constexpr uint16 kMaxDrawWaitFrames     = 120;
        // A sculpt stroke bumps the heightmap every frame, so the forest waits for the terrain to settle.
        constexpr uint32 kTerrainSettleFrames   = 30;
        // Tiles nearer than this hold every species, and further ones leave out the plants that vanish before them.
        constexpr int32  kNearBand              = 0;
        constexpr int32  kMidBand               = 1;
        // Past the forest's ImpostorDistance, where plants with an impostor become points.
        constexpr int32  kFarBand               = 2;
        constexpr float  kNearBandDistance      = 160.0f;

        // The share of the transition either side of it the cull dithers instances across, from Culling.slang.
        constexpr float  kProxyTransitionBand   = 0.25f;

        // Plants culled before the band starts are left out of its tiles.
        float GetBandStart(const FForestState& State, int32 Band)
        {
            return Band == kNearBand ? 0.0f : Band == kMidBand ? kNearBandDistance : State.PointBandStart;
        }

        // Past this every plant with an impostor already draws as it in the cull, so its tiles can hold points instead of slots.
        float ComputePointBandStart(CWorld& World, const SForestComponent& Forest, const FForestState& State)
        {
            const float Configured = Math::Max(Forest.ImpostorDistance, kNearBandDistance);
            const CRendererSettings* Renderer = GetDefault<CRendererSettings>();
            CCameraSubsystem* Camera = World.GetCamera();
            if (Renderer == nullptr || !Renderer->bFarProxies || Renderer->ImpostorTransitionPixels <= 0.0f || Camera == nullptr)
            {
                return Configured;
            }

            const float    FieldOfView = Camera->GetFieldOfView();
            const FVector2 Viewport    = Camera->GetViewportSize();
            float Radius = 0.0f;
            for (const SFoliageType& Type : State.Types)
            {
                const CStaticMesh* Mesh = Type.Mesh.Get();
                if (Mesh != nullptr && Mesh->HasImpostor())
                {
                    Radius = Math::Max(Radius, Math::Length(Mesh->GetAABB().GetSize()) * 0.5f * Type.ScaleMax);
                }
            }
            if (FieldOfView <= 0.0f || Viewport.x <= 0.0f || Viewport.y <= 0.0f || Radius <= 0.0f)
            {
                return Configured;
            }

            // A plant at the corner of the view sits closer in view depth than in distance, by the half-diagonal's cosine.
            const float TanHalf   = Math::Tan(Math::Radians(FieldOfView) * 0.5f);
            const float Aspect    = Viewport.x / Viewport.y;
            const float OffAxis   = Math::Sqrt(1.0f + TanHalf * TanHalf * (1.0f + Aspect * Aspect));
            const float Threshold = Renderer->ImpostorTransitionPixels * (1.0f - kProxyTransitionBand);
            const float Start     = Radius / TanHalf * Viewport.y / Threshold * OffAxis;

            // Whole tiles, so a small change in the view does not move the band and rebuild its edge.
            const float TileSize = State.Rules != nullptr ? State.Rules->TileSize : 64.0f;
            return Math::Clamp(Math::Ceil(Start / TileSize) * TileSize, kNearBandDistance, Configured);
        }

        bool Overlaps(const FTileKey& A, const FTileKey& B)
        {
            return A.X == B.X && A.Z == B.Z;
        }

        FTileKey UnpackKey(uint64 Packed)
        {
            return { (int32)(int8)(uint8)(Packed >> 56), (int32)((Packed >> 28) & 0x0FFFFFFFull), (int32)(Packed & 0x0FFFFFFFull) };
        }

        FVector2 GetTileMin(const Forest::FRules& Rules, const FTileKey& Key)
        {
            return Rules.GetMinCorner() + FVector2((float)Key.X, (float)Key.Z) * Rules.TileSize;
        }

        // The tile keeps its entity on screen while the rebuild runs, and ApplyBuild swaps it out.
        void RebuildUnder(FForestState& State, const TVector<Forest::FArea>& Areas)
        {
            for (auto& [Packed, Tile] : State.Tiles)
            {
                const FVector2      Min  = GetTileMin(*State.Rules, UnpackKey(Packed));
                const Forest::FArea Area = { Min, Min + FVector2(State.Rules->TileSize) };
                if (Algo::AnyOf(Areas, [&](const Forest::FArea& Changed) { return Changed.Overlaps(Area); }))
                {
                    Tile.bReady = false;
                    if (Tile.Build)
                    {
                        Tile.Build->bCanceled.store(true, std::memory_order_relaxed);
                    }
                    Tile.Build.reset();
                }
            }
        }
    }

    void CForestSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
    }

    bool CForestSystem::HasWork(const SSystemContext&)
    {
        CWorld* World = GetWorld();
        return !Forests.IsEmpty() || (World != nullptr && !World->View<SForestComponent>().IsEmpty());
    }

    void CForestSystem::OnExclusiveUpdate(const SExclusiveContext&)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace ForestSystemInternal;

        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return;
        }

        TVector<ECS::FEntity> Live;
        for (ECS::FEntity Entity : World->View<SForestComponent, STransformComponent>())
        {
            if (!World->HasComponent<SDisabledTag>(Entity))
            {
                Live.Add(Entity);
            }
        }

        TVector<ECS::FEntity> Gone;
        for (auto& [Entity, State] : Forests)
        {
            if (!Algo::Contains(Live, Entity))
            {
                ReleaseTiles(*World, State);
                Gone.Add(Entity);
            }
        }
        for (ECS::FEntity Entity : Gone)
        {
            Forests.Remove(Entity);
        }

        // Tracked so an untouched forest skips hashing its paint, which runs to megabytes on a big painted world.
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        Registry.TrackChanges<SForestComponent>();
        const ECS::FSparseSet* Pool = Registry.FindStorage(ECS::GetComponentTypeID<SForestComponent>());
        for (ECS::FEntity Entity : Live)
        {
            FForestState& State = Forests.FindOrAdd(Entity);
            UpdateForest(*World, Entity, World->GetComponent<const SForestComponent>(Entity), Pool->HasChangedSince(Entity, State.CheckedTick), State);
        }
        // Taken after the reads, so any later write stamps newer than what each forest just checked.
        const uint32 Tick = ECS::FSparseSet::AdvanceChangeTick();
        for (ECS::FEntity Entity : Live)
        {
            Forests[Entity].CheckedTick = Tick;
        }
    }

    void CForestSystem::OnTeardown(const SExclusiveContext&)
    {
        Forests.Reset();
    }

    void CForestSystem::ReleaseTiles(CWorld& World, ForestSystemInternal::FForestState& State)
    {
        for (auto& [Packed, Tile] : State.Tiles)
        {
            if (Tile.Build)
            {
                Tile.Build->bCanceled.store(true, std::memory_order_relaxed);
            }
            if (World.IsValidEntity(Tile.Entity))
            {
                World.DestroyEntity(Tile.Entity);
            }
        }
        State.Tiles.Reset();
    }

    bool CForestSystem::RefreshState(CWorld& World, const SForestComponent& Forest, const FVector3& Center, bool bWritten, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        // Every terrain the forest overlaps, since a coarse backdrop around the playable one carries the plants beyond it.
        const float ForestHalf = Forest.Extent * 0.5f;
        TVector<FTerrainTrack> Tracks;
        for (ECS::FEntity Entity : World.View<STerrainComponent, STransformComponent>())
        {
            const STerrainComponent& Terrain = World.GetComponent<STerrainComponent>(Entity);
            const FVector3 Origin = World.GetComponent<STransformComponent>(Entity).GetWorldLocation();
            const float Reach = Terrain.TileWorldSize * 0.5f + ForestHalf;
            if (Math::Abs(Center.x - Origin.x) > Reach || Math::Abs(Center.z - Origin.z) > Reach)
            {
                continue;
            }

            const auto Previous = Algo::Find(State.Terrains, Entity, &FTerrainTrack::Entity);
            FTerrainTrack Track = Previous != State.Terrains.end() ? Move(*Previous) : FTerrainTrack{ Entity };
            Track.Spacing = Terrain.TileWorldSize / (float)Math::Max(Terrain.Resolution - 1, 1);

            const uint32 Version = Terrain.CPUState.HeightmapVersion;
            Track.StableFrames = Version == Track.SeenVersion ? Track.StableFrames + 1u : 0u;
            Track.SeenVersion  = Version;

            const bool bOriginMoved = Track.Snapshot && Track.Snapshot->Origin != Origin;
            const bool bSettled     = Track.StableFrames >= kTerrainSettleFrames || Track.Version == ~0u;
            if ((Version != Track.Version && bSettled) || bOriginMoved)
            {
                Track.Snapshot = Forest::FTerrainSnapshot::Capture(FTerrainSculptSystem::MakeHeightView(Terrain, Origin));
                Track.Version  = Version;
            }
            Tracks.Add(Move(Track));
        }
        Algo::Sort(Tracks, [](const FTerrainTrack& A, const FTerrainTrack& B) { return A.Spacing < B.Spacing; });
        State.Terrains = Move(Tracks);

        TVector<TSharedPtr<const Forest::FTerrainSnapshot>> Snapshots;
        for (const FTerrainTrack& Track : State.Terrains)
        {
            if (Track.Snapshot)
            {
                Snapshots.Add(Track.Snapshot);
            }
        }

        TSharedPtr<Forest::FRules> Rules = MakeShared<Forest::FRules>();
        Forest::CaptureRules(Forest, Center, Move(Snapshots), *Rules);

        uint64 Hash = Rules->Hash;
        for (const SForestSpecies& Species : Forest.Species)
        {
            const SFoliageType& Type = Species.Type;
            Hash = Containers::CombineHash(Hash, (uint64)(uintptr_t)Type.Mesh.Get());
            const float Render[] = { Type.CullDistance, Type.ThinningStartDistance, Type.WorldPositionOffsetDisableDistance };
            Hash = Containers::CombineHash(Hash, Containers::HashBytes(Render, sizeof(Render)));
            Hash = Containers::CombineHash(Hash, (uint64)Type.bCastShadow | ((uint64)Type.bReceiveShadow << 1) | ((uint64)Type.bEnableCollision << 2)
                                               | ((uint64)Type.bShadowWorldPositionOffset << 3));
        }

        const uint64 PaintHash = bWritten ? Forest::HashPaint(Forest) : State.PaintHash;
        if (Hash == State.StateHash)
        {
            if (PaintHash == State.PaintHash)
            {
                return false;
            }

            TSharedPtr<Forest::FPaint> Paint = MakeShared<Forest::FPaint>();
            Forest::CapturePaint(Forest, Center, *Paint);
            TVector<Forest::FArea> Changed;
            Forest::DiffPaint(*State.Rules, State.Rules->Paint.get(), Paint.get(), Changed);

            TSharedPtr<Forest::FRules> Repainted = MakeShared<Forest::FRules>(*State.Rules);
            Repainted->Paint = Move(Paint);
            State.Rules = Repainted;
            State.PaintHash = PaintHash;
            RebuildUnder(State, Changed);
            return false;
        }

        TSharedPtr<Forest::FPaint> Paint = MakeShared<Forest::FPaint>();
        Forest::CapturePaint(Forest, Center, *Paint);
        Rules->Paint = Move(Paint);

        ReleaseTiles(World, State);
        State.StateHash = Hash;
        State.PaintHash = PaintHash;
        State.Rules = Rules;
        State.Types.Reset();
        for (const SForestSpecies& Species : Forest.Species)
        {
            State.Types.Add(Species.Type);
        }
        return true;
    }

    void CForestSystem::ApplyBuild(CWorld& World, const ForestSystemInternal::FTileKey& Key, ForestSystemInternal::FTile& Tile, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        TSharedPtr<FBuild> Build = Move(Tile.Build);
        Tile.bReady      = true;
        Tile.ReadyFrames = 0;
        Tile.DrawnFrames = 0;
        // A repainted tile held its old entity until now.
        if (World.IsValidEntity(Tile.Entity))
        {
            World.DestroyEntity(Tile.Entity);
        }
        Tile.Entity = ECS::NullEntity;
        if (Build->Plants.IsEmpty())
        {
            return;
        }

        const ECS::FEntity Entity = World.ConstructEntity("ForestTile");
        World.EmplaceComponent<FTransientEntityTag>(Entity);
        World.EmplaceComponent<FHideInSceneOutliner>(Entity);
        SFoliageComponent& Foliage = World.EmplaceComponent<SFoliageComponent>(Entity);
        Foliage.Types           = State.Types;
        Foliage.bImpostorPoints = Key.Band == kFarBand;
        Foliage.Instances.Reserve(Build->Plants.Num());
        for (const Forest::FPlant& Plant : Build->Plants)
        {
            SFoliageInstance& Instance = Foliage.Instances.EmplaceGetRef();
            Instance.Position  = Plant.Position;
            Instance.SetRotationQuat(Plant.Rotation);
            Instance.Scale     = FVector3(Plant.Scale);
            Instance.TypeIndex = (int32)Plant.Species;
        }
        Foliage.MarkInstancesChanged();
        Tile.Entity = Entity;

    }

    void CForestSystem::UpdateForest(CWorld& World, ECS::FEntity Entity, const SForestComponent& Forest, bool bWritten, ForestSystemInternal::FForestState& State)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace ForestSystemInternal;

        const FVector3 Center = World.GetComponent<STransformComponent>(Entity).GetWorldLocation();
        RefreshState(World, Forest, Center, bWritten, State);
        if (State.Rules->Species.IsEmpty())
        {
            return;
        }

        TVector<FWanted> Wanted;
        State.PointBandStart = ComputePointBandStart(World, Forest, State);
        GatherWanted(World, Forest, Center, State, Wanted);

        THashSet<uint64> WantedSet;
        TVector<FTileKey> Unready;
        TrackDrawnTiles(World, State);
        StartBuilds(Forest, Wanted, State, WantedSet, Unready);
        ApplyFinishedBuilds(World, Wanted, State);
        RetireStaleTiles(World, WantedSet, Unready, State);
        PublishStats(World, State);
    }

    void CForestSystem::GatherWanted(CWorld& World, const SForestComponent& Forest, const FVector3& Center, const ForestSystemInternal::FForestState& State, TVector<ForestSystemInternal::FWanted>& Out)
    {
        using namespace ForestSystemInternal;

        const auto& Rules = *State.Rules;
        CCameraSubsystem* Camera = World.GetCamera();
        const FVector3 ViewPosition = Camera != nullptr ? Camera->GetViewPosition() : Center;
        const float    ViewDistance = Math::Max(Forest.ViewDistance, Rules.TileSize);
        const float    Size         = Rules.TileSize;
        const int32    Tiles        = (int32)Math::Ceil(Rules.Extent / Size);
        const FVector2 ForestMin    = Rules.GetMinCorner();

        // Only the tiles the view distance can reach, so a big forest never walks its whole grid.
        const int32 MinX = Math::Max((int32)Math::Floor((ViewPosition.x - ViewDistance - ForestMin.x) / Size), 0);
        const int32 MinZ = Math::Max((int32)Math::Floor((ViewPosition.z - ViewDistance - ForestMin.y) / Size), 0);
        const int32 MaxX = Math::Min((int32)Math::Floor((ViewPosition.x + ViewDistance - ForestMin.x) / Size), Tiles - 1);
        const int32 MaxZ = Math::Min((int32)Math::Floor((ViewPosition.z + ViewDistance - ForestMin.y) / Size), Tiles - 1);
        for (int32 Z = MinZ; Z <= MaxZ; ++Z)
        {
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const FVector2 Min = ForestMin + FVector2((float)X, (float)Z) * Size;
                float Ground = Center.y;
                Forest::FindGround(Rules, Min.x + Size * 0.5f, Min.y + Size * 0.5f, Ground);
                // Plants stand on the ground and reach a few tens of meters up, which is the box the view is measured to.
                const FVector3 BoxMin(Min.x, Ground - 20.0f, Min.y);
                const FVector3 BoxMax(Min.x + Size, Ground + 40.0f, Min.y + Size);
                const float    Distance = Math::Length(Math::Clamp(ViewPosition, BoxMin, BoxMax) - ViewPosition);
                if (Distance <= ViewDistance)
                {
                    const int32 Band = Distance < kNearBandDistance ? kNearBand : Distance < State.PointBandStart ? kMidBand : kFarBand;
                    Out.Add({ { Band, X, Z }, Distance });
                }
            }
        }

        // Nearest first, so the ground under the view fills in before the horizon.
        Algo::Sort(Out, [](const FWanted& A, const FWanted& B) { return A.Distance < B.Distance; });
    }

    void CForestSystem::StartBuilds(const SForestComponent& Forest, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State,
                                    THashSet<uint64>& OutWantedSet, TVector<ForestSystemInternal::FTileKey>& OutUnready)
    {
        using namespace ForestSystemInternal;

        State.Running.RemoveAll([](const TSharedPtr<FBuild>& Build) { return Build->bDone.load(std::memory_order_acquire); });
        int32 Builds = (int32)Algo::CountIf(State.Running, [](const TSharedPtr<FBuild>& Build) { return !Build->bCanceled.load(std::memory_order_relaxed); });

        for (const FWanted& Want : Wanted)
        {
            const uint64 Packed = Want.Key.Pack();
            OutWantedSet.Add(Packed);
            FTile& Tile = State.Tiles.FindOrAdd(Packed);
            if (Tile.bReady)
            {
                if (Tile.DrawnFrames < kDrawnSettleFrames && Tile.ReadyFrames < kMaxDrawWaitFrames)
                {
                    OutUnready.Add(Want.Key);
                }
                continue;
            }
            OutUnready.Add(Want.Key);
            if (Tile.Build || Builds >= Math::Max(Forest.MaxConcurrentBuilds, 1))
            {
                continue;
            }

            TSharedPtr<FBuild> Build = MakeShared<FBuild>();
            Tile.Build = Build;
            State.Running.Add(Build);
            ++Builds;

            const FVector2 Min = GetTileMin(*State.Rules, Want.Key);
            const float MinViewDistance = GetBandStart(State, Want.Key.Band);
            Jobs::Spawn(Jobs::EJobPriority::Low, [](TSharedPtr<FBuild> Target, TSharedPtr<const Forest::FRules> TileRules, FVector2 TileMin, float NearestView) -> Jobs::TTask<void>
            {
                if (!Target->bCanceled.load(std::memory_order_relaxed))
                {
                    Forest::ScatterRect(*TileRules, TileMin, TileMin + FVector2(TileRules->TileSize), Target->Plants, NearestView);
                }
                Target->bDone.store(true, std::memory_order_release);
                co_return;
            }, Build, State.Rules, Min, MinViewDistance);
        }
    }

    void CForestSystem::ApplyFinishedBuilds(CWorld& World, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        uint32 Applied      = 0;
        uint32 PointApplied = 0;
        for (const FWanted& Want : Wanted)
        {
            if (Applied >= kMaxAppliesPerFrame && PointApplied >= kMaxPointAppliesPerFrame)
            {
                return;
            }
            const bool bPoints = Want.Key.Band == kFarBand;
            if (bPoints ? PointApplied >= kMaxPointAppliesPerFrame : Applied >= kMaxAppliesPerFrame)
            {
                continue;
            }
            FTile* Tile = State.Tiles.Find(Want.Key.Pack());
            if (Tile == nullptr || Tile->bReady || !Tile->Build || !Tile->Build->bDone.load(std::memory_order_acquire))
            {
                continue;
            }
            ApplyBuild(World, Want.Key, *Tile, State);
            ++(bPoints ? PointApplied : Applied);
        }
    }

    void CForestSystem::RetireStaleTiles(CWorld& World, const THashSet<uint64>& WantedSet, const TVector<ForestSystemInternal::FTileKey>& Unready, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        // A tile the view no longer wants stays until whatever replaces it over its ground is ready, so the forest never opens a hole.
        TVector<uint64> Stale;
        for (auto& [Packed, Tile] : State.Tiles)
        {
            if (WantedSet.Contains(Packed))
            {
                continue;
            }
            const FTileKey Key = UnpackKey(Packed);
            const bool bCovering = Tile.bReady && World.IsValidEntity(Tile.Entity)
                && Algo::AnyOf(Unready, [&](const FTileKey& Pending) { return Overlaps(Key, Pending); });
            if (!bCovering)
            {
                Stale.Add(Packed);
            }
        }
        for (uint64 Packed : Stale)
        {
            FTile* Tile = State.Tiles.Find(Packed);
            if (Tile != nullptr && Tile->Build)
            {
                Tile->Build->bCanceled.store(true, std::memory_order_relaxed);
            }
            if (Tile != nullptr && World.IsValidEntity(Tile->Entity))
            {
                World.DestroyEntity(Tile->Entity);
            }
            State.Tiles.Remove(Packed);
        }
    }

    void CForestSystem::TrackDrawnTiles(CWorld& World, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        const auto IsDrawn = [&World](const FTile& Tile)
        {
            // An empty tile draws nothing, so nothing waits on it.
            if (!World.IsValidEntity(Tile.Entity))
            {
                return true;
            }
            if (const SFoliageComponent* Foliage = World.TryGetComponent<SFoliageComponent>(Tile.Entity))
            {
                return Foliage->BakedVersion == Foliage->InstancesVersion && !Foliage->bBakeIncomplete
                    && Algo::AllOf(Foliage->Types, [](const SFoliageType& Type) { return Type.ResolveHandle != Constants::kIndexNoneU32; });
            }
            return true;
        };

        int64 Awaiting = 0;
        for (auto& [Packed, Tile] : State.Tiles)
        {
            if (!Tile.bReady)
            {
                continue;
            }
            Tile.ReadyFrames = (uint16)Math::Min<uint32>(Tile.ReadyFrames + 1u, kMaxDrawWaitFrames);
            Tile.DrawnFrames = IsDrawn(Tile) ? (uint16)Math::Min<uint32>(Tile.DrawnFrames + 1u, kDrawnSettleFrames) : 0u;
            Awaiting += Tile.DrawnFrames < kDrawnSettleFrames && Tile.ReadyFrames < kMaxDrawWaitFrames ? 1 : 0;
        }
        LUMINA_PROFILE_VALUE("Forest/AwaitingDraw", Awaiting);
    }

    void CForestSystem::PublishStats(CWorld& World, const ForestSystemInternal::FForestState& State)
    {
        int64 Tiles = 0;
        int64 Plants = 0;
        for (const auto& [Packed, Tile] : State.Tiles)
        {
            if (!World.IsValidEntity(Tile.Entity))
            {
                continue;
            }
            if (const SFoliageComponent* Foliage = World.TryGetComponent<SFoliageComponent>(Tile.Entity))
            {
                ++Tiles;
                Plants += Foliage->Instances.Num();
            }
        }
        LUMINA_PROFILE_VALUE("Forest/InstanceTiles", Tiles);
        LUMINA_PROFILE_VALUE("Forest/Plants", Plants);
        LUMINA_PROFILE_VALUE("Forest/BuildsRunning", (int64)State.Running.Num());
    }
}
