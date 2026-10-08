#include "EditorPCH.h"
#include "Scene/DefaultScene.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Log/Log.h"
#include "Renderer/MaterialTypes.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "UI/Tools/TerrainEditMode.h"
#include "World/Entity/Components/CloudComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TextComponent.h"
#include "World/World.h"

namespace Lumina::DefaultScene
{
    namespace
    {
        // The cube, sphere and cylinder primitives all span one meter at unit scale.
        constexpr float kGroundSize      = 400.0f;
        constexpr float kGroundThickness = 1.0f;

        constexpr float kStageRadius = 5.5f;
        constexpr float kStageHeight = 0.25f;

        // Wider than the stage by a few centimeters, so only its outer band shows as a glowing lip.
        constexpr float kStageRimOverhang = 0.06f;
        constexpr float kStageRimHeight   = 0.05f;

        constexpr float kShowcaseRadius  = 0.65f;
        constexpr float kShowcaseSpacing = 1.8f;
        constexpr float kShowcaseZ       = 0.8f;

        constexpr float kWordmarkZ    = -2.6f;
        constexpr float kWordmarkY    = 3.0f;
        constexpr float kTaglineY     = 1.85f;

        // A half turn aims the text at the camera, which sits on the +Z side.
        constexpr float kFacingCameraYaw = 180.0f;

        // Just outside the wordmark's ends and behind the stage, so they frame it without crowding the lineup.
        constexpr float kMonolithX      = 5.6f;
        constexpr float kMonolithZ      = -3.2f;
        constexpr float kMonolithWidth  = 0.7f;
        constexpr float kMonolithHeight = 4.6f;

        constexpr float kStripWidth  = 0.08f;
        constexpr float kStripInset  = 0.3f;
        constexpr float kStripDepth  = 0.02f;

        // The one emissive hue in the scene, shared by the rim, the monolith seams and the glow sphere.
        const FVector3 kAccentGlow = FVector3(0.18f, 1.2f, 1.6f);

        // Vectors and scalars index separate arrays, so their slots overlap.
        enum class EShowcaseParameter : uint16
        {
            BaseColor = 0,
            Emissive  = 1,
            Metallic  = 0,
            Roughness = 1,
        };

        struct FSurface
        {
            FVector3 BaseColor;
            float    Metallic;
            float    Roughness;
            FVector3 Emissive = FVector3(0.0f);
        };

        FMaterialParameter MakeParameter(const char* Name, EMaterialParameterType Type, EShowcaseParameter Index,
                                         float ScalarDefault, const FVector4& VectorDefault)
        {
            FMaterialParameter Parameter;
            Parameter.ParameterName = Name;
            Parameter.Type          = Type;
            Parameter.Index         = (uint16)Index;
            Parameter.ScalarDefault = ScalarDefault;
            Parameter.VectorDefault = VectorDefault;
            return Parameter;
        }

        CMaterial* CreateShowcaseMaterial()
        {
            FString PixelInputs;
            PixelInputs += "\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";
            PixelInputs += "\tMaterial.Diffuse   = GetMaterialVec4(MaterialIndex, 0).rgb * VertexColor.rgb;\n";
            PixelInputs += "\tMaterial.Emissive  = GetMaterialVec4(MaterialIndex, 1).rgb;\n";
            PixelInputs += "\tMaterial.Metallic  = GetMaterialScalar(MaterialIndex, 0);\n";
            PixelInputs += "\tMaterial.Roughness = GetMaterialScalar(MaterialIndex, 1);\n";

            const FMaterialParameter Parameters[] =
            {
                MakeParameter("BaseColor", EMaterialParameterType::Vector, EShowcaseParameter::BaseColor, 0.0f, FVector4(0.8f, 0.8f, 0.8f, 1.0f)),
                MakeParameter("Emissive",  EMaterialParameterType::Vector, EShowcaseParameter::Emissive,  0.0f, FVector4(0.0f)),
                MakeParameter("Metallic",  EMaterialParameterType::Scalar, EShowcaseParameter::Metallic,  0.0f, FVector4(0.0f)),
                MakeParameter("Roughness", EMaterialParameterType::Scalar, EShowcaseParameter::Roughness, 0.5f, FVector4(0.0f)),
            };

            return CMaterial::CreateBuiltinMaterial("EditorShowcaseSurface", PixelInputs, Parameters);
        }

