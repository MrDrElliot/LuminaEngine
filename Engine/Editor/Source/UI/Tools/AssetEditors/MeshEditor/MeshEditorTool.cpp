#include "MeshEditorTool.h"

#include "ImGuiDrawUtils.h"
#include "Core/Object/Cast.h"
#include "Core/Math/Math.h"
#include "Tools/Import/ImportHelpers.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    // Bounded by Meshlets.size() so a malformed asset can't overrun during UI rendering.
    static uint32 SumTrianglesInRange(const TVector<FMeshlet>& Meshlets, uint32 Offset, uint32 Count)
    {
        uint32 Tris = 0;
        const uint32 End = Math::Min(Offset + Count, (uint32)Meshlets.size());
        for (uint32 m = Offset; m < End; ++m)
        {
            Tris += Meshlets[m].TriangleCount;
        }
        return Tris;
    }

    FStaticMeshEditorTool::FStaticMeshEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FStaticMeshEditorTool::OnInitialize()
    {
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
    
            FMeshResource& Resource = StaticMesh->GetMeshResource();
            const FAABB& BoundingBox = StaticMesh->GetAABB();
            
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Mesh Statistics");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
            
            if (ImGui::BeginTable("##MeshStats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
    
                auto PropertyRow = [](const char* label, const FString& value, const ImVec4* color = nullptr)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    if (color)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, *color);
                    }
                    ImGui::TextUnformatted(value.c_str());
                    if (color)
                    {
                        ImGui::PopStyleColor();
                    }
                };
    
                PropertyRow("Vertices", Format("{}", Resource.GetNumVertices()));
                PropertyRow("Meshlets", Format("{}", Resource.MeshletData.Meshlets.size()));
                PropertyRow("Surfaces", Format("{}", Resource.GetNumSurfaces()));

                // Maximum LOD count across surfaces.
                uint32 MaxLODsAcrossSurfaces = 0;
                for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
                {
                    MaxLODsAcrossSurfaces = Math::Max(MaxLODsAcrossSurfaces, Surface.NumLODs);
                }
                PropertyRow("LOD Levels", Format("{}", MaxLODsAcrossSurfaces));
                
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 4));
                
                const float vertexSizeKB     = (Resource.GetNumVertices() * Resource.GetVertexTypeSize()) / 1024.0f;
                const float meshletSizeKB    = (Resource.MeshletData.Meshlets.size() * sizeof(FMeshlet)
                                              + Resource.MeshletData.MeshletSpheres.size() * sizeof(FMeshletSphere)
                                              + Resource.MeshletData.MeshletCones.size() * sizeof(FMeshletCone)
                                              + Resource.MeshletData.MeshletVertices.size() * sizeof(uint32)
                                              + Resource.MeshletData.MeshletTriangles.size() * sizeof(uint32)) / 1024.0f;
                const float totalSizeKB      = vertexSizeKB + meshletSizeKB;

                PropertyRow("Vertex Buffer", Format("{}", static_cast<int>(vertexSizeKB)) + " KB");
                PropertyRow("Meshlet Data",  Format("{}", static_cast<int>(meshletSizeKB)) + " KB");

                ImVec4 totalColor = totalSizeKB > 1024 ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f) : ImVec4(0.7f, 1.0f, 0.7f, 1.0f);
                PropertyRow("Total Memory", Format("{}", static_cast<int>(totalSizeKB)) + " KB", &totalColor);
                
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 4));
                
                PropertyRow("Bounds Min", Math::ToString(BoundingBox.Min).c_str());
                PropertyRow("Bounds Max", Math::ToString(BoundingBox.Max).c_str());
                
                FVector3 extents = BoundingBox.Max - BoundingBox.Min;
                PropertyRow("Bounds Extents", Math::ToString(extents).c_str());
    
                ImGui::EndTable();
            }
    
            ImGui::Spacing();
            ImGui::SeparatorText("Meshlets");
            ImGui::Spacing();

            const TVector<FMeshlet>&       Meshlets = Resource.MeshletData.Meshlets;
            const TVector<FMeshletSphere>& Bounds   = Resource.MeshletData.MeshletSpheres;
            ImGui::Text("Total Meshlets: %zu", Meshlets.size());
            ImGui::Spacing();

            if (ImGui::BeginTable("##Meshlets", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                ImVec2(0, 300)))
            {
                ImGui::TableSetupColumn("Index",     ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Verts",     ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Tris",      ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Bounds (center, radius)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                ImGuiListClipper Clipper;
                Clipper.Begin((int)Meshlets.size());
                while (Clipper.Step())
                {
                    for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                    {
                        const FMeshlet& M = Meshlets[i];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", M.VertexCount);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%u", M.TriangleCount);
                        ImGui::TableSetColumnIndex(3);
                        if (i < (int)Bounds.size())
                        {
                            const FMeshletSphere& B = Bounds[i];
                            ImGui::Text("(%.2f, %.2f, %.2f)  r=%.2f", B.Center.x, B.Center.y, B.Center.z, B.Radius);
                        }
                    }
                }
                ImGui::EndTable();
            }
            
            ImGui::Spacing();
            ImGui::Spacing();

            // LOD override is pushed onto SStaticMeshComponent each frame in Update(); thresholds persist on FGeometrySurface.
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Levels of Detail");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            {
                // Listing all MAX_MESH_LODS made a mesh whose simplifier stopped early look broken.
                uint32 AvailableLODs = 1;
                for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
                {
                    AvailableLODs = Math::Max(AvailableLODs, Surface.NumLODs);
                }
                AvailableLODs = Math::Clamp(AvailableLODs, 1u, (uint32)MAX_MESH_LODS);

                // Names built at runtime; no parallel string list needed for MAX_MESH_LODS tracking.
                char        LODNames[MAX_MESH_LODS][24];
                const char* PreviewItems[MAX_MESH_LODS + 1];
                PreviewItems[0] = "Automatic (distance)";
                for (uint32 i = 0; i < AvailableLODs; ++i)
                {
                    if (i == 0)
                    {
                        snprintf(LODNames[i], sizeof(LODNames[i]), "LOD %u (full detail)", i);
                    }
                    else
                    {
                        snprintf(LODNames[i], sizeof(LODNames[i]), "LOD %u", i);
                    }
                    PreviewItems[i + 1] = LODNames[i];
                }

                // A stale selection from a mesh with more LODs would otherwise index past the list.
                PreviewLODIndex = Math::Min(PreviewLODIndex, (int32)AvailableLODs - 1);

                int PreviewItem = (PreviewLODIndex < 0) ? 0 : PreviewLODIndex + 1;
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Preview LOD", &PreviewItem, PreviewItems, (int)AvailableLODs + 1))
                {
                    PreviewLODIndex = (PreviewItem == 0) ? -1 : PreviewItem - 1;
                }
                ImGuiX::TextTooltip("Force the viewport mesh to a specific LOD regardless of camera distance.");

                if (AvailableLODs == 1)
                {
                    ImGui::TextDisabled("This mesh has one LOD, so there is nothing to switch between.");
                }
            }

            ImGui::Spacing();

            // Per-LOD aggregate stats across surfaces.
            uint32 LODAggMeshlets[MAX_MESH_LODS]  = { 0 };
            uint32 LODAggTriangles[MAX_MESH_LODS] = { 0 };
            uint32 LODAggSurfaces[MAX_MESH_LODS]  = { 0 };
            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                for (uint32 lod = 0; lod < Surface.NumLODs; ++lod)
                {
                    const uint32 MCnt = Surface.LODMeshletCount[lod];
                    LODAggMeshlets[lod]  += MCnt;
                    LODAggSurfaces[lod]  += (MCnt > 0) ? 1u : 0u;
                    LODAggTriangles[lod] += SumTrianglesInRange(Resource.MeshletData.Meshlets, Surface.LODMeshletOffset[lod], MCnt);
                }
            }

            const uint32 LOD0Tris = LODAggTriangles[0];

            if (ImGui::BeginTable("##LODSummary", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("LOD",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Surfaces",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Meshlets",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("vs LOD 0",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (uint32 lod = 0; lod < MAX_MESH_LODS; ++lod)
                {
                    if (LODAggSurfaces[lod] == 0)
                    {
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%u", lod);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%u", LODAggSurfaces[lod]);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%u", LODAggMeshlets[lod]);
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%u", LODAggTriangles[lod]);
                    ImGui::TableSetColumnIndex(4);
                    if (LOD0Tris > 0)
                    {
                        const float Ratio = (float)LODAggTriangles[lod] / (float)LOD0Tris;
                        ImGui::Text("%.1f%%", Ratio * 100.0f);
                    }
                    else
                    {
                        ImGui::TextDisabled("--");
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Distance thresholds (distance / radius). The renderer picks the highest LOD whose threshold the instance has exceeded.");
            ImGui::Spacing();

            // Shared threshold editor writes to every surface in lockstep; per-surface overrides still work below.
            if (!Resource.GeometrySurfaces.empty())
            {
                // A surface with fewer LODs leaves FLT_MAX in its high slots, so read from one that has that LOD.
                float SharedThresholds[MAX_MESH_LODS];
                for (uint32 i = 0; i < MAX_MESH_LODS; ++i)
                {
                    SharedThresholds[i] = (i == 0) ? 0.0f : FLT_MAX;
                    for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
                    {
                        if (i < Surface.NumLODs)
                        {
                            SharedThresholds[i] = Surface.LODScreenThreshold[i];
                            break;
                        }
                    }
                }

                bool bThresholdChanged = false;
                const float ResetColW = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;

                if (ImGui::BeginTable("##LODThresholds", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("LOD",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("##Reset",   ImGuiTableColumnFlags_WidthFixed, ResetColW);
                    ImGui::TableHeadersRow();

                    for (uint32 lod = 0; lod < MAX_MESH_LODS; ++lod)
                    {
                        if (LODAggSurfaces[lod] == 0)
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%u", lod);
                        ImGui::TableSetColumnIndex(1);

                        if (lod == 0)
                        {
                            ImGui::TextDisabled("0  (always active)");
                            continue;
                        }

                        ImGui::PushID((int)lod);
                        ImGui::SetNextItemWidth(-FLT_MIN);

                        float Value = SharedThresholds[lod];
                        // Clamp min to previous threshold + epsilon; monotonic ramp required for first-miss-wins picker.
                        const float MinValue = SharedThresholds[lod - 1] + 0.01f;
                        if (ImGui::DragFloat("##Threshold", &Value, 0.5f, MinValue, 1024.0f, "%.2f"))
                        {
                            SharedThresholds[lod] = Math::Max(Value, MinValue);
                            bThresholdChanged = true;
                        }

                        // Clamped against the level below, since the picker stops at the first non-monotonic miss.
                        ImGui::TableSetColumnIndex(2);
                        const float DefaultValue = Import::Mesh::GetDefaultLODScreenThreshold(lod);
                        ImGui::BeginDisabled(SharedThresholds[lod] == DefaultValue);
                        if (ImGui::SmallButton(LE_ICON_REFRESH "##ResetLOD"))
                        {
                            SharedThresholds[lod] = Math::Max(DefaultValue, MinValue);
                            bThresholdChanged = true;
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::SetTooltip("Reset to default (%.2f)", DefaultValue);
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                if (ImGui::Button(LE_ICON_REFRESH " Reset All Thresholds"))
                {
                    // Ascending walk so each level clamps against the one already restored below it.
                    for (uint32 lod = 1; lod < MAX_MESH_LODS; ++lod)
                    {
                        SharedThresholds[lod] = Math::Max(Import::Mesh::GetDefaultLODScreenThreshold(lod),
                                                          SharedThresholds[lod - 1] + 0.01f);
                    }
                    bThresholdChanged = true;
                }
                ImGuiX::TextTooltip("{}", "Restore every level to the ramp the importer bakes, on every surface.");

                if (bThresholdChanged)
                {
                    // Unused high slots keep their FLT_MAX sentinel, which is never read since the picker stops early.
                    for (FGeometrySurface& Surface : Resource.GeometrySurfaces)
                    {
                        for (uint32 i = 0; i < Surface.NumLODs; ++i)
                        {
                            Surface.LODScreenThreshold[i] = SharedThresholds[i];
                        }
                    }
                    // The resolve cache holds the squared copy the picker reads, so without a bump this needs a save.
                    NotifyAssetDataChanged();
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Geometry Surfaces");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
            
            if (Resource.GeometrySurfaces.empty())
            {
                ImGui::TextDisabled("No surfaces defined");
            }
            else
            {
                ImGui::TextDisabled("Click a surface to highlight its bounds in the viewport.");
                ImGui::Spacing();

                for (size_t i = 0; i < Resource.GeometrySurfaces.size(); ++i)
                {
                    const FGeometrySurface& Surface = Resource.GeometrySurfaces[i];
                    ImGui::PushID((int)i);

                    CMaterialInterface* Mat        = StaticMesh->GetMaterialAtSlot((size_t)Surface.MaterialIndex);
                    const FString       MaterialName = IsValid(Mat) ? Mat->GetName().ToString() : FString("(none)");

                    const bool   bSelected     = ((int32)i == SelectedSurfaceIndex);
                    const uint32 LOD0Meshlets  = Surface.LODMeshletCount[0];
                    FString      Label         = "Surface " + Format("{}", i)
                                               + "  |  " + MaterialName
                                               + "  |  " + Format("{}", Surface.IndexCount / 3) + " tris, "
                                               + Format("{}", LOD0Meshlets) + " meshlets";

                    if (ImGui::Selectable(Label.c_str(), bSelected, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        SelectedSurfaceIndex = bSelected ? -1 : (int32)i;
                    }

                    if (bSelected)
                    {
                        ImGui::Indent(16.0f);
                        if (ImGui::BeginTable("##SurfaceDetails", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                            ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);

                            auto DetailRow = [](const char* label, const FString& value)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", label);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(value.c_str());
                            };

                            const uint32 LOD0Off = Surface.LODMeshletOffset[0];
                            const uint32 LOD0Cnt = Surface.LODMeshletCount[0];

                            DetailRow("Material:",       MaterialName);
                            DetailRow("Material Index:", Format("{}", Surface.MaterialIndex));
                            DetailRow("Start Index:",    Format("{}", Surface.StartIndex));
                            DetailRow("Index Count:",    Format("{}", Surface.IndexCount));
                            DetailRow("Triangles:",      Format("{}", Surface.IndexCount / 3));
                            DetailRow("LOD 0 Range:",    Format("{}", LOD0Off) + " .. " + Format("{}", LOD0Off + LOD0Cnt));
                            DetailRow("LOD 0 Meshlets:", Format("{}", LOD0Cnt));
                            DetailRow("LOD Levels:",     Format("{}", Surface.NumLODs));

                            ImGui::EndTable();
                        }

                        // Per-surface threshold overrides (e.g. small bolt pops coarser earlier than large hull).
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##SurfaceLODs", 5,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::TableSetupColumn("LOD",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
                            ImGui::TableSetupColumn("Meshlets",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                            ImGui::TableSetupColumn("Range",     ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                            ImGui::TableHeadersRow();

                            FGeometrySurface&        SurfaceRW = Resource.GeometrySurfaces[i];
                            const TVector<FMeshlet>& MeshletsRef = Resource.MeshletData.Meshlets;

                            for (uint32 lod = 0; lod < SurfaceRW.NumLODs; ++lod)
                            {
                                const uint32 MOff = SurfaceRW.LODMeshletOffset[lod];
                                const uint32 MCnt = SurfaceRW.LODMeshletCount[lod];
                                const uint32 Tris = SumTrianglesInRange(MeshletsRef, MOff, MCnt);

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%u", lod);
                                ImGui::TableSetColumnIndex(1); ImGui::Text("%u", MCnt);
                                ImGui::TableSetColumnIndex(2); ImGui::Text("%u", Tris);
                                ImGui::TableSetColumnIndex(3); ImGui::Text("%u .. %u", MOff, MOff + MCnt);
                                ImGui::TableSetColumnIndex(4);

                                if (lod == 0)
                                {
                                    ImGui::TextDisabled("0");
                                }
                                else
                                {
                                    ImGui::PushID((int)lod);

                                    // Leave room for the trailing reset; -FLT_MIN would eat the whole cell.
                                    const float SurfResetW = ImGui::GetFrameHeight();
                                    const float Spacing    = ImGui::GetStyle().ItemInnerSpacing.x;
                                    ImGui::SetNextItemWidth(Math::Max(ImGui::GetContentRegionAvail().x - SurfResetW - Spacing, 1.0f));

                                    float Value = SurfaceRW.LODScreenThreshold[lod];
                                    const float MinValue = SurfaceRW.LODScreenThreshold[lod - 1] + 0.01f;
                                    if (ImGui::DragFloat("##SurfaceThreshold", &Value, 0.5f, MinValue, 1024.0f, "%.2f"))
                                    {
                                        SurfaceRW.LODScreenThreshold[lod] = Math::Max(Value, MinValue);
                                        NotifyAssetDataChanged();
                                    }

                                    ImGui::SameLine(0.0f, Spacing);
                                    const float SurfDefault = Import::Mesh::GetDefaultLODScreenThreshold(lod);
                                    ImGui::BeginDisabled(SurfaceRW.LODScreenThreshold[lod] == SurfDefault);
                                    if (ImGui::SmallButton(LE_ICON_REFRESH "##ResetSurfaceLOD"))
                                    {
                                        SurfaceRW.LODScreenThreshold[lod] = Math::Max(SurfDefault, MinValue);
                                        NotifyAssetDataChanged();
                                    }
                                    ImGui::EndDisabled();
                                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                                    {
                                        ImGui::SetTooltip("Reset this surface's LOD %u to default (%.2f)", lod, SurfDefault);
                                    }

                                    ImGui::PopID();
                                }
                            }

                            ImGui::EndTable();
                        }

                        ImGui::Unindent(16.0f);
                    }

                    ImGui::PopID();
                }

                // Asset reload may shrink the surface list; clear stale selection rather than draw garbage.
                if (SelectedSurfaceIndex >= (int32)Resource.GeometrySurfaces.size())
                {
                    SelectedSurfaceIndex = -1;
                }
            }
    
            ImGui::Spacing();
    
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Asset Details");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
            PropertyTable.DrawTree();
        });
    }

    void FStaticMeshEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();
        
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

        const FAABB Bounds = StaticMesh->GetAABB();
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
        
        // @TODO figure out why this needs to exist..
        if (World->GetRenderer())
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }

        SStaticMeshComponent& StaticMeshComponent = World->GetComponent<SStaticMeshComponent>(MeshEntity);
        STransformComponent&  Transform           = World->GetComponent<STransformComponent>(MeshEntity);

        // The renderer only rewrites primitives that report a change, and marking every frame re-resolves.
        if (StaticMeshComponent.ForcedLODIndex != PreviewLODIndex)
        {
            StaticMeshComponent.ForcedLODIndex = PreviewLODIndex;
            StaticMeshComponent.MarkRenderStateDirty();
        }

        if (bShowAABB)
        {
            FAABB AABB = StaticMeshComponent.StaticMesh->GetAABB().ToWorld(Transform.GetWorldMatrix());
            World->DrawBox(AABB.GetCenter(), AABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Green);
        }

        // Derive overlay AABB from the currently rendered LOD's meshlet range; LOD 0 bounds look stale after a forced LOD change.
        if (SelectedSurfaceIndex >= 0 && IsValid(StaticMeshComponent.StaticMesh))
        {
            const FMeshResource& Resource = StaticMeshComponent.StaticMesh->GetMeshResource();
            if (SelectedSurfaceIndex < (int32)Resource.GeometrySurfaces.size())
            {
                const FGeometrySurface& Surface = Resource.GeometrySurfaces[SelectedSurfaceIndex];
                const FMeshletData&     MD      = Resource.MeshletData;

                // Use LOD 0 for auto preview; forced preview uses selected LOD clamped to surface NumLODs.
                const uint32 OverlayLOD = (PreviewLODIndex >= 0 && Surface.NumLODs > 0)
                    ? (uint32)Math::Min((int32)Surface.NumLODs - 1, PreviewLODIndex)
                    : 0u;

                const uint32 OverlayOffset = Surface.LODMeshletOffset[OverlayLOD];
                const uint32 OverlayCount  = Surface.LODMeshletCount[OverlayLOD];

                if (OverlayCount > 0 && !MD.Meshlets.empty())
                {
                    FVector3 Lo( FLT_MAX);
                    FVector3 Hi(-FLT_MAX);
                    const uint32 End = OverlayOffset + OverlayCount;
                    for (uint32 m = OverlayOffset; m < End; ++m)
                    {
                        const FMeshletSphere& B = MD.MeshletSpheres[m];
                        const FVector3 BoxLo = B.Center - FVector3(B.Radius);
                        const FVector3 BoxHi = B.Center + FVector3(B.Radius);
                        Lo = Math::Min(Lo, BoxLo);
                        Hi = Math::Max(Hi, BoxHi);
                    }

                    FAABB SurfaceAABB;
                    SurfaceAABB.Min = Lo;
                    SurfaceAABB.Max = Hi;
                    SurfaceAABB     = SurfaceAABB.ToWorld(Transform.GetWorldMatrix());
                    World->DrawBox(SurfaceAABB.GetCenter(), SurfaceAABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Yellow, 2.0f);
                }
            }
        }

        if (IsValid(StaticMeshComponent.StaticMesh) && !StaticMeshComponent.StaticMesh->Sockets.empty())
        {
            const FMatrix4 EntityMatrix = Transform.GetWorldMatrix();
            constexpr float AxisLength = 0.18f;

            for (const FMeshSocket& Socket : StaticMeshComponent.StaticMesh->Sockets)
            {
                const FMatrix4 SocketMatrix = EntityMatrix * Socket.RelativeTransform.GetMatrix();
                const FVector3 Position = FVector3(SocketMatrix[3]);

                World->DrawSphere(Position, 0.045f, FVector4(1.0f, 0.82f, 0.4f, 1.0f), 12, 2.5f, false);
                World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 3.0f, false);
                World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 3.0f, false);
                World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 3.0f, false);
            }
        }
    }

    void FStaticMeshEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FStaticMeshEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        DrawCameraModeSelector();
    }

    void FStaticMeshEditorTool::OnAssetLoadFinished()
    {
    }

    void FStaticMeshEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();

        // The unwrap is built from the old meshlets and the indices bound arrays that just changed length.
        UVViewer.Invalidate();
        SelectedSurfaceIndex = -1;
        PreviewLODIndex = -1;
    }

    void FStaticMeshEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("LODs",
            "Each LOD is a separate index buffer over the same vertex stream. The Preview LOD picker forces "
            "a specific level so you can inspect its geometry; -1 returns to automatic distance-based selection.");
        DrawHelpTextRow("Surfaces",
            "Surfaces (sub-meshes) are the unit at which materials are assigned. Click a row to highlight "
            "its AABB in the viewport.");
        DrawHelpTextRow("Materials",
            "Material slots come from the source asset. Drag a Material or Material Instance from the "
            "Content Browser onto a slot to override.");
        DrawHelpTextRow("Visualizers",
            "Toggle wireframe, normals, tangents, AABB from the View menu. Useful for verifying imports "
            "and diagnosing lighting issues.");
        DrawHelpTextRow("Reimport",
            "If the source FBX/GLTF on disk changed, use File > Reimport to refresh, preserves material "
            "overrides where slot names match.");
    }

    void FStaticMeshEditorTool::DrawViewModeExtraItems()
    {
        ImGui::Separator();
        ImGui::MenuItem(LE_ICON_CUBE_OUTLINE " Show AABB", nullptr, &bShowAABB);

        if (ImGui::MenuItem(LE_ICON_RELOAD " Reload Mesh Buffers"))
        {
            Cast<CStaticMesh>(Asset.Get())->PostLoad();
        }
    }

    void FStaticMeshEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);
        
        // Gizmo Control Dropdown
        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo Control"))
        {
            const char* operations[] = { "Translate", "Rotate", "Scale" };
            static int currentOp = 0;

            if (ImGui::Combo("##", &currentOp, operations, IM_ARRAYSIZE(operations)))
            {
                switch (currentOp)
                {
                case 0: GuizmoOp = ImGuizmo::TRANSLATE; break;
                case 1: GuizmoOp = ImGuizmo::ROTATE;    break;
                case 2: GuizmoOp = ImGuizmo::SCALE;     break;
                }
            }

            ImGui::EndMenu();
        }

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
                Mesh->DistanceFieldSettings.bEnabled = false;
                Mesh->BuildDistanceField();
                Asset->GetPackage()->MarkDirty();
                PropertyTable.MarkDirty();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_LINK " Sockets"))
        {
            if (ImGui::MenuItem(LE_ICON_PLUS " Add Socket"))
            {
                CStaticMesh* Mesh = Cast<CStaticMesh>(Asset.Get());

                FString Base("Socket");
                FName SocketName(Base.c_str());
                int32 Suffix = 1;
                while (Mesh->FindSocket(SocketName) != nullptr)
                {
                    FString Numbered = Base;
                    Numbered += "_";
                    Numbered += Format("{}", Suffix++).c_str();
                    SocketName = FName(Numbered.c_str());
                }

                FMeshSocket& Socket = Mesh->Sockets.emplace_back();
                Socket.SocketName = SocketName;

                Asset->GetPackage()->MarkDirty();
                PropertyTable.MarkDirty();
            }
            ImGuiX::TextTooltip("{}", "Add a named attach point; edit its name and offset in Mesh Properties");

            ImGui::EndMenu();
        }
    }

    void FStaticMeshEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &rightDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        // The unwrap wants the same large canvas, and this is an inspect-one-then-the-other workflow.
        ImGui::DockBuilderDockWindow(GetToolWindowName(UVViewerName.data()).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MeshPropertiesName.data()).c_str(), rightDockID);
    }
}
