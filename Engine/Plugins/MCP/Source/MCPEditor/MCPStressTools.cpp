#include "MCPStressTools.h"
#include "MCPSceneTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentAssetResolve.h"
#include "Containers/HashTable.h"
#include "Agent/AgentToolRegistry.h"
#include "Core/Math/Random.h"
#include "Core/Object/Cast.h"
#include "Session/SessionOps.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/InterpolatingMovementComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

#include "Core/Json/Json.h"

namespace Lumina::MCP
{
    namespace MCPStressToolsInternal
    {
        // Every name starts with this, which is what stress.clear finds them by.
        constexpr const char* StressPrefix = "Stress";
        constexpr const char* StressLightName = "StressLight";
        constexpr const char* StressCasterName = "StressCaster";
        constexpr const char* StressBodyName = "StressBody";
        constexpr const char* StressMoverName = "StressMover";
        constexpr const char* StressParticleName = "StressParticles";

        CWorld* ResolveStressWorld(bool bEditorWorld, FString& OutLabel, FString& OutError)
        {
            if (CWorld* Game = bEditorWorld ? nullptr : FindGameWorld())
            {
                OutLabel = "Game";
                return Game;
            }

            OutLabel = "Editor";
            return SessionOps::GetSceneWorld(OutError);
        }

        bool IsStressEntity(const SNameComponent& Name)
        {
            const FString Text(*Name.Name.ToString());
            return FStringView(Text).StartsWith(FStringView(StressPrefix));
        }

        int32 CountStressEntities(CWorld* World)
        {
            int32 Count = 0;
            auto View = World->GetEntityRegistry().View<SNameComponent>();
            for (ECS::FEntity Entity : View)
            {
                Count += IsStressEntity(View.Get<SNameComponent>(Entity)) ? 1 : 0;
            }
            return Count;
        }

        bool ResolveStressCenter(CWorld* World, const FString& Text, FVector2& OutCenter, FString& OutError)
        {
            if (!Text.IsEmpty())
            {
                FJsonValue Parsed;
                if (!FJsonValue::Parse(Text, Parsed) || !Parsed.IsArray() || Parsed.Num() != 2 || !Parsed[0].IsNumber() || !Parsed[1].IsNumber())
                {
                    OutError = "Center is [x, z].";
                    return false;
                }
                OutCenter = FVector2(Parsed[0].AsFloat(), Parsed[1].AsFloat());
                return true;
            }

            const ECS::FEntity Camera = World->GetCamera()->GetActiveCameraEntity();
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
            const SRayResult Hit = World->GetPhysics()->Raycast(Eye, Eye + Forward * TraceDistance, ECS::NullEntity);
            const FVector3 Target = Hit.bHit ? Hit.Location : Eye + Forward * FallbackDistance;
            OutCenter = FVector2(Target.x, Target.z);
            return true;
        }

