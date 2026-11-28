#include "RuntimePCH.h"
#include "TerrainCollisionSystem.h"
#include "SystemContext.h"
#include "Containers/Algorithm.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/CameraSubsystem.h"

namespace Lumina
{
    namespace TerrainCollisionInternal
    {
        // A patch is this many quads a side, 256 m at the 2 m spacing of a very large map.
        constexpr int32  kPatchQuads = 128;

        // A terrain of at most this many patches, a 2049 map, keeps all of them so a query anywhere hits.
        constexpr int32  kAlwaysResidentPatches = 256;

        // Characters and moving bodies keep the patches within this of them, which covers a body crossing a seam between scans.
        constexpr float  kBodyReach = 32.0f;

        // Bodies are many and slow next to the frame, so they are scanned this often while cameras are every frame.
        constexpr uint32 kBodyScanFrames = 8;

        // A patch stays this long after nothing wants it, so a source on a seam does not churn bodies.
        constexpr uint32 kKeepFrames = 120;

        constexpr int32  kMaxBuildsInFlight  = 8;
        constexpr int32  kMaxCommitsPerFrame = 8;

        uint64 PackPatch(int32 X, int32 Z)
        {
            return (uint64(uint32(X)) << 32) | uint64(uint32(Z));
        }

        FIntVector2 UnpackPatch(uint64 Key)
        {
            return FIntVector2(int32(Key >> 32), int32(Key & 0xFFFFFFFFu));
        }

        int32 PatchesPerSide(const FTerrainLayout& Layout)
        {
            return (Layout.Resolution - 1 + kPatchQuads - 1) / kPatchQuads;
        }

        // The patch's first sample and its sample counts, the last patch of a row ending on the map's edge.
        void PatchSamples(const FTerrainLayout& Layout, uint64 Key, FIntVector2& OutMin, FIntVector2& OutCount)
        {
            const FIntVector2 Patch = UnpackPatch(Key);
            const int32 Last = Layout.Resolution - 1;
            OutMin   = Patch * kPatchQuads;
            OutCount = FIntVector2(Math::Min(kPatchQuads, Last - OutMin.x) + 1, Math::Min(kPatchQuads, Last - OutMin.y) + 1);
        }

        void DestroyPatchBody(Physics::IPhysicsScene* Scene, FPatch& Patch)
        {
            if (Scene != nullptr && Patch.GroupID != 0)
            {
                Scene->DestroyStaticBodyGroup(Patch.GroupID);
            }
            Patch.GroupID = 0;
        }

        void CancelBuild(FPatch& Patch)
        {
            if (Patch.Build != nullptr)
            {
                Patch.Build->bCanceled.store(true, std::memory_order_relaxed);
                Patch.Build = nullptr;
            }
        }
    }

