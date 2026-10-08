#include "MCPStressTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentAssetResolve.h"
#include "Containers/HashTable.h"
#include "Agent/AgentToolRegistry.h"
#include "Core/Math/Random.h"
#include "Core/Object/Cast.h"
#include "Physics/PhysicsLibrary.h"
#include "Session/SessionOps.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/InterpolatingMovementComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "World/WorldManager.h"

#include "nlohmann/json.hpp"

namespace Lumina::MCP
{
    namespace
    {
        // Every name starts with this, which is what stress.clear finds them by.
        constexpr const char* StressPrefix = "Stress";
        constexpr const char* LightName = "StressLight";
        constexpr const char* CasterName = "StressCaster";
        constexpr const char* BodyName = "StressBody";
        constexpr const char* MoverName = "StressMover";
        constexpr float NoGround = -1.0e9f;

        CWorld* ResolveWorld(bool bEditorWorld, FString& OutLabel, FString& OutError)
        {
            if (!bEditorWorld && GWorldManager != nullptr)
            {
                for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
                {
                    if (Context && Context->World.IsValid()
                        && (Context->Type == EWorldType::Game || Context->Type == EWorldType::Simulation))
                    {
                        OutLabel = "Game";
                        return Context->World.Get();
                    }
                }
            }

            OutLabel = "Editor";
            return SessionOps::GetSceneWorld(OutError);
        }

        bool IsStressEntity(const SNameComponent& Name)
        {
            const FString Text(Name.Name.ToString().c_str());
            return FStringView(Text).starts_with(FStringView(StressPrefix));
        }

        int32 CountStressEntities(CWorld* World)
        {
            int32 Count = 0;
            auto View = ECS::GetWorldRegistry(*World).View<SNameComponent>();
            for (ECS::FEntity Entity : View)
            {
                Count += IsStressEntity(View.Get<SNameComponent>(Entity)) ? 1 : 0;
            }
            return Count;
        }

        float GroundAt(CWorld* World, float X, float Z)
        {
            const SRayResult Hit = CPhysicsLibrary::Raycast(World, FVector3(X, 5000.0f, Z), FVector3(X, -5000.0f, Z), ECS::NullEntity);
            return Hit.bHit ? Hit.Location.y : NoGround;
        }

        bool ResolveCenter(CWorld* World, const FString& Text, FVector2& OutCenter, FString& OutError)
        {
            if (!Text.empty())
            {
                const nlohmann::json Parsed = nlohmann::json::parse(Text.c_str(), nullptr, false);
                if (!Parsed.is_array() || Parsed.size() != 2 || !Parsed[0].is_number() || !Parsed[1].is_number())
                {
                    OutError = "Center is [x, z].";
                    return false;
                }
                OutCenter = FVector2(Parsed[0].get<float>(), Parsed[1].get<float>());
                return true;
            }

            const ECS::FEntity Camera = World->GetActiveCameraEntity();
            const STransformComponent* Transform = Camera != ECS::NullEntity && World->IsValidEntity(Camera)
                ? World->TryGetComponent<STransformComponent>(Camera) : nullptr;
            const SCameraComponent* CameraComponent = Transform != nullptr ? World->TryGetComponent<SCameraComponent>(Camera) : nullptr;
            if (CameraComponent == nullptr)
            {
                OutError = "The world has no live camera to center on; pass Center.";
                return false;
            }

            // Where the view meets the ground, or a fixed distance ahead when it looks at the sky.
            constexpr float FallbackDistance = 40.0f;
            constexpr float TraceDistance = 2000.0f;
            const FVector3 Eye = Transform->GetWorldLocation();
            const FVector3 Forward = CameraComponent->GetForwardVector();
            const SRayResult Hit = CPhysicsLibrary::Raycast(World, Eye, Eye + Forward * TraceDistance, ECS::NullEntity);
            const FVector3 Target = Hit.bHit ? Hit.Location : Eye + Forward * FallbackDistance;
            OutCenter = FVector2(Target.x, Target.z);
            return true;
        }

        CStaticMesh* ResolveCasterMesh(const FString& Asset, FString& OutError)
        {
            if (Asset.empty())
            {
                return CPrimitiveManager::Get().CubeMesh.Get();
            }

            CObject* Object = nullptr;
            if (!Agent::ResolveAssetObject(FStringView(Asset), Object, OutError))
            {
                return nullptr;
            }
            CStaticMesh* Mesh = Cast<CStaticMesh>(Object);
            if (Mesh == nullptr)
            {
                OutError = Lumina::Format("'{}' is not a static mesh.", Asset);
            }
            return Mesh;
        }

