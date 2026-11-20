#include "MeshEditorTool.h"

#include "Tools/UI/ImGui/EditorColors.h"

#include "Core/Object/Cast.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/World.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "Containers/StringFormat.h"
#include "FileSystem/FileSystem.h"
#include "UI/InfoTable.h"
#include "UI/EditorUI.h"
#include "LuminaEditor.h"


namespace Lumina
{
    // Bounded by Meshlets.Num() so a malformed asset can't overrun during UI rendering.
    static uint32 SumTrianglesInRange(const TVector<FMeshlet>& Meshlets, uint32 Offset, uint32 Count)
    {
        uint32 Tris = 0;
        const uint32 End = Math::Min(Offset + Count, (uint32)Meshlets.Num());
        for (uint32 m = Offset; m < End; ++m)
        {
            Tris += Meshlets[m].TriangleCount;
        }
        return Tris;
    }

    FStaticMeshEditorTool::FStaticMeshEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, *InAsset->GetName(), InAsset, NewObject<CWorld>())
    {
    }

    void FStaticMeshEditorTool::OnInitialize()
    {
        CollisionEditing = MakeUnique<FMeshCollisionEditing>(GetTransactionManager());
        CollisionEditing->SetMesh(Cast<CStaticMesh>(Asset.Get()));

        // Scrolling is off, since the UV canvas claims the wheel for zoom and a scrollable host would fight.
        CreateToolWindow(UVViewerName, [&](bool bFocused)
        {
            if (CStaticMesh* StaticMesh = Cast<CStaticMesh>(Asset.Get()))
            {
                UVViewer.Draw(StaticMesh->GetMeshResource());
            }
        }, ImVec2(-1, -1), true);

        CreateToolWindow(MeshPropertiesName, [&](bool bFocused)
        {
            CStaticMesh* StaticMesh = Cast<CStaticMesh>(Asset.Get());
            if (!StaticMesh)
            {
                return;
            }

            DrawMeshSummary(*StaticMesh);
            DrawLODSection(*StaticMesh);
            DrawSurfaceList(*StaticMesh);

            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Properties");
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
            PropertyTable.DrawTree();

            DrawAdvancedSection(*StaticMesh);
        });

        // After Details, so a layout saved before this tab existed docks it beside that window instead of floating it.
        CreateToolWindow(CollisionName, [&](bool bFocused)
        {
            DrawCollisionWindow();
        });
    }

    namespace
    {
        FString FormatCount(uint64 Value)
        {
            FString Digits = *Format("{}", Value);
            FString Out;
            const int32 Length = Digits.Len();
            for (int32 Index = 0; Index < Length; ++Index)
            {
                Out += Digits[Index];
                const int32 Remaining = Length - Index - 1;
                if (Remaining > 0 && Remaining % 3 == 0)
                {
                    Out += ',';
                }
            }
            return Out;
        }

        void SectionTitle(const char* Title)
        {
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText(Title);
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
        }

        uint32 CountSlots(const FMeshResource& Resource)
        {
            uint32 Count = 1;
            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                Count = Math::Max(Count, Surface.NumLODs);
            }
            return Math::Clamp(Count, 1u, (uint32)MAX_MESH_LODS);
        }
    }

    void FStaticMeshEditorTool::DrawMeshSummary(CStaticMesh& Mesh)
    {
        const FMeshResource& Resource = Mesh.GetMeshResource();
        const FVector3 Size = Mesh.GetAABB().Max - Mesh.GetAABB().Min;

        uint64 Triangles = 0;
        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            Triangles += SumTrianglesInRange(Resource.MeshletData.Meshlets, Surface.LODMeshletOffset[0], Surface.FinestMeshletCount(Resource.MeshletData));
        }

        ImGui::BeginChild("##MeshSummary", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::LargeBold);
        ImGui::TextUnformatted(LE_ICON_CUBE " ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(*Mesh.GetName());
        ImGuiX::Font::PopFont();

        if (Mesh.SourcePath.IsEmpty())
        {
            ImGui::TextDisabled("No source file, so it cannot be reimported.");
        }
        else
        {
            const FStringView Source(*Mesh.SourcePath, Mesh.SourcePath.Len());
            const FStringView FileName = VFS::FileName(Source);
            ImGui::TextDisabled("Imported from %.*s", FileName.Len(), FileName.GetData());
            ImGuiX::TextTooltip("{}", Mesh.SourcePath);
        }

        ImGui::Spacing();
        if (ImGui::BeginTable("##SummaryStats", 3, ImGuiTableFlags_SizingStretchSame))
        {
            const FString TriangleText = FormatCount(Triangles);
            const FString MaterialText = *Format("{}", Mesh.GetNumMaterials());
            const FString SlotText     = *Format("{}", CountSlots(Resource));

            const char* Values[] = { *TriangleText, *MaterialText, *SlotText };
            const char* Labels[] = { "Triangles", "Materials", "Hierarchy slots" };
            ImGui::TableNextRow();
            for (int32 Column = 0; Column < 3; ++Column)
            {
                ImGui::TableSetColumnIndex(Column);
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
                ImGui::TextUnformatted(Values[Column]);
                ImGuiX::Font::PopFont();
                ImGui::TextDisabled("%s", Labels[Column]);
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Size");
        ImGui::SameLine();
        ImGui::Text("%.2f x %.2f x %.2f m", Size.x, Size.y, Size.z);

        ImGui::EndChild();
    }

    void FStaticMeshEditorTool::DrawLODSection(CStaticMesh& Mesh)
    {
        const FMeshResource& Resource = Mesh.GetMeshResource();
        const FMeshletData&  MD       = Resource.MeshletData;
        const uint32         Slots    = CountSlots(Resource);

        SectionTitle("Level of Detail");
        ImGui::TextDisabled("The GPU cuts the hierarchy per meshlet, so the view mixes slots by distance.");

        uint32 SourceTriangles              = 0;
        uint32 SlotTriangles[MAX_MESH_LODS] = {};
        float  SlotError[MAX_MESH_LODS]     = {};
        float  SlotParent[MAX_MESH_LODS]    = {};
        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            SourceTriangles += SumTrianglesInRange(MD.Meshlets, Surface.LODMeshletOffset[0], Surface.FinestMeshletCount(MD));
            for (uint32 Slot = 0; Slot < Surface.NumLODs && Slot < MAX_MESH_LODS; ++Slot)
            {
                SlotTriangles[Slot] += SumTrianglesInRange(MD.Meshlets, Surface.LODMeshletOffset[Slot], Surface.LODMeshletCount[Slot]);
                SlotError[Slot]  = Math::Max(SlotError[Slot], Surface.LODError[Slot]);
                SlotParent[Slot] = Math::Max(SlotParent[Slot], Surface.LODParentError[Slot]);
            }
        }

        ImGui::Spacing();
        if (ImGui::BeginTable("##Slots", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Of source", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            for (uint32 Slot = 0; Slot < Slots; ++Slot)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", Slot);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(*FormatCount(SlotTriangles[Slot]));

                ImGui::TableSetColumnIndex(2);
                const float Ratio = SourceTriangles > 0 ? Math::Min((float)SlotTriangles[Slot] / (float)SourceTriangles, 1.0f) : 0.0f;
                const ImVec2 BarPos = ImGui::GetCursorScreenPos();
                const float BarWidth = Math::Max(ImGui::GetContentRegionAvail().x - 46.0f, 10.0f);
                const float BarHeight = ImGui::GetTextLineHeight() * 0.5f;
                const float BarY = BarPos.y + (ImGui::GetTextLineHeight() - BarHeight) * 0.5f;
                ImDrawList* DrawList = ImGui::GetWindowDrawList();
                DrawList->AddRectFilled(ImVec2(BarPos.x, BarY), ImVec2(BarPos.x + BarWidth, BarY + BarHeight), IM_COL32(255, 255, 255, 18), 3.0f);
                DrawList->AddRectFilled(ImVec2(BarPos.x, BarY), ImVec2(BarPos.x + BarWidth * Ratio, BarY + BarHeight), IM_COL32(110, 170, 240, 200), 3.0f);
                ImGui::Dummy(ImVec2(BarWidth, ImGui::GetTextLineHeight()));
                ImGui::SameLine();
                ImGui::TextDisabled("%3.0f%%", Ratio * 100.0f);

                ImGui::TableSetColumnIndex(3);
                if (SlotParent[Slot] >= kMeshletLODErrorInfinite)
                {
                    ImGui::Text("%.3g and up", SlotError[Slot]);
                }
                else
                {
                    ImGui::Text("%.3g to %.3g", SlotError[Slot], SlotParent[Slot]);
                }
            }
            ImGui::EndTable();
        }
    }

    void FStaticMeshEditorTool::DrawSurfaceList(CStaticMesh& Mesh)
    {
        const FMeshResource& Resource = Mesh.GetMeshResource();
        if (SelectedSurfaceIndex >= Resource.GeometrySurfaces.Num())
        {
            SelectedSurfaceIndex = -1;
        }

        SectionTitle("Surfaces");
        if (Resource.GeometrySurfaces.IsEmpty())
        {
            ImGui::TextDisabled("This mesh has no surfaces.");
            return;
        }

        for (int32 Index = 0; Index < Resource.GeometrySurfaces.Num(); ++Index)
        {
            const FGeometrySurface& Surface = Resource.GeometrySurfaces[Index];
            CMaterialInterface* Material = Mesh.GetMaterialAtSlot((size_t)Surface.MaterialIndex);
            const FString MaterialName = IsValid(Material) ? Material->GetName().ToString() : FString("No material");
            const uint32 Triangles = SumTrianglesInRange(Resource.MeshletData.Meshlets, Surface.LODMeshletOffset[0], Surface.FinestMeshletCount(Resource.MeshletData));
            const FString TriangleText = FormatCount(Triangles) + " tris";

            ImGui::PushID(Index);
            const bool bSelected = SelectedSurfaceIndex == Index;
            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            const float RowWidth = ImGui::GetContentRegionAvail().x;
            const float RowHeight = ImGui::GetFrameHeight() + 4.0f;
            if (ImGui::Selectable("##Surface", bSelected, ImGuiSelectableFlags_None, ImVec2(RowWidth, RowHeight)))
            {
                SelectedSurfaceIndex = bSelected ? -1 : Index;
            }
            ImGuiX::TextTooltip("{}", "Outline this surface in the viewport");

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const float TextY = RowPos.y + (RowHeight - ImGui::GetFontSize()) * 0.5f;
            char SlotText[16];
            snprintf(SlotText, sizeof(SlotText), "%d", Surface.MaterialIndex);
            const ImVec2 SlotSize = ImGui::CalcTextSize(SlotText);
            const ImVec2 Badge0(RowPos.x + 4.0f, TextY - 2.0f);
            const ImVec2 Badge1(Badge0.x + Math::Max(SlotSize.x + 12.0f, 22.0f), TextY + SlotSize.y + 2.0f);
            DrawList->AddRectFilled(Badge0, Badge1, IM_COL32(110, 170, 240, 60), 4.0f);
            DrawList->AddText(ImVec2((Badge0.x + Badge1.x - SlotSize.x) * 0.5f, TextY), IM_COL32(200, 220, 250, 255), SlotText);

            const float TrisW = ImGui::CalcTextSize(*TriangleText).x;
            DrawList->AddText(ImVec2(RowPos.x + RowWidth - TrisW - 6.0f, TextY), ImGui::GetColorU32(ImGuiCol_TextDisabled), *TriangleText);
            DrawList->PushClipRect(ImVec2(Badge1.x + 8.0f, RowPos.y), ImVec2(RowPos.x + RowWidth - TrisW - 14.0f, RowPos.y + RowHeight), true);
            DrawList->AddText(ImVec2(Badge1.x + 8.0f, TextY), ImGui::GetColorU32(ImGuiCol_Text), *MaterialName);
            DrawList->PopClipRect();
            ImGui::PopID();
        }
        ImGui::TextDisabled("Assign materials per slot in Properties below.");
    }

    void FStaticMeshEditorTool::DrawAdvancedSection(CStaticMesh& Mesh)
    {
        ImGui::Spacing();
        if (!ImGui::CollapsingHeader("Advanced"))
        {
            return;
        }

        const FMeshResource& Resource = Mesh.GetMeshResource();
        const SAABB& Bounds = Mesh.GetAABB();
        const FDistanceFieldVolume& Field = Resource.DistanceField;

        if (ImGui::BeginTable("##Advanced", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);

            InfoTable::Row("GPU memory", ImGuiX::FormatSize(Resource.MeshBuffers.GeometryBlock.Size));
            InfoTable::Row("Meshlets", FormatCount(Resource.MeshletData.Meshlets.Num()) + " across every level");
            InfoTable::Row("Bounds min", *Math::ToString(Bounds.Min));
            InfoTable::Row("Bounds max", *Math::ToString(Bounds.Max));
            if (Field.IsValid())
            {
                InfoTable::Row("Distance field", *Format("{} x {} x {}, {}", Field.Dimensions.x, Field.Dimensions.y, Field.Dimensions.z,
                                             *ImGuiX::FormatSize(Field.GetSizeBytes())));
            }
            else
            {
                InfoTable::Row("Distance field", "Not built");
            }
            ImGui::EndTable();
        }
    }

    void FStaticMeshEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();
        World->GetRenderSettings().bDrawBillboards = false;

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);
        
        CStaticMesh* StaticMesh = Cast<CStaticMesh>(Asset.Get());

        CameraState.Speed = 5.0f;

        MeshEntity = World->ConstructEntity("MeshEntity");
        World->EmplaceComponent<SStaticMeshComponent>(MeshEntity).SetStaticMesh(StaticMesh);
        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);

        float FloorY = MeshTransform.GetLocation().y + StaticMesh->GetAABB().Min.y;
        CreateFloorPlane(FloorY);

        const SAABB Bounds = StaticMesh->GetAABB();
        const FVector3 Center = MeshTransform.GetLocation() + Bounds.GetCenter();
        const float Radius = Math::Max(Math::Length(Bounds.GetSize() * 0.5f), 0.5f);
        SetOrbitTarget(Center, Radius * 3.0f);
        SetCameraMode(EEditorCameraMode::Orbit);
    }

    void FStaticMeshEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid())
        {
            return;
        }

        SStaticMeshComponent& StaticMeshComponent = World->GetComponent<SStaticMeshComponent>(MeshEntity);
        STransformComponent&  Transform           = World->GetComponent<STransformComponent>(MeshEntity);

        if (bShowAABB && IsValid(StaticMeshComponent.StaticMesh.Get()))
        {
            SAABB AABB = StaticMeshComponent.StaticMesh->GetAABB().ToWorld(Transform.GetWorldMatrix());
            World->GetDebugInterface()->DrawBox(AABB.GetCenter(), AABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Green);
        }

        // The overlay bounds the surface's source meshlets, which every coarser level stays within.
        if (SelectedSurfaceIndex >= 0 && IsValid(StaticMeshComponent.StaticMesh.Get()))
        {
            const FMeshResource& Resource = StaticMeshComponent.StaticMesh->GetMeshResource();
            if (SelectedSurfaceIndex < Resource.GeometrySurfaces.Num())
            {
                const FGeometrySurface& Surface = Resource.GeometrySurfaces[SelectedSurfaceIndex];
                const FMeshletData&     MD      = Resource.MeshletData;

                const uint32 OverlayOffset = Surface.LODMeshletOffset[0];
                const uint32 OverlayCount  = Surface.FinestMeshletCount(MD);

                if (OverlayCount > 0 && !MD.Meshlets.IsEmpty())
                {
                    FVector3 Lo( FLT_MAX);
                    FVector3 Hi(-FLT_MAX);
                    const uint32 End = Math::Min(OverlayOffset + OverlayCount, (uint32)MD.MeshletSpheres.Num());
                    for (uint32 m = OverlayOffset; m < End; ++m)
                    {
                        const FMeshletSphere& B = MD.MeshletSpheres[m];
                        const FVector3 BoxLo = B.Center - FVector3(B.Radius);
                        const FVector3 BoxHi = B.Center + FVector3(B.Radius);
                        Lo = Math::Min(Lo, BoxLo);
                        Hi = Math::Max(Hi, BoxHi);
                    }

                    SAABB SurfaceAABB;
                    SurfaceAABB.Min = Lo;
                    SurfaceAABB.Max = Hi;
                    SurfaceAABB     = SurfaceAABB.ToWorld(Transform.GetWorldMatrix());
                    World->GetDebugInterface()->DrawBox(SurfaceAABB.GetCenter(), SurfaceAABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Yellow, 2.0f);
                }
            }
        }

        CollisionEditing->Tick(World.Get());

        if (IsValid(StaticMeshComponent.StaticMesh.Get()) && !StaticMeshComponent.StaticMesh->Sockets.IsEmpty())
        {
            const FMatrix4 EntityMatrix = Transform.GetWorldMatrix();
            constexpr float AxisLength = 0.18f;

            for (const SMeshSocket& Socket : StaticMeshComponent.StaticMesh->Sockets)
            {
                const FMatrix4 SocketMatrix = EntityMatrix * Socket.RelativeTransform.GetMatrix();
                const FVector3 Position = FVector3(SocketMatrix[3]);

                World->GetDebugInterface()->DrawSphere(Position, 0.045f, FVector4(1.0f, 0.82f, 0.4f, 1.0f), 12, 2.5f, false);
                World->GetDebugInterface()->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 3.0f, false);
                World->GetDebugInterface()->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 3.0f, false);
                World->GetDebugInterface()->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 3.0f, false);
            }
        }
    }

    void FStaticMeshEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        CollisionEditing->CloseOpenDragTransaction();
    }

    void FStaticMeshEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        DrawCameraModeSelector();
        CollisionEditing->TickViewport(World.Get(), ViewportScreenMin, ViewportSize, bViewportHovered, ShouldSuppressViewportClickInput());
    }

    void FStaticMeshEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();
        CollisionEditing->OnUndoRedo();

        CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());
        if (Mesh != nullptr && Mesh->DistanceFieldSettings.bEnabled != Mesh->HasDistanceField())
        {
            Mesh->BuildDistanceField();
        }
    }

    void FStaticMeshEditorTool::DrawCollisionWindow()
    {
        CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());
        if (Mesh == nullptr)
        {
            return;
        }

        const SMeshCollision& Collision = Mesh->Collision;

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Collision");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        if (Collision.bUseTriangleMesh && Collision.TriangleIndices.IsEmpty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Warning());
            ImGui::TextWrapped(LE_ICON_ALERT " Use Triangle Mesh is on but no triangles are baked, so this mesh has no collision. "
                               "Turn it off in Settings to use the shapes below.");
            ImGui::PopStyleColor();
        }
        else if (!Collision.HasCollision())
        {
            ImGui::TextWrapped("This mesh has no collision yet. Click a shape to wrap the mesh, or drag one onto the mesh to "
                               "place it. Every entity that shows this mesh with a Mesh Collider uses it.");
        }
        else if (Collision.IsConcave())
        {
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT " Exact Mesh only collides on objects that never simulate.");
        }
        else
        {
            ImGui::TextWrapped("Every entity that shows this mesh with a Mesh Collider uses this collision. Save the mesh to keep it.");
        }
        ImGui::Spacing();

        ImGui::SeparatorText("Add Shapes");
        CollisionEditing->DrawPalette();

        ImGui::Spacing();
        ImGui::SeparatorText("Shapes");
        ImGui::Checkbox("Draw Solid", &CollisionEditing->bDrawSolid);
        CollisionEditing->DrawShapeList();

        if (CollisionEditing->HasSelection())
        {
            ImGui::Spacing();
            ImGui::SeparatorText("Selected Shape");
            ImGui::TextDisabled("Drag the dots to resize, use the gizmo to move. Space switches to rotate.");
            CollisionEditing->DrawPrimitiveDetails();
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Settings");
        CollisionEditing->DrawSettings();
    }

    void FStaticMeshEditorTool::DrawCollisionMenu()
    {
        if (!ImGui::BeginMenu(LE_ICON_CUBE_OUTLINE " Collision"))
        {
            return;
        }

        CollisionEditing->DrawViewMenuItems();
        ImGui::Separator();

        using Physics::CollisionGen::ECollisionGenerator;
        constexpr ECollisionGenerator Generators[] =
        {
            ECollisionGenerator::Box, ECollisionGenerator::Sphere, ECollisionGenerator::Capsule,
            ECollisionGenerator::SingleHull, ECollisionGenerator::PerSurfaceHulls, ECollisionGenerator::TriangleMesh,
        };

        if (ImGui::BeginMenu(LE_ICON_PLUS " Add Shape"))
        {
            for (ECollisionGenerator Generator : Generators)
            {
                if (ImGui::MenuItem(FMeshCollisionEditing::GetGeneratorLabel(Generator)))
                {
                    CollisionEditing->AddGenerated(Generator);
                }
                ImGuiX::TextTooltip("{}", FMeshCollisionEditing::GetGeneratorTooltip(Generator));
            }
            ImGui::EndMenu();
        }

        const CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());
        if (ImGui::MenuItem(LE_ICON_BROOM " Clear Collision", nullptr, false, Mesh != nullptr && Mesh->Collision.HasCollision()))
        {
            CollisionEditing->ClearAll();
        }

        ImGui::EndMenu();
    }

    void FStaticMeshEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();

        // The unwrap is built from the old meshlets and the indices bound arrays that just changed length.
        UVViewer.Invalidate();
        CollisionEditing->RebuildHullWireframes();
        SelectedSurfaceIndex = -1;
    }

    void FStaticMeshEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("LODs",
            "The cluster hierarchy the importer built, grouped into slots. The GPU picks each meshlet's level by its distance; "
            "LOD Distance Scale in Properties moves where the levels change.");
        DrawHelpTextRow("Surfaces",
            "Surfaces are the parts of the mesh that each draw with one material slot. Click a surface to outline "
            "its bounds in the viewport.");
        DrawHelpTextRow("Materials",
            "Material slots come from the source asset. Assign them under Properties > Materials, or drag a Material "
            "or Material Instance from the Content Browser onto a slot.");
        DrawHelpTextRow("Visualizers",
            "Toggle the mesh AABB from the View menu, and click a surface row to outline its bounds.");
        DrawHelpTextRow("Collision",
            "The Collision tab makes and edits this mesh's collision. Pick Box, Sphere, Capsule, Convex Hull, Hull per Part "
            "or Exact Mesh to create it, then click shapes in the viewport to move and resize them. Any entity showing "
            "this mesh with a Mesh Collider uses it. Saving the mesh saves its collision too.");
        DrawHelpTextRow("Reimport",
            "If the source file changed, right-click the mesh in the Content Browser and pick Reimport From File.");
    }

    void FStaticMeshEditorTool::DrawViewModeExtraItems()
    {
        ImGui::Separator();
        ImGui::MenuItem(LE_ICON_CUBE_OUTLINE " Show AABB", nullptr, &bShowAABB);
    }

    void FStaticMeshEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_CUBE_OUTLINE " Distance Field"))
        {
            CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());
            const FDistanceFieldVolume& Volume = Mesh->GetMeshResource().DistanceField;

            if (Volume.IsValid())
            {
                ImGuiX::Text("{0} x {1} x {2}", Volume.Dimensions.x, Volume.Dimensions.y, Volume.Dimensions.z);
                ImGuiX::Text("{0}{1}", ImGuiX::FormatSize(Volume.GetSizeBytes()), Volume.bTwoSided ? ", two-sided" : "");
                ImGuiX::Text("Band: {:.3f} units", Volume.MaxDistance);
            }
            else
            {
                ImGui::TextDisabled("No distance field baked.");
            }
            ImGui::Separator();

            // Keeping the settings on the property table means undo and the dirty flag already work on them.
            if (ImGui::MenuItem(LE_ICON_REFRESH " Build Distance Field"))
            {
                // Voxelizes the baked meshlets, so it works with no source file, and a 48 cubed field is sub-second.
                Mesh->BuildDistanceField();
                Asset->GetPackage()->MarkDirty();
                PropertyTable.MarkDirty();
            }
            ImGuiX::TextTooltip("{}", "Rebuild from the settings in Mesh Properties > Distance Field. "
                                      "Cost scales with the cube of Resolution.");

            if (Volume.IsValid() && ImGui::MenuItem(LE_ICON_DELETE " Clear Distance Field"))
            {
                BeginAssetTransaction("Clear Distance Field");
                Mesh->DistanceFieldSettings.bEnabled = false;
                Mesh->BuildDistanceField();
                EndAssetTransaction();
                Asset->GetPackage()->MarkDirty();
                PropertyTable.MarkDirty();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_CUBE_OUTLINE " Virtual Geometry"))
        {
            CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());
            const FMeshResource& Resource = Mesh->GetMeshResource();

            ImGuiX::Text("Cluster hierarchy, {0} slots, {1} meshlets", CountSlots(Resource), Resource.MeshletData.Meshlets.Num());
            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_REFRESH " Rebuild Virtual Geometry"))
            {
                // Rebuilt from the baked finest level, so it needs no source file.
                if (Mesh->RebuildVirtualGeometry())
                {
                    Asset->GetPackage()->MarkDirty();
                    PropertyTable.MarkDirty();
                }
            }
            ImGuiX::TextTooltip("{}", "Rebuild the meshlets and cluster hierarchy from the baked finest level.");

            if (ImGui::MenuItem(LE_ICON_CUBE_OUTLINE " Bake Impostor") && Global::GEditorEngine != nullptr)
            {
                static_cast<FEditorUI*>(Global::GEditorEngine->GetDevelopmentToolsUI())->GetImpostorBaker().Enqueue(Mesh);
            }
            ImGuiX::TextTooltip("{}", Mesh->HasImpostor() ? "Rebake the views far instances draw in place of their meshlets."
                                                          : "Bake the views far instances draw in place of their meshlets.");

            ImGui::EndMenu();
        }

        DrawCollisionMenu();

        if (ImGui::BeginMenu(LE_ICON_LINK " Sockets"))
        {
            if (ImGui::MenuItem(LE_ICON_PLUS " Add Socket"))
            {
                CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());

                FString Base("Socket");
                FName SocketName(*Base);
                int32 Suffix = 1;
                while (Mesh->FindSocket(SocketName) != nullptr)
                {
                    FString Numbered = Base;
                    Numbered += "_";
                    Numbered += *Format("{}", Suffix++);
                    SocketName = FName(*Numbered);
                }

                BeginAssetTransaction("Add Socket");
                SMeshSocket& Socket = Mesh->Sockets.EmplaceGetRef();
                Socket.SocketName = SocketName;
                EndAssetTransaction();

                Asset->GetPackage()->MarkDirty();
                PropertyTable.MarkDirty();
            }
            ImGuiX::TextTooltip("{}", "Add a named attach point; edit its name and offset in Mesh Properties");

            ImGui::EndMenu();
        }
    }

    void FStaticMeshEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGuiID leftDockID = 0, rightDockID = 0;
        SplitDockingLayoutLeftRight(InDockspaceID, 0.3f, leftDockID, rightDockID);

        ImGui::DockBuilderDockWindow(*GetToolWindowName(ViewportWindowName), leftDockID);
        // The unwrap wants the same large canvas, and this is an inspect-one-then-the-other workflow.
        ImGui::DockBuilderDockWindow(*GetToolWindowName(UVViewerName.GetData()), leftDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(MeshPropertiesName.GetData()), rightDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(CollisionName.GetData()), rightDockID);
    }
}