        CStaticMesh* ResolveStressCasterMesh(const FString& Asset, FString& OutError)
        {
            if (Asset.IsEmpty())
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

        bool ResolveStressParticleSystems(const FString& List, TVector<CParticleSystem*>& OutSystems, FString& OutError)
        {
            FStringView Remaining(List);
            while (!Remaining.IsEmpty())
            {
                const int32 Comma = Remaining.Find(',');
                FStringView Entry = Comma == Constants::kIndexNone ? Remaining : Remaining.Left(Comma);
                Remaining = Comma == Constants::kIndexNone ? FStringView() : Remaining.Mid(Comma + 1);
                while (!Entry.IsEmpty() && Entry[0] == ' ') { Entry.RemovePrefix(1); }
                while (!Entry.IsEmpty() && Entry.Last() == ' ') { Entry.RemoveSuffix(1); }
                if (Entry.IsEmpty())
                {
                    continue;
                }

                CObject* Object = nullptr;
                if (!Agent::ResolveAssetObject(Entry, Object, OutError))
                {
                    return false;
                }
                CParticleSystem* System = Cast<CParticleSystem>(Object);
                if (System == nullptr)
                {
                    OutError = Lumina::Format("'{}' is not a particle system.", FString(Entry.GetData(), Entry.Len()));
                    return false;
                }
                OutSystems.Add(System);
            }

            if (OutSystems.IsEmpty())
            {
                OutError = "Particles needs at least one asset in ParticleSystems.";
                return false;
            }
            return true;
        }

        float VaryStress(FRandomStream& Random, float Base)
        {
            return Base * (0.5f + Random.NextFloat());
        }

        FVector2 StressPointInDisc(FRandomStream& Random, const FVector2& Center, float Radius)
        {
            const float Distance = Radius * Math::Sqrt(Random.NextFloat());
            const float Angle = Random.NextFloat() * 2.0f * Math::Pi<float>();
            return Center + FVector2(Math::Cos(Angle), Math::Sin(Angle)) * Distance;
        }

        // Ground position of the live camera, or none when the world has no camera.
        TOptional<FVector2> StressCameraGround(CWorld* World)
        {
            const ECS::FEntity Camera = World->GetCamera()->GetActiveCameraEntity();
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
        FVector2 PlaceStressPoint(FRandomStream& Random, const FVector2& Center, float Radius, const TOptional<FVector2>& Camera, float KeepClear)
        {
            constexpr int32 MaxTries = 16;
            FVector2 Point = StressPointInDisc(Random, Center, Radius);
            for (int32 Try = 1; Try < MaxTries && Camera.IsSet() && Math::Distance(Point, *Camera) < KeepClear; ++Try)
            {
                Point = StressPointInDisc(Random, Center, Radius);
            }
            return Point;
        }

        FVector3 RandomStressColor(FRandomStream& Random)
        {
            // Saturated but never black, so every light shows up in the shading cost.
            return FVector3(0.3f + 0.7f * Random.NextFloat(), 0.3f + 0.7f * Random.NextFloat(), 0.3f + 0.7f * Random.NextFloat());
        }

        Agent::FToolResult SpawnStress(const SStressSpawnParams& In, SStressSpawnResult& Out)
        {
            FString Error;
            CWorld* World = ResolveStressWorld(In.bEditorWorld, Out.World, Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.IsEmpty() ? FString("No world is open.") : Error);
            }

            FVector2 Center;
            if (!ResolveStressCenter(World, In.Center, Center, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CStaticMesh* CasterMesh = nullptr;
            if ((In.Casters > 0 || In.Bodies > 0 || In.Movers > 0) && (CasterMesh = ResolveStressCasterMesh(In.CasterMesh, Error)) == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            TVector<CParticleSystem*> ParticleSystems;
            if (In.Particles > 0 && !ResolveStressParticleSystems(In.ParticleSystems, ParticleSystems, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            constexpr int32 MaxPerCall = 20000;
            const int32 LightCount = Math::Clamp(In.PointLights, 0, MaxPerCall) + Math::Clamp(In.SpotLights, 0, MaxPerCall);
            FRandomStream Random((uint64)(uint32)In.Seed, 7u);
            const TOptional<FVector2> Camera = StressCameraGround(World);
            const float KeepClear = Math::Max(In.KeepClear, 0.0f);
            const float ShadowFraction = Math::Clamp(In.ShadowFraction, 0.0f, 1.0f);
            const float VolumetricFraction = Math::Clamp(In.VolumetricFraction, 0.0f, 1.0f);

            for (int32 Index = 0; Index < LightCount; ++Index)
            {
                const bool bSpot = Index >= Math::Clamp(In.PointLights, 0, MaxPerCall);
                const FVector2 Spot = PlaceStressPoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundHeightAt(World, Spot.x, Spot.y);
                const FVector3 Location(Spot.x, (Ground > GNoGroundHeight ? Ground : 0.0f) + VaryStress(Random, In.Height), Spot.y);
                const bool bShadows = Random.NextFloat() < ShadowFraction;
                const bool bVolumetric = Random.NextFloat() < VolumetricFraction;

                // Pitched down around the +Z forward axis, with a tilt so the cones do not all stack.
                constexpr float DownPitch = 90.0f;
                constexpr float MaxTilt = 35.0f;
                const FVector3 Euler(DownPitch + (Random.NextFloat() * 2.0f - 1.0f) * MaxTilt, Random.NextFloat() * 360.0f, 0.0f);
                const ECS::FEntity Entity = World->ConstructEntity(FName(StressLightName), FTransform(Location, Euler, FVector3(1.0f)));
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }

                if (bSpot)
                {
                    SSpotLightComponent& Light = World->EmplaceComponent<SSpotLightComponent>(Entity);
                    Light.LightColor = RandomStressColor(Random);
                    Light.Intensity = VaryStress(Random, In.Intensity);
                    Light.Attenuation = VaryStress(Random, In.LightRadius);
                    Light.InnerConeAngle = 15.0f + Random.NextFloat() * 15.0f;
                    Light.OuterConeAngle = Light.InnerConeAngle + 5.0f + Random.NextFloat() * 15.0f;
                    Light.bCastShadows = bShadows;
                    Light.bVolumetric = bVolumetric;
                }
                else
                {
                    SPointLightComponent& Light = World->EmplaceComponent<SPointLightComponent>(Entity);
                    Light.LightColor = RandomStressColor(Random);
                    Light.Intensity = VaryStress(Random, In.Intensity);
                    Light.Attenuation = VaryStress(Random, In.LightRadius);
                    Light.bCastShadows = bShadows;
                    Light.bVolumetric = bVolumetric;
                }

                ++Out.Lights;
                Out.ShadowCasting += bShadows ? 1 : 0;
            }

            const int32 CasterCount = Math::Clamp(In.Casters, 0, MaxPerCall);
            for (int32 Index = 0; Index < CasterCount; ++Index)
            {
                const FVector2 Spot = PlaceStressPoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundHeightAt(World, Spot.x, Spot.y);
                const float Size = VaryStress(Random, In.CasterScale);
                const FVector3 Scale(Size, Size * (0.5f + 1.5f * Random.NextFloat()), Size);
                const FVector3 Location(Spot.x, (Ground > GNoGroundHeight ? Ground : 0.0f) + Scale.y * 0.5f, Spot.y);
                const FVector3 Euler(0.0f, Random.NextFloat() * 360.0f, 0.0f);

                const ECS::FEntity Entity = World->ConstructEntity(FName(StressCasterName), FTransform(Location, Euler, Scale));
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
                const FVector2 Spot = PlaceStressPoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundHeightAt(World, Spot.x, Spot.y);
                const float Size = VaryStress(Random, In.BodyScale);
                const FVector3 Location(Spot.x, (Ground > GNoGroundHeight ? Ground : 0.0f) + MinDrop + Random.NextFloat() * DropSpread, Spot.y);
                const FVector3 Euler(Random.NextFloat() * 360.0f, Random.NextFloat() * 360.0f, Random.NextFloat() * 360.0f);

                const ECS::FEntity Entity = World->ConstructEntity(FName(StressBodyName), FTransform(Location, Euler, FVector3(Size)));
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
                const FVector2 Spot = PlaceStressPoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundHeightAt(World, Spot.x, Spot.y);
                const float Size = VaryStress(Random, In.BodyScale);
                const FVector3 Location(Spot.x, (Ground > GNoGroundHeight ? Ground : 0.0f) + 2.0f + Random.NextFloat() * 4.0f, Spot.y);

                const ECS::FEntity Entity = World->ConstructEntity(FName(StressMoverName), FTransform(Location, FVector3(0.0f), FVector3(Size)));
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

            const int32 ParticleCount = Math::Clamp(In.Particles, 0, MaxPerCall);
            for (int32 Index = 0; Index < ParticleCount; ++Index)
            {
                const FVector2 Spot = PlaceStressPoint(Random, Center, In.AreaRadius, Camera, KeepClear);
                const float Ground = GroundHeightAt(World, Spot.x, Spot.y);
                const FVector3 Location(Spot.x, (Ground > GNoGroundHeight ? Ground : 0.0f) + Random.NextFloat() * In.Height, Spot.y);

                const ECS::FEntity Entity = World->ConstructEntity(FName(StressParticleName), FTransform(Location, FVector3(0.0f), FVector3(1.0f)));
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }
                World->EmplaceComponent<SParticleSystemComponent>(Entity).ParticleSystem = ParticleSystems[Index % ParticleSystems.Num()];
                ++Out.Particles;
            }

            Out.Center = { Center.x, Center.y };
            Out.Total = CountStressEntities(World);
            return Agent::FToolResult::Ok(Lumina::Format(
                "Spawned {} light(s), {} casting shadows, {} caster(s), {} body(ies), {} mover(s) and {} particle system(s) around [{:.1f}, {:.1f}] in the {} world. "
                "{} stress entities in total.",
                Out.Lights, Out.ShadowCasting, Out.Casters, Out.Bodies, Out.Movers, Out.Particles, Center.x, Center.y, Out.World, Out.Total));
        }

        Agent::FToolResult ClearStress(const SStressClearParams& In, SStressClearResult& Out)
        {
            FString Label;
            FString Error;
            CWorld* World = ResolveStressWorld(In.bEditorWorld, Label, Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.IsEmpty() ? FString("No world is open.") : Error);
            }

            TVector<ECS::FEntity> Doomed;
            auto View = World->GetEntityRegistry().View<SNameComponent>();
            for (ECS::FEntity Entity : View)
            {
                if (IsStressEntity(View.Get<SNameComponent>(Entity)))
                {
                    Doomed.Add(Entity);
                }
            }
            for (ECS::FEntity Entity : Doomed)
            {
                World->DestroyEntity(Entity);
            }

            Out.Destroyed = Doomed.Num();
            return Agent::FToolResult::Ok(Lumina::Format("Destroyed {} stress entities in the {} world.", Out.Destroyed, Label));
        }

        constexpr const char* StressGridPrefix = "StressGrid_";
        constexpr const char* StressDecoyName = "StressDecoy";
        constexpr float StressGridMeshFill = 0.6f;
        constexpr float StressGridMoveFraction = 0.08f;
        constexpr float StressDecoyOffset = 3000.0f;

        // Cell index from a grid entity's name, or none for every other entity.
        TOptional<int32> StressGridCellOf(const SNameComponent& Name)
        {
            const FString Text(*Name.Name.ToString());
            const FStringView View(Text);
            if (!View.StartsWith(FStringView(StressGridPrefix)))
            {
                return {};
            }
            return (int32)std::strtol(*Text + std::strlen(StressGridPrefix), nullptr, 10);
        }

        FVector3 StressGridCellCenter(const SStressGridParams& In, const FVector2& Origin, int32 Cell)
        {
            const int32 Column = Cell % In.Columns;
            const int32 Row = Cell / In.Columns;
            const float Size = In.CellSize * StressGridMeshFill;
            return FVector3(Origin.x + ((float)Column + 0.5f) * In.CellSize, Size * 0.5f, Origin.y + ((float)Row + 0.5f) * In.CellSize);
        }

        ECS::FEntity SpawnStressGridMesh(CWorld* World, const SStressGridParams& In, const FVector2& Origin, int32 Cell, CStaticMesh* Mesh)
        {
            const FVector3 Location = StressGridCellCenter(In, Origin, Cell);
            const FName Name(*Lumina::Format("{}{}", StressGridPrefix, Cell));
            const ECS::FEntity Entity = World->ConstructEntity(Name, FTransform(Location, FVector3(0.0f), FVector3(In.CellSize * StressGridMeshFill)));
            if (Entity != ECS::NullEntity)
            {
                World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(Mesh);
            }
            return Entity;
        }

        Agent::FToolResult EditStressGrid(const SStressGridParams& In, SStressGridResult& Out)
        {
            FString Label;
            FString Error;
            CWorld* World = ResolveStressWorld(/*bEditorWorld*/ true, Label, Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.IsEmpty() ? FString("No world is open.") : Error);
            }
            if (In.Columns <= 0 || In.CellSize <= 0.0f)
            {
                return Agent::FToolResult::Error("Columns and CellSize must be positive.");
            }

            FVector2 Origin(0.0f);
            if (!In.Origin.IsEmpty() && !ResolveStressCenter(World, In.Origin, Origin, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CStaticMesh* Cube = CPrimitiveManager::Get().CubeMesh.Get();
            CStaticMesh* Sphere = CPrimitiveManager::Get().SphereMesh.Get();

            THashMap<int32, ECS::FEntity> Cells;
            TVector<ECS::FEntity> Decoys;
            {
                auto View = World->GetEntityRegistry().View<SNameComponent>();
                for (ECS::FEntity Entity : View)
                {
                    const SNameComponent& Name = View.Get<SNameComponent>(Entity);
                    if (const TOptional<int32> Cell = StressGridCellOf(Name))
                    {
                        Cells.Add(*Cell, Entity);
                    }
                    else if (Name.Name == FName(StressDecoyName))
                    {
                        Decoys.Add(Entity);
                    }
                }
            }

            if (In.bSetOccupancy)
            {
                THashSet<int32> Wanted;
                for (int32 Cell : In.Occupied)
                {
                    Wanted.Add(Cell);
                }
                for (auto It = Cells.begin(); It != Cells.end();)
                {
                    if (!Wanted.Contains(It->Key))
                    {
                        World->DestroyEntity(It->Value);
                        ++Out.Destroyed;
                        It = Cells.Remove(It);
                    }
                    else
                    {
                        ++It;
                    }
                }
                for (int32 Cell : Wanted)
                {
                    if (Cell >= 0 && !Cells.Contains(Cell))
                    {
                        const ECS::FEntity Entity = SpawnStressGridMesh(World, In, Origin, Cell, Cube);
                        if (Entity != ECS::NullEntity)
                        {
                            Cells.Add(Cell, Entity);
                            ++Out.Created;
                        }
                    }
                }
            }

            FRandomStream Random((uint64)(uint32)In.Seed, 11u);
            for (int32 Cell : In.Move)
            {
                auto Found = Cells.Find(Cell);
                if (Found == nullptr)
                {
                    continue;
                }
                const float Reach = In.CellSize * StressGridMoveFraction;
                const FVector3 Offset((Random.NextFloat() * 2.0f - 1.0f) * Reach, 0.0f, (Random.NextFloat() * 2.0f - 1.0f) * Reach);
                STransformComponent& Transform = World->GetComponent<STransformComponent>(*Found);
                Transform.SetLocation(StressGridCellCenter(In, Origin, Cell) + Offset);
                Transform.SetRotationFromEuler(FVector3(0.0f, Random.NextFloat() * 360.0f, 0.0f));
                ++Out.Moved;
            }

            for (int32 Cell : In.Swap)
            {
                auto Found = Cells.Find(Cell);
                if (Found == nullptr)
                {
                    continue;
                }
                SStaticMeshComponent& MeshComponent = World->GetComponent<SStaticMeshComponent>(*Found);
                MeshComponent.SetStaticMesh(MeshComponent.GetStaticMesh() == Cube ? Sphere : Cube);
                ++Out.Swapped;
            }

            for (int32 Cell : In.Respawn)
            {
                auto Found = Cells.Find(Cell);
                if (Found == nullptr)
                {
                    continue;
                }
                World->DestroyEntity(*Found);
                (*Found) = SpawnStressGridMesh(World, In, Origin, Cell, Cube);
                ++Out.Respawned;
            }

            if (In.bClearDecoys)
            {
                for (ECS::FEntity Entity : Decoys)
                {
                    World->DestroyEntity(Entity);
                }
                Decoys.Reset();
            }

            constexpr int32 MaxDecoysPerCall = 200000;
            const int32 DecoyCount = Math::Clamp(In.Decoys, 0, MaxDecoysPerCall);
            for (int32 Index = 0; Index < DecoyCount; ++Index)
            {
                // Far off to the side, so they cost slots and uploads but never reach the picture being checked.
                const FVector3 Location(StressDecoyOffset + Random.NextFloat() * 400.0f, 0.0f, Random.NextFloat() * 400.0f - 200.0f);
                const ECS::FEntity Entity = World->ConstructEntity(FName(StressDecoyName), FTransform(Location, FVector3(0.0f), FVector3(1.0f)));
                if (Entity != ECS::NullEntity)
                {
                    World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(Cube);
                    Decoys.Add(Entity);
                }
            }

            Out.GridMeshes = Cells.Num();
            Out.DecoyMeshes = Decoys.Num();
            return Agent::FToolResult::Ok(Lumina::Format(
                "Grid now holds {} meshes ({} created, {} destroyed, {} moved, {} swapped, {} respawned); {} decoys.",
                Out.GridMeshes, Out.Created, Out.Destroyed, Out.Moved, Out.Swapped, Out.Respawned, Out.DecoyMeshes));
        }

        void RegisterStressSpawn(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SStressSpawnParams, SStressSpawnResult>(
                Owner, "stress.spawn",
                "Scatter seeded point and spot lights, shadow-casting meshes, physics bodies, movers and particle systems around a "
                "center. Bypasses undo; stress.clear removes it all.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &SpawnStress);
        }

        void RegisterStressClear(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SStressClearParams, SStressClearResult>(
                Owner, "stress.clear",
                "Destroy every entity stress.spawn and stress.grid made.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &ClearStress);
        }

        void RegisterStressGrid(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SStressGridParams, SStressGridResult>(
                Owner, "stress.grid",
                "Edit a grid of meshes in the edited world in one frame to check the renderer against a known layout: set occupied "
                "cells, move, swap or respawn cells, add or clear off-screen decoys. Bypasses undo.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &EditStressGrid);
        }
    }

    using namespace MCPStressToolsInternal;

    void RegisterStressTools(FStringView Owner)
    {
        RegisterStressSpawn(Owner);
        RegisterStressClear(Owner);
        RegisterStressGrid(Owner);
    }
}
