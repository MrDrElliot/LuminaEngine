#include "CollisionShapeEditorTool.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Physics/CollisionShapeGen.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/Transactions/EditorTransaction.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "imgui.h"

namespace Lumina
{
    static const char* CollisionShapeListWindowName    = "Shapes";
    static const char* CollisionShapeDetailsWindowName = "Details";

    static const FVector4 ShapeColor    = FVector4(0.35f, 0.80f, 1.00f, 1.0f);
    static const FVector4 ShapeSelColor = FVector4(1.00f, 0.75f, 0.15f, 1.0f);
    static const FVector4 TriMeshColor  = FVector4(0.55f, 1.00f, 0.60f, 1.0f);

    static const char* PrimitiveIcon(ECollisionPrimitiveType Type)
    {
        switch (Type)
        {
        case ECollisionPrimitiveType::Sphere:  return LE_ICON_CIRCLE_OUTLINE;
        case ECollisionPrimitiveType::Capsule: return LE_ICON_BOWLING;
        case ECollisionPrimitiveType::ConvexHull: return LE_ICON_VECTOR_POLYLINE;
        default: return LE_ICON_CUBE_OUTLINE;
        }
    }

    static const char* PrimitiveTypeName(ECollisionPrimitiveType Type)
    {
        switch (Type)
        {
        case ECollisionPrimitiveType::Sphere:  return "Sphere";
        case ECollisionPrimitiveType::Capsule: return "Capsule";
        case ECollisionPrimitiveType::ConvexHull: return "Convex Hull";
        default: return "Box";
        }
    }

    static bool ClosestParamOnAxisToRay(const FVector3& Anchor, const FVector3& Axis,
                                        const FVector3& RayOrigin, const FVector3& RayDirection, float& OutT)
    {
        const FVector3 ToAnchor = Anchor - RayOrigin;
        const float AxisDotRay = Math::Dot(Axis, RayDirection);
        const float Denominator = 1.0f - AxisDotRay * AxisDotRay;
        if (Math::Abs(Denominator) < 1e-5f)
        {
            return false;
        }

        OutT = (AxisDotRay * Math::Dot(RayDirection, ToAnchor) - Math::Dot(Axis, ToAnchor)) / Denominator;
        return true;
    }

    static bool RayHitsSphere(const FVector3& Origin, const FVector3& Direction, const FVector3& Center, float Radius, float& OutT)
    {
        const FVector3 ToOrigin = Origin - Center;
        const float B = Math::Dot(ToOrigin, Direction);
        const float C = Math::Dot(ToOrigin, ToOrigin) - Radius * Radius;

        if (C > 0.0f && B > 0.0f)
        {
            return false;
        }

        const float Discriminant = B * B - C;
        if (Discriminant < 0.0f)
        {
            return false;
        }

        OutT = Math::Max(0.0f, -B - Math::Sqrt(Discriminant));
        return true;
    }

    static bool RayHitsBox(const FVector3& Origin, const FVector3& Direction, const FMatrix4& Matrix,
                           const FVector3& HalfExtent, float& OutT)
    {
        const FMatrix4 Inverse = Math::Inverse(Matrix);
        const FVector3 LocalOrigin = FVector3(Inverse * FVector4(Origin, 1.0f));
        const FVector3 LocalDirection = FVector3(Inverse * FVector4(Direction, 0.0f));

        const float OriginAxis[3]    = { LocalOrigin.x, LocalOrigin.y, LocalOrigin.z };
        const float DirectionAxis[3] = { LocalDirection.x, LocalDirection.y, LocalDirection.z };
        const float ExtentAxis[3]    = { HalfExtent.x, HalfExtent.y, HalfExtent.z };

        float TMin = 0.0f;
        float TMax = 1e30f;

        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            if (Math::Abs(DirectionAxis[Axis]) < 1e-6f)
            {
                if (OriginAxis[Axis] < -ExtentAxis[Axis] || OriginAxis[Axis] > ExtentAxis[Axis])
                {
                    return false;
                }
                continue;
            }

            float TNear = (-ExtentAxis[Axis] - OriginAxis[Axis]) / DirectionAxis[Axis];
            float TFar  = ( ExtentAxis[Axis] - OriginAxis[Axis]) / DirectionAxis[Axis];
            if (TNear > TFar)
            {
                std::swap(TNear, TFar);
            }

            TMin = Math::Max(TMin, TNear);
            TMax = Math::Min(TMax, TFar);
            if (TMin > TMax)
            {
                return false;
            }
        }