        // Built on first use and rooted for the session, since every welcome scene instances the same parent.
        CMaterial* GetShowcaseMaterial()
        {
            static CMaterial* ShowcaseMaterial = CreateShowcaseMaterial();
            return ShowcaseMaterial;
        }

        CMaterialInterface* MakeSurface(const FSurface& Surface)
        {
            CMaterial* Parent = GetShowcaseMaterial();
            if (Parent == nullptr)
            {
                return nullptr;
            }

            CMaterialInstance* Instance = CMaterialInstance::CreateDynamic(Parent);
            Instance->SetVectorValue("BaseColor", FVector4(Surface.BaseColor, 1.0f));
            Instance->SetVectorValue("Emissive",  FVector4(Surface.Emissive, 0.0f));
            Instance->SetScalarValue("Metallic",  Surface.Metallic);
            Instance->SetScalarValue("Roughness", Surface.Roughness);
            return Instance;
        }

        ECS::FEntity SpawnMesh(CWorld* World, const char* Name, CStaticMesh* Mesh, const FVector3& Location,
                               const FVector3& Scale, CMaterialInterface* Material)
        {
            const ECS::FEntity Entity = World->ConstructEntity(Name, FTransform(Location, FVector3(0.0f), Scale));
            SStaticMeshComponent& MeshComponent = World->EmplaceComponent<SStaticMeshComponent>(Entity);
            MeshComponent.SetStaticMesh(Mesh);
            if (Material != nullptr)
            {
                MeshComponent.SetMaterialAtSlot(Material, 0);
            }
            return Entity;
        }

        void AddRigidBody(CWorld* World, ECS::FEntity Entity, EBodyType BodyType)
        {
            World->EmplaceComponent<SRigidBodyComponent>(Entity).BodyType = BodyType;
        }

        // Every component here takes its defaults, which are tuned to be this look out of the box.
        ECS::FEntity BuildSunAndSky(CWorld* World)
        {
            World->EmplaceComponent<SEnvironmentComponent>(World->ConstructEntity("Environment"));
            World->EmplaceComponent<SSkyLightComponent>(World->ConstructEntity("Sky Light"));
            World->EmplaceComponent<SCloudComponent>(World->ConstructEntity("Clouds"));

            const ECS::FEntity Sun = World->ConstructEntity("Sun");
            World->EmplaceComponent<SDirectionalLightComponent>(Sun);
            return Sun;
        }

        void BuildGround(CWorld* World, CMaterialInterface* Material)
        {
            const ECS::FEntity Ground = SpawnMesh(World, "Ground", CPrimitiveManager::Get().CubeMesh.Get(),
                FVector3(0.0f, -kGroundThickness * 0.5f, 0.0f),
                FVector3(kGroundSize, kGroundThickness, kGroundSize), Material);
            World->EmplaceComponent<SBoxColliderComponent>(Ground);
            AddRigidBody(World, Ground, EBodyType::Static);
        }

        void BuildAtmosphereAndGrading(CWorld* World)
        {
            World->EmplaceComponent<SExponentialHeightFogComponent>(World->ConstructEntity("Height Fog"));

            // The level's global look, so it has no bounds to sit inside and no edge to blend across.
            World->EmplaceComponent<SPostProcessComponent>(World->ConstructEntity("Post Process")).bInfiniteExtent = true;
        }

        void BuildStage(CWorld* World)
        {
            CStaticMesh* Cylinder = CPrimitiveManager::Get().CylinderMesh.Get();

            const ECS::FEntity Stage = SpawnMesh(World, "Stage", Cylinder,
                FVector3(0.0f, kStageHeight * 0.5f, 0.0f),
                FVector3(kStageRadius * 2.0f, kStageHeight, kStageRadius * 2.0f),
                MakeSurface({ FVector3(0.03f, 0.032f, 0.036f), 0.0f, 0.18f }));
            World->EmplaceComponent<SCylinderColliderComponent>(Stage);
            AddRigidBody(World, Stage, EBodyType::Static);

            const float RimDiameter = (kStageRadius + kStageRimOverhang) * 2.0f;
            SpawnMesh(World, "Stage Rim", Cylinder,
                FVector3(0.0f, kStageHeight - kStageRimHeight, 0.0f),
                FVector3(RimDiameter, kStageRimHeight, RimDiameter),
                MakeSurface({ FVector3(0.02f), 0.0f, 0.5f, kAccentGlow }));
        }

