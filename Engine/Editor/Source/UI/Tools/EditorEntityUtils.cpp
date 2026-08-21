#include "EditorEntityUtils.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Components/EditorEntityTags.h"
#include "GUID/GUID.h"
#include "Containers/String.h"
#include "Core/Object/ObjectCore.h"
#include "Log/Log.h"
#include "Core/Math/AABB.h"
#include "World/Entity/EntityUtils.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TextComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "Core/Math/Math.h"
#include "Tools/FontManager/FontManager.h"
#include <cfloat>
#include "Containers/StringFormat.h"

namespace Lumina::EditorEntityUtils
{
    bool IsEditorOnlyComponent(const entt::type_info& Type)
    {
        return IsEditorOnlyComponent(Type.hash());
    }

    bool IsEditorOnlyComponent(entt::id_type TypeHash)
    {
        // Mirror this list in the prefab commit and duplicate filters, so editor-only state has one source.
        return TypeHash == entt::type_hash<FRelationshipComponent>::value()
            || TypeHash == entt::type_hash<FSelectedInEditorComponent>::value()
            || TypeHash == entt::type_hash<FHideInSceneOutliner>::value()
            || TypeHash == entt::type_hash<FEditorComponent>::value()
            || TypeHash == entt::type_hash<FLastSelectedTag>::value()
            || TypeHash == entt::type_hash<FCopiedTag>::value()
            || TypeHash == entt::type_hash<FNeedsTransformUpdate>::value();
    }

    bool DefaultDuplicateFilter(const entt::type_info& Type)
    {
        // CWorld::DuplicateEntity rebuilds parent links and re-emits the dirty flag itself.
        const entt::id_type Hash = Type.hash();
        return !(Hash == entt::type_hash<FSelectedInEditorComponent>::value()
              || Hash == entt::type_hash<FCopiedTag>::value()
              || Hash == entt::type_hash<FLastSelectedTag>::value());
    }

    void CycleGizmoOp(ImGuizmo::OPERATION& InOutOp)
    {
        switch (InOutOp)
        {
        case ImGuizmo::TRANSLATE: InOutOp = ImGuizmo::ROTATE;    break;
        case ImGuizmo::ROTATE:    InOutOp = ImGuizmo::SCALE;     break;
        case ImGuizmo::SCALE:     InOutOp = ImGuizmo::TRANSLATE; break;
        default:                  InOutOp = ImGuizmo::TRANSLATE; break;
        }
    }

    void ToggleGizmoMode(ImGuizmo::MODE& InOutMode)
    {
        InOutMode = (InOutMode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }

    void ApplyWorldMatrixToTransform(FEntityRegistry& Registry, entt::entity Entity, const FMatrix4& WorldMatrix)
    {
        if (!Registry.valid(Entity))
        {
            return;
        }

        STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FMatrix4 LocalMatrix = WorldMatrix;
        if (FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Entity))
        {
            if (Rel->Parent != entt::null && Registry.valid(Rel->Parent))
            {
                if (STransformComponent* ParentTransform = Registry.try_get<STransformComponent>(Rel->Parent))
                {
                    LocalMatrix = Math::Inverse(ParentTransform->GetWorldMatrix()) * WorldMatrix;
                }
            }
        }

        FVector3 LocalLocation, LocalScale, LocalSkew;
        FQuat LocalRotation;
        FVector4 LocalPersp;
        Math::Decompose(LocalMatrix, LocalScale, LocalRotation, LocalLocation, LocalSkew, LocalPersp);

        Transform->SetLocalLocation(LocalLocation);
        Transform->SetLocalRotation(LocalRotation);
        Transform->SetLocalScale(LocalScale);
    }

    FFixedString MakeOutlinerDisplayName(const SNameComponent* Name, entt::entity Entity, const char* Icon)
    {
        FFixedString Out;
        Out.append(Icon).append(" ");
        Out.append(Name ? Name->Name.c_str() : "<unnamed>");
        Out.append(FString(" - (" + Format("{}", entt::to_integral(Entity)) + ")"));
        return Out;
    }