        const FVector3 WorldHit = FVector3(Matrix * FVector4(LocalOrigin + LocalDirection * TMin, 1.0f));
        OutT = Math::Length(WorldHit - Origin);
        return true;
    }

    FCollisionShapeEditorTool::FCollisionShapeEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FCollisionShapeEditorTool::OnInitialize()
    {
        DetailsTable = MakeUnique<FPropertyTable>();
        DetailsTable->SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            RebuildHullWireframes();
            NotifyAssetDataChanged();
        });

        CreateToolWindow(CollisionShapeListWindowName, [this](bool) { DrawShapeListWindow(); });
        CreateToolWindow(CollisionShapeDetailsWindowName, [this](bool) { DrawDetailsWindow(); });

        CachedSourceMesh = GetAsset<CCollisionShape>()->SourceMesh;
        RebuildHullWireframes();
    }

    void FCollisionShapeEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        CreateFloorPlane();

        LightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(LightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(LightEntity);
        World->EmplaceComponent<SSkyLightComponent>(LightEntity);

        CameraState.Speed = 5.0f;

        RefreshPreviewMesh();
    }

    void FCollisionShapeEditorTool::RefreshPreviewMesh()
    {
        CachedSourceMesh = GetAsset<CCollisionShape>()->SourceMesh;

        if (!World.IsValid())
        {
            return;
        }

        if (MeshEntity != entt::null)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = entt::null;
        }

        CStaticMesh* Mesh = CachedSourceMesh.Get();
        if (Mesh == nullptr)
        {
            return;
        }

        MeshEntity = World->ConstructEntity("SourceMesh");
        SStaticMeshComponent& MeshComponent = World->EmplaceComponent<SStaticMeshComponent>(MeshEntity);
        MeshComponent.SetStaticMesh(Mesh);
    }

    void FCollisionShapeEditorTool::RebuildHullWireframes()
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();

        HullWireframes.clear();
        HullWireframes.resize(Shape->Primitives.size());

        for (int32 i = 0; i < (int32)Shape->Primitives.size(); ++i)
        {
            const SCollisionPrimitive& Primitive = Shape->Primitives[i];
            if (Primitive.Type != ECollisionPrimitiveType::ConvexHull)
            {
                continue;
            }

            Physics::CollisionGen::BuildHullWireframe(Primitive.HullPoints,
                HullWireframes[i].Vertices, HullWireframes[i].Edges);
        }
    }

    void FCollisionShapeEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();
        RefreshPreviewMesh();
        RebuildHullWireframes();
    }

    void FCollisionShapeEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // A restore rewrites Primitives wholesale, so the details pointer and selection go stale.
        DetailsTarget = nullptr;

        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        if (SelectedPrimitive >= (int32)Shape->Primitives.size())
        {
            SelectedPrimitive = INDEX_NONE;
        }

        RebuildHullWireframes();
        SyncDetailsTable();
    }

    void FCollisionShapeEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid())
        {
            return;
        }

        if (CachedSourceMesh.Get() != GetAsset<CCollisionShape>()->SourceMesh.Get())
        {
            RefreshPreviewMesh();
        }

        if (World->GetRenderer() != nullptr)
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }

        SyncDetailsTable();

        if (bDrawShapes)
        {
            DrawPrimitives();
        }
    }

    FMatrix4 FCollisionShapeEditorTool::GetPrimitiveMatrix(const SCollisionPrimitive& Primitive) const
    {
        return Math::Translate(FMatrix4(1.0f), Primitive.Center)
             * Math::ToMatrix4(FQuat(Math::Radians(Primitive.Rotation)));
    }

    void FCollisionShapeEditorTool::DrawPrimitives()
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();

        // A baked triangle mesh replaces the primitives, so draw it alone to match what gets built.
        if (Shape->IsConcave())
        {
            for (SIZE_T i = 0; i + 2 < Shape->TriangleIndices.size(); i += 3)
            {
                const FVector3& A = Shape->TriangleVertices[Shape->TriangleIndices[i]];
                const FVector3& B = Shape->TriangleVertices[Shape->TriangleIndices[i + 1]];
                const FVector3& C = Shape->TriangleVertices[Shape->TriangleIndices[i + 2]];

                World->DrawLine(A, B, TriMeshColor, 1.0f, false);
                World->DrawLine(B, C, TriMeshColor, 1.0f, false);
                World->DrawLine(C, A, TriMeshColor, 1.0f, false);
            }
            return;
        }

        for (int32 i = 0; i < (int32)Shape->Primitives.size(); ++i)
        {
            const SCollisionPrimitive& Primitive = Shape->Primitives[i];

            const bool bSelected = (i == SelectedPrimitive);
            const FVector4 Color = bSelected ? ShapeSelColor : ShapeColor;
            const float Thickness = bSelected ? 3.0f : 1.5f;

            const FMatrix4 Matrix = GetPrimitiveMatrix(Primitive);
            const FVector3 Center = Primitive.Center;

            switch (Primitive.Type)
            {
            case ECollisionPrimitiveType::Box:
                World->DrawBox(Center, Primitive.HalfExtent, FQuat(Math::Radians(Primitive.Rotation)), Color, Thickness, false);
                break;

            case ECollisionPrimitiveType::Sphere:
                World->DrawSphere(Center, Primitive.Radius, Color, 16, Thickness, false);
                break;

            case ECollisionPrimitiveType::Capsule:
                {
                    const FVector3 Axis = Math::Normalize(FVector3(Matrix[1]));
                    World->DrawCapsule(Center - Axis * Primitive.HalfHeight, Center + Axis * Primitive.HalfHeight,
                                       Primitive.Radius, Color, 16, Thickness, false);
                }
                break;

            case ECollisionPrimitiveType::ConvexHull:
                {
                    if (i >= (int32)HullWireframes.size())
                    {
                        break;
                    }

                    const FHullWireframe& Wire = HullWireframes[i];
                    for (SIZE_T e = 0; e + 1 < Wire.Edges.size(); e += 2)
                    {
                        const FVector3 A = FVector3(Matrix * FVector4(Wire.Vertices[Wire.Edges[e]], 1.0f));
                        const FVector3 B = FVector3(Matrix * FVector4(Wire.Vertices[Wire.Edges[e + 1]], 1.0f));
                        World->DrawLine(A, B, Color, Thickness, false);
                    }
                }
                break;
            }
        }
    }

    int32 FCollisionShapeEditorTool::AddPrimitive(ECollisionPrimitiveType Type)
    {
        BeginAssetTransaction("Add Collision Shape");

        CCollisionShape* Shape = GetAsset<CCollisionShape>();

        SCollisionPrimitive& Primitive = Shape->Primitives.emplace_back();
        Primitive.Type = Type;

        // A hand-added shape replaces a baked triangle mesh; the two are alternatives, not a stack.
        Shape->bUseTriangleMesh = false;

        SelectedPrimitive = (int32)Shape->Primitives.size() - 1;

        RebuildHullWireframes();
        NotifyAssetDataChanged();
        EndAssetTransaction();

        return SelectedPrimitive;
    }

    void FCollisionShapeEditorTool::RemovePrimitiveAt(int32 Index)
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        if (Index < 0 || Index >= (int32)Shape->Primitives.size())
        {
            return;
        }

        BeginAssetTransaction("Remove Collision Shape");

        Shape->Primitives.erase(Shape->Primitives.begin() + Index);
        SelectedPrimitive = INDEX_NONE;

        RebuildHullWireframes();
        NotifyAssetDataChanged();
        EndAssetTransaction();
    }

    void FCollisionShapeEditorTool::RunGenerator(int32 GeneratorIndex)
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        CStaticMesh* Mesh = Shape->SourceMesh.Get();

        if (Mesh == nullptr)
        {
            ImGuiX::Notifications::NotifyWarning("This collision shape has no source mesh to generate from.");
            return;
        }

        static const char* Names[] = { "Generate Single Hull", "Generate Per-Surface Hulls",
                                       "Bake Triangle Mesh", "Fit Box", "Fit Sphere" };

        BeginAssetTransaction(Names[GeneratorIndex]);

        bool bSuccess = false;
        switch (GeneratorIndex)
        {
        case 0: bSuccess = Physics::CollisionGen::GenerateSingleHull(Mesh, Shape); break;
        case 1: bSuccess = Physics::CollisionGen::GeneratePerSurfaceHulls(Mesh, Shape); break;
        case 2: bSuccess = Physics::CollisionGen::GenerateTriangleMesh(Mesh, Shape); break;
        case 3: bSuccess = Physics::CollisionGen::GenerateFittedBox(Mesh, Shape); break;
        case 4: bSuccess = Physics::CollisionGen::GenerateFittedSphere(Mesh, Shape); break;
        }

        SelectedPrimitive = INDEX_NONE;
        RebuildHullWireframes();
        NotifyAssetDataChanged();
        EndAssetTransaction();

        if (!bSuccess)
        {
            ImGuiX::Notifications::NotifyWarning("'{0}' produced no collision; the mesh may be skinned or have no LOD 0 geometry.",
                                                 Mesh->GetName());
        }
    }

    void FCollisionShapeEditorTool::SelectPrimitive(int32 Index)
    {
        SelectedPrimitive = Index;
    }

    void FCollisionShapeEditorTool::SyncDetailsTable()
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();

        void* Target = nullptr;
        if (SelectedPrimitive >= 0 && SelectedPrimitive < (int32)Shape->Primitives.size())
        {
            Target = &Shape->Primitives[SelectedPrimitive];
        }

        // Comparing the address catches a vector reallocation as well as a selection change.
        if (Target != DetailsTarget)
        {
            DetailsTarget = Target;
            if (Target != nullptr)
            {
                DetailsTable->SetObject(Target, SCollisionPrimitive::StaticStruct());
                DetailsTable->MarkDirty();
            }
        }
    }

    void FCollisionShapeEditorTool::BeginAssetTransaction(FName Name)
    {
        FTransactionManager& Manager = GetTransactionManager();
        Manager.BeginTransaction(Name);
        Manager.Record(MakeUnique<FObjectSnapshotCommand>(Asset.Get(), Name));
    }

    void FCollisionShapeEditorTool::EndAssetTransaction()
    {
        GetTransactionManager().CommitTransaction();
    }

    void FCollisionShapeEditorTool::CloseOpenDragTransaction()
    {
        if (bHandleTransactionOpen || bGizmoTransactionOpen)
        {
            bHandleTransactionOpen = false;
            bGizmoTransactionOpen = false;
            EndAssetTransaction();
        }
    }

    void FCollisionShapeEditorTool::DrawShapeListWindow()
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();

        if (Shape->SourceMesh.IsValid())
        {
            if (ImGui::Button(LE_ICON_AUTO_FIX " Single Hull"))   { RunGenerator(0); }
            ImGuiX::TextTooltip("One hull around the whole mesh. Always dynamic-safe, but fills in concavities.");

            ImGui::SameLine();
            if (ImGui::Button("Per-Surface Hulls"))               { RunGenerator(1); }
            ImGuiX::TextTooltip("One hull per geometry surface. Uses the split already authored in the mesh.");

            ImGui::SameLine();
            if (ImGui::Button("Triangle Mesh"))                   { RunGenerator(2); }
            ImGuiX::TextTooltip("Exact concave geometry. Static and kinematic bodies only.");

            if (ImGui::Button("Fit Box"))                         { RunGenerator(3); }
            ImGui::SameLine();
            if (ImGui::Button("Fit Sphere"))                      { RunGenerator(4); }
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), LE_ICON_ALERT " No source mesh.");
            ImGui::TextWrapped("Create collision shapes from a mesh's right-click menu so they are bound to it.");
        }

        ImGui::Separator();

        if (ImGui::Button(LE_ICON_PLUS " Box"))     { AddPrimitive(ECollisionPrimitiveType::Box); }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Sphere"))  { AddPrimitive(ECollisionPrimitiveType::Sphere); }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Capsule")) { AddPrimitive(ECollisionPrimitiveType::Capsule); }

        ImGui::Separator();

        if (Shape->IsConcave())
        {
            ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.6f, 1.0f), LE_ICON_VECTOR_POLYLINE " Triangle Mesh");
            ImGui::TextDisabled("%d triangles", Shape->NumTriangles());
            ImGui::TextWrapped("Static and kinematic bodies only. Adding a primitive replaces it.");
            return;
        }

        ImGui::TextDisabled("%d shapes, %d hull points", (int)Shape->Primitives.size(), Shape->NumHullPoints());
        ImGui::Spacing();

        int32 PendingRemoval = INDEX_NONE;

        for (int32 i = 0; i < (int32)Shape->Primitives.size(); ++i)
        {
            const SCollisionPrimitive& Primitive = Shape->Primitives[i];

            ImGui::PushID(i);

            char Label[96];
            if (Primitive.Type == ECollisionPrimitiveType::ConvexHull)
            {
                snprintf(Label, sizeof(Label), "%s  %s (%d pts)", PrimitiveIcon(Primitive.Type),
                    PrimitiveTypeName(Primitive.Type), (int)Primitive.HullPoints.size());
            }
            else
            {
                snprintf(Label, sizeof(Label), "%s  %s", PrimitiveIcon(Primitive.Type), PrimitiveTypeName(Primitive.Type));
            }

            if (ImGui::Selectable(Label, i == SelectedPrimitive))
            {
                SelectPrimitive(i);
            }

            if (ImGui::BeginPopupContextItem("##ShapeContext"))
            {
                if (ImGui::MenuItem(LE_ICON_DELETE " Remove"))
                {
                    PendingRemoval = i;
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        // Deferred, since erasing mid-iteration invalidates the loop and every row after it.
        if (PendingRemoval != INDEX_NONE)
        {
            RemovePrimitiveAt(PendingRemoval);
        }
    }

    void FCollisionShapeEditorTool::DrawDetailsWindow()
    {
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Shape");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        if (DetailsTarget != nullptr)
        {
            DetailsTable->DrawTree();
        }
        else
        {
            ImGui::TextDisabled("Select a shape in the list or the viewport.");
        }

        ImGui::Spacing();
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Collision Shape");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        PropertyTable.DrawTree();
    }

    bool FCollisionShapeEditorTool::BuildViewportRay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                                     const ImVec2& ScreenPos, FVector3& OutOrigin, FVector3& OutDirection)
    {
        SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera == nullptr)
        {
            return false;
        }

        const float LocalX = ScreenPos.x - ViewportOrigin.x;
        const float LocalY = ScreenPos.y - ViewportOrigin.y;
        if (LocalX < 0.0f || LocalY < 0.0f || LocalX >= ViewportSize.x || LocalY >= ViewportSize.y)
        {
            return false;
        }

        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;
        const FMatrix4 InverseViewProjection = Math::Inverse(Projection * Camera->GetViewMatrix());

        const float NdcX = (LocalX / ViewportSize.x) * 2.0f - 1.0f;
        const float NdcY = 1.0f - (LocalY / ViewportSize.y) * 2.0f;

        const FVector4 FarPoint = InverseViewProjection * FVector4(NdcX, NdcY, 1.0f, 1.0f);
        if (Math::Abs(FarPoint.w) < 1e-6f)
        {
            return false;
        }

        OutOrigin = Camera->GetPosition();
        OutDirection = Math::Normalize(FVector3(FarPoint) / FarPoint.w - OutOrigin);
        return true;
    }

    bool FCollisionShapeEditorTool::ProjectToScreen(const FMatrix4& ViewProj, const ImVec2& ViewportOrigin,
                                                    const ImVec2& ViewportSize, const FVector3& WorldPosition, ImVec2& OutScreen)
    {
        const FVector4 Clip = ViewProj * FVector4(WorldPosition, 1.0f);
        if (Clip.w <= 1e-6f)
        {
            return false;
        }

        const FVector3 Ndc = FVector3(Clip) / Clip.w;
        OutScreen = ImVec2(ViewportOrigin.x + (Ndc.x * 0.5f + 0.5f) * ViewportSize.x,
                           ViewportOrigin.y + (0.5f - Ndc.y * 0.5f) * ViewportSize.y);
        return true;
    }

    int32 FCollisionShapeEditorTool::PickPrimitive(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();

        int32 Best = INDEX_NONE;
        float BestDistance = 1e30f;

        for (int32 i = 0; i < (int32)Shape->Primitives.size(); ++i)
        {
            const SCollisionPrimitive& Primitive = Shape->Primitives[i];
            const FMatrix4 Matrix = GetPrimitiveMatrix(Primitive);

            float Distance = 0.0f;
            bool bHit = false;

            switch (Primitive.Type)
            {
            case ECollisionPrimitiveType::Sphere:
                bHit = RayHitsSphere(RayOrigin, RayDirection, Primitive.Center, Primitive.Radius, Distance);
                break;

            case ECollisionPrimitiveType::Capsule:
                // Approximated by its bounding box, which is exact enough for the only user, picking.
                bHit = RayHitsBox(RayOrigin, RayDirection, Matrix,
                    FVector3(Primitive.Radius, Primitive.HalfHeight + Primitive.Radius, Primitive.Radius), Distance);
                break;

            case ECollisionPrimitiveType::ConvexHull:
                {
                    if (i >= (int32)HullWireframes.size() || HullWireframes[i].Vertices.empty())
                    {
                        break;
                    }

                    FVector3 Min = HullWireframes[i].Vertices[0];
                    FVector3 Max = Min;
                    for (const FVector3& P : HullWireframes[i].Vertices)
                    {
                        Min = FVector3(Math::Min(Min.x, P.x), Math::Min(Min.y, P.y), Math::Min(Min.z, P.z));
                        Max = FVector3(Math::Max(Max.x, P.x), Math::Max(Max.y, P.y), Math::Max(Max.z, P.z));
                    }

                    const FMatrix4 HullMatrix = Matrix * Math::Translate(FMatrix4(1.0f), (Min + Max) * 0.5f);
                    bHit = RayHitsBox(RayOrigin, RayDirection, HullMatrix, (Max - Min) * 0.5f, Distance);
                }
                break;

            default:
                bHit = RayHitsBox(RayOrigin, RayDirection, Matrix, Primitive.HalfExtent, Distance);
                break;
            }

            if (bHit && Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = i;
            }
        }

        return Best;
    }

    void FCollisionShapeEditorTool::GatherHandles(TVector<FCollisionHandle>& OutHandles)
    {
        OutHandles.clear();

        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        if (SelectedPrimitive < 0 || SelectedPrimitive >= (int32)Shape->Primitives.size())
        {
            return;
        }

        const SCollisionPrimitive& Primitive = Shape->Primitives[SelectedPrimitive];
        const FMatrix4 Matrix = GetPrimitiveMatrix(Primitive);

        const FVector3 Center = Primitive.Center;
        const FVector3 AxisX = Math::Normalize(FVector3(Matrix[0]));
        const FVector3 AxisY = Math::Normalize(FVector3(Matrix[1]));
        const FVector3 AxisZ = Math::Normalize(FVector3(Matrix[2]));

        switch (Primitive.Type)
        {
        case ECollisionPrimitiveType::Sphere:
            OutHandles.push_back({ ECollisionHandle::Radius, Center + AxisX * Primitive.Radius, AxisX, Center });
            break;

        case ECollisionPrimitiveType::Capsule:
            OutHandles.push_back({ ECollisionHandle::Radius,     Center + AxisX * Primitive.Radius,     AxisX, Center });
            OutHandles.push_back({ ECollisionHandle::HalfHeight, Center + AxisY * Primitive.HalfHeight, AxisY, Center });
            break;

        case ECollisionPrimitiveType::Box:
            OutHandles.push_back({ ECollisionHandle::ExtentX, Center + AxisX * Primitive.HalfExtent.x, AxisX, Center });
            OutHandles.push_back({ ECollisionHandle::ExtentY, Center + AxisY * Primitive.HalfExtent.y, AxisY, Center });
            OutHandles.push_back({ ECollisionHandle::ExtentZ, Center + AxisZ * Primitive.HalfExtent.z, AxisZ, Center });
            break;

        default:
            // A hull's shape is baked geometry; there is no dimension to drag.
            break;
        }
    }

    void FCollisionShapeEditorTool::ApplyHandleDrag(const FCollisionHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        if (SelectedPrimitive < 0 || SelectedPrimitive >= (int32)Shape->Primitives.size())
        {
            return;
        }

        float AxisDistance = 0.0f;
        if (!ClosestParamOnAxisToRay(Handle.Anchor, Handle.Axis, RayOrigin, RayDirection, AxisDistance))
        {
            return;
        }

        SCollisionPrimitive& Primitive = Shape->Primitives[SelectedPrimitive];
        const float Positive = Math::Max(AxisDistance, 0.001f);

        switch (Handle.Type)
        {
        case ECollisionHandle::Radius:     Primitive.Radius = Positive; break;
        case ECollisionHandle::HalfHeight: Primitive.HalfHeight = Math::Max(AxisDistance, 0.0f); break;
        case ECollisionHandle::ExtentX:    Primitive.HalfExtent.x = Positive; break;
        case ECollisionHandle::ExtentY:    Primitive.HalfExtent.y = Positive; break;
        case ECollisionHandle::ExtentZ:    Primitive.HalfExtent.z = Positive; break;
        default: return;
        }

        NotifyAssetDataChanged();
    }

    void FCollisionShapeEditorTool::ApplyGizmo(const FMatrix4& NewMatrix)
    {
        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        if (SelectedPrimitive < 0 || SelectedPrimitive >= (int32)Shape->Primitives.size())
        {
            return;
        }

        SCollisionPrimitive& Primitive = Shape->Primitives[SelectedPrimitive];

        Primitive.Center = FVector3(NewMatrix[3]);
        Primitive.Rotation = Math::Degrees(Math::EulerAngles(Math::Normalize(Math::ToQuat(NewMatrix))));

        NotifyAssetDataChanged();
    }

    void FCollisionShapeEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        const ImVec2 ViewportOrigin = ViewportScreenMin;

        FAssetEditorTool::DrawViewportOverlayElements(UpdateContext, ViewportTexture, ViewportSize);

        SCameraComponent* Camera = World.IsValid() ? World->GetActiveCamera() : nullptr;
        if (Camera == nullptr)
        {
            ActiveHandle = ECollisionHandle::None;
            return;
        }

        if (bViewportHovered && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            GizmoOp = (GizmoOp == ImGuizmo::TRANSLATE) ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
        }

        FMatrix4 ViewMatrix = Camera->GetViewMatrix();
        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;
        const FMatrix4 ViewProj = Projection * ViewMatrix;

        CCollisionShape* Shape = GetAsset<CCollisionShape>();
        const bool bSelected = SelectedPrimitive >= 0 && SelectedPrimitive < (int32)Shape->Primitives.size();

        bool bGizmoOwnsInput = false;
        if (bSelected)
        {
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
            ImGuizmo::SetRect(ViewportOrigin.x, ViewportOrigin.y, ViewportSize.x, ViewportSize.y);

            FMatrix4 Matrix = GetPrimitiveMatrix(Shape->Primitives[SelectedPrimitive]);

            const bool bGizmoInert = ShouldSuppressViewportClickInput() && !ImGuizmo::IsUsing();
            if (bGizmoInert)
            {
                ImGuizmo::Enable(false);
            }

            ImGuizmo::Manipulate(Math::ValuePtr(ViewMatrix), Math::ValuePtr(Projection),
                GizmoOp, ImGuizmo::LOCAL, Math::ValuePtr(Matrix));

            if (bGizmoInert)
            {
                ImGuizmo::Enable(true);
            }

            if (ImGuizmo::IsUsing())
            {
                if (!bGizmoTransactionOpen)
                {
                    BeginAssetTransaction(GizmoOp == ImGuizmo::ROTATE ? "Rotate Collision Shape" : "Move Collision Shape");
                    bGizmoTransactionOpen = true;
                }

                ApplyGizmo(Matrix);
            }
            else if (bGizmoTransactionOpen)
            {
                bGizmoTransactionOpen = false;
                EndAssetTransaction();
            }

            bGizmoOwnsInput = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
        }

        if (bGizmoOwnsInput && ActiveHandle == ECollisionHandle::None)
        {
            return;
        }

        TVector<FCollisionHandle> Handles;
        GatherHandles(Handles);

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 MousePos = ImGui::GetMousePos();

        constexpr float HandleRadius = 6.0f;
        constexpr float HandleGrabRadius = 10.0f;

        int32 HoveredHandle = INDEX_NONE;

        for (int32 i = 0; i < (int32)Handles.size(); ++i)
        {
            ImVec2 Screen;
            if (!ProjectToScreen(ViewProj, ViewportOrigin, ViewportSize, Handles[i].Position, Screen))
            {
                continue;
            }

            const float DX = MousePos.x - Screen.x;
            const float DY = MousePos.y - Screen.y;
            const bool bHot = (ActiveHandle == Handles[i].Type)
                           || (ActiveHandle == ECollisionHandle::None && (DX * DX + DY * DY) <= HandleGrabRadius * HandleGrabRadius);

            if (bHot && ActiveHandle == ECollisionHandle::None)
            {
                HoveredHandle = i;
            }

            DrawList->AddCircleFilled(Screen, bHot ? HandleRadius + 1.5f : HandleRadius,
                bHot ? IM_COL32(255, 200, 60, 255) : IM_COL32(90, 180, 255, 235));
            DrawList->AddCircle(Screen, bHot ? HandleRadius + 1.5f : HandleRadius, IM_COL32(15, 15, 20, 220), 0, 1.5f);
        }

        const bool bCanInteract = bViewportHovered && !ShouldSuppressViewportClickInput();

        if (ActiveHandle != ECollisionHandle::None)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ActiveHandle = ECollisionHandle::None;
                if (bHandleTransactionOpen)
                {
                    bHandleTransactionOpen = false;
                    EndAssetTransaction();
                }
            }
            else
            {
                for (const FCollisionHandle& Handle : Handles)
                {
                    if (Handle.Type != ActiveHandle)
                    {
                        continue;
                    }

                    FVector3 RayOrigin, RayDirection;
                    if (BuildViewportRay(ViewportOrigin, ViewportSize, MousePos, RayOrigin, RayDirection))
                    {
                        ApplyHandleDrag(Handle, RayOrigin, RayDirection);
                    }
                    break;
                }
            }
            return;
        }

        if (!bCanInteract)
        {
            return;
        }

        if (HoveredHandle != INDEX_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ActiveHandle = Handles[HoveredHandle].Type;
            BeginAssetTransaction("Resize Collision Shape");
            bHandleTransactionOpen = true;
            return;
        }

        // Picks on release with a drag threshold, so a click that became a camera move does not reselect.
        if (HoveredHandle == INDEX_NONE && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const ImVec2 Drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (Drag.x * Drag.x + Drag.y * Drag.y <= 16.0f)
            {
                FVector3 RayOrigin, RayDirection;
                if (BuildViewportRay(ViewportOrigin, ViewportSize, MousePos, RayOrigin, RayDirection))
                {
                    SelectPrimitive(PickPrimitive(RayOrigin, RayDirection));
                }
            }
        }
    }

    void FCollisionShapeEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_CUBE_OUTLINE " Collision"))
        {
            bool bShapes = bDrawShapes;
            if (ImGui::MenuItem("Draw Shapes", nullptr, &bShapes))
            {
                bDrawShapes = bShapes;
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Move Shape", "Space", GizmoOp == ImGuizmo::TRANSLATE))
            {
                GizmoOp = ImGuizmo::TRANSLATE;
            }
            if (ImGui::MenuItem("Rotate Shape", "Space", GizmoOp == ImGuizmo::ROTATE))
            {
                GizmoOp = ImGuizmo::ROTATE;
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_REFRESH " Rebuild Hull Wireframes"))
            {
                RebuildHullWireframes();
            }

            ImGui::EndMenu();
        }
    }

    void FCollisionShapeEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Shapes",
            "Collision is a set of primitives and convex hulls drawn over the source mesh. Click one in the "
            "viewport or the list to select it; the dots resize it and the gizmo moves its frame.");
        DrawHelpTextRow("Generating",
            "Single Hull is the safe default: always convex, so it works on dynamic bodies, but it fills in "
            "concavities. Per-Surface Hulls splits on the mesh's own surfaces, which handles a table's legs.");
        DrawHelpTextRow("Triangle Mesh",
            "Exact concave collision, restricted by the physics engine to static and kinematic bodies. It "
            "replaces the primitives rather than adding to them, so adding a primitive clears it.");
        DrawHelpTextRow("Source Mesh",
            "Fixed when the asset is created from a mesh's right-click menu. Hulls baked against one mesh "
            "are meaningless on another, so make a new asset rather than repointing this one.");
    }

    void FCollisionShapeEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftID = 0, CenterID = 0, RightID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.26f, &RightID, &CenterID);
        ImGui::DockBuilderSplitNode(CenterID, ImGuiDir_Left, 0.24f, &LeftID, &CenterID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), CenterID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(CollisionShapeListWindowName).c_str(), LeftID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(CollisionShapeDetailsWindowName).c_str(), RightID);
    }
}
