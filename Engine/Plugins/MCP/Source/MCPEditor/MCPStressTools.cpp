#include "MCPStressTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentAssetResolve.h"
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
    }
}
