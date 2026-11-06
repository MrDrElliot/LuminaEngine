#pragma once

#include <cmath>
#include <thread>

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavTypes.h"
#include "Containers/Function.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Platform/Time/PlatformTime.h"
#include "World/World.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/PathFollowSystem.h"
#include "WorldTestAccess.h"

namespace Lumina::NavTest
{
    inline void BindTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    inline ECS::FEntity SpawnBox(ECS::FRegistry& Registry, const FVector3& Location, const FVector3& Half)
    {
        const ECS::FEntity Entity = Registry.Create();
        FTransform Transform;
        Transform.SetLocation(Location);
        Registry.Emplace<STransformComponent>(Entity, Transform);
        Registry.Emplace<SBoxColliderComponent>(Entity).HalfExtent = Half;
        return Entity;
    }

    // Gentle hills, low enough that every slope stays walkable.
    inline float GroundHeight(float X, float Z)
    {
        return 3.0f * std::sin(X * 0.011f) * std::cos(Z * 0.013f) + 1.0f * std::sin(X * 0.05f + Z * 0.04f);
    }

    constexpr float kTerrainBase = -5.0f;
    constexpr float kTerrainRange = 10.0f;

    // One heightfield spanning Size at a meter per sample, sitting where GroundHeight says.
    inline ECS::FEntity SpawnTerrain(ECS::FRegistry& Registry, float Size)
    {
        const ECS::FEntity Entity = Registry.Create();
        FTransform Transform;
        Transform.SetLocation(FVector3(0.0f, kTerrainBase, 0.0f));
        Registry.Emplace<STransformComponent>(Entity, Transform);

        STerrainComponent Terrain;
        Terrain.Resolution = (int32)Size + 1;
        Terrain.TileWorldSize = Size;
        Terrain.MaxHeight = kTerrainRange;
        Terrain.Heightmap.SetNum(Lumina::IntCastChecked<Lumina::int32>(static_cast<int64>(Terrain.Resolution) * Terrain.Resolution));
        const float Stride = Size / (float)(Terrain.Resolution - 1);
        for (int32 Row = 0; Row < Terrain.Resolution; ++Row)
        {
            for (int32 Col = 0; Col < Terrain.Resolution; ++Col)
            {
                const float Height = GroundHeight(-Size * 0.5f + Col * Stride, -Size * 0.5f + Row * Stride);
                Terrain.Heightmap[Row * Terrain.Resolution + Col] = (Height - kTerrainBase) / kTerrainRange;
            }
        }
        Registry.Emplace<STerrainComponent>(Entity, std::move(Terrain));
        Registry.Emplace<STerrainColliderComponent>(Entity);
        return Entity;
    }

    // A level as the engine sees it, colliders in a registry and a nav volume ticked by the real system.
    struct FNavWorld
    {
        CWorld*            World = nullptr;
        SNavMeshSystem*    System = nullptr;
        SPathFollowSystem* Follow = nullptr;
        ECS::FEntity       Volume;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        SNavMeshComponent& Nav() const { return Registry().Get<SNavMeshComponent>(Volume); }

        // A world with only ground under the volume, square and Size across, flat slabs or one terrain.
        void Create(float Size, float GroundSlab = 256.0f, bool bTerrain = false)
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("NavTestWorld"), FGuid::New(), OF_Transient);
            ECS::FRegistry& R = Registry();
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindTransform>();

            const float Slab = Math::Min(GroundSlab, Size);
            const int32 Slabs = bTerrain ? 0 : Math::Max(1, (int32)(Size / Slab));
            for (int32 Z = 0; Z < Slabs; ++Z)
            {
                for (int32 X = 0; X < Slabs; ++X)
                {
                    SpawnBox(R, FVector3(-Size * 0.5f + (X + 0.5f) * Slab, -0.5f, -Size * 0.5f + (Z + 0.5f) * Slab), FVector3(Slab * 0.5f, 0.5f, Slab * 0.5f));
                }
            }
            if (bTerrain)
            {
                SpawnTerrain(R, Size);
            }

            Volume = R.Create();
            R.Emplace<STransformComponent>(Volume);
            SNavMeshComponent& Mesh = R.Emplace<SNavMeshComponent>(Volume);
            Mesh.Center = FVector3(0.0f, 2.0f, 0.0f);
            Mesh.Extents = FVector3(Size * 0.5f, 8.0f, Size * 0.5f);
            Mesh.bAutoBake = false;
            Mesh.bDynamicRebuild = true;
            Mesh.DynamicRebuildInterval = 0.0f;
        }

        void Start(bool bWithPathFollow = false)
        {
            Nav().bBakeRequested = true;
            System = NewObject<SNavMeshSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();
            World->Systems.Add(System);
            System->OnStartup(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
            if (bWithPathFollow)
            {
                Follow = NewObject<SPathFollowSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
                Follow->SetOwningWorld(World);
                Follow->ConfigureSystem();
                World->Systems.Add(Follow);
                Follow->OnStartup(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
            }
        }

        void Stop()
        {
            if (Follow != nullptr)
            {
                Follow->OnTeardown(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
                Follow = nullptr;
            }
            if (System != nullptr)
            {
                System->OnTeardown(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
                System = nullptr;
            }
        }

        double TickFollow()
        {
            PlatformTime::FStopwatch Watch;
            Follow->OnUpdate(FWorldTestAccess::SystemContext(*World));
            return Watch.ElapsedMilliseconds();
        }

        double Tick()
        {
            PlatformTime::FStopwatch Watch;
            System->OnUpdate(FWorldTestAccess::SystemContext(*World));
            return Watch.ElapsedMilliseconds();
        }

        bool TickUntil(const TFunction<bool()>& Done, double TimeoutMs)
        {
            PlatformTime::FStopwatch Watch;
            while (!Done())
            {
                if (Watch.ElapsedMilliseconds() > TimeoutMs)
                {
                    return false;
                }
                Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return true;
        }

        bool WaitReady(double TimeoutMs = 60000.0)
        {
            return TickUntil([this] { return Nav().Runtime.State == ENavBakeState::Ready; }, TimeoutMs);
        }

        // Settled once nothing is dirty, settling or rebaking, after a couple of scans have had their look.
        bool WaitSettled(double TimeoutMs = 60000.0)
        {
            Tick();
            Tick();
            return TickUntil([this]
            {
                const FNavMeshRuntime& Runtime = Nav().Runtime;
                return Runtime.DirtyTiles.IsEmpty() && Runtime.SettlingTiles.IsEmpty() && Runtime.PendingRebakes.IsEmpty() && Runtime.PendingSourceChanges.IsEmpty();
            }, TimeoutMs);
        }

        // True where an agent could stand on the ground itself, rather than on something set down there.
        bool IsGroundWalkable(float X, float Z, float GroundY = 0.0f) const
        {
            FVector3 Out;
            FNavQueryFilter Filter;
            // The nearest polygon can be the ground beside an obstacle, so the answer has to stay where it was asked.
            return Nav().Runtime.Mesh && Nav().Runtime.Mesh->ProjectPoint(FVector3(X, GroundY, Z), FVector3(0.1f, 0.3f, 0.1f), Filter, Out)
                && std::fabs(Out.y - GroundY) < 0.45f && std::fabs(Out.x - X) < 0.05f && std::fabs(Out.z - Z) < 0.05f;
        }
    };
}
