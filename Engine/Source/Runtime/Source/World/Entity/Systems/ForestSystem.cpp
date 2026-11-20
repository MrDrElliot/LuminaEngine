#include "RuntimePCH.h"
#include "ForestSystem.h"

#include "SystemContext.h"
#include "Containers/Algorithm.h"
#include "Containers/HashPrimitives.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
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
        // Screen height the voxel budget is measured against, so a forest looks the same in every viewport size.
        constexpr float  kReferenceScreenHeight = 1440.0f;
        // New entities per frame, since each one uploads geometry or binds a tile of foliage.
        constexpr uint32 kMaxAppliesPerFrame    = 6;
        // Updates a new node must already draw in before the one it replaces goes, so the swap never shows a hole.
        constexpr uint16 kDrawnSettleFrames     = 3;
        // A node that cannot draw, such as one whose mesh never resolves, stops holding the old one after this.
        constexpr uint16 kMaxDrawWaitFrames     = 120;
        // Aggregates nearer than this queue ahead of the rest, since they are what the view sees popping in.
        constexpr float  kNearBuildDistance     = 400.0f;
        // Scattering a tile is cheap next to voxelizing one, so plants get their own allowance.
        constexpr int32  kMaxInstanceBuilds     = 16;
        // A sculpt stroke bumps the heightmap every frame, so the forest waits for the terrain to settle.
        constexpr uint32 kTerrainSettleFrames   = 30;
        // Instance tiles follow a stroke live, and aggregates, which take seconds, wait this long after it.
        constexpr uint32 kPaintSettleFrames     = 20;
        // Tiles nearer than this hold every species, and further ones leave out the plants that vanish before it.
        constexpr int32  kNearInstanceLevel     = -1;
        constexpr int32  kFarInstanceLevel      = -2;
        constexpr float  kNearBandDistance      = 160.0f;
        // Ground behind the view refines as if this many times further, since it only shows once the view turns, and the coarser cover holds until then.
        constexpr float  kBehindDistanceScale   = 4.0f;
        // Degrees past the horizontal half angle of the view still treated as in front, for the edges of a wide screen and a turning head.
        constexpr float  kViewMarginDegrees     = 20.0f;
        constexpr float  kReferenceAspect       = 16.0f / 9.0f;
        constexpr int32  kMaxLevels             = 12;

        bool Overlaps(const Forest::FNodeKey& A, const Forest::FNodeKey& B)
        {
            const int32 LA = Math::Max(A.Level, 0);
            const int32 LB = Math::Max(B.Level, 0);
            if (LA >= LB)
            {
                return (B.X >> (LA - LB)) == A.X && (B.Z >> (LA - LB)) == A.Z;
            }
            return (A.X >> (LB - LA)) == B.X && (A.Z >> (LB - LA)) == B.Z;
        }

        Forest::FNodeKey UnpackKey(uint64 Packed)
        {
            return { (int32)(int8)(uint8)(Packed >> 56), (int32)((Packed >> 28) & 0x0FFFFFFFull), (int32)(Packed & 0x0FFFFFFFull) };
        }

        Forest::FArea GetNodeArea(const Forest::FRules& Rules, const Forest::FNodeKey& Key)
        {
            const int32 Level = Math::Max(Key.Level, 0);
            const FVector2 Min = Forest::GetNodeMin(Rules, { Level, Key.X, Key.Z });
            return { Min, Min + FVector2(Forest::GetNodeSize(Rules, Level)) };
        }

        // The node keeps its entity on screen while the rebuild runs, and ApplyBuild swaps it out.
        void RebuildUnder(FForestState& State, const TVector<Forest::FArea>& Areas, bool bAggregates)
        {
            for (auto& [Packed, Node] : State.Nodes)
            {
                const Forest::FNodeKey Key = UnpackKey(Packed);
                if ((Key.Level >= 0) != bAggregates)
                {
                    continue;
                }
                const Forest::FArea Area = GetNodeArea(*State.Rules, Key);
                if (Algo::AnyOf(Areas, [&](const Forest::FArea& Changed) { return Changed.Overlaps(Area); }))
                {
                    Node.bReady = false;
                    if (Node.Build)
                    {
                        Node.Build->bCanceled.store(true, std::memory_order_relaxed);
                    }
                    Node.Build.reset();
                }
            }
        }

        uint32 AggregateResolution(const SForestComponent& Forest)
        {
            return (uint32)Math::NextPowerOfTwo(Math::Clamp(Forest.AggregateResolution, 16, 512));
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
                ReleaseNodes(*World, State);
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

    void CForestSystem::ReleaseNodes(CWorld& World, ForestSystemInternal::FForestState& State)
    {
        for (auto& [Packed, Node] : State.Nodes)
        {
            if (Node.Build)
            {
                Node.Build->bCanceled.store(true, std::memory_order_relaxed);
            }
            if (World.IsValidEntity(Node.Entity))
            {
                World.DestroyEntity(Node.Entity);
            }
        }
        State.Nodes.Reset();
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

            const bool bOriginMoved = Track.Snapshot && Track.Snapshot->View.Origin != Origin;
            const bool bSettled     = Track.StableFrames >= kTerrainSettleFrames || Track.Version == ~0u;
            if ((Version != Track.Version && bSettled) || bOriginMoved)
            {
                TSharedPtr<Forest::FTerrainSnapshot> Snapshot = MakeShared<Forest::FTerrainSnapshot>();
                Snapshot->View = FTerrainSculptSystem::MakeHeightView(Terrain, Origin);
                if (Snapshot->View.Heights != nullptr)
                {
                    const int32 Count = Terrain.Resolution * Terrain.Resolution;
                    Snapshot->Heights.Append(Snapshot->View.Heights, Count);
                    Snapshot->View.Heights = Snapshot->Heights.GetData();
                    const float Shape[] = { Snapshot->View.TileWorldSize, Snapshot->View.MaxHeight, Origin.x, Origin.y, Origin.z };
                    Snapshot->Hash = Containers::CombineHash(Containers::HashBytes(Snapshot->Heights.GetData(), Count * sizeof(float)),
                                                             Containers::HashBytes(Shape, sizeof(Shape)));
                }
                Track.Snapshot = Move(Snapshot);
                Track.Version  = Version;
            }
            Tracks.Add(Move(Track));
        }
        Algo::Sort(Tracks, [](const FTerrainTrack& A, const FTerrainTrack& B) { return A.Spacing < B.Spacing; });
        State.Terrains = Move(Tracks);

        TVector<TSharedPtr<const Forest::FTerrainSnapshot>> Snapshots;
        for (const FTerrainTrack& Track : State.Terrains)
        {
            if (Track.Snapshot && Track.Snapshot->View.Heights != nullptr)
            {
                Snapshots.Add(Track.Snapshot);
            }
        }

        TSharedPtr<Forest::FRules> Rules = MakeShared<Forest::FRules>();
        Forest::CaptureRules(Forest, Center, Move(Snapshots), *Rules);

        uint64 Hash = Containers::CombineHash(Rules->Hash, AggregateResolution(Forest));
        Hash = Containers::CombineHash(Hash, Forest.bCacheAggregates ? 1u : 0u);
        for (const SForestSpecies& Species : Forest.Species)
        {
            const SFoliageType& Type = Species.Type;
            const CStaticMesh*  Mesh = Type.Mesh.Get();
            const bool bDecodable = Mesh != nullptr && Mesh->TryGetMeshResource() != nullptr && !Mesh->TryGetMeshResource()->MeshletData.IsEmpty();
            Hash = Containers::CombineHash(Hash, (uint64)(uintptr_t)Mesh);
            Hash = Containers::CombineHash(Hash, bDecodable ? 1u : 0u);
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
                ++State.PaintStableFrames;
                if (State.PaintStableFrames >= kPaintSettleFrames && !State.StaleAggregateAreas.IsEmpty())
                {
                    RebuildUnder(State, State.StaleAggregateAreas, true);
                    State.StaleAggregateAreas.Reset();
                }
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
            State.PaintStableFrames = 0;
            RebuildUnder(State, Changed, false);
            State.StaleAggregateAreas.Append(Changed);
            return false;
        }

        TSharedPtr<Forest::FPaint> Paint = MakeShared<Forest::FPaint>();
        Forest::CapturePaint(Forest, Center, *Paint);
        Rules->Paint = Move(Paint);

        ReleaseNodes(World, State);
        State.StateHash = Hash;
        State.PaintHash = PaintHash;
        State.StaleAggregateAreas.Reset();
        State.Rules = Rules;
        State.Geometry.Reset();
        State.Types.Reset();
        for (const SForestSpecies& Species : Forest.Species)
        {
            const CStaticMesh* Mesh = Species.Type.Mesh.Get();
            State.Geometry.Add(Mesh != nullptr ? Forest::DecodeSpecies(*Mesh) : nullptr);
            State.Types.Add(Species.Type);
        }
        return true;
    }

    void CForestSystem::ApplyBuild(CWorld& World, const Forest::FNodeKey& Key, ForestSystemInternal::FNode& Node, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        TSharedPtr<FBuild> Build = Move(Node.Build);
        Node.bReady      = true;
        Node.ReadyFrames = 0;
        Node.DrawnFrames = 0;
        // A repainted node held its old entity until now.
        if (World.IsValidEntity(Node.Entity))
        {
            World.DestroyEntity(Node.Entity);
        }
        Node.Entity = ECS::NullEntity;

        if (Key.Level < 0)
        {
            if (Build->Plants.IsEmpty())
            {
                return;
            }
            const ECS::FEntity Entity = World.ConstructEntity("ForestTile");
            World.EmplaceComponent<FTransientEntityTag>(Entity);
            World.EmplaceComponent<FHideInSceneOutliner>(Entity);
            SFoliageComponent& Foliage = World.EmplaceComponent<SFoliageComponent>(Entity);
            Foliage.Types = State.Types;
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
            Node.Entity = Entity;
            return;
        }

        Forest::FAggregateResult& Result = Build->Aggregate;
        if (Result.bEmpty)
        {
            return;
        }

        const ECS::FEntity Entity = World.ConstructEntity("ForestAggregate", FTransform(Result.Origin));
        World.EmplaceComponent<FTransientEntityTag>(Entity);
        World.EmplaceComponent<FHideInSceneOutliner>(Entity);
        SDynamicMeshComponent& Mesh = World.EmplaceComponent<SDynamicMeshComponent>(Entity);
        // A far aggregate's sway is lost under a shadow texel, so its shadow stays still and the sun's cache can hold it.
        Mesh.bShadowWorldPositionOffset = false;
        for (uint32 Source : Result.SectionSources)
        {
            const uint32 Species = Source >> 16;
            const uint32 Surface = Source & 0xFFFFu;
            const Forest::FSpeciesGeometry* Geometry = Species < (uint32)State.Geometry.Num() ? State.Geometry[Species].get() : nullptr;
            CMaterialInterface* Material = Geometry != nullptr && Surface < (uint32)Geometry->Surfaces.Num() ? Geometry->Surfaces[Surface].Material : nullptr;
            Mesh.MaterialOverrides.Add(TStrongObjectPtr<CMaterialInterface>(Material));
        }
        Mesh.CommitBuiltResource(Move(Result.Resource), Result.LocalMin, Result.LocalMax);
        Node.Entity = Entity;
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
        GatherWanted(World, Forest, Center, *State.Rules, Wanted);

        THashSet<uint64> WantedSet;
        TVector<Forest::FNodeKey> Unready;
        TrackDrawnNodes(World, State);
        StartBuilds(Forest, Wanted, State, WantedSet, Unready);
        ApplyFinishedBuilds(World, Wanted, State);
        RetireStaleNodes(World, WantedSet, Unready, State);
        PublishStats(World, State);
    }

    void CForestSystem::GatherWanted(CWorld& World, const SForestComponent& Forest, const FVector3& Center, const Forest::FRules& Rules, TVector<ForestSystemInternal::FWanted>& Out)
    {
        using namespace ForestSystemInternal;

        CCameraSubsystem* Camera = World.GetCamera();
        const FVector3 ViewPosition = Camera != nullptr ? Camera->GetViewPosition() : Center;
        const float    FOV          = Camera != nullptr && Camera->GetFieldOfView() > 1.0f ? Camera->GetFieldOfView() : 60.0f;
        const float    PixelAngle   = 2.0f * Math::Tan(Math::Radians(FOV) * 0.5f) / kReferenceScreenHeight;
        const uint32   Resolution   = AggregateResolution(Forest);
        const float    MaxPixels    = Math::Max(Forest.MaxVoxelPixels, 0.25f);
        const FVector3 ViewForward  = Camera != nullptr ? Camera->GetViewForward() : FVector3(0.0f, 0.0f, 1.0f);
        const float    HalfWidth    = Math::Degrees(Math::Atan(Math::Tan(Math::Radians(FOV) * 0.5f) * kReferenceAspect));
        const float    InViewCos    = Math::Cos(Math::Radians(Math::Min(HalfWidth + kViewMarginDegrees, 179.0f)));

        int32 TopLevel = 0;
        while (TopLevel < kMaxLevels && Forest::GetNodeSize(Rules, TopLevel) < Rules.Extent)
        {
            ++TopLevel;
        }

        const FVector2 ForestMax = Rules.GetMinCorner() + FVector2(Rules.Extent);

        TVector<Forest::FNodeKey> Stack;
        Stack.Add({ TopLevel, 0, 0 });
        while (!Stack.IsEmpty())
        {
            const Forest::FNodeKey Key = Stack.Pop();
            const float    Size = Forest::GetNodeSize(Rules, Key.Level);
            const FVector2 Min  = Forest::GetNodeMin(Rules, Key);
            if (Min.x >= ForestMax.x || Min.y >= ForestMax.y)
            {
                continue;
            }

            float Ground = Center.y;
            Forest::FindGround(Rules, Min.x + Size * 0.5f, Min.y + Size * 0.5f, Ground);
            // Plants stand on the ground and reach a few tens of meters up, which is the box the view is measured to.
            const FVector3 BoxMin(Min.x, Ground - 20.0f, Min.y);
            const FVector3 BoxMax(Min.x + Size, Ground + 40.0f, Min.y + Size);
            const FVector3 ToBox    = Math::Clamp(ViewPosition, BoxMin, BoxMax) - ViewPosition;
            const float    Distance = Math::Length(ToBox);
            const float    Cos      = Distance > 1e-3f ? Math::Dot(ToBox / Distance, ViewForward) : 1.0f;
            const float    Behind   = Math::Clamp((InViewCos - Cos) / (InViewCos + 1.0f), 0.0f, 1.0f);
            // Real plants carry collision, so the band around the view keeps them on every side.
            const float    Weighted = Key.Level > 0 ? Distance * Math::Mix(1.0f, kBehindDistanceScale, Behind) : Distance;
            const float VoxelPixels = (Size / (float)Resolution) / Math::Max(Weighted * PixelAngle, 1e-6f);

            if (!Forest.bInstancesOnly && VoxelPixels <= MaxPixels)
            {
                Out.Add({ Key, Distance });
            }
            else if (Key.Level == 0)
            {
                Out.Add({ { Distance < kNearBandDistance ? kNearInstanceLevel : kFarInstanceLevel, Key.X, Key.Z }, Distance });
            }
            else
            {
                for (int32 Child = 0; Child < 4; ++Child)
                {
                    Stack.Add({ Key.Level - 1, Key.X * 2 + (Child & 1), Key.Z * 2 + (Child >> 1) });
                }
            }
        }

        // Nearest first, so the ground under the view fills in before the horizon.
        Algo::Sort(Out, [](const FWanted& A, const FWanted& B) { return A.Distance < B.Distance; });
    }

    void CForestSystem::StartBuilds(const SForestComponent& Forest, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State,
                                    THashSet<uint64>& OutWantedSet, TVector<Forest::FNodeKey>& OutUnready)
    {
        using namespace ForestSystemInternal;

        State.Running.RemoveAll([](const TSharedPtr<FBuild>& Build) { return Build->bDone.load(std::memory_order_acquire); });
        int32 AggregateBuilds = 0;
        int32 InstanceBuilds  = 0;
        for (const TSharedPtr<FBuild>& Build : State.Running)
        {
            if (!Build->bCanceled.load(std::memory_order_relaxed))
            {
                ++(Build->bInstances ? InstanceBuilds : AggregateBuilds);
            }
        }

        for (const FWanted& Want : Wanted)
        {
            const uint64 Packed = Want.Key.Pack();
            OutWantedSet.Add(Packed);
            FNode& Node = State.Nodes.FindOrAdd(Packed);
            if (Node.bReady)
            {
                if (Node.DrawnFrames < kDrawnSettleFrames && Node.ReadyFrames < kMaxDrawWaitFrames)
                {
                    OutUnready.Add(Want.Key);
                }
                continue;
            }
            OutUnready.Add(Want.Key);
            if (Node.Build)
            {
                continue;
            }

            const bool bInstances = Want.Key.Level < 0;
            if ((bInstances && InstanceBuilds >= kMaxInstanceBuilds) || (!bInstances && AggregateBuilds >= Forest.MaxConcurrentBuilds))
            {
                continue;
            }

            TSharedPtr<FBuild> Build = MakeShared<FBuild>();
            Build->bInstances = bInstances;
            Node.Build = Build;
            State.Running.Add(Build);

            if (bInstances)
            {
                ++InstanceBuilds;
                const FVector2 Min = Forest::GetNodeMin(*State.Rules, { 0, Want.Key.X, Want.Key.Z });
                const float MinViewDistance = Want.Key.Level == kFarInstanceLevel ? kNearBandDistance : 0.0f;
                // A scatter is a few milliseconds and the plants around the view wait on it, so it does not queue behind aggregate builds.
                Jobs::Spawn(Jobs::EJobPriority::Low, [](TSharedPtr<FBuild> Target, TSharedPtr<const Forest::FRules> TileRules, FVector2 TileMin, float NearestView) -> Jobs::TTask<void>
                {
                    if (!Target->bCanceled.load(std::memory_order_relaxed))
                    {
                        Forest::ScatterRect(*TileRules, TileMin, TileMin + FVector2(TileRules->TileSize), Target->Plants, NearestView);
                    }
                    Target->bDone.store(true, std::memory_order_release);
                    co_return;
                }, Build, State.Rules, Min, MinViewDistance);
                continue;
            }

            ++AggregateBuilds;
            Forest::FAggregateRequest Request;
            Request.Rules      = State.Rules;
            Request.Geometry   = State.Geometry;
            Request.Key        = Want.Key;
            Request.Resolution = AggregateResolution(Forest);
            Request.bUseCache  = Forest.bCacheAggregates;
            const Jobs::EJobPriority Priority = Want.Distance < kNearBuildDistance ? Jobs::EJobPriority::Low : Jobs::EJobPriority::Background;
            Jobs::Spawn(Priority, [](TSharedPtr<FBuild> Target, Forest::FAggregateRequest Input) -> Jobs::TTask<void>
            {
                if (!Target->bCanceled.load(std::memory_order_relaxed))
                {
                    Forest::BuildAggregate(Input, Target->Aggregate);
                }
                Target->bDone.store(true, std::memory_order_release);
                co_return;
            }, Build, Move(Request));
        }
    }

    void CForestSystem::ApplyFinishedBuilds(CWorld& World, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        uint32 Applied = 0;
        for (const FWanted& Want : Wanted)
        {
            if (Applied >= kMaxAppliesPerFrame)
            {
                return;
            }
            FNode* Node = State.Nodes.Find(Want.Key.Pack());
            if (Node == nullptr || Node->bReady || !Node->Build || !Node->Build->bDone.load(std::memory_order_acquire))
            {
                continue;
            }
            ApplyBuild(World, Want.Key, *Node, State);
            ++Applied;
        }
    }

    void CForestSystem::RetireStaleNodes(CWorld& World, const THashSet<uint64>& WantedSet, const TVector<Forest::FNodeKey>& Unready, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        // A node the view no longer wants stays until whatever replaces it over its ground is ready, so the forest never opens a hole.
        TVector<uint64> Stale;
        for (auto& [Packed, Node] : State.Nodes)
        {
            if (WantedSet.Contains(Packed))
            {
                continue;
            }
            const Forest::FNodeKey Key = UnpackKey(Packed);
            const bool bCovering = Node.bReady && World.IsValidEntity(Node.Entity)
                && Algo::AnyOf(Unready, [&](const Forest::FNodeKey& Pending) { return Overlaps(Key, Pending); });
            if (!bCovering)
            {
                Stale.Add(Packed);
            }
        }
        for (uint64 Packed : Stale)
        {
            FNode* Node = State.Nodes.Find(Packed);
            if (Node != nullptr && Node->Build)
            {
                Node->Build->bCanceled.store(true, std::memory_order_relaxed);
            }
            if (Node != nullptr && World.IsValidEntity(Node->Entity))
            {
                World.DestroyEntity(Node->Entity);
            }
            State.Nodes.Remove(Packed);
        }
    }

    void CForestSystem::TrackDrawnNodes(CWorld& World, ForestSystemInternal::FForestState& State)
    {
        using namespace ForestSystemInternal;

        const auto IsDrawn = [&World](const FNode& Node)
        {
            // An empty node draws nothing, so nothing waits on it.
            if (!World.IsValidEntity(Node.Entity))
            {
                return true;
            }
            if (const SDynamicMeshComponent* Mesh = World.TryGetComponent<SDynamicMeshComponent>(Node.Entity))
            {
                const uint32 Version = Mesh->LoadRenderDataVersion();
                return Version != 0u && Mesh->SyncedRenderDataVersion == Version && Mesh->ResolvedRenderDataVersion == Version;
            }
            if (const SFoliageComponent* Foliage = World.TryGetComponent<SFoliageComponent>(Node.Entity))
            {
                return Foliage->BakedVersion == Foliage->InstancesVersion && !Foliage->bBakeIncomplete
                    && Algo::AllOf(Foliage->Types, [](const SFoliageType& Type) { return Type.ResolveHandle != Constants::kIndexNoneU32; });
            }
            return true;
        };

        int64 Awaiting = 0;
        for (auto& [Packed, Node] : State.Nodes)
        {
            if (!Node.bReady)
            {
                continue;
            }
            Node.ReadyFrames = (uint16)Math::Min<uint32>(Node.ReadyFrames + 1u, kMaxDrawWaitFrames);
            Node.DrawnFrames = IsDrawn(Node) ? (uint16)Math::Min<uint32>(Node.DrawnFrames + 1u, kDrawnSettleFrames) : 0u;
            Awaiting += Node.DrawnFrames < kDrawnSettleFrames && Node.ReadyFrames < kMaxDrawWaitFrames ? 1 : 0;
        }
        LUMINA_PROFILE_VALUE("Forest/AwaitingDraw", Awaiting);
    }

    void CForestSystem::PublishStats(CWorld& World, const ForestSystemInternal::FForestState& State)
    {
        int64 Aggregates = 0;
        int64 AggregateTriangles = 0;
        int64 Tiles = 0;
        int64 Plants = 0;
        for (const auto& [Packed, Node] : State.Nodes)
        {
            if (!World.IsValidEntity(Node.Entity))
            {
                continue;
            }
            if (const SDynamicMeshComponent* Mesh = World.TryGetComponent<SDynamicMeshComponent>(Node.Entity))
            {
                ++Aggregates;
                AggregateTriangles += Mesh->GetTriangleCount();
            }
            else if (const SFoliageComponent* Foliage = World.TryGetComponent<SFoliageComponent>(Node.Entity))
            {
                ++Tiles;
                Plants += Foliage->Instances.Num();
            }
        }
        LUMINA_PROFILE_VALUE("Forest/Aggregates", Aggregates);
        LUMINA_PROFILE_VALUE("Forest/AggregateTriangles", AggregateTriangles);
        LUMINA_PROFILE_VALUE("Forest/InstanceTiles", Tiles);
        LUMINA_PROFILE_VALUE("Forest/Plants", Plants);
        LUMINA_PROFILE_VALUE("Forest/BuildsRunning", (int64)State.Running.Num());
    }
}