    bool ComputeFocusBoundsForEntity(FEntityRegistry& Registry, entt::entity Entity, FVector3& OutCenter, float& OutRadius)
    {
        if (!Registry.valid(Entity))
        {
            return false;
        }

        // A lazy resolve propagates to the whole subtree, making a per-descendant call quadratic.
        ECS::Utils::ResolveAllDirtyTransforms(Registry);

        FVector3 Min(FLT_MAX);
        FVector3 Max(-FLT_MAX);
        bool bAny = false;

        auto Accumulate = [&](entt::entity E)
        {
            if (!Registry.valid(E))
            {
                return;
            }

            const STransformComponent* Transform = Registry.try_get<STransformComponent>(E);
            if (Transform == nullptr)
            {
                return;
            }

            const FMatrix4 WorldMatrix = Transform->GetWorldMatrix();

            if (const SStaticMeshComponent* Mesh = Registry.try_get<SStaticMeshComponent>(E))
            {
                if (Mesh->StaticMesh)
                {
                    const FAABB Box = Mesh->GetAABB().ToWorld(WorldMatrix);
                    Min = Math::Min(Min, Box.Min);
                    Max = Math::Max(Max, Box.Max);
                    bAny = true;
                    return;
                }
            }

            if (const SSkeletalMeshComponent* Skinned = Registry.try_get<SSkeletalMeshComponent>(E))
            {
                if (Skinned->SkeletalMesh)
                {
                    const FAABB Box = Skinned->GetAABB().ToWorld(WorldMatrix);
                    Min = Math::Min(Min, Box.Min);
                    Max = Math::Max(Max, Box.Max);
                    bAny = true;
                    return;
                }
            }

            const FVector3 Loc = Transform->GetWorldLocation();
            Min = Math::Min(Min, Loc);
            Max = Math::Max(Max, Loc);
            bAny = true;
        };

        Accumulate(Entity);
        ECS::Utils::ForEachDescendant(Registry, Entity, [&](entt::entity Desc)
        {
            Accumulate(Desc);
        });

        if (!bAny)
        {
            return false;
        }

        OutCenter = (Min + Max) * 0.5f;
        OutRadius = Math::Max(Math::Length(Max - Min) * 0.5f, 0.5f);
        return true;
    }

    bool GetEntityDrawBox(FEntityRegistry& Registry, entt::entity Entity, FVector3& OutCenter, FVector3& OutHalfExtents, FQuat& OutRotation)
    {
        if (!Registry.valid(Entity))
        {
            return false;
        }

        const STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return false;
        }

        OutRotation = Transform->GetWorldRotation();
        const FVector3 WorldScale = Transform->GetWorldScale();

        // Each contributor scales on its own basis, so they are unioned in the entity's rotated frame.
        auto VMin = [](const FVector3& A, const FVector3& B) { return FVector3(Math::Min(A.x, B.x), Math::Min(A.y, B.y), Math::Min(A.z, B.z)); };
        auto VMax = [](const FVector3& A, const FVector3& B) { return FVector3(Math::Max(A.x, B.x), Math::Max(A.y, B.y), Math::Max(A.z, B.z)); };

        FVector3 Min( FLT_MAX,  FLT_MAX,  FLT_MAX);
        FVector3 Max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        bool     bHasBounds = false;
        auto Accumulate = [&](const FVector3& BMin, const FVector3& BMax)
        {
            Min = VMin(Min, BMin);
            Max = VMax(Max, BMax);
            bHasBounds = true;
        };

        // A renderable mesh contributes its local-space AABB scaled by the transform.
        const SStaticMeshComponent*   Mesh    = Registry.try_get<SStaticMeshComponent>(Entity);
        const SSkeletalMeshComponent* Skinned = Registry.try_get<SSkeletalMeshComponent>(Entity);
        if (Mesh && Mesh->StaticMesh)
        {
            const FAABB Local = Mesh->GetAABB();
            Accumulate(Local.Min * WorldScale, Local.Max * WorldScale);
        }
        else if (Skinned && Skinned->SkeletalMesh)
        {
            const FAABB Local = Skinned->GetAABB();
            Accumulate(Local.Min * WorldScale, Local.Max * WorldScale);
        }

