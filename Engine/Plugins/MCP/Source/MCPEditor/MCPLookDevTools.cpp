#include "MCPLookDevTools.h"

#include "Agent/AgentEntityToken.h"
#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolRegistry.h"
#include "Asset/AssetOps.h"
#include "Assets/AssetTypes/Foliage/GrassType.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Containers/Algorithm.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "MCPEditorModule.h"
#include "MCPEditorSessionTools.h"
#include "MCPPackageTools.h"
#include "Paths/Paths.h"
#include "Platform/Time/PlatformTime.h"
#include "Session/SessionOps.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "UI/Tools/TerrainEditMode.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/GrassComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace MCPLookDevToolsInternal
    {
        // Well clear of the welcome and starter scenes, which keep their props around the world origin.
        const FVector3 kOrigin(1000.0f, 0.0f, 0.0f);

        constexpr float kGroundSize      = 200.0f;
        constexpr float kGroundThickness = 1.0f;

        constexpr int32  kTerrainResolution = 65;
        constexpr float  kTerrainSize       = 32.0f;
        constexpr float  kTerrainCenterZ    = 26.0f;
        constexpr uint32 kGrassLayer        = 1;

        // Raised off the ground slab it overlaps, so the two never fight for depth.
        constexpr float kTerrainLift = 0.02f;

        constexpr float kGrassDensity  = 40.0f;
        constexpr float kGrassScaleMin = 0.10f;
        constexpr float kGrassScaleMax = 0.18f;

        constexpr int32  kMaxWarmupFrames   = 600;
        constexpr int32  kMaxFrameCount     = 120;
        constexpr double kFrameStallSeconds = 5.0;

        struct FLookDevProbe
        {
            const char* Name;
            TStrongObjectPtr<CStaticMesh> CPrimitiveManager::* Mesh;
            FVector3 Center;
            FVector3 Scale;
        };

        // Offsets from kOrigin, whose height is the ground's top; every primitive spans one meter at unit scale around its center.
        const FLookDevProbe kProbes[] =
        {
            { "LookDev Ground",    &CPrimitiveManager::CubeMesh,     FVector3(0.0f, -kGroundThickness * 0.5f, 0.0f), FVector3(kGroundSize, kGroundThickness, kGroundSize) },
            { "LookDev Cube 10cm", &CPrimitiveManager::CubeMesh,     FVector3(0.0f, 0.05f, 0.0f),   FVector3(0.1f) },
            { "LookDev Pole 5cm",  &CPrimitiveManager::CylinderMesh, FVector3(0.6f, 0.5f, 0.0f),    FVector3(0.05f, 1.0f, 0.05f) },
            { "LookDev Sphere",    &CPrimitiveManager::SphereMesh,   FVector3(-0.6f, 0.25f, 0.0f),  FVector3(0.5f) },
            { "LookDev Pole 10m",  &CPrimitiveManager::CylinderMesh, FVector3(0.0f, 5.0f, -4.0f),   FVector3(0.2f, 10.0f, 0.2f) },
            { "LookDev Overhang",  &CPrimitiveManager::CubeMesh,     FVector3(2.5f, 0.55f, 0.0f),   FVector3(1.5f, 0.1f, 1.0f) },
        };

        const FVector3 kPointLightOffset(1.2f, 1.2f, 1.0f);
        const FVector3 kSpotLightOffset(-1.5f, 2.5f, 1.5f);
        const FVector3 kSpotLightTarget(-0.6f, 0.0f, 0.0f);

        struct FLookDevPreset
        {
            const char* Name;
            FVector3 Eye;
            FVector3 Target;
        };

        const FLookDevPreset kPresets[] =
        {
            { "ContactClose", FVector3(0.35f, 0.18f, 0.55f), FVector3(0.0f, 0.05f, 0.0f) },
            { "Overhang",     FVector3(2.5f, 0.35f, 2.2f),   FVector3(2.5f, 0.05f, 0.0f) },
            { "GrassGrazing", FVector3(0.0f, 0.3f, 11.0f),   FVector3(0.0f, 0.1f, 30.0f) },
            { "FarVista",     FVector3(-30.0f, 12.0f, 45.0f), FVector3(0.0f, 2.0f, 0.0f) },
        };

        FString ValidateBuild(const SLookDevBuildParams& In)
        {
            if (!(In.SunElevationDegrees > 0.0f && In.SunElevationDegrees <= 90.0f))
            {
                return Lumina::Format("SunElevationDegrees is {}, but has to be above 0 and at most 90.", In.SunElevationDegrees);
            }
            return {};
        }

        FString ValidateCapture(const SLookDevCaptureParams& In, TVector<const FLookDevPreset*>& OutPresets)
        {
            FString LeafReason;
            if (!AssetOps::IsValidLeafName(FStringView(In.Label), &LeafReason))
            {
                return Lumina::Format("Label '{}' cannot name a folder. {}", In.Label, LeafReason);
            }
            if (In.WarmupFrames < 0 || In.WarmupFrames > kMaxWarmupFrames)
            {
                return Lumina::Format("WarmupFrames is {}, but has to be 0 to {}.", In.WarmupFrames, kMaxWarmupFrames);
            }
            if (In.FrameCount < 1 || In.FrameCount > kMaxFrameCount)
            {
                return Lumina::Format("FrameCount is {}, but has to be 1 to {}.", In.FrameCount, kMaxFrameCount);
            }

            if (In.Presets.IsEmpty())
            {
                for (const FLookDevPreset& Preset : kPresets)
                {
                    OutPresets.Add(&Preset);
                }
            }
            for (const FString& Name : In.Presets)
            {
                const FLookDevPreset* Found = Algo::FindIf(kPresets, [&Name](const FLookDevPreset& Preset) { return Name.EqualsIgnoreCase(Preset.Name); });
                if (Found == std::end(kPresets))
                {
                    return Lumina::Format("No preset is named '{}'. Use ContactClose, Overhang, GrassGrazing or FarVista.", Name);
                }
                OutPresets.Add(Found);
            }
            return {};
        }

        FVector3 SunDirection(float ElevationDegrees, float AzimuthDegrees)
        {
            const float Elevation = Math::Radians(ElevationDegrees);
            const float Azimuth   = Math::Radians(AzimuthDegrees);
            return FVector3(Math::Cos(Elevation) * Math::Sin(Azimuth), Math::Sin(Elevation), Math::Cos(Elevation) * Math::Cos(Azimuth));
        }

        // Copies the default terrain material's compiled shaders, so the grass needs no graph compile and no package.
        TStrongObjectPtr<CMaterial> MakeGrassTerrainMaterial(FString& OutReason)
        {
            const CMaterial* Default = CMaterial::GetDefaultTerrainMaterial();
            if (Default == nullptr || Default->Stages.IsEmpty())
            {
                OutReason = "The default terrain material has no compiled shaders to copy, so the patch has no grass.";
                return {};
            }

            TStrongObjectPtr<CGrassType> Grass = NewObject<CGrassType>(OF_Transient);
            Grass->Mesh     = CPrimitiveManager::Get().ConeMesh;
            Grass->Density  = kGrassDensity;
            Grass->ScaleMin = kGrassScaleMin;
            Grass->ScaleMax = kGrassScaleMax;

            // The cone sits on its center, so half its height lifts it out of the ground.
            Grass->ZOffset  = (kGrassScaleMin + kGrassScaleMax) * 0.25f;

            TStrongObjectPtr<CMaterial> Material = NewObject<CMaterial>(OF_Transient);
            Material->MaterialType = EMaterialType::Terrain;
            Material->Stages       = Default->Stages;
            SGrassOutput& Output   = Material->GrassOutputs.EmplaceGetRef();
            Output.GrassType  = Grass;
            Output.LayerIndex = kGrassLayer;
            Material->PostLoad();
            return Material;
        }

        ECS::FEntity BuildTerrainPatch(CWorld* World, CMaterial* GrassMaterial)
        {
            const ECS::FEntity Entity = FTerrainEditMode::CreateDefaultTerrain(World);
            if (Entity == ECS::NullEntity)
            {
                return Entity;
            }
            World->GetComponent<SNameComponent>(Entity).Name = "LookDev Terrain";
            World->SetEntityLocation(Entity, kOrigin + FVector3(0.0f, kTerrainLift, kTerrainCenterZ));

            STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(Entity);
            Terrain.Resolution    = kTerrainResolution;
            Terrain.TileWorldSize = kTerrainSize;

            const int32 Samples = kTerrainResolution * kTerrainResolution;
            Terrain.Heightmap.Init(0.0f, Samples);
            Terrain.LayerWeights.Init(uint8(0), Samples * Terrain.Layers.Num());
            for (int32 i = 0; i < Samples; ++i)
            {
                Terrain.LayerWeights[kGrassLayer * Samples + i] = 255;
            }
            Terrain.MarkHeightmapReplaced();
            Terrain.CPUState.bFullWeightsDirty = true;

            if (GrassMaterial != nullptr)
            {
                Terrain.Material = GrassMaterial;
                World->EmplaceComponent<SGrassComponent>(Entity);
            }
            return Entity;
        }

        ECS::FEntity SpawnLight(CWorld* World, const char* Name, const FVector3& Offset, const FVector3& Target)
        {
            const FVector3 Location = kOrigin + Offset;
            FTransform Transform(Location);
            Transform.SetRotation(Math::FindLookAtRotation(kOrigin + Target, Location));
            return World->ConstructEntity(Name, Transform);
        }

        void ClearFixture(FLookDevFixtureState& State, CWorld* World, int32& OutDestroyed)
        {
            if (State.World.Get() == World)
            {
                for (ECS::FEntity Entity : State.Entities)
                {
                    if (World->IsValidEntity(Entity))
                    {
                        World->DestroyEntity(Entity);
                        ++OutDestroyed;
                    }
                }
                for (ECS::FEntity Sun : State.DisabledSuns)
                {
                    if (World->IsValidEntity(Sun))
                    {
                        World->GetEntityRegistry().Remove<SDisabledTag>(Sun);
                    }
                }
            }
            State = {};
        }

        void BuildFixture(const SLookDevBuildParams& In, FLookDevFixtureState& State, CWorld* World, CMaterial* GrassMaterial)
        {
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            State.World = World;

            World->View<SDirectionalLightComponent>().ForEach([&](ECS::FEntity Entity, SDirectionalLightComponent&)
            {
                if (!Registry.HasAll<SDisabledTag>(Entity))
                {
                    State.DisabledSuns.Add(Entity);
                }
            });
            for (ECS::FEntity Sun : State.DisabledSuns)
            {
                Registry.Emplace<SDisabledTag>(Sun);
            }

            for (const FLookDevProbe& Probe : kProbes)
            {
                const ECS::FEntity Entity = World->ConstructEntity(Probe.Name, FTransform(kOrigin + Probe.Center, FVector3(0.0f), Probe.Scale));
                World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh((CPrimitiveManager::Get().*Probe.Mesh).Get());
                State.Entities.Add(Entity);
            }

            State.Entities.Add(BuildTerrainPatch(World, GrassMaterial));

            const ECS::FEntity Sun = World->ConstructEntity("LookDev Sun", FTransform(kOrigin + FVector3(0.0f, 5.0f, 0.0f)));
            World->EmplaceComponent<SDirectionalLightComponent>(Sun).Direction = SunDirection(In.SunElevationDegrees, In.SunAzimuthDegrees);
            State.Entities.Add(Sun);

            const ECS::FEntity Point = SpawnLight(World, "LookDev Point Light", kPointLightOffset, FVector3(0.0f));
            SPointLightComponent& PointLight = World->EmplaceComponent<SPointLightComponent>(Point);
            PointLight.Intensity    = 5.0f;
            PointLight.Attenuation  = 6.0f;
            PointLight.bCastShadows = true;
            State.Entities.Add(Point);

            const ECS::FEntity Spot = SpawnLight(World, "LookDev Spot Light", kSpotLightOffset, kSpotLightTarget);
            SSpotLightComponent& SpotLight = World->EmplaceComponent<SSpotLightComponent>(Spot);
            SpotLight.Intensity    = 20.0f;
            SpotLight.Attenuation  = 8.0f;
            SpotLight.bCastShadows = true;
            State.Entities.Add(Spot);
        }

        Agent::FToolResult BuildLookDevScene(const SLookDevBuildParams& In, SLookDevBuildResult& Out)
        {
            if (const FString Invalid = ValidateBuild(In); !Invalid.IsEmpty())
            {
                return Agent::FToolResult::Error(Invalid);
            }

            FString SceneError;
            CWorld* World = SessionOps::GetSceneWorld(SceneError);
            FLookDevFixtureState* State = FMCPEditorModule::GetLookDevFixture();
            if (World == nullptr || State == nullptr)
            {
                return Agent::FToolResult::Error(SceneError.IsEmpty() ? FString("The MCP editor module is not running.") : SceneError);
            }
            if (SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error("Stop play-in-editor first.");
            }

            FString GrassReason;
            const TStrongObjectPtr<CMaterial> GrassMaterial = MakeGrassTerrainMaterial(GrassReason);

            int32 Cleared = 0;
            SessionOps::RunTransacted("Build Look-dev Fixture (agent)", [&]()
            {
                ClearFixture(*State, World, Cleared);
                BuildFixture(In, *State, World, GrassMaterial.Get());
            }, SceneError);

            ECS::FRegistry& Registry = World->GetEntityRegistry();
            for (ECS::FEntity Entity : State->Entities)
            {
                SEntityInfo& Info = Out.Entities.EmplaceGetRef();
                Info.Id   = Agent::FEntityTokens::Mint(Registry, Entity);
                Info.Name = FString(*World->GetEntityName(Entity).ToString());
            }
            Out.bGrass = GrassMaterial.IsValid();

            return Agent::FToolResult::Ok(Lumina::Format("Built {} look-dev entities after clearing {}. {}",
                Out.Entities.Num(), Cleared, Out.bGrass ? FString("The terrain patch grows GPU grass.") : GrassReason));
        }

        Agent::FToolResult ClearLookDevScene(const SPackageEmptyParams&, SDestroyEntitiesResult& Out)
        {
            FString SceneError;
            CWorld* World = SessionOps::GetSceneWorld(SceneError);
            FLookDevFixtureState* State = FMCPEditorModule::GetLookDevFixture();
            if (World == nullptr || State == nullptr)
            {
                return Agent::FToolResult::Error(SceneError.IsEmpty() ? FString("The MCP editor module is not running.") : SceneError);
            }

            SessionOps::RunTransacted("Clear Look-dev Fixture (agent)", [&]() { ClearFixture(*State, World, Out.Destroyed); }, SceneError);
            return Agent::FToolResult::Ok(Lumina::Format("Removed {} look-dev entities.", Out.Destroyed));
        }

        uint64 CurrentFrame()
        {
            return GEngine->GetUpdateContext().GetFrame();
        }

        // Fails only when the editor stops presenting frames, so a long warmup on a slow machine still finishes.
        bool WaitForFrame(uint64 Target, int32 GateTimeout)
        {
            uint64 Last = 0;
            double StallDeadline = PlatformTime::Seconds() + kFrameStallSeconds;
            while (true)
            {
                uint64 Now = 0;
                if (Agent::FGameThreadGate::Run([&Now] { Now = CurrentFrame(); }, GateTimeout) != Agent::EGameThreadResult::Ran)
                {
                    return false;
                }
                if (Now >= Target)
                {
                    return true;
                }
                if (Now != Last)
                {
                    Last = Now;
                    StallDeadline = PlatformTime::Seconds() + kFrameStallSeconds;
                }
                else if (PlatformTime::Seconds() > StallDeadline)
                {
                    return false;
                }
                Threading::Sleep(1);
            }
        }

        Agent::FToolResult PlaceCamera(const FLookDevPreset& Preset)
        {
            const FLookDevFixtureState* State = FMCPEditorModule::GetLookDevFixture();
            if (State == nullptr || State->Entities.IsEmpty())
            {
                return Agent::FToolResult::Error("There is no look-dev fixture. Run lookdev.build_scene first.");
            }
            if (FindGameWorld() != nullptr)
            {
                return Agent::FToolResult::Error("Stop play-in-editor first, or the capture shows the game instead of the fixture.");
            }

            const FVector3 Eye    = kOrigin + Preset.Eye;
            const FVector3 Target = kOrigin + Preset.Target;
            SSceneCameraParams Camera;
            Camera.Action   = ESceneCameraAction::Set;
            Camera.Location = { Eye.x, Eye.y, Eye.z };
            Camera.Target   = { Target.x, Target.y, Target.z };
            SSceneCameraResult Ignored;
            return SceneCameraSet(Camera, Ignored);
        }

        // Off the game thread, since it waits for frames the game thread has to render.
        Agent::FToolResult CaptureLookDev(const SLookDevCaptureParams& In, SLookDevCaptureResult& Out)
        {
            TVector<const FLookDevPreset*> Presets;
            if (const FString Invalid = ValidateCapture(In, Presets); !Invalid.IsEmpty())
            {
                return Agent::FToolResult::Error(Invalid);
            }

            const FFixedString Folder = Paths::Combine(Paths::Parent(Paths::GetEngineDirectory()), "Saved/LumiShots", In.Label);
            const int32 GateTimeout = Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds();
            const Agent::FToolResult GateMissed = Agent::FToolResult::Error("The game thread did not pick up the capture.");
            const Agent::FToolResult Stalled = Agent::FToolResult::Error("The editor stopped presenting frames, which a minimized window does.");

            for (const FLookDevPreset* Preset : Presets)
            {
                Agent::FToolResult Step = GateMissed;
                uint64 Frame = 0;
                (void)Agent::FGameThreadGate::Run([&] { Step = PlaceCamera(*Preset); Frame = CurrentFrame(); }, GateTimeout);
                if (Step.bIsError)
                {
                    return Step;
                }

                for (int32 Index = 0; Index < In.FrameCount; ++Index)
                {
                    if (!WaitForFrame(Frame + (Index == 0 ? In.WarmupFrames : 1), GateTimeout))
                    {
                        return Stalled;
                    }

                    SScreenshotParams Shot;
                    const FFixedString Path = Paths::Combine(Folder, Lumina::Format("{}_{}.png", Preset->Name, Index));
                    Shot.OutputPath = FString(Path.GetData(), Path.Len());
                    SScreenshotResult Saved;
                    Step = GateMissed;
                    (void)Agent::FGameThreadGate::Run([&] { Step = CaptureEditorScene(Shot, Saved); Frame = CurrentFrame(); }, GateTimeout);
                    if (Step.bIsError)
                    {
                        return Step;
                    }
                    Out.Paths.Add(Saved.Path);
                }
            }

            return Agent::FToolResult::Ok(Lumina::Format("Wrote {} frames to {}.", Out.Paths.Num(), Folder));
        }
    }

    using namespace MCPLookDevToolsInternal;

    void RegisterLookDevTools(FStringView Owner)
    {
        Agent::FToolRegistry& Registry = Agent::FToolRegistry::Get();

        Registry.Register<SLookDevBuildParams, SLookDevBuildResult>(
            Owner, "lookdev.build_scene",
            "Build a transient shadow and grass test bed far from the origin: ground, small and tall probes, an overhang, a grass "
            "terrain patch, a sun and shadowing point and spot lights. Replaces the last one; nothing is saved.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &BuildLookDevScene);

        Registry.Register<SPackageEmptyParams, SDestroyEntitiesResult>(
            Owner, "lookdev.clear",
            "Remove the look-dev fixture and turn the scene's own suns back on.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread, &ClearLookDevScene);

        Registry.Register<SLookDevCaptureParams, SLookDevCaptureResult>(
            Owner, "lookdev.capture",
            "Aim the editor camera at each look-dev preset, wait WarmupFrames, then write FrameCount consecutive frames to "
            "Saved/LumiShots/<Label>/<Preset>_<n>.png.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::Any, &CaptureLookDev);
    }
}