        // One sphere per broad family of surface, so the lineup reads as a material test at a glance.
        void BuildMaterialLineup(CWorld* World)
        {
            struct FShowcaseSphere
            {
                const char* Name;
                FSurface    Surface;
            };

            const FShowcaseSphere Spheres[] =
            {
                { "Clay Sphere",     { FVector3(0.80f, 0.78f, 0.74f), 0.0f, 0.85f } },
                { "Gold Sphere",     { FVector3(1.00f, 0.77f, 0.34f), 1.0f, 0.22f } },
                { "Chrome Sphere",   { FVector3(0.96f, 0.96f, 0.97f), 1.0f, 0.04f } },
                { "Lacquer Sphere",  { FVector3(0.70f, 0.04f, 0.05f), 0.0f, 0.12f } },
                { "Glow Sphere",     { FVector3(0.02f), 0.0f, 0.4f, kAccentGlow } },
            };

            CStaticMesh* SphereMesh = CPrimitiveManager::Get().SphereMesh.Get();
            const float  FirstX     = -kShowcaseSpacing * (float)(std::size(Spheres) - 1) * 0.5f;

            for (size_t i = 0; i < std::size(Spheres); ++i)
            {
                const FVector3 Location(FirstX + kShowcaseSpacing * (float)i, kStageHeight + kShowcaseRadius, kShowcaseZ);
                SpawnMesh(World, Spheres[i].Name, SphereMesh, Location, FVector3(kShowcaseRadius * 2.0f),
                    MakeSurface(Spheres[i].Surface));
            }

            // An emissive surface lights nothing by itself, so a matching point light carries its color onto the stage.
            const FVector3 GlowLocation(-FirstX, kStageHeight + kShowcaseRadius, kShowcaseZ);
            const ECS::FEntity Glow = World->ConstructEntity("Glow Light",
                FTransform(GlowLocation, FVector3(0.0f), FVector3(1.0f)));
            SPointLightComponent& Light = World->EmplaceComponent<SPointLightComponent>(Glow);
            Light.LightColor  = FVector3(0.25f, 0.85f, 1.0f);
            Light.Intensity   = 3.0f;
            Light.Attenuation = 5.0f;
            Light.bVolumetric = true;
        }

        void BuildWordmark(CWorld* World)
        {
            auto SpawnText = [World](const char* Name, const char* Text, float Y, float Size, const FVector3& Color, float Intensity)
            {
                const ECS::FEntity Entity = World->ConstructEntity(Name, FTransform(
                    FVector3(0.0f, Y, kWordmarkZ), FVector3(0.0f, kFacingCameraYaw, 0.0f), FVector3(1.0f)));
                STextComponent& TextComponent = World->EmplaceComponent<STextComponent>(Entity);
                TextComponent.Text            = Text;
                TextComponent.WorldSize       = Size;
                TextComponent.Color           = FVector4(Color, 1.0f);
                TextComponent.Intensity       = Intensity;
                TextComponent.HorizontalAlign = ETextHorizontalAlign::Center;
                TextComponent.VerticalAlign   = ETextVerticalAlign::Middle;
                TextComponent.bBillboard      = false;
                TextComponent.bDepthTest      = true;
            };

            // The wordmark runs above 1.0 so it still reads as the brightest thing on the stage once tonemapped.
            SpawnText("Wordmark", "LUMINA", kWordmarkY, 2.2f, FVector3(0.62f, 0.86f, 1.0f), 2.5f);
            SpawnText("Tagline", "Create or open a level to start building", kTaglineY, 0.32f, FVector3(0.85f, 0.9f, 1.0f), 0.9f);
        }