        float Vary(FRandomStream& Random, float Base)
        {
            return Base * (0.5f + Random.NextFloat());
        }

        FVector2 PointInDisc(FRandomStream& Random, const FVector2& Center, float Radius)
        {
            const float Distance = Radius * Math::Sqrt(Random.NextFloat());
            const float Angle = Random.NextFloat() * 2.0f * Math::Pi<float>();
            return Center + FVector2(Math::Cos(Angle), Math::Sin(Angle)) * Distance;
        }

        // Ground position of the live camera, or none when the world has no camera.
        TOptional<FVector2> CameraGround(CWorld* World)
        {
            const ECS::FEntity Camera = World->GetActiveCameraEntity();
            const STransformComponent* Transform = Camera != ECS::NullEntity && World->IsValidEntity(Camera)
                ? World->TryGetComponent<STransformComponent>(Camera) : nullptr;
            if (Transform == nullptr)
            {
                return {};
            }
            const FVector3 Eye = Transform->GetWorldLocation();
            return FVector2(Eye.x, Eye.z);
        }

        // Draws disc points until one lands outside the camera's clear zone, giving up after a few tries.
        FVector2 PlacePoint(FRandomStream& Random, const FVector2& Center, float Radius, const TOptional<FVector2>& Camera, float KeepClear)
        {
            constexpr int32 MaxTries = 16;
            FVector2 Point = PointInDisc(Random, Center, Radius);
            for (int32 Try = 1; Try < MaxTries && Camera.IsSet() && Math::Distance(Point, *Camera) < KeepClear; ++Try)
            {
                Point = PointInDisc(Random, Center, Radius);
            }
            return Point;
        }

        FVector3 RandomColor(FRandomStream& Random)
        {
            // Saturated but never black, so every light shows up in the shading cost.
            return FVector3(0.3f + 0.7f * Random.NextFloat(), 0.3f + 0.7f * Random.NextFloat(), 0.3f + 0.7f * Random.NextFloat());
        }

        Agent::FToolResult Spawn(const SStressSpawnParams& In, SStressSpawnResult& Out)
        {
            FString Error;
            CWorld* World = ResolveWorld(In.bEditorWorld, Out.World, Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.empty() ? FString("No world is open.") : Error);
            }

