#include "SkeletalMeshEditorTool.h"

#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "assets/assettypes/mesh/skeleton/skeleton.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "world/entity/components/environmentcomponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include <UI/Tools/AssetEditors/AssetEditorTool.h>
#include <UI/Tools/EditorTool.h>
#include <Lumina.h>
#include <Containers/Array.h>
#include <Containers/String.h>
#include <Core/Math/AABB.h>
#include <Core/Math/Color.h>
#include <Core/Math/Math.h>
#include <Core/Math/Transform.h>
#include <Core/Object/Object.h>
#include <Core/Object/ObjectCore.h>
#include <Platform/GenericPlatform.h>
#include <Renderer/MeshData.h>
#include <Renderer/Vertex.h>
#include <Tools/UI/ImGui/ImGuiDesignIcons.h>
#include <World/Entity/Components/TransformComponent.h>
#include <World/World.h>
#include <EASTL/string.h>
#include "Core/Math/Math.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>


namespace Lumina
{
    FSkeletalMeshEditorTool::FSkeletalMeshEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FSkeletalMeshEditorTool::OnInitialize()
    {
        // Scrolling disabled: the UV canvas claims the wheel for zoom, and a scrollable host would fight it.
        CreateToolWindow(UVViewerName, [&](bool bFocused)
        {
            if (CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get()))
            {
                UVViewer.Draw(SkeletalMesh->GetMeshResource());
            }
        }, ImVec2(-1, -1), true);

        CreateToolWindow(MeshPropertiesName, [&](bool bFocused)
        {
            CSkeletalMesh* SkeletalMesh = CastAsserted<CSkeletalMesh>(Asset.Get());
    
            const FMeshResource& Resource = SkeletalMesh->GetMeshResource();
            const FAABB& BoundingBox = SkeletalMesh->GetAABB();
            
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
    
                // Geometry counts
                PropertyRow("Vertices", eastl::to_string(Resource.GetNumVertices()));
                PropertyRow("Meshlets", eastl::to_string(Resource.MeshletData.Meshlets.size()));
                PropertyRow("Surfaces", eastl::to_string(Resource.GetNumSurfaces()));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 4));

                // Memory usage
                const float vertexSizeKB  = (Resource.GetNumVertices() * Resource.GetVertexTypeSize()) / 1024.0f;
                const float meshletSizeKB = (Resource.MeshletData.Meshlets.size() * sizeof(FMeshlet)
                                           + Resource.MeshletData.MeshletBounds.size() * sizeof(FMeshletBounds)
                                           + Resource.MeshletData.MeshletVertices.size() * sizeof(uint32)
                                           + Resource.MeshletData.MeshletTriangles.size() * sizeof(uint32)) / 1024.0f;
                const float totalSizeKB   = vertexSizeKB + meshletSizeKB;

                PropertyRow("Vertex Buffer", eastl::to_string(static_cast<int>(vertexSizeKB)) + " KB");
                PropertyRow("Meshlet Data",  eastl::to_string(static_cast<int>(meshletSizeKB)) + " KB");
                
