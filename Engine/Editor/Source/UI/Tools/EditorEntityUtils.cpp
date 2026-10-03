#include "EditorEntityUtils.h"
#include "World/ECS/Registry.h"

#include "Components/EditorEntityTags.h"
#include "Containers/String.h"
#include "Core/Object/ObjectCore.h"
#include "Log/Log.h"
#include "Core/Math/AABB.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/EditorComponent.h"
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
    bool IsEditorOnlyComponent(const ECS::FComponentTypeInfo& Type)
    {
        return IsEditorOnlyComponent(Type.TypeID);
    }

    bool IsEditorOnlyComponent(uint32 TypeHash)
    {
        // Mirror this list in the prefab commit and duplicate filters, so editor-only state has one source.
        return TypeHash == ECS::GetComponentTypeID<FRelationshipComponent>()
            || TypeHash == ECS::GetComponentTypeID<FSelectedInEditorComponent>()
            || TypeHash == ECS::GetComponentTypeID<FHideInSceneOutliner>()
            || TypeHash == ECS::GetComponentTypeID<FEditorComponent>()
            || TypeHash == ECS::GetComponentTypeID<FLastSelectedTag>()
            || TypeHash == ECS::GetComponentTypeID<FCopiedTag>()
            || TypeHash == ECS::GetComponentTypeID<FNeedsTransformUpdate>();
    }

    bool DefaultDuplicateFilter(const ECS::FComponentTypeInfo& Type)
    {
        // CWorld::DuplicateEntity rebuilds parent links and re-emits the dirty flag itself.
        const uint32 Hash = Type.TypeID;
        return !(Hash == ECS::GetComponentTypeID<FSelectedInEditorComponent>()
              || Hash == ECS::GetComponentTypeID<FCopiedTag>()
              || Hash == ECS::GetComponentTypeID<FLastSelectedTag>());
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

    void ApplyWorldMatrixToTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FMatrix4& WorldMatrix)
    {
        if (!Registry.IsValid(Entity))
        {
            return;
        }

        STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FMatrix4 LocalMatrix = WorldMatrix;
        if (FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity))
        {
            if (Rel->Parent != ECS::NullEntity && Registry.IsValid(Rel->Parent))
            {
                if (STransformComponent* ParentTransform = Registry.TryGet<STransformComponent>(Rel->Parent))
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

    FFixedString MakeOutlinerDisplayName(const SNameComponent* Name, ECS::FEntity Entity, const char* Icon)
    {
        FFixedString Out;
        Out.append(Icon).append(" ");
        Out.append(Name ? Name->Name.c_str() : "<unnamed>");
        Out.append(FString(" - (" + Format("{}", (Entity).Value) + ")"));
        return Out;
    }

    bool ComputeFocusBoundsForEntity(ECS::FRegistry& Registry, ECS::FEntity Entity, FVector3& OutCenter, float& OutRadius)
    {
        if (!Registry.IsValid(Entity))
        {
            return false;
        }

        // A lazy resolve propagates to the whole subtree, making a per-descendant call quadratic.
        ECS::Utils::ResolveAllDirtyTransforms(Registry);

        FVector3 Min(FLT_MAX);
        FVector3 Max(-FLT_MAX);
        bool bAny = false;

        auto Accumulate = [&](ECS::FEntity E)
        {
            if (!Registry.IsValid(E))
            {
                return;
            }

            const STransformComponent* Transform = Registry.TryGet<STransformComponent>(E);
            if (Transform == nullptr)
            {
                return;
            }

            const FMatrix4 WorldMatrix = Transform->GetWorldMatrix();

            if (const SStaticMeshComponent* Mesh = Registry.TryGet<SStaticMeshComponent>(E))
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

            if (const SSkeletalMeshComponent* Skinned = Registry.TryGet<SSkeletalMeshComponent>(E))
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
        ECS::Utils::ForEachDescendant(Registry, Entity, [&](ECS::FEntity Desc)
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

    bool GetEntityDrawBox(ECS::FRegistry& Registry, ECS::FEntity Entity, FVector3& OutCenter, FVector3& OutHalfExtents, FQuat& OutRotation)
    {
        if (!Registry.IsValid(Entity))
        {
            return false;
        }

        const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
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
        const SStaticMeshComponent*   Mesh    = Registry.TryGet<SStaticMeshComponent>(Entity);
        const SSkeletalMeshComponent* Skinned = Registry.TryGet<SSkeletalMeshComponent>(Entity);
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
        if (const STextComponent* Text = Registry.TryGet<STextComponent>(Entity); Text && !Text->Text.empty())
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

    void DrawEntityBounds(CWorld* World, ECS::FEntity Entity, const FVector4& Color, float Thickness, bool bDepthTest)
    {
        FVector3 Center, HalfExtents;
        FQuat Rotation;
        if (World && GetEntityDrawBox(ECS::GetWorldRegistry(*World), Entity, Center, HalfExtents, Rotation))
        {
            World->DrawBoxCorners(Center, HalfExtents, Rotation, Color, Thickness, bDepthTest);
        }
    }

    void DrawEntitySelectionBox(CWorld* World, ECS::FEntity Entity, const FVector4& Color, float CornerFraction, float Thickness, bool bDepthTest)
    {
        FVector3 Center, HalfExtents;
        FQuat Rotation;
        if (World && GetEntityDrawBox(ECS::GetWorldRegistry(*World), Entity, Center, HalfExtents, Rotation))
        {
            World->DrawBoxCorners(Center, HalfExtents, Rotation, Color, CornerFraction, Thickness, bDepthTest);
        }
    }
}