            FVector2 Center;
            if (!ResolveCenter(World, In.Center, Center, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CStaticMesh* CasterMesh = nullptr;
            if ((In.Casters > 0 || In.Bodies > 0 || In.Movers > 0) && (CasterMesh = ResolveCasterMesh(In.CasterMesh, Error)) == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            constexpr int32 MaxPerCall = 20000;
            const int32 LightCount = Math::Clamp(In.PointLights, 0, MaxPerCall) + Math::Clamp(In.SpotLights, 0, MaxPerCall);
            FRandomStream Random((uint64)(uint32)In.Seed, 7u);
            const TOptional<FVector2> Camera = CameraGround(World);
            const float KeepClear = Math::Max(In.KeepClear, 0.0f);
            const float ShadowFraction = Math::Clamp(In.ShadowFraction, 0.0f, 1.0f);
            const float VolumetricFraction = Math::Clamp(In.VolumetricFraction, 0.0f, 1.0f);

            for (int32 Index = 0; Index < LightCount; ++Index)
            {
                const bool bSpot = Index >= Math::Clamp(In.PointLights, 0, MaxPerCall);
                const FVector2 Spot = PlacePoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundAt(World, Spot.x, Spot.y);
                const FVector3 Location(Spot.x, (Ground > NoGround ? Ground : 0.0f) + Vary(Random, In.Height), Spot.y);
                const bool bShadows = Random.NextFloat() < ShadowFraction;
                const bool bVolumetric = Random.NextFloat() < VolumetricFraction;

                // Pitched down around the +Z forward axis, with a tilt so the cones do not all stack.
                constexpr float DownPitch = 90.0f;
                constexpr float MaxTilt = 35.0f;
                const FVector3 Euler(DownPitch + (Random.NextFloat() * 2.0f - 1.0f) * MaxTilt, Random.NextFloat() * 360.0f, 0.0f);
                const ECS::FEntity Entity = World->ConstructEntity(FName(LightName), FTransform(Location, Euler, FVector3(1.0f)));
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }

                if (bSpot)
                {
                    SSpotLightComponent& Light = World->EmplaceComponent<SSpotLightComponent>(Entity);
                    Light.LightColor = RandomColor(Random);
                    Light.Intensity = Vary(Random, In.Intensity);
                    Light.Attenuation = Vary(Random, In.LightRadius);
                    Light.InnerConeAngle = 15.0f + Random.NextFloat() * 15.0f;
                    Light.OuterConeAngle = Light.InnerConeAngle + 5.0f + Random.NextFloat() * 15.0f;
                    Light.bCastShadows = bShadows;
                    Light.bVolumetric = bVolumetric;
                }
                else
                {
                    SPointLightComponent& Light = World->EmplaceComponent<SPointLightComponent>(Entity);
                    Light.LightColor = RandomColor(Random);
                    Light.Intensity = Vary(Random, In.Intensity);
                    Light.Attenuation = Vary(Random, In.LightRadius);
                    Light.bCastShadows = bShadows;
                    Light.bVolumetric = bVolumetric;
                }

                ++Out.Lights;
                Out.ShadowCasting += bShadows ? 1 : 0;
            }

            const int32 CasterCount = Math::Clamp(In.Casters, 0, MaxPerCall);
            for (int32 Index = 0; Index < CasterCount; ++Index)
            {
                const FVector2 Spot = PlacePoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundAt(World, Spot.x, Spot.y);
                const float Size = Vary(Random, In.CasterScale);
                const FVector3 Scale(Size, Size * (0.5f + 1.5f * Random.NextFloat()), Size);
                const FVector3 Location(Spot.x, (Ground > NoGround ? Ground : 0.0f) + Scale.y * 0.5f, Spot.y);
                const FVector3 Euler(0.0f, Random.NextFloat() * 360.0f, 0.0f);

                const ECS::FEntity Entity = World->ConstructEntity(FName(CasterName), FTransform(Location, Euler, Scale));
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }
                World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(CasterMesh);
                ++Out.Casters;
            }

            const int32 BodyCount = Math::Clamp(In.Bodies, 0, MaxPerCall);
            for (int32 Index = 0; Index < BodyCount; ++Index)
            {
                // Dropped from a spread of heights, so they land over time instead of all in one frame.
                constexpr float MinDrop = 5.0f;
                constexpr float DropSpread = 25.0f;
                const FVector2 Spot = PlacePoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundAt(World, Spot.x, Spot.y);
                const float Size = Vary(Random, In.BodyScale);
                const FVector3 Location(Spot.x, (Ground > NoGround ? Ground : 0.0f) + MinDrop + Random.NextFloat() * DropSpread, Spot.y);
                const FVector3 Euler(Random.NextFloat() * 360.0f, Random.NextFloat() * 360.0f, Random.NextFloat() * 360.0f);

                const ECS::FEntity Entity = World->ConstructEntity(FName(BodyName), FTransform(Location, Euler, FVector3(Size)));
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }
                World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(CasterMesh);
                // A collider brings its own rigid body along, so that one is fetched rather than added twice.
                World->EmplaceComponent<SBoxColliderComponent>(Entity);
                World->GetOrEmplaceComponent<SRigidBodyComponent>(Entity).BodyType = EBodyType::Dynamic;
                ++Out.Bodies;
            }

            const int32 MoverCount = Math::Clamp(In.Movers, 0, MaxPerCall);
            for (int32 Index = 0; Index < MoverCount; ++Index)
            {
                const FVector2 Spot = PlacePoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundAt(World, Spot.x, Spot.y);
                const float Size = Vary(Random, In.BodyScale);
                const FVector3 Location(Spot.x, (Ground > NoGround ? Ground : 0.0f) + 2.0f + Random.NextFloat() * 4.0f, Spot.y);

                const ECS::FEntity Entity = World->ConstructEntity(FName(MoverName), FTransform(Location, FVector3(0.0f), FVector3(Size)));
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }
                World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(CasterMesh);
                SSineWaveMovementComponent& Wave = World->EmplaceComponent<SSineWaveMovementComponent>(Entity);
                Wave.Amplitude   = 0.5f + Random.NextFloat() * 1.5f;
                Wave.Frequency   = 0.2f + Random.NextFloat() * 0.8f;
                Wave.PhaseOffset = Random.NextFloat() * Math::TwoPi<float>();
                ++Out.Movers;
            }

