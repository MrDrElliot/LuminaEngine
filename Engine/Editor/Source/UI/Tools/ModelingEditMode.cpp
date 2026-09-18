#include "ModelingEditMode.h"

#include <cmath>

#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/BlockoutSystem.h"
#include "World/Subsystems/BlockoutMeshBuilder.h"
#include "World/Subsystems/TerrainSculptSystem.h"
#include "World/World.h"

namespace Lumina
{
    namespace ModelingEditModePrivate
    {
        const FVector4 kPreviewColor(0.35f, 0.85f, 1.00f, 1.0f);
        const FVector4 kDragColor   (1.00f, 0.80f, 0.25f, 1.0f);
        const FVector4 kSurfaceColor(0.35f, 0.85f, 1.00f, 0.6f);

        constexpr float kFallbackPlacementDistance = 10.0f;
        constexpr float kMinimumTraceDistance = 0.01f;

        bool TryBuildRayFromScreen(const SCameraComponent& Camera, ImVec2 PixelWithinViewport, ImVec2 ViewportSize,
                                FVector3& OutOrigin, FVector3& OutDir)
        {
            if (PixelWithinViewport.x < 0.0f || PixelWithinViewport.y < 0.0f
                || PixelWithinViewport.x > ViewportSize.x || PixelWithinViewport.y > ViewportSize.y)
            {
                return false;
            }

            const float W = Math::Max(ViewportSize.x, 1.0f);
            const float H = Math::Max(ViewportSize.y, 1.0f);

            const float Sx = (PixelWithinViewport.x / W) * 2.0f - 1.0f;
            const float Sy = 1.0f - (PixelWithinViewport.y / H) * 2.0f;

            const FViewVolume& View    = Camera.GetViewVolume();
            const FVector3     Forward = View.GetForwardVector();
            const FVector3     Up      = View.GetUpVector();
            const FVector3     Right   = Math::Normalize(Math::Cross(Up, Forward));

            const float AspectRatio = W / H;
            const float TanHalfFov  = std::tan(Math::Radians(View.GetFOV()) * 0.5f);

            OutOrigin = Camera.GetPosition();
            OutDir    = Math::Normalize(Forward
                                      + Right * (Sx * TanHalfFov * AspectRatio)
                                      + Up    * (Sy * TanHalfFov));
            return true;
        }

        bool RaycastHorizontalPlane(const FVector3& Origin, const FVector3& Dir, float PlaneY, FVector3& OutHit)
        {
            if (Math::Abs(Dir.y) < 1e-6f)
            {
                return false;
            }
            const float T = (PlaneY - Origin.y) / Dir.y;
            if (T <= 0.0f)
            {
                return false;
            }
            OutHit = Origin + Dir * T;
            return true;
        }

        bool RaycastLocalBox(const FVector3& Origin, const FVector3& Dir, const FMatrix4& LocalToWorld,
                             const FVector3& LocalMin, const FVector3& LocalMax, float& OutDistance)
        {
            const FMatrix4 WorldToLocal = Math::Inverse(LocalToWorld);
            const FVector3 LocalOrigin  = FVector3(WorldToLocal * FVector4(Origin, 1.0f));
            const FVector3 LocalDir     = FVector3(WorldToLocal * FVector4(Dir, 0.0f));

            float TMin = 0.0f;
            float TMax = 1e30f;

            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                if (Math::Abs(LocalDir[Axis]) < 1e-6f)
                {
                    if (LocalOrigin[Axis] < LocalMin[Axis] || LocalOrigin[Axis] > LocalMax[Axis])
                    {
                        return false;
                    }
                    continue;
                }

                float TNear = (LocalMin[Axis] - LocalOrigin[Axis]) / LocalDir[Axis];
                float TFar  = (LocalMax[Axis] - LocalOrigin[Axis]) / LocalDir[Axis];
                if (TNear > TFar)
                {
                    const float Swap = TNear;
                    TNear = TFar;
                    TFar  = Swap;
                }

                TMin = Math::Max(TMin, TNear);
                TMax = Math::Min(TMax, TFar);
                if (TMin > TMax)
                {
                    return false;
                }
            }