        // Two dark slabs with a lit seam each, the same cyan as the stage rim so the frame reads as one piece.
        void BuildMonoliths(CWorld* World)
        {
            CStaticMesh* Cube = CPrimitiveManager::Get().CubeMesh.Get();
            const float  StripHeight = kMonolithHeight - kStripInset * 2.0f;
            const float  FrontFaceZ  = kMonolithZ + kMonolithWidth * 0.5f + kStripDepth * 0.5f;

            for (const float Side : { -1.0f, 1.0f })
            {
                const ECS::FEntity Monolith = SpawnMesh(World, "Monolith", Cube,
                    FVector3(Side * kMonolithX, kMonolithHeight * 0.5f, kMonolithZ),
                    FVector3(kMonolithWidth, kMonolithHeight, kMonolithWidth),
                    MakeSurface({ FVector3(0.025f, 0.027f, 0.03f), 0.6f, 0.25f }));
                World->EmplaceComponent<SBoxColliderComponent>(Monolith);
                AddRigidBody(World, Monolith, EBodyType::Static);

                SpawnMesh(World, "Monolith Strip", Cube,
                    FVector3(Side * kMonolithX, kMonolithHeight * 0.5f, FrontFaceZ),
                    FVector3(kStripWidth, StripHeight, kStripDepth),
                    MakeSurface({ FVector3(0.02f), 0.0f, 0.5f, kAccentGlow }));
            }
        }

        ECS::FEntity BuildStarterLevel(CWorld* World, CMaterialInterface* GroundMaterial)
        {
            const ECS::FEntity Sun = BuildSunAndSky(World);
            BuildGround(World, GroundMaterial);
            BuildAtmosphereAndGrading(World);
            return Sun;
        }
    }

    void PopulateStarterLevel(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        // Saved levels keep the default material, because a runtime instance has no package to be saved into.
        BuildStarterLevel(World, nullptr);
    }

    void PopulateWelcomeScene(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        const ECS::FEntity Sun = BuildStarterLevel(World, MakeSurface({ FVector3(0.16f, 0.155f, 0.15f), 0.0f, 0.9f }));

        // Low and raking from the side the camera does not sit on, so every sphere shows a terminator and a long shadow.
        World->GetComponent<SDirectionalLightComponent>(Sun).Direction = Math::Normalize(FVector3(-0.92f, 0.26f, -0.08f));
        BuildStage(World);
        BuildMaterialLineup(World);
        BuildWordmark(World);
        BuildMonoliths(World);
    }

    FCameraPose GetWelcomeCameraPose()
    {
        // Off to one side, so the lineup recedes a little instead of reading as a flat row.
        return { FVector3(3.2f, 3.4f, 11.5f), FVector3(0.0f, 1.5f, -0.6f) };
    }

    namespace
    {
        // The camera looks down a valley toward -Z, with ridges on both sides and a range closing it off.
        constexpr float kRangeCameraZ       = 310.0f;
        constexpr float kRangeTerrainSize   = 6144.0f;
        constexpr int32 kRangeTerrainRes    = 1025;
        constexpr float kRangeTerrainZ      = -2300.0f;
        constexpr float kRangeMaxHeight     = 640.0f;
        constexpr float kRangeValleyFloor   = 8.0f;

        constexpr float kRangeTowerDistances[] = { 100.0f, 150.0f, 300.0f, 600.0f, 1000.0f, 1600.0f, 2400.0f };

        float Hash2(int32 X, int32 Z)
        {
            uint32 H = (uint32)X * 374761393u + (uint32)Z * 668265263u;
            H = (H ^ (H >> 13)) * 1274126177u;
            return (float)((H ^ (H >> 16)) & 0xFFFFu) / 65535.0f;
        }

        float ValueNoise(float X, float Z)
        {
            const float X0 = std::floor(X);
            const float Z0 = std::floor(Z);
            const float Tx = Math::SmoothStep(0.0f, 1.0f, X - X0);
            const float Tz = Math::SmoothStep(0.0f, 1.0f, Z - Z0);
            const int32 Ix = (int32)X0;
            const int32 Iz = (int32)Z0;
            const float A = Math::Lerp(Hash2(Ix, Iz),     Hash2(Ix + 1, Iz),     Tx);
            const float B = Math::Lerp(Hash2(Ix, Iz + 1), Hash2(Ix + 1, Iz + 1), Tx);
            return Math::Lerp(A, B, Tz) * 2.0f - 1.0f;
        }