                ImVec4 totalColor = totalSizeKB > 1024 ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f) : ImVec4(0.7f, 1.0f, 0.7f, 1.0f);
                PropertyRow("Total Memory", eastl::to_string(static_cast<int>(totalSizeKB)) + " KB", &totalColor);
                
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 4));
                
                // Bounding box
                PropertyRow("Bounds Min", Math::ToString(BoundingBox.Min).c_str());
                PropertyRow("Bounds Max", Math::ToString(BoundingBox.Max).c_str());
                
                FVector3 extents = BoundingBox.Max - BoundingBox.Min;
                PropertyRow("Bounds Extents", Math::ToString(extents).c_str());
    
                ImGui::EndTable();
            }
    
            ImGui::Spacing();
            ImGui::Spacing();
            
                 ImGui::Spacing();
                ImGui::Spacing();
                
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
                ImGui::SeparatorText("Geometry Data");
                ImGuiX::Font::PopFont();
                
                ImGui::Spacing();
                
                if (ImGui::BeginTabBar("##GeometryTabs"))
                {
                    // Vertices Tab
                    if (ImGui::BeginTabItem("Vertices"))
                    {
                        ImGui::Text("Total Vertices: %zu", Resource.GetNumVertices());
                        ImGui::Spacing();
                        
                        if (ImGui::BeginTable("##Vertices", 7, 
                            ImGuiTableFlags_Borders | 
                            ImGuiTableFlags_RowBg | 
                            ImGuiTableFlags_ScrollY | 
                            ImGuiTableFlags_SizingFixedFit,
                            ImVec2(0, 400)))
                        {
                            ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Normal", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("UV", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Weights", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Joints", ImGuiTableColumnFlags_WidthStretch);

                            ImGui::TableHeadersRow();
                            
                            ImGuiListClipper clipper;
                            clipper.Begin(static_cast<int>(Resource.GetNumVertices()));
                            
                            while (clipper.Step())
                            {
                                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                                {
                                    const FVector3& Position = Resource.GetPositionAt(i);
                                    FVector3 Normal = UnpackNormal(Resource.GetNormalAt(i));
                                    FU16Vector2 UV = Resource.GetUVAt(i);
                                    FVector4 Color = UnpackColor(Resource.GetColorAt(i));
                                    FVector4 Weights = FVector4(Resource.GetJointWeightsAt(i)) / 255.0f;
                                    FU8Vector4 Joints = Resource.GetJointIndicesAt(i);

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%d", i);
                                    
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.3f, %.3f, %.3f", Position.x, Position.y, Position.z);
                                    
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.3f, %.3f, %.3f", Normal.x, Normal.y, Normal.z);
                                    
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.3hu, %.3hu", UV.x, UV.y);
                                    
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%f, %f, %f", Color.x, Color.y, Color.z);  
                                    
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%f, %f, %f, %f", Weights.x, Weights.y, Weights.z, Weights.w);
                                    
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%i, %i, %i, %i", Joints.x, Joints.y, Joints.z, Joints.w);
                                }
                            }
                            
                            ImGui::EndTable();
                        }
                        
                        ImGui::EndTabItem();
                    }
                    
                    if (ImGui::BeginTabItem("Indices"))
                    {
                        ImGui::Text("Total Indices: %zu (Triangles: %zu)", 
                            Resource.Indices.size(), Resource.Indices.size() / 3);
                        ImGui::Spacing();
                        
                        static bool bShowAsTriangles = true;
                        ImGui::Checkbox("Show as Triangles", &bShowAsTriangles);
                        ImGui::Spacing();
                        
                        if (bShowAsTriangles)
                        {
                            if (ImGui::BeginTable("##Triangles", 4,
                                ImGuiTableFlags_Borders | 
                                ImGuiTableFlags_RowBg | 
                                ImGuiTableFlags_ScrollY | 
                                ImGuiTableFlags_SizingFixedFit,
                                ImVec2(0, 400)))
                            {
                                ImGui::TableSetupColumn("Triangle", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableSetupColumn("Index 0", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableSetupColumn("Index 1", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableSetupColumn("Index 2", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableHeadersRow();
                                
                                const int triangleCount = static_cast<int>(Resource.Indices.size() / 3);
                                ImGuiListClipper clipper;
                                clipper.Begin(triangleCount);
                                
                                while (clipper.Step())
                                {
                                    for (int tri = clipper.DisplayStart; tri < clipper.DisplayEnd; tri++)
                                    {
                                        const int baseIdx = tri * 3;
                                        
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::Text("%d", tri);
                                        
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::Text("%u", Resource.Indices[baseIdx + 0]);
                                        
                                        ImGui::TableSetColumnIndex(2);
                                        ImGui::Text("%u", Resource.Indices[baseIdx + 1]);
                                        
                                        ImGui::TableSetColumnIndex(3);
                                        ImGui::Text("%u", Resource.Indices[baseIdx + 2]);
                                    }
                                }
                                
                                ImGui::EndTable();
                            }
                        }
                        else
                        {
                            // Show raw index list
                            if (ImGui::BeginTable("##IndicesList", 2,
                                ImGuiTableFlags_Borders | 
                                ImGuiTableFlags_RowBg | 
                                ImGuiTableFlags_ScrollY | 
                                ImGuiTableFlags_SizingFixedFit,
                                ImVec2(0, 400)))
                            {
                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableHeadersRow();
                                
                                ImGuiListClipper clipper;
                                clipper.Begin(static_cast<int>(Resource.Indices.size()));
                                
                                while (clipper.Step())
                                {
                                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                                    {
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::Text("%d", i);
                                        
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::Text("%u", Resource.Indices[i]);
                                    }
                                }
                                
                                ImGui::EndTable();
                            }
                        }
                        
                        ImGui::EndTabItem();
                    }
                    
                    ImGui::EndTabBar();
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
                for (size_t i = 0; i < Resource.GeometrySurfaces.size(); ++i)
                {
                    const FGeometrySurface& Surface = Resource.GeometrySurfaces[i];
                    ImGui::PushID(static_cast<int>(i));
                    
                    FString headerLabel = "Surface " + eastl::to_string(i) + ": " + Surface.ID.ToString();
                    if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Indent(16.0f);
                        
                        
                        float ImageWidth = 160.0f;
                        float AvailWidth = ImGui::GetContentRegionAvail().x;
                        
                        if (ImGui::BeginChild("##TableChild", ImVec2(AvailWidth - ImageWidth - 5.0f, 0), ImGuiChildFlags_AutoResizeY))
                        {
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
                            
                                DetailRow("Material Index:", eastl::to_string(Surface.MaterialIndex));
                                DetailRow("Start Index:", eastl::to_string(Surface.StartIndex));
                                DetailRow("Index Count:", eastl::to_string(Surface.IndexCount));
                                DetailRow("Triangle Count:", eastl::to_string(Surface.IndexCount / 3));
                            
                                ImGui::EndTable();
                            }
                        }
                        ImGui::EndChild();
                        
                        ImGui::SameLine();
                        
                        if (ImGui::BeginChild("OP", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                        {
                            if (CMaterialInterface* Material = SkeletalMesh->GetMaterialAtSlot(Surface.MaterialIndex))
                            {
                                ImGui::Text("Material: %s", Material->GetName().c_str());
                            }
                        }
                        ImGui::EndChild();
                        
                        
                        ImGui::Unindent(16.0f);
                    }
                    
                    ImGui::PopID();
                    
                    if (i < Resource.GeometrySurfaces.size() - 1)
                    {
                        ImGui::Spacing();
                    }
                }
            }
    
            ImGui::Spacing();
            ImGui::Spacing();
    
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Asset Details");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
            PropertyTable.DrawTree();
        });
    }

    void FSkeletalMeshEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();
        
        CreateFloorPlane();
        
        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);
        
        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        
        CameraState.Speed = 5.0f;

        MeshEntity = World->ConstructEntity("MeshEntity");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SetSkeletalMesh(SkeletalMesh);
        if (!SkeletalMesh->Skeleton.IsValid())
        {
            return;
        }

        SkeletalMesh->Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
        MeshComponent.bRenderBonesDirty = true;
        
        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);

        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetLocation() + FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(Rotation);
    }

    void FSkeletalMeshEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);
        
        if (!World.IsValid())
        {
            return;
        }
        
        if (World->GetRenderer())
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }

        if (bShowAABB)
        {
            SSkeletalMeshComponent& MeshComponent = World->GetComponent<SSkeletalMeshComponent>(MeshEntity);
            STransformComponent& Transform = World->GetComponent<STransformComponent>(MeshEntity);

            FAABB AABB = MeshComponent.GetAABB().ToWorld(Transform.GetWorldMatrix());
            
            World->DrawBox(AABB.GetCenter(), AABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Green);
        }
        
        CSkeleton* Skeleton = GetAsset<CSkeletalMesh>()->Skeleton;
        if (bShowBones && Skeleton)
        {
            STransformComponent& Transform = World->GetComponent<STransformComponent>(MeshEntity);

            FSkeletonResource* SkeletonResource = Skeleton->GetSkeletonResource();
            TVector<FMatrix4> WorldTransforms;
            WorldTransforms.resize(SkeletonResource->GetNumBones());
            FMatrix4 EntityMatrix = Transform.GetWorldMatrix();

            for (int i = 0; i < SkeletonResource->GetNumBones(); ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(i);
                if (Bone.ParentIndex == INDEX_NONE)
                {
                    WorldTransforms[i] = EntityMatrix * Bone.LocalTransform;
                }
                else
                {
                    WorldTransforms[i] = WorldTransforms[Bone.ParentIndex] * Bone.LocalTransform;
                }
            }
            
            for (int32 i = 0; i < SkeletonResource->GetNumBones(); ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(i);
        
                FVector3 BonePosition = FVector3(WorldTransforms[i][3]);
        
                World->DrawSphere(BonePosition, 0.05f, FColor::Red, 8);
        
                if (Bone.ParentIndex != -1)
                {
                    FVector3 ParentPosition = FVector3(WorldTransforms[Bone.ParentIndex][3]);
                    World->DrawLine(ParentPosition, BonePosition, FColor::Green);
                }
            }
        }
    }

    void FSkeletalMeshEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FSkeletalMeshEditorTool::OnAssetLoadFinished()
    {
    }

    void FSkeletalMeshEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();

        // The unwrap was built from meshlets that no longer exist.
        UVViewer.Invalidate();
    }

    void FSkeletalMeshEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Skeleton",
            "The mesh references its skeleton asset; toggling Show Bones overlays the skeleton hierarchy. "
            "Edit the skeleton in its own editor for socket / bone-name changes.");
        DrawHelpTextRow("Visualizers",
            "Toggle wireframe, normals, tangents, AABB, bones from the View menu. Combine with the "
            "Animation editor's preview to spot weighting issues live.");
        DrawHelpTextRow("Materials",
            "Slot mapping comes from the source asset. Drag a material onto the slot to override.");
        DrawHelpTextRow("Reimport",
            "File > Reimport refreshes from disk; preserves material overrides and skeleton mapping "
            "when slot/bone names match.");
    }

    void FSkeletalMeshEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
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
    }

    void FSkeletalMeshEditorTool::DrawViewModeExtraItems()
    {
        ImGui::Separator();
        ImGui::MenuItem(LE_ICON_CUBE_OUTLINE " Show AABB", nullptr, &bShowAABB);
        ImGui::MenuItem(LE_ICON_BONE " Show Bones", nullptr, &bShowBones);

        if (ImGui::MenuItem(LE_ICON_RELOAD " Reload Mesh Buffers"))
        {
            Asset->PostLoad();
        }
    }

    void FSkeletalMeshEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &rightDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        // Tabbed with the viewport rather than split off it: the unwrap wants the same large canvas, and
        // it is an inspect-one-then-the-other workflow, not a side-by-side one.
        ImGui::DockBuilderDockWindow(GetToolWindowName(UVViewerName.data()).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MeshPropertiesName.data()).c_str(), rightDockID);
    }
}