            Out.Center = { Center.x, Center.y };
            Out.Total = CountStressEntities(World);
            return Agent::FToolResult::Ok(Lumina::Format(
                "Spawned {} light(s), {} casting shadows, {} caster(s), {} body(ies) and {} mover(s) around [{:.1f}, {:.1f}] in the {} world. "
                "{} stress entities in total.",
                Out.Lights, Out.ShadowCasting, Out.Casters, Out.Bodies, Out.Movers, Center.x, Center.y, Out.World, Out.Total));
        }

        Agent::FToolResult Clear(const SStressClearParams& In, SStressClearResult& Out)
        {
            FString Label;
            FString Error;
            CWorld* World = ResolveWorld(In.bEditorWorld, Label, Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.empty() ? FString("No world is open.") : Error);
            }

            TVector<ECS::FEntity> Doomed;
            auto View = ECS::GetWorldRegistry(*World).View<SNameComponent>();
            for (ECS::FEntity Entity : View)
            {
                if (IsStressEntity(View.Get<SNameComponent>(Entity)))
                {
                    Doomed.push_back(Entity);
                }
            }
            for (ECS::FEntity Entity : Doomed)
            {
                World->DestroyEntity(Entity);
            }

            Out.Destroyed = (int32)Doomed.size();
            return Agent::FToolResult::Ok(Lumina::Format("Destroyed {} stress entities in the {} world.", Out.Destroyed, Label));
        }

        constexpr const char* GridPrefix = "StressGrid_";
        constexpr const char* DecoyName = "StressDecoy";
        constexpr float GridMeshFill = 0.6f;
        constexpr float GridMoveFraction = 0.08f;
        constexpr float DecoyOffset = 3000.0f;

        // Cell index from a grid entity's name, or none for every other entity.
        TOptional<int32> GridCellOf(const SNameComponent& Name)
        {
            const FString Text(Name.Name.ToString().c_str());
            const FStringView View(Text);
            if (!View.starts_with(FStringView(GridPrefix)))
            {
                return {};
            }
            return (int32)std::strtol(Text.c_str() + std::strlen(GridPrefix), nullptr, 10);
        }

        FVector3 GridCellCenter(const SStressGridParams& In, const FVector2& Origin, int32 Cell)
        {
            const int32 Column = Cell % In.Columns;
            const int32 Row = Cell / In.Columns;
            const float Size = In.CellSize * GridMeshFill;
            return FVector3(Origin.x + ((float)Column + 0.5f) * In.CellSize, Size * 0.5f, Origin.y + ((float)Row + 0.5f) * In.CellSize);
        }

        ECS::FEntity SpawnGridMesh(CWorld* World, const SStressGridParams& In, const FVector2& Origin, int32 Cell, CStaticMesh* Mesh)
        {
            const FVector3 Location = GridCellCenter(In, Origin, Cell);
            const FName Name(Lumina::Format("{}{}", GridPrefix, Cell).c_str());
            const ECS::FEntity Entity = World->ConstructEntity(Name, FTransform(Location, FVector3(0.0f), FVector3(In.CellSize * GridMeshFill)));
            if (Entity != ECS::NullEntity)
            {
                World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(Mesh);
            }
            return Entity;
        }

        Agent::FToolResult Grid(const SStressGridParams& In, SStressGridResult& Out)
        {
            FString Label;
            FString Error;
            CWorld* World = ResolveWorld(/*bEditorWorld*/ true, Label, Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.empty() ? FString("No world is open.") : Error);
            }
            if (In.Columns <= 0 || In.CellSize <= 0.0f)
            {
                return Agent::FToolResult::Error("Columns and CellSize must be positive.");
            }

            FVector2 Origin(0.0f);
            if (!In.Origin.empty() && !ResolveCenter(World, In.Origin, Origin, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CStaticMesh* Cube = CPrimitiveManager::Get().CubeMesh.Get();
            CStaticMesh* Sphere = CPrimitiveManager::Get().SphereMesh.Get();

            THashMap<int32, ECS::FEntity> Cells;
            TVector<ECS::FEntity> Decoys;
            {
                auto View = ECS::GetWorldRegistry(*World).View<SNameComponent>();
                for (ECS::FEntity Entity : View)
                {
                    const SNameComponent& Name = View.Get<SNameComponent>(Entity);
                    if (const TOptional<int32> Cell = GridCellOf(Name))
                    {
                        Cells[*Cell] = Entity;
                    }
                    else if (Name.Name == FName(DecoyName))
                    {
                        Decoys.push_back(Entity);
                    }
                }
            }

            if (In.bSetOccupancy)
            {
                THashSet<int32> Wanted;
                for (int32 Cell : In.Occupied)
                {
                    Wanted.insert(Cell);
                }
                for (auto It = Cells.begin(); It != Cells.end();)
                {
                    if (!Wanted.contains(It->first))
                    {
                        World->DestroyEntity(It->second);
                        ++Out.Destroyed;
                        It = Cells.erase(It);
                    }
                    else
                    {
                        ++It;
                    }
                }
                for (int32 Cell : Wanted)
                {
                    if (Cell >= 0 && !Cells.contains(Cell))
                    {
                        const ECS::FEntity Entity = SpawnGridMesh(World, In, Origin, Cell, Cube);
                        if (Entity != ECS::NullEntity)
                        {
                            Cells[Cell] = Entity;
                            ++Out.Created;
                        }
                    }
                }
            }

            FRandomStream Random((uint64)(uint32)In.Seed, 11u);
            for (int32 Cell : In.Move)
            {
                auto Found = Cells.find(Cell);
                if (Found == Cells.end())
                {
                    continue;
                }
                const float Reach = In.CellSize * GridMoveFraction;
                const FVector3 Offset((Random.NextFloat() * 2.0f - 1.0f) * Reach, 0.0f, (Random.NextFloat() * 2.0f - 1.0f) * Reach);
                STransformComponent& Transform = World->GetComponent<STransformComponent>(Found->second);
                Transform.SetLocation(GridCellCenter(In, Origin, Cell) + Offset);
                Transform.SetRotationFromEuler(FVector3(0.0f, Random.NextFloat() * 360.0f, 0.0f));
                ++Out.Moved;
            }

            for (int32 Cell : In.Swap)
            {
                auto Found = Cells.find(Cell);
                if (Found == Cells.end())
                {
                    continue;
                }
                SStaticMeshComponent& MeshComponent = World->GetComponent<SStaticMeshComponent>(Found->second);
                MeshComponent.SetStaticMesh(MeshComponent.GetStaticMesh() == Cube ? Sphere : Cube);
                ++Out.Swapped;
            }

            for (int32 Cell : In.Respawn)
            {
                auto Found = Cells.find(Cell);
                if (Found == Cells.end())
                {
                    continue;
                }
                World->DestroyEntity(Found->second);
                Found->second = SpawnGridMesh(World, In, Origin, Cell, Cube);
                ++Out.Respawned;
            }

            if (In.bClearDecoys)
            {
                for (ECS::FEntity Entity : Decoys)
                {
                    World->DestroyEntity(Entity);
                }
                Decoys.clear();
            }

            constexpr int32 MaxDecoysPerCall = 200000;
            const int32 DecoyCount = Math::Clamp(In.Decoys, 0, MaxDecoysPerCall);
            for (int32 Index = 0; Index < DecoyCount; ++Index)
            {
                // Far off to the side, so they cost slots and uploads but never reach the picture being checked.
                const FVector3 Location(DecoyOffset + Random.NextFloat() * 400.0f, 0.0f, Random.NextFloat() * 400.0f - 200.0f);
                const ECS::FEntity Entity = World->ConstructEntity(FName(DecoyName), FTransform(Location, FVector3(0.0f), FVector3(1.0f)));
                if (Entity != ECS::NullEntity)
                {
                    World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(Cube);
                    Decoys.push_back(Entity);
                }
            }

            Out.GridMeshes = (int32)Cells.size();
            Out.DecoyMeshes = (int32)Decoys.size();
            return Agent::FToolResult::Ok(Lumina::Format(
                "Grid now holds {} meshes ({} created, {} destroyed, {} moved, {} swapped, {} respawned); {} decoys.",
                Out.GridMeshes, Out.Created, Out.Destroyed, Out.Moved, Out.Swapped, Out.Respawned, Out.DecoyMeshes));
        }
    }

    void RegisterStressTools(FStringView Owner)
    {
        Agent::FToolRegistry& Registry = Agent::FToolRegistry::Get();

        Registry.Register<SStressSpawnParams, SStressSpawnResult>(
            Owner, "stress.spawn",
            "Scatter point and spot lights, with chosen shares casting shadows or scattering through fog, and meshes for "
            "them to shadow, across the ground around a center. Seeded, so the same call rebuilds the same scene. Bypasses "
            "undo; remove everything it made with stress.clear.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &Spawn);

        Registry.Register<SStressClearParams, SStressClearResult>(
            Owner, "stress.clear",
            "Destroy every entity stress.spawn made.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &Clear);

        Registry.Register<SStressGridParams, SStressGridResult>(
            Owner, "stress.grid",
            "Edit a grid of meshes in the edited world in one frame, for checking the renderer against a known layout. "
            "Sets which cells hold a mesh, moves, swaps or respawns cells, and adds or clears off-screen decoys that grow "
            "the scene's buffers under those changes. Bypasses undo; stress.clear removes it all.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &Grid);
    }
}