        float Fbm(float X, float Z)
        {
            float Sum       = 0.0f;
            float Amplitude = 0.5f;
            for (int32 Octave = 0; Octave < 5; ++Octave)
            {
                Sum       += ValueNoise(X, Z) * Amplitude;
                X          = X * 2.03f + 17.0f;
                Z          = Z * 2.03f + 31.0f;
                Amplitude *= 0.5f;
            }
            return Sum;
        }

        // Height above the terrain's base, in meters, so props can sit on the ground the heightmap describes.
        float RangeHeight(float X, float Z)
        {
            const float Walls  = 380.0f * Math::SmoothStep(150.0f, 1100.0f, std::abs(X));
            const float Closer = 560.0f * Math::SmoothStep(-2600.0f, -4300.0f, Z);
            const float Shape  = Math::Max(Walls, Closer);
            const float Detail = Fbm(X * 0.0025f, Z * 0.0025f) * (6.0f + 0.4f * Shape);
            return Math::Clamp(kRangeValleyFloor + Shape + Detail, 0.0f, kRangeMaxHeight);
        }

        void BuildRangeTerrain(CWorld* World)
        {
            const ECS::FEntity Entity = FTerrainEditMode::CreateDefaultTerrain(World);
            if (Entity == ECS::NullEntity)
            {
                return;
            }
            World->SetEntityLocation(Entity, FVector3(0.0f, 0.0f, kRangeTerrainZ));

            STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(Entity);
            Terrain.Resolution    = kRangeTerrainRes;
            Terrain.TileWorldSize = kRangeTerrainSize;
            Terrain.MaxHeight     = kRangeMaxHeight;

            const size_t Count  = (size_t)kRangeTerrainRes * (size_t)kRangeTerrainRes;
            const float  Stride = kRangeTerrainSize / (float)(kRangeTerrainRes - 1);
            const float  MinX   = -kRangeTerrainSize * 0.5f;
            const float  MinZ   = kRangeTerrainZ - kRangeTerrainSize * 0.5f;
            Terrain.Heightmap.resize(Count);
            for (int32 Row = 0; Row < kRangeTerrainRes; ++Row)
            {
                for (int32 Col = 0; Col < kRangeTerrainRes; ++Col)
                {
                    const float Height = RangeHeight(MinX + (float)Col * Stride, MinZ + (float)Row * Stride);
                    Terrain.Heightmap[(size_t)Row * kRangeTerrainRes + Col] = Height / kRangeMaxHeight;
                }
            }
            Terrain.LayerWeights.assign(Count * Terrain.Layers.size(), uint8(0));
            Terrain.MarkHeightmapReplaced();
        }

        ECS::FEntity SpawnOnGround(CWorld* World, const char* Name, CStaticMesh* Mesh, float X, float Z, float BaseY,
                                   const FVector3& Scale, CMaterialInterface* Material, bool bCastFarShadow)
        {
            const float Y = RangeHeight(X, Z) + BaseY + Scale.y * 0.5f;
            const ECS::FEntity Entity = SpawnMesh(World, Name, Mesh, FVector3(X, Y, Z), Scale, Material);
            World->GetComponent<SStaticMeshComponent>(Entity).bCastFarShadow = bCastFarShadow;
            return Entity;
        }

        void SpawnLabel(CWorld* World, const char* Text, const FVector3& Location, float Size)
        {
            const ECS::FEntity Entity = World->ConstructEntity("Range Label", FTransform(Location, FVector3(0.0f), FVector3(1.0f)));
            STextComponent& Label = World->EmplaceComponent<STextComponent>(Entity);
            Label.Text            = Text;
            Label.WorldSize       = Size;
            Label.Color           = FVector4(1.0f, 0.95f, 0.85f, 1.0f);
            Label.Intensity       = 1.5f;
            Label.HorizontalAlign = ETextHorizontalAlign::Center;
            Label.VerticalAlign   = ETextVerticalAlign::Middle;
            Label.bBillboard      = true;
            Label.bDepthTest      = true;
        }