        // The shaped glyph extent gives a label entity a box around its text rather than a unit cube.
        if (const STextComponent* Text = Registry.try_get<STextComponent>(Entity); Text && !Text->Text.empty())
        {
            CFont* Font = Text->Font.Get();
            if (Font == nullptr || !Font->HasAtlas())
            {
                Font = CFontManager::Get().GetDefaultFont();
            }
            if (Font != nullptr && Font->HasAtlas())
            {
                const float HAlign = (Text->HorizontalAlign == ETextHorizontalAlign::Left)   ? 0.0f
                                   : (Text->HorizontalAlign == ETextHorizontalAlign::Center) ? 0.5f : 1.0f;
                const float VAlign = (Text->VerticalAlign == ETextVerticalAlign::Top)        ? 1.0f
                                   : (Text->VerticalAlign == ETextVerticalAlign::Middle)     ? 0.5f : 0.0f;

                TVector<FShapedGlyph> Shaped;
                if (Font->ShapeText(Text->Text, HAlign, VAlign, Text->LineSpacing, Shaped) && !Shaped.empty())
                {
                    FVector2 EmMin( FLT_MAX,  FLT_MAX);
                    FVector2 EmMax(-FLT_MAX, -FLT_MAX);
                    for (const FShapedGlyph& S : Shaped)
                    {
                        EmMin = FVector2(Math::Min(EmMin.x, S.Min.x), Math::Min(EmMin.y, S.Min.y));
                        EmMax = FVector2(Math::Max(EmMax.x, S.Max.x), Math::Max(EmMax.y, S.Max.y));
                    }
                    const float WS    = Text->WorldSize;
                    const float ThinZ = WS * 0.05f; // planar text -> give the box a little depth so it's visible
                    Accumulate(FVector3(EmMin.x * WS, EmMin.y * WS, -ThinZ),
                               FVector3(EmMax.x * WS, EmMax.y * WS,  ThinZ));
                }
            }
        }

        if (bHasBounds)
        {
            constexpr float Padding = 1.05f; // sit just outside the silhouette
            const FVector3 LocalCenter = (Min + Max) * 0.5f;
            const FVector3 LocalHalf   = (Max - Min) * 0.5f;

            OutCenter      = Transform->GetWorldLocation() + (OutRotation * LocalCenter);
            OutHalfExtents = LocalHalf * Padding;
        }
        else
        {
            // With no mesh or text bounds, a unit box scaled by the transform stands in.
            OutCenter      = Transform->GetWorldLocation();
            OutHalfExtents = WorldScale;
        }

