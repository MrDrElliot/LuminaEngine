#include "EditorEntityUtils.h"

#include "Components/EditorEntityTags.h"
#include "Containers/String.h"
#include "EASTL/string.h"
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

namespace Lumina::EditorEntityUtils
{
    bool IsEditorOnlyComponent(const entt::type_info& Type)
    {
        return IsEditorOnlyComponent(Type.hash());
    }

    bool IsEditorOnlyComponent(entt::id_type TypeHash)
    {
        // Mirror this list in CommitPreviewWorldToPrefab() / DuplicateEntity filters so
        // every editor-only state is excluded from save and copy by one source of truth.
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
        // Same set, minus FRelationshipComponent and FNeedsTransformUpdate which CWorld::DuplicateEntity
        // handles itself (it rebuilds parent/child links and re-emits the dirty flag).
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
        Out.append_convert(FString(" - (" + eastl::to_string(entt::to_integral(Entity)) + ")"));
        return Out;
    }

    bool ComputeFocusBoundsForEntity(FEntityRegistry& Registry, entt::entity Entity, FVector3& OutCenter, float& OutRadius)
    {
        if (!Registry.valid(Entity))
        {
            return false;
        }

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

        // Accumulate a box in the entity's rotated frame, in WORLD units. Each contributor scales on its own
        // basis (mesh local AABB * WorldScale; text em-extent * WorldSize), so they're unioned here.
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

        // Renderable mesh asset: local-space AABB scaled by the transform.
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

        // World text: include the shaped glyph extent (em units * WorldSize) in the entity's local X/Y plane,
        // so a nameplate/label entity gets a box around the text rather than a unit cube.
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
            // No mesh/text bounds (lights, audio, empties, ...): a unit box scaled by the transform.
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
}