    void CTerrainCollisionSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RequireUpdate(EUpdateStage::Paused);
    }

    bool CTerrainCollisionSystem::HasWork(const SSystemContext&)
    {
        CWorld* World = GetWorld();
        return !Terrains.IsEmpty() || (World != nullptr && !World->View<STerrainColliderComponent>().IsEmpty());
    }

    bool CTerrainCollisionSystem::StreamsCollision(int32 Resolution)
    {
        using namespace TerrainCollisionInternal;
        const int32 Side = (Resolution - 1 + kPatchQuads - 1) / kPatchQuads;
        return Side * Side > kAlwaysResidentPatches;
    }

    int32 CTerrainCollisionSystem::GetResidentPatchCount() const
    {
        int32 Count = 0;
        for (const auto& [Entity, State] : Terrains)
        {
            for (const auto& [Key, Patch] : State.Patches)
            {
                Count += Patch.GroupID != 0 ? 1 : 0;
            }
        }
        return Count;
    }

    bool CTerrainCollisionSystem::HasPendingBuilds() const
    {
        for (const auto& [Entity, State] : Terrains)
        {
            for (const auto& [Key, Patch] : State.Patches)
            {
                if (Patch.Build != nullptr)
                {
                    return true;
                }
            }
        }
        return false;
    }

    void CTerrainCollisionSystem::GatherSources(const SExclusiveContext& Context, bool bScanBodies)
    {
        using namespace TerrainCollisionInternal;

        Views.Reset();
        if (CCameraSubsystem* Camera = Context.GetWorld()->GetCamera())
        {
            Views.Add(Camera->GetViewPosition());
        }

        if (!bScanBodies)
        {
            Bodies.Reset();
            return;
        }
        if (Frame % kBodyScanFrames != 1 && !Bodies.IsEmpty())
        {
            return;
        }
        Bodies.Reset();
        ECS::FRegistry& Registry = Context.GetRegistry();
        Registry.View<SCharacterPhysicsComponent, STransformComponent>().ForEach([&](ECS::FEntity, const SCharacterPhysicsComponent&, const STransformComponent& Transform)
        {
            Bodies.Add(Transform.GetWorldLocation());
        });
        Registry.View<SRigidBodyComponent, STransformComponent>().ForEach([&](ECS::FEntity, const SRigidBodyComponent& Body, const STransformComponent& Transform)
        {
            if (Body.BodyType != EBodyType::Static)
            {
                Bodies.Add(Transform.GetWorldLocation());
            }
        });
    }

    void CTerrainCollisionSystem::GatherWanted(const TerrainCollisionInternal::FTerrainLayout& Layout, float StreamingRadius, TVector<TerrainCollisionInternal::FWantedPatch>& Out) const
    {
        using namespace TerrainCollisionInternal;

        Out.Reset();
        const int32 Side      = PatchesPerSide(Layout);
        const float PatchSize = float(kPatchQuads) * Layout.Spacing;
        auto NearestView = [&](int32 X, int32 Z)
        {
            const FVector2 Min(Layout.Corner.x + float(X) * PatchSize, Layout.Corner.z + float(Z) * PatchSize);
            float Nearest = TNumericLimits<float>::Max();
            for (const FVector3& View : Views)
            {
                const FVector2 Point(View.x, View.z);
                Nearest = Math::Min(Nearest, Math::Length(Math::Clamp(Point, Min, Min + FVector2(PatchSize)) - Point));
            }
            return Nearest;
        };

        if (!StreamsCollision(Layout.Resolution))
        {
            for (int32 Z = 0; Z < Side; ++Z)
            {
                for (int32 X = 0; X < Side; ++X)
                {
                    Out.Add({ PackPatch(X, Z), NearestView(X, Z) });
                }
            }
        }
        else
        {
            auto AddAround = [&](const FVector3& Point, float Reach, bool bView)
            {
                const int32 MinX = Math::Max((int32)Math::Floor((Point.x - Reach - Layout.Corner.x) / PatchSize), 0);
                const int32 MinZ = Math::Max((int32)Math::Floor((Point.z - Reach - Layout.Corner.z) / PatchSize), 0);
                const int32 MaxX = Math::Min((int32)Math::Floor((Point.x + Reach - Layout.Corner.x) / PatchSize), Side - 1);
                const int32 MaxZ = Math::Min((int32)Math::Floor((Point.z + Reach - Layout.Corner.z) / PatchSize), Side - 1);
                for (int32 Z = MinZ; Z <= MaxZ; ++Z)
                {
                    for (int32 X = MinX; X <= MaxX; ++X)
                    {
                        // Something stands on a body's patch already, so it builds before anything a view only might reach.
                        Out.Add({ PackPatch(X, Z), bView ? NearestView(X, Z) : -1.0f });
                    }
                }
            };
            for (const FVector3& View : Views)
            {
                AddAround(View, StreamingRadius, true);
            }
            for (const FVector3& Body : Bodies)
            {
                AddAround(Body, kBodyReach, false);
            }
        }
        Algo::Sort(Out, [](const FWantedPatch& A, const FWantedPatch& B) { return A.Distance < B.Distance; });
    }

    void CTerrainCollisionSystem::UpdateTerrain(ECS::FEntity Entity, const TerrainCollisionInternal::FTerrainLayout& Layout, const STerrainColliderComponent& Collider,
                                                Physics::IPhysicsScene& Scene, TerrainCollisionInternal::FTerrainCollision& State)
    {
        using namespace TerrainCollisionInternal;

        GatherWanted(Layout, Collider.StreamingRadius, WantedScratch);

        int32 Running = 0;
        for (const auto& [Key, Patch] : State.Patches)
        {
            Running += Patch.Build != nullptr ? 1 : 0;
        }

        int32 Commits = 0;
        for (const FWantedPatch& Want : WantedScratch)
        {
            FPatch& Patch = State.Patches.FindOrAdd(Want.Key);
            Patch.LastWanted = Frame;

            if (Patch.Build != nullptr && Patch.Build->bDone.load(std::memory_order_acquire))
            {
                if (Commits >= kMaxCommitsPerFrame)
                {
                    continue;
                }
                TSharedPtr<const Physics::FCollisionShape> Shape = Move(Patch.Build->Shape);
                Patch.Build = nullptr;
                --Running;
                ++Commits;
                DestroyPatchBody(&Scene, Patch);
                if (Shape != nullptr)
                {
                    FIntVector2 Min, Count;
                    PatchSamples(Layout, Want.Key, Min, Count);
                    Physics::FStaticInstanceDesc Desc;
                    Desc.Position = Layout.Corner + FVector3(float(Min.x) * Layout.Spacing, 0.0f, float(Min.y) * Layout.Spacing);
                    Desc.Shape    = Move(Shape);
                    Desc.Material = Collider.PhysicsMaterial.Get();
                    Patch.GroupID = Scene.CreateStaticBodyGroup(Entity, TSpan<const Physics::FStaticInstanceDesc>(&Desc, 1));
                }
            }

            const bool bNeedsBuild = Patch.Build == nullptr && (Patch.GroupID == 0 || Patch.bStale);
            if (!bNeedsBuild || Running >= kMaxBuildsInFlight)
            {
                continue;
            }

            TSharedPtr<FPatchBuild> Build = MakeShared<FPatchBuild>();
            Patch.Build  = Build;
            Patch.bStale = false;
            ++Running;

            Physics::FHeightFieldDesc Desc;
            FIntVector2 Min;
            FIntVector2 Count;
            PatchSamples(Layout, Want.Key, Min, Count);
            Desc.CountX    = Count.x;
            Desc.CountZ    = Count.y;
            Desc.Spacing   = Layout.Spacing;
            Desc.MaxHeight = Layout.MaxHeight;
            Jobs::Spawn(Jobs::EJobPriority::Normal, [](TSharedPtr<FPatchBuild> Target, TSharedPtr<FTerrainStore> Store, FIntVector2 First, Physics::FHeightFieldDesc Field) -> Jobs::TTask<void>
            {
                Physics::IPhysicsContext* Physics = Physics::GetPhysicsContext();
                if (!Target->bCanceled.load(std::memory_order_relaxed) && Physics != nullptr)
                {
                    Field.Heights.SetNum(Field.CountX * Field.CountZ);
                    Store->ReadHeights(0, First, FIntVector2(Field.CountX, Field.CountZ), Field.Heights.GetData());
                    for (float& Height : Field.Heights)
                    {
                        Height *= Field.MaxHeight;
                    }
                    Target->Shape = Physics->CookHeightField(Field);
                }
                Target->bDone.store(true, std::memory_order_release);
                co_return;
            }, Build, Layout.Store, Min, Move(Desc));
        }

        // Patches nothing has wanted for a while give their bodies back.
        TVector<uint64> Dropped;
        for (auto& [Key, Patch] : State.Patches)
        {
            if (Frame - Patch.LastWanted > kKeepFrames)
            {
                Dropped.Add(Key);
            }
        }
        for (uint64 Key : Dropped)
        {
            FPatch& Patch = State.Patches[Key];
            CancelBuild(Patch);
            DestroyPatchBody(&Scene, Patch);
            State.Patches.Remove(Key);
        }
    }

    void CTerrainCollisionSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace TerrainCollisionInternal;

        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();
        // A new scene holds none of the old bodies, so nothing is destroyed through it.
        if (Scene != BoundScene)
        {
            ReleaseAll(nullptr);
            BoundScene = Scene;
        }
        if (Scene == nullptr)
        {
            return;
        }

        ++Frame;

        // Every patch of a small terrain stays resident, so with no large one the crowd is never scanned.
        bool bAnyStreams = false;
        Context.GetRegistry().View<STerrainColliderComponent, STerrainComponent>().ForEach([&](ECS::FEntity, const STerrainColliderComponent&, const STerrainComponent& Terrain)
        {
            bAnyStreams = bAnyStreams || (Terrain.Store != nullptr && StreamsCollision(Terrain.Store->GetResolution()));
        });
        GatherSources(Context, bAnyStreams);

        TVector<ECS::FEntity> Live;
        Context.GetRegistry().View<STerrainColliderComponent, STerrainComponent, STransformComponent>().ForEach(
            [&](ECS::FEntity Entity, const STerrainColliderComponent& Collider, STerrainComponent& Terrain, const STransformComponent& Transform)
        {
            if (Terrain.Store == nullptr || Terrain.Resolution < 2)
            {
                return;
            }
            Live.Add(Entity);

            FTerrainLayout Layout;
            Layout.Store      = Terrain.Store;
            Layout.Resolution = Terrain.Store->GetResolution();
            Layout.Spacing    = Terrain.TileWorldSize / float(Layout.Resolution - 1);
            Layout.MaxHeight  = Terrain.MaxHeight;
            // Terrain collides axis aligned and unscaled, centered on its entity like it draws.
            Layout.Corner     = Transform.GetWorldLocation() - FVector3(Terrain.TileWorldSize * 0.5f, 0.0f, Terrain.TileWorldSize * 0.5f);

            FTerrainCollision& State = Terrains.FindOrAdd(Entity);
            if (!(State.Layout == Layout))
            {
                ReleaseTerrain(Scene, State);
                State.Layout = Layout;
            }

            // A stroke changes the heights every frame, so patches under it rebuild once a frame passes without a change.
            FTerrainCPUState& CPU = Terrain.CPUState;
            const uint32 Version = CPU.HeightmapVersion;
            if (Version == State.SeenVersion && CPU.CollisionDirtyMax.x >= CPU.CollisionDirtyMin.x)
            {
                const FIntVector2 Lo = FIntVector2(Math::Max(CPU.CollisionDirtyMin.x - 1, 0), Math::Max(CPU.CollisionDirtyMin.y - 1, 0)) / kPatchQuads;
                const FIntVector2 Hi = CPU.CollisionDirtyMax / kPatchQuads;
                for (auto& [Key, Patch] : State.Patches)
                {
                    const FIntVector2 At = UnpackPatch(Key);
                    if (At.x >= Lo.x && At.x <= Hi.x && At.y >= Lo.y && At.y <= Hi.y)
                    {
                        CancelBuild(Patch);
                        Patch.bStale = true;
                    }
                }
                CPU.CollisionDirtyMin = FIntVector2(INT32_MAX);
                CPU.CollisionDirtyMax = FIntVector2(INT32_MIN);
            }
            State.SeenVersion = Version;

            UpdateTerrain(Entity, Layout, Collider, *Scene, State);
        });

        TVector<ECS::FEntity> Gone;
        for (auto& [Entity, State] : Terrains)
        {
            if (!Algo::Contains(Live, Entity))
            {
                Gone.Add(Entity);
            }
        }
        for (ECS::FEntity Entity : Gone)
        {
            ReleaseTerrain(Scene, Terrains[Entity]);
            Terrains.Remove(Entity);
        }
    }

    void CTerrainCollisionSystem::ReleaseTerrain(Physics::IPhysicsScene* Scene, TerrainCollisionInternal::FTerrainCollision& State)
    {
        using namespace TerrainCollisionInternal;
        for (auto& [Key, Patch] : State.Patches)
        {
            CancelBuild(Patch);
            DestroyPatchBody(Scene, Patch);
        }
        State.Patches.Reset();
    }

    void CTerrainCollisionSystem::ReleaseAll(Physics::IPhysicsScene* Scene)
    {
        for (auto& [Entity, State] : Terrains)
        {
            ReleaseTerrain(Scene, State);
        }
        Terrains.Reset();
    }

    void CTerrainCollisionSystem::OnTeardown(const SExclusiveContext& Context)
    {
        ReleaseAll(Context.GetPhysicsScene() == BoundScene ? BoundScene : nullptr);
        BoundScene = nullptr;
    }
}