        return true;
    }

    void DrawEntityBounds(CWorld* World, entt::entity Entity, const FVector4& Color, float Thickness, bool bDepthTest)
    {
        FVector3 Center, HalfExtents;
        FQuat Rotation;
        if (World && GetEntityDrawBox(ECS::GetWorldRegistry(*World), Entity, Center, HalfExtents, Rotation))
        {
            World->DrawBoxCorners(Center, HalfExtents, Rotation, Color, Thickness, bDepthTest);
        }
    }

    void DrawEntitySelectionBox(CWorld* World, entt::entity Entity, const FVector4& Color, float CornerFraction, float Thickness, bool bDepthTest)
    {
        FVector3 Center, HalfExtents;
        FQuat Rotation;
        if (World && GetEntityDrawBox(ECS::GetWorldRegistry(*World), Entity, Center, HalfExtents, Rotation))
        {
            World->DrawBoxCorners(Center, HalfExtents, Rotation, Color, CornerFraction, Thickness, bDepthTest);
        }
    }

    namespace
    {
        // The cube primitive is half-extent 1, so this scale lands the top face exactly on y = 0.
        constexpr float kFloorHalfSize  = 10.0f;
        constexpr float kFloorHalfDepth = 0.5f;

        // Unit-radius primitive, dropped from high enough to visibly fall rather than start resting.
        constexpr float kSphereRadius = 1.0f;
        constexpr float kSphereStartY = 4.0f;

        // Level with the resting sphere top, so the Z offset is what keeps them from overlapping.
        constexpr float kTextY = 2.0f;
        constexpr float kTextZ = -3.0f;

        // The extension is optional, since GetAssetByPath strips it from both sides before comparing.
        constexpr const char* kPreviewMaterialPath = "/Engine/Resources/Content/M_EditorPreview";

        // Placed at the resting center height, so they wrap the sphere instead of uplighting it.
        constexpr float kAccentLightRadius = 2.0f;
        constexpr float kAccentLightY      = kSphereRadius;

        // The convention is that -Z points at the viewer, so a half turn aims the text out of the screen.
        constexpr float kTextYawDegrees = 180.0f;
    }

    void PopulateDefaultScene(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        entt::entity Entity = World->ConstructEntity("Environment");
        World->EmplaceComponent<SEnvironmentComponent>(Entity);

        // A light pointing where the camera looks flattens everything, so the key rakes from the side.
        Entity = World->ConstructEntity("DirectionalLight");
        {
            SDirectionalLightComponent& Light = World->EmplaceComponent<SDirectionalLightComponent>(Entity);
            Light.Direction     = FVector3(-0.45f, 0.72f, 0.53f);
            Light.Color         = FVector3(1.0f, 0.94f, 0.84f);
            Light.Intensity     = 3.2f;
            Light.bCastShadows  = true;
        }

        // The warm and cool split stops the shadow side going flat gray, and it costs nothing.
        Entity = World->ConstructEntity("SkyLight");
        {
            SSkyLightComponent& SkyLight = World->EmplaceComponent<SSkyLightComponent>(Entity);
            SkyLight.AmbientColor = FVector3(0.42f, 0.55f, 0.78f);
            SkyLight.Intensity    = 0.22f;
        }

        // A cube needs no rotation, so the drawn box and the collider match without a second set of numbers.
        Entity = World->ConstructEntity("Floor", FTransform(
            FVector3(0.0f, -kFloorHalfDepth, 0.0f),
            FVector3(0.0f, 0.0f, 0.0f),
            FVector3(kFloorHalfSize, kFloorHalfDepth, kFloorHalfSize)));
        World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(CPrimitiveManager::Get().CubeMesh);
        World->EmplaceComponent<SBoxColliderComponent>(Entity).HalfExtent = FVector3(1.0f);
        World->EmplaceComponent<SRigidBodyComponent>(Entity).BodyType = EBodyType::Static;

        Entity = World->ConstructEntity("Sphere", FTransform(
            FVector3(0.0f, kSphereStartY, 0.0f),
            FVector3(0.0f, 0.0f, 0.0f),
            FVector3(1.0f, 1.0f, 1.0f)));
        {
            SStaticMeshComponent& Mesh = World->EmplaceComponent<SStaticMeshComponent>(Entity);
            Mesh.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);

            // The two failure modes are reported apart, since an undiscovered path needs a different fix.
            if (const FAssetData* PreviewData = FAssetRegistry::Get().GetAssetByPath(kPreviewMaterialPath))
            {
                if (CMaterialInterface* PreviewMaterial = LoadObject<CMaterialInterface>(kPreviewMaterialPath))
                {
                    Mesh.SetMaterialAtSlot(PreviewMaterial, 0);
                }
                else
                {
                    LOG_WARN("Default scene: preview material '{}' is registered (GUID {}, class {}) but "
                             "failed to load; the sphere falls back to the default material.",
                        kPreviewMaterialPath,
                        PreviewData->AssetGUID.ToString().c_str(),
                        PreviewData->AssetClass.c_str());
                }
            }
            else
            {
                LOG_WARN("Default scene: preview material '{}' is not in the asset registry; the sphere "
                         "falls back to the default material.", kPreviewMaterialPath);
            }
        }

        World->EmplaceComponent<SSphereColliderComponent>(Entity).Radius = kSphereRadius;
        World->EmplaceComponent<SRigidBodyComponent>(Entity).BodyType = EBodyType::Dynamic;

        // Volumetric on so the fog picks the colors up as shafts, with everything else left at defaults.
        {
            struct FAccentLight
            {
                const char* Name;
                FVector3    Color;
                float       AngleDegrees;
            };

            // Blue sits at 180 on the far side, the one position whose spill reaches the text.
            constexpr FAccentLight AccentLights[] =
            {
                { "PointLight_Red",   FVector3(1.0f, 0.0f, 0.0f),  60.0f },
                { "PointLight_Green", FVector3(0.0f, 1.0f, 0.0f), 300.0f },
                { "PointLight_Blue",  FVector3(0.0f, 0.0f, 1.0f), 180.0f },
            };

            for (const FAccentLight& Accent : AccentLights)
            {
                const float Radians = Math::Radians(Accent.AngleDegrees);

                Entity = World->ConstructEntity(Accent.Name, FTransform(
                    FVector3(Math::Sin(Radians) * kAccentLightRadius,
                             kAccentLightY,
                             Math::Cos(Radians) * kAccentLightRadius),
                    FVector3(0.0f, 0.0f, 0.0f),
                    FVector3(1.0f, 1.0f, 1.0f)));

                SPointLightComponent& Light = World->EmplaceComponent<SPointLightComponent>(Entity);
                Light.LightColor  = Accent.Color;
                Light.bVolumetric = true;
            }
        }

        // Font is left null so the extractor falls back to the engine default world-text font.
        Entity = World->ConstructEntity("Welcome Text", FTransform(
            FVector3(0.0f, kTextY, kTextZ),
            FVector3(0.0f, kTextYawDegrees, 0.0f),
            FVector3(1.0f, 1.0f, 1.0f)));
        {
            STextComponent& Text = World->EmplaceComponent<STextComponent>(Entity);
            Text.Text            = "Welcome to Lumina!";
            Text.WorldSize       = 0.85f;

            // The color feeds the same HDR buffer bloom reads, so over white is what makes the text bloom.
            Text.Color           = FVector4(1.35f, 1.5f, 1.9f, 1.0f);
            Text.HorizontalAlign = ETextHorizontalAlign::Center;
            Text.VerticalAlign   = ETextVerticalAlign::Middle;

            // A banner that swings to track the camera reads as a bug, so this stays fixed and depth-tested.
            Text.bBillboard      = false;
            Text.bDepthTest      = true;
        }

        // This is the level's global look, so there is no box to sit inside and no boundary to blend.
        Entity = World->ConstructEntity("Post Process");
        {
            SPostProcessComponent& PostProcess = World->EmplaceComponent<SPostProcessComponent>(Entity);
            PostProcess.bInfiniteExtent = true;
            PostProcess.BlendWeight     = 1.0f;

            SPostProcessSettings& Settings = PostProcess.Settings;

            // ACES hue-shifts saturated reds and crushes highlights, which is exactly this bloomed text.
            Settings.ToneMapper          = EToneMapper::AGX;
            Settings.ExposureCompensation = 0.15f;

            // Small numbers on purpose, since a heavily graded default is one somebody has to undo first.
            Settings.Temperature = 0.12f;
            Settings.Contrast    = 1.06f;
            Settings.Saturation  = 1.08f;

            Settings.BloomIntensity = 0.35f;
            Settings.BloomThreshold = 1.0f;
            Settings.BloomScatter   = 0.82f;
            Settings.BloomTint      = FVector3(0.85f, 0.92f, 1.0f);

            // Draws the eye to the center of frame where the sphere and text sit.
            Settings.VignetteIntensity  = 0.28f;
            Settings.VignetteSmoothness = 0.55f;
        }

        // The floor slab ends abruptly, and fog softens that seam into distance.
        Entity = World->ConstructEntity("Height Fog");
        {
            SExponentialHeightFogComponent& Fog = World->EmplaceComponent<SExponentialHeightFogComponent>(Entity);
            Fog.FogVisibilityDistance       = 500.0f;
            Fog.FogHeightFalloff            = 0.35f;
            Fog.FogBaseHeight               = -0.5f;
            Fog.FogMaxOpacity               = 0.65f;
            Fog.FogInscatteringColor        = FVector3(0.38f, 0.47f, 0.62f);
            Fog.DirectionalInscatteringColor = FVector3(1.0f, 0.88f, 0.68f);
        }
    }

    void GetDefaultScenePreviewPose(FVector3& OutLocation, FVector3& OutTarget)
    {
        // The scene is looked at far more after it settles than during the first second of the drop.
        OutTarget   = FVector3(0.0f, (kSphereRadius + kTextY) * 0.5f, kTextZ * 0.35f);

        // The key rakes from -X, so sitting on +X keeps the lit side and the terminator toward the camera.
        OutLocation = FVector3(6.0f, 4.0f, 9.5f);
    }
}
