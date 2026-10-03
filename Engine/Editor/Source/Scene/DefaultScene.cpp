#include "EditorPCH.h"
#include "Scene/DefaultScene.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Containers/String.h"
#include "Log/Log.h"
#include "Renderer/MaterialTypes.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "World/Entity/Components/CloudComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
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

            // Bloom thresholds at 0.5, so only the wordmark is pushed far enough to glow.
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
}