            const FVector3 WorldHit = FVector3(LocalToWorld * FVector4(LocalOrigin + LocalDir * TMin, 1.0f));
            OutDistance = Math::Length(WorldHit - Origin);
            return true;
        }

        const char* GetShapeIcon(EBlockoutShape Shape)
        {
            switch (Shape)
            {
                case EBlockoutShape::Box:      return LE_ICON_CUBE_OUTLINE;
                case EBlockoutShape::Plane:    return LE_ICON_SQUARE_OUTLINE;
                case EBlockoutShape::Ramp:     return LE_ICON_SET_SQUARE;
                case EBlockoutShape::Stairs:   return LE_ICON_STAIRS;
                case EBlockoutShape::Cylinder: return LE_ICON_CYLINDER;
                case EBlockoutShape::Cone:     return LE_ICON_CONE;
                case EBlockoutShape::Sphere:   return LE_ICON_SPHERE;
                case EBlockoutShape::Capsule:  return LE_ICON_PILL;
                case EBlockoutShape::Torus:    return LE_ICON_CIRCLE_DOUBLE;
                case EBlockoutShape::Arch:     return LE_ICON_TUNNEL;
                case EBlockoutShape::Pipe:     return LE_ICON_PIPE;
            }
            return LE_ICON_SHAPE;
        }