        // Each distance gets a warm tower that casts into the far cascade and a cool one that does not.
        void BuildRangeTowers(CWorld* World)
        {
            CStaticMesh* Cube = CPrimitiveManager::Get().CubeMesh.Get();
            CMaterialInterface* FarCaster  = MakeSurface({ FVector3(0.72f, 0.55f, 0.38f), 0.0f, 0.8f });
            CMaterialInterface* NearCaster = MakeSurface({ FVector3(0.36f, 0.42f, 0.50f), 0.0f, 0.8f });

            for (const float Distance : kRangeTowerDistances)
            {
                const float Z      = kRangeCameraZ - Distance;
                const float Height = 18.0f + Distance * 0.06f;
                const float Width  = Height * 0.18f;
                const float Offset = 25.0f + Distance * 0.06f;

                SpawnOnGround(World, "Far Caster Tower", Cube, -Offset, Z, -2.0f, FVector3(Width, Height, Width), FarCaster, true);
                SpawnOnGround(World, "Near Caster Tower", Cube, Offset, Z, -2.0f, FVector3(Width, Height, Width), NearCaster, false);

                const FString Text = Lumina::Format("{:.0f} m", Distance);
                const float   Size = 3.0f + Distance * 0.02f;
                SpawnLabel(World, Text.c_str(), FVector3(0.0f, RangeHeight(0.0f, Z) + Size * 0.6f, Z), Size);
            }
        }

        // Two blocks of simple trees on the valley walls, the left one marked as far casters.
        void BuildRangeForests(CWorld* World)
        {
            CStaticMesh* Cylinder = CPrimitiveManager::Get().CylinderMesh.Get();
            CStaticMesh* Sphere   = CPrimitiveManager::Get().SphereMesh.Get();
            CMaterialInterface* Bark  = MakeSurface({ FVector3(0.20f, 0.14f, 0.09f), 0.0f, 0.9f });
            CMaterialInterface* Crown = MakeSurface({ FVector3(0.10f, 0.20f, 0.07f), 0.0f, 0.85f });

            constexpr int32 Columns = 14;
            constexpr int32 Rows    = 16;
            constexpr float Spacing = 26.0f;
            for (const float Side : { -1.0f, 1.0f })
            {
                const bool bFar = Side < 0.0f;
                for (int32 Row = 0; Row < Rows; ++Row)
                {
                    for (int32 Col = 0; Col < Columns; ++Col)
                    {
                        const int32 Seed    = (int32)(Side * 1000.0f) + Row * Columns + Col;
                        const float X       = Side * (300.0f + (float)Col * Spacing + Hash2(Seed, 1) * Spacing);
                        const float Z       = kRangeCameraZ - 510.0f - (float)Row * Spacing - Hash2(Seed, 2) * Spacing;
                        const float Size    = 0.8f + Hash2(Seed, 3) * 0.5f;
                        const float Trunk   = 9.0f * Size;
                        const float CrownD  = 10.0f * Size;

                        SpawnOnGround(World, "Tree Trunk", Cylinder, X, Z, -1.0f, FVector3(1.2f * Size, Trunk, 1.2f * Size), Bark, bFar);
                        SpawnOnGround(World, "Tree Crown", Sphere, X, Z, Trunk - 2.0f, FVector3(CrownD), Crown, bFar);
                    }
                }
            }
        }
    }

    void PopulateShadowRangeDemo(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        const ECS::FEntity Sun = BuildSunAndSky(World);
        BuildAtmosphereAndGrading(World);

        // Low from the side and behind the camera, so every shadow runs across the valley and away from the viewer.
        SDirectionalLightComponent& Light = World->GetComponent<SDirectionalLightComponent>(Sun);
        Light.Direction         = Math::Normalize(FVector3(0.9f, 0.45f, -0.35f));
        Light.bFarShadowCascade = true;

        BuildRangeTerrain(World);
        BuildRangeTowers(World);
        BuildRangeForests(World);
    }

    FCameraPose GetShadowRangeCameraPose()
    {
        const float GroundY = RangeHeight(0.0f, kRangeCameraZ);
        return { FVector3(0.0f, GroundY + 50.0f, kRangeCameraZ), FVector3(0.0f, GroundY, kRangeCameraZ - 600.0f) };
    }
}