        void DrawWireBox(CWorld* World, const FVector3& Center, const FVector3& HalfExtents, const FVector4& Color, float Thickness)
        {
            World->DrawBox(Center, HalfExtents, FQuat::Identity(), Color, Thickness, false, -1.0f);
        }
    }
    using namespace ModelingEditModePrivate;

    float FModelingEditMode::SnapAxis(float Value) const
    {
        if (!bSnapEnabled || GridSize <= 0.0f)
        {
            return Value;
        }
        return std::round(Value / GridSize) * GridSize;
    }

    void FModelingEditMode::OnEnter(CWorld* World)
    {
        (void)World;
        bPlacementArmed = true;
        bDragging       = false;
        bHoverValid     = false;
    }

    void FModelingEditMode::OnExit(CWorld* World)
    {
        (void)World;
        bDragging      = false;
        bHoverValid    = false;
        BoundShapeData = nullptr;

        if (bPropertyTransactionOpen && Context != nullptr)
        {
            Context->EndModeTransaction("Edit Blockout");
            bPropertyTransactionOpen = false;
        }
    }

    bool FModelingEditMode::TraceSurface(CWorld* World, const FVector3& RayOrigin, const FVector3& RayDir, FVector3& OutHit) const
    {
        float BestDistance = FLT_MAX;
        bool  bHit = false;

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        for (auto&& [Entity, Terrain] : Registry.View<STerrainComponent>().Each())
        {
            FVector3 TerrainOrigin(0.0f);
            if (const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
            {
                TerrainOrigin = Transform->GetWorldLocation();
            }

            FVector3 Hit;
            if (FTerrainSculptSystem::Raycast(Terrain, TerrainOrigin, RayOrigin, RayDir, Hit))
            {
                const float Distance = Math::Distance(RayOrigin, Hit);
                if (Distance < BestDistance)
                {
                    BestDistance = Distance;
                    OutHit = Hit;
                    bHit = true;
                }
            }
        }

        // Blockout shapes are traced as their own oriented boxes, which is what makes stacking land flush.
        for (auto&& [Entity, Shape, Transform] : Registry.View<SBlockoutComponent, STransformComponent>().Each())
        {
            FVector3 LocalMin, LocalMax;
            BlockoutMesh::GetLocalBounds(Shape, LocalMin, LocalMax);

            // A camera sitting inside a shape reports distance zero, which would win every trace.
            float Distance = 0.0f;
            if (RaycastLocalBox(RayOrigin, RayDir, Transform.GetWorldMatrix(), LocalMin, LocalMax, Distance)
                && Distance > kMinimumTraceDistance && Distance < BestDistance)
            {
                BestDistance = Distance;
                OutHit = RayOrigin + RayDir * Distance;
                bHit = true;
            }
        }

        if (bHit)
        {
            return true;
        }

        if (RaycastHorizontalPlane(RayOrigin, RayDir, 0.0f, OutHit))
        {
            return true;
        }

        OutHit = RayOrigin + RayDir * kFallbackPlacementDistance;
        return true;
    }

    void FModelingEditMode::GetPendingFootprint(FVector3& OutCenter, float& OutSizeX, float& OutSizeZ) const
    {
        if (!bDragging)
        {
            OutCenter = HoverPoint;
            OutSizeX  = Template.Size.x;
            OutSizeZ  = Template.Size.z;
            return;
        }

        const float MinX = Math::Min(DragAnchor.x, DragCurrent.x);
        const float MaxX = Math::Max(DragAnchor.x, DragCurrent.x);
        const float MinZ = Math::Min(DragAnchor.z, DragCurrent.z);
        const float MaxZ = Math::Max(DragAnchor.z, DragCurrent.z);

        const float Width = MaxX - MinX;
        const float Depth = MaxZ - MinZ;

        // A drag too small to read as a rectangle is a click, and a click places the template footprint.
        const float MinimumDrag = Math::Max(GridSize, 0.05f);
        if (Width < MinimumDrag || Depth < MinimumDrag)
        {
            OutCenter = FVector3(DragAnchor.x, DragAnchor.y, DragAnchor.z);
            OutSizeX  = Template.Size.x;
            OutSizeZ  = Template.Size.z;
            return;
        }

        OutCenter = FVector3((MinX + MaxX) * 0.5f, DragAnchor.y, (MinZ + MaxZ) * 0.5f);
        OutSizeX  = Width;
        OutSizeZ  = Depth;
    }

    void FModelingEditMode::CreateShape(CWorld* World, const FVector3& Center, float SizeX, float SizeZ)
    {
        if (Context != nullptr)
        {
            Context->BeginModeCreationTransaction();
        }

        const char* ShapeName = BlockoutMesh::GetShapeName(Template.Shape);
        const ECS::FEntity Entity = World->ConstructEntity(FName(ShapeName), FTransform(Center, FVector3(0.0f), FVector3(1.0f)));

        SBlockoutComponent& Shape = World->EmplaceComponent<SBlockoutComponent>(Entity);
        Shape = Template;
        Shape.Size.x       = SizeX;
        Shape.Size.z       = SizeZ;
        Shape.bBuilt       = false;
        Shape.BuiltHash    = 0;
        Shape.BuiltMaterial = nullptr;

        SDynamicMeshComponent& Mesh = World->EmplaceComponent<SDynamicMeshComponent>(Entity);

        if (bCreateCollision)
        {
            World->EmplaceComponent<SDynamicMeshColliderComponent>(Entity);
        }

        // Built now rather than on the next system tick so the shape is visible under the cursor immediately.
        SBlockoutSystem::Rebuild(Shape, Mesh, bCreateCollision);

        if (Context != nullptr)
        {
            Context->EndModeTransaction("Create Blockout");
            Context->SetModeSelection(Entity);
        }
    }

    void FModelingEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered,
                                 ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        bHoverValid = false;

        if (World == nullptr || !bPlacementArmed)
        {
            bDragging = false;
            return;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (bDragging)
            {
                bDragging = false;
            }
            else
            {
                bPlacementArmed = false;
            }
            return;
        }

        const ImVec2 MousePos = ImGui::GetMousePos();
        const ImVec2 Local    = ImVec2(MousePos.x - ViewportScreenOrigin.x, MousePos.y - ViewportScreenOrigin.y);

        FVector3 RayOrigin, RayDir;
        const bool bRayValid = TryBuildRayFromScreen(Camera, Local, ViewportSize, RayOrigin, RayDir);

        // Before the hover gate, or releasing off the viewport strands the drag and it finishes stale later.
        if (bDragging)
        {
            if (bRayValid)
            {
                FVector3 PlaneHit;
                if (RaycastHorizontalPlane(RayOrigin, RayDir, DragAnchor.y, PlaneHit))
                {
                    DragCurrent = FVector3(SnapAxis(PlaneHit.x), DragAnchor.y, SnapAxis(PlaneHit.z));
                }
            }

            HoverPoint  = DragCurrent;
            bHoverValid = true;

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                FVector3 Center;
                float SizeX = 0.0f;
                float SizeZ = 0.0f;
                GetPendingFootprint(Center, SizeX, SizeZ);

                bDragging = false;
                CreateShape(World, Center, SizeX, SizeZ);
            }
            return;
        }

        if (!bViewportHovered || !bRayValid)
        {
            return;
        }

        FVector3 Hit;
        if (!TraceSurface(World, RayOrigin, RayDir, Hit))
        {
            return;
        }

        HoverPoint  = FVector3(SnapAxis(Hit.x), Hit.y, SnapAxis(Hit.z));
        bHoverValid = true;

        // Alt and left drag is the editor's orbit gesture, so a click carrying it is camera intent.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
        {
            bDragging   = true;
            DragAnchor  = HoverPoint;
            DragCurrent = HoverPoint;
        }
    }

    void FModelingEditMode::DrawOverlay(CWorld* World, ImVec2, ImVec2, const SCameraComponent&)
    {
        if (World == nullptr || !bPlacementArmed || !bHoverValid)
        {
            return;
        }

        FVector3 Center;
        float SizeX = 0.0f;
        float SizeZ = 0.0f;
        GetPendingFootprint(Center, SizeX, SizeZ);

        SBlockoutComponent Preview = Template;
        Preview.Size.x = SizeX;
        Preview.Size.z = SizeZ;

        FVector3 LocalMin, LocalMax;
        BlockoutMesh::GetLocalBounds(Preview, LocalMin, LocalMax);

        const FVector3 BoxCenter = Center + (LocalMin + LocalMax) * 0.5f;
        const FVector3 HalfExtents = (LocalMax - LocalMin) * 0.5f;

        DrawWireBox(World, BoxCenter, HalfExtents, bDragging ? kDragColor : kPreviewColor, 2.0f);

        // A flat marker on the surface, so the footprint still reads when the shape is tall.
        const FVector3 A(Center.x - SizeX * 0.5f, Center.y, Center.z - SizeZ * 0.5f);
        const FVector3 B(Center.x + SizeX * 0.5f, Center.y, Center.z - SizeZ * 0.5f);
        const FVector3 C(Center.x + SizeX * 0.5f, Center.y, Center.z + SizeZ * 0.5f);
        const FVector3 D(Center.x - SizeX * 0.5f, Center.y, Center.z + SizeZ * 0.5f);

        World->DrawLine(A, B, kSurfaceColor, 2.0f, false, -1.0f);
        World->DrawLine(B, C, kSurfaceColor, 2.0f, false, -1.0f);
        World->DrawLine(C, D, kSurfaceColor, 2.0f, false, -1.0f);
        World->DrawLine(D, A, kSurfaceColor, 2.0f, false, -1.0f);
    }

    void FModelingEditMode::RebindShapeSettings(CWorld* World)
    {
        void* Desired = &Template;

        if (Context != nullptr && World != nullptr)
        {
            const ECS::FEntity Focus = Context->GetModeSelectionFocus();
            if (Focus != ECS::NullEntity && World->IsValidEntity(Focus))
            {
                if (SBlockoutComponent* Selected = World->TryGetComponent<SBlockoutComponent>(Focus))
                {
                    Desired = Selected;
                }
            }
        }

        // The component lives in ECS storage, which moves it on a swap-remove, so compare the address.
        if (Desired == BoundShapeData)
        {
            return;
        }

        BoundShapeData = Desired;
        ShapeSettings.SetObject(Desired, SBlockoutComponent::StaticStruct());
        ShapeSettings.SetShowSearchBar(false);

        ShapeSettings.SetStartEditCallback([this](const FPropertyChangedEvent&)
        {
            // The template is mode state rather than world state, so editing it is not an undo step.
            if (Context == nullptr || BoundShapeData == &Template || bPropertyTransactionOpen)
            {
                return;
            }
            Context->BeginModeTransaction();
            bPropertyTransactionOpen = true;
        });

        ShapeSettings.SetFinishEditCallback([this](const FPropertyChangedEvent&)
        {
            if (Context == nullptr || !bPropertyTransactionOpen)
            {
                return;
            }
            Context->EndModeTransaction("Edit Blockout");
            bPropertyTransactionOpen = false;
        });
    }

    void FModelingEditMode::DrawShapePalette()
    {
        ImGui::SeparatorText("Shape");

        constexpr int32 ButtonsPerRow = 4;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float ButtonWidth = (ImGui::GetContentRegionAvail().x - Spacing * float(ButtonsPerRow - 1)) / float(ButtonsPerRow);

        for (int32 Index = 0; Index < BlockoutMesh::ShapeCount; ++Index)
        {
            const EBlockoutShape Shape = (EBlockoutShape)Index;
            const bool bSelected = (Template.Shape == Shape);

            if (Index % ButtonsPerRow != 0)
            {
                ImGui::SameLine();
            }

            ImGui::PushID(Index);
            ImGui::PushStyleColor(ImGuiCol_Button, bSelected ? IM_COL32(60, 110, 160, 255) : ImGui::GetColorU32(ImGuiCol_Button));

            char Label[64];
            ImFormatString(Label, sizeof(Label), "%s %s", GetShapeIcon(Shape), BlockoutMesh::GetShapeName(Shape));
            if (ImGui::Button(Label, ImVec2(ButtonWidth, 0.0f)))
            {
                Template.Shape = Shape;
                bPlacementArmed = true;
            }

            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    void FModelingEditMode::DrawPlacementSettings()
    {
        ImGui::SeparatorText("Placement");

        ImGui::Checkbox("Snap to grid", &bSnapEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Collision", &bCreateCollision);
        ImGuiX::TextTooltip("{}", "Gives each new shape a mesh collider, so it blocks the player while you test the layout.");

        static const float GridPresets[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };

        char Preview[32];
        ImFormatString(Preview, sizeof(Preview), "%.2f m", GridSize);

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##GridSize", Preview))
        {
            for (float Preset : GridPresets)
            {
                char Label[32];
                ImFormatString(Label, sizeof(Label), "%.2f m", Preset);
                if (ImGui::Selectable(Label, GridSize == Preset))
                {
                    GridSize = Preset;
                }
            }
            ImGui::EndCombo();
        }
    }

    void FModelingEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        if (World == nullptr)
        {
            return;
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, bPlacementArmed ? IM_COL32(60, 110, 160, 255) : ImGui::GetColorU32(ImGuiCol_Button));
        if (ImGui::Button(bPlacementArmed ? LE_ICON_SHAPE_PLUS " Place" : LE_ICON_CURSOR_DEFAULT " Select", ImVec2(0, ButtonSize)))
        {
            bPlacementArmed = !bPlacementArmed;
            bDragging = false;
        }
        ImGui::PopStyleColor();
        ImGuiX::TextTooltip("{}", "Arm placement, or return to selection and the transform gizmo. Escape disarms.");

        ImGui::SameLine();
        if (ImGui::Button(bShowPanel ? LE_ICON_SHAPE " Hide Shapes" : LE_ICON_SHAPE " Shapes", ImVec2(0, ButtonSize)))
        {
            bShowPanel = !bShowPanel;
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, bSnapEnabled ? IM_COL32(60, 110, 160, 255) : ImGui::GetColorU32(ImGuiCol_Button));
        char SnapLabel[48];
        ImFormatString(SnapLabel, sizeof(SnapLabel), "%s %.2fm", LE_ICON_MAGNET, GridSize);
        if (ImGui::Button(SnapLabel, ImVec2(0, ButtonSize)))
        {
            bSnapEnabled = !bSnapEnabled;
        }
        ImGui::PopStyleColor();

        if (!bShowPanel)
        {
            return;
        }

        ImVec2 AnchorPos = ImGui::GetWindowPos();
        AnchorPos.y += ImGui::GetWindowSize().y + 4.0f;
        ImGui::SetNextWindowPos(AnchorPos);
        ImGui::SetNextWindowBgAlpha(0.85f);

        // Wide enough for the material picker, whose thumbnail eats the first 64 pixels of its row.
        constexpr float PanelWidth = 460.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(PanelWidth, 0.0f), ImVec2(PanelWidth, FLT_MAX));
        ImGui::Begin("##ModelingShapePanel", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);

        DrawShapePalette();
        DrawPlacementSettings();

        RebindShapeSettings(World);

        const bool bEditingSelection = (BoundShapeData != &Template);
        ImGui::SeparatorText(bEditingSelection ? "Selected Shape" : "New Shape Defaults");
        ShapeSettings.DrawTree();

        ImGui::TextDisabled("Drag to size the footprint, click to place at the default size.");
        ImGui::End();
    }
}
