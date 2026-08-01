#include "MaterialInstanceEditorTool.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Math/Math.h"
#include "Paths/Paths.h"
#include "Renderer/RenderManager.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/ContentBrowserEditorTool.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    static const char* MaterialInstanceParametersName = "Material Parameters";

    FMaterialInstanceEditorTool::FMaterialInstanceEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , MeshEntity()
        , DirectionalLightEntity()
    {
    }

    void FMaterialInstanceEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(MaterialInstanceParametersName, [this](bool bFocused)
        {
            DrawParameterEditor(bFocused);
        });
    }

    void FMaterialInstanceEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FMaterialInstanceEditorTool::SetupWorldForTool()
    {
        FAssetEditorTool::SetupWorldForTool();
        
        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        MeshEntity = World->ConstructEntity("MeshEntity");
        SStaticMeshComponent& StaticMeshComponent = World->EmplaceComponent<SStaticMeshComponent>(MeshEntity);
        StaticMeshComponent.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);

        const STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        SetOrbitTarget(MeshTransform.GetLocation(), 4.0f);
        SetCameraMode(EEditorCameraMode::Orbit);

        CMaterialInterface* Material = CastAsserted<CMaterialInterface>(Asset.Get());
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            StaticMeshComponent.MaterialOverrides.push_back(Material);
        }
        else if (Material->GetMaterialType() == EMaterialType::PostProcess)
        {
            World->GetComponent<SCameraComponent>(EditorEntity).PostProcessMaterials.push_back(Material);
        }
    }

    void FMaterialInstanceEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);
        
        // @TODO figure out why this needs to exist..
        if (World->GetRenderer())
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }
    }

    void FMaterialInstanceEditorTool::SetDebugMesh(EDebugMesh Mesh)
    {
        SStaticMeshComponent& Component = World->GetComponent<SStaticMeshComponent>(MeshEntity);
        switch (Mesh)
        {
        case EDebugMesh::Sphere:    Component.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);   break;
        case EDebugMesh::Cube:      Component.SetStaticMesh(CPrimitiveManager::Get().CubeMesh);     break;
        case EDebugMesh::Plane:     Component.SetStaticMesh(CPrimitiveManager::Get().PlaneMesh);    break;
        case EDebugMesh::Cylinder:  Component.SetStaticMesh(CPrimitiveManager::Get().CylinderMesh); break;
        case EDebugMesh::Cone:      Component.SetStaticMesh(CPrimitiveManager::Get().ConeMesh);     break;
        }
    }

    void FMaterialInstanceEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        struct FPreviewMeshEntry
        {
            const char* Label;
            EDebugMesh  Value;
        };
        static const FPreviewMeshEntry Entries[] =
        {
            { "Sphere",   EDebugMesh::Sphere   },
            { "Cube",     EDebugMesh::Cube     },
            { "Plane",    EDebugMesh::Plane    },
            { "Cylinder", EDebugMesh::Cylinder },
            { "Cone",     EDebugMesh::Cone     },
        };

        const char* PreviewString = "Sphere";
        for (const FPreviewMeshEntry& Entry : Entries)
        {
            if (Entry.Value == DebugMesh)
            {
                PreviewString = Entry.Label;
                break;
            }
        }

        ImGui::PushItemWidth(95.0f);
        if (ImGui::BeginCombo("##PreviewMesh", PreviewString, ImGuiComboFlags_HeightLarge))
        {
            for (const FPreviewMeshEntry& Entry : Entries)
            {
                if (ImGui::Selectable(Entry.Label, DebugMesh == Entry.Value))
                {
                    DebugMesh = Entry.Value;
                    SetDebugMesh(DebugMesh);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        DrawCameraModeSelector();
    }

    void FMaterialInstanceEditorTool::OnAssetLoadFinished()
    {
    }

    void FMaterialInstanceEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
    }

    void FMaterialInstanceEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What this is",
            "An instance overrides parameters on a parent material, no shader recompile, no graph editing. "
            "If you need new logic, edit the parent Material; if you only need different values, override here.");
        DrawHelpTextRow("Parameters",
            "Each row corresponds to a parameter exposed in the parent material's graph (Constant + ParameterName). "
            "Toggle the Override checkbox to capture an override; clear it to fall back to the parent's value.");
        DrawHelpTextRow("Texture Slots",
            "Drag a texture from the Content Browser onto a slot. The picker filter matches by name, useful for "
            "very large libraries.");
        DrawHelpTextRow("Preview Mesh",
            "Use the Mesh menu to swap between sphere/cube/plane/cylinder/cone for the preview viewport.");
        DrawHelpTextRow("Inheritance",
            "Changing the parent re-imports parameter defaults. Existing overrides are preserved when their "
            "name + type match.");
    }

    void FMaterialInstanceEditorTool::DrawParameterEditor(bool bFocused)
    {
        CMaterialInstance* Instance = Cast<CMaterialInstance>(Asset.Get());
        if (Instance == nullptr)
        {
            ImGui::TextUnformatted("Asset is not a material instance.");
            return;
        }

        // The asset's own properties (parent material, etc.).
        PropertyTable.DrawTree();
        ImGui::Spacing();
        ImGui::Separator();

        if (!Instance->Material.IsValid())
        {
            ImGui::TextUnformatted("Assign a parent Material to edit parameters.");
            return;
        }

        if (Instance->GetMaterialParams().empty())
        {
            ImGui::TextUnformatted("Parent material exposes no parameters.");
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);

        if (ImGui::CollapsingHeader("Parameter Overrides", ImGuiTreeNodeFlags_DefaultOpen))
        {
            constexpr ImGuiTableFlags TableFlags =
                ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_NoBordersInBodyUntilResize |
                ImGuiTableFlags_SizingFixedFit;

            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 8));

            if (ImGui::BeginTable("MaterialInstanceParamsTable", 3, TableFlags))
            {
                ImGui::TableSetupColumn("##Override", ImGuiTableColumnFlags_WidthFixed, 24);
                ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthFixed, 175);
                ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);

                // Iterate a stable copy: toggling an override mutates the instance's parameter list
                // (RemoveOverride rebuilds it), which would invalidate a live range-for mid-loop.
                const TVector<FMaterialParameter> Params = Instance->GetMaterialParams();
                for (const FMaterialParameter& Param : Params)
                {
                    ImGui::PushID(Param.ParameterName.c_str());
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    bool bEnabled = Instance->IsOverrideEnabled(Param.ParameterName);
                    if (ImGui::Checkbox("##Override", &bEnabled))
                    {
                        // The toggle only flips application; the override value is retained while disabled.
                        Instance->SetOverrideEnabled(Param.ParameterName, bEnabled);
                        Asset->GetPackage()->MarkDirty();
                    }

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (bEnabled)
                    {
                        ImGui::TextUnformatted(Param.ParameterName.c_str());
                    }
                    else
                    {
                        ImGui::TextDisabled("%s", Param.ParameterName.c_str());
                    }

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();

                    // Editor disabled while the override is off: the retained value shows but cannot be edited.
                    ImGui::BeginDisabled(!bEnabled);

                    // Show the stored override value (kept even while disabled), else the parent default.
                    const FMaterialParameterOverride* Override = Instance->FindOverride(Param.ParameterName);

                    switch (Param.Type)
                    {
                    case EMaterialParameterType::Scalar:
                    {
                        if (Param.Index >= MAX_SCALARS)
                        {
                            ImGui::TextDisabled("Invalid index");
                            break;
                        }
                        float Value = Override ? Override->Scalar : Instance->GetMaterialUniforms()->Scalars[Param.Index];
                        if (ImGui::DragFloat("##Scalar", &Value, 0.01f))
                        {
                            Instance->SetScalarValue(Param.ParameterName, Value);
                            Asset->GetPackage()->MarkDirty();
                        }
                        break;
                    }

                    case EMaterialParameterType::Vector:
                    {
                        if (Param.Index >= MAX_VECTORS)
                        {
                            ImGui::TextDisabled("Invalid index");
                            break;
                        }
                        FVector4 Value = Override ? Override->Vector : Instance->GetMaterialUniforms()->Vectors[Param.Index];
                        if (ImGui::ColorEdit4("##Vector", Math::ValuePtr(Value), ImGuiColorEditFlags_Float))
                        {
                            Instance->SetVectorValue(Param.ParameterName, Value);
                            Asset->GetPackage()->MarkDirty();
                        }
                        break;
                    }

                    case EMaterialParameterType::Texture:
                    {
                        DrawTextureParameterColumn(Instance, Param, bEnabled);
                        break;
                    }
                    }

                    ImGui::EndDisabled();
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }

        ImGui::PopStyleColor(3);
    }

    void FMaterialInstanceEditorTool::DrawTextureParameterColumn(CMaterialInstance* Instance, const FMaterialParameter& Param, bool bEnabled)
    {
        // Texture in effect for this slot: this instance's override, else the
        // parent material's default for the slot.
        CTexture* DisplayTexture = nullptr;
        for (FMaterialParameterOverride& O : Instance->Overrides)
        {
            if (O.ParameterName == Param.ParameterName && O.Type == EMaterialParameterType::Texture)
            {
                DisplayTexture = O.Texture.Get();
                break;
            }
        }
        if (DisplayTexture == nullptr && Param.Index < Instance->Material->Textures.size())
        {
            DisplayTexture = Instance->Material->Textures[Param.Index].Get();
        }

        const ImVec2 ThumbSize(64, 64);
        const ImVec2 ButtonSize(64 + ImGui::GetStyle().FramePadding.x * 2.0f,
                                64 + ImGui::GetStyle().FramePadding.y * 2.0f);

        // ImageButton gives us a real interactive item: click opens the picker, drag-drop targets work,
        // and double-click jumps to the texture asset editor.
        bool bClicked = false;
        if (DisplayTexture && DisplayTexture->GetResourceID() >= 0)
        {
            bClicked = ImGui::ImageButton("##TexThumb",
                ImGuiX::ToImTextureRef((uint32)DisplayTexture->GetResourceID()),
                ThumbSize);
        }
        else
        {
            bClicked = ImGui::Button("(none)##TexThumb", ButtonSize);
        }

        // Drag-drop target, works on the ImageButton's hit rect even while the row is BeginDisabled.
        if (ImGui::BeginDragDropTarget())
        {
            if (CTexture* DroppedTexture = DragDrop::AcceptAsset<CTexture>())
            {
                Instance->SetTextureValue(Param.ParameterName, DroppedTexture);
                Asset->GetPackage()->MarkDirty();
            }
            ImGui::EndDragDropTarget();
        }

        // Click → open searchable picker; respects the disabled state so click-to-pick is gated by override toggle.
        if (bClicked && bEnabled)
        {
            TexturePickerFilter.Clear();
            ImGui::OpenPopup("##TexturePickerPopup");
        }

        ImGui::SameLine();
        ImGui::BeginGroup();

        // Name + clear button next to the thumbnail.
        if (DisplayTexture)
        {
            ImGui::TextUnformatted(DisplayTexture->GetName().c_str());
        }
        else
        {
            ImGui::TextDisabled("None");
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
        if (ImGui::SmallButton(LE_ICON_CLOSE_CIRCLE " Clear"))
        {
            Instance->SetTextureValue(Param.ParameterName, nullptr);
            Asset->GetPackage()->MarkDirty();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::SmallButton(LE_ICON_FILE_SEARCH " Browse"))
        {
            TexturePickerFilter.Clear();
            ImGui::OpenPopup("##TexturePickerPopup");
        }

        ImGui::EndGroup();
        
        ImGui::SetNextWindowSize(ImVec2(360, 400));
        if (ImGui::BeginPopup("##TexturePickerPopup"))
        {
            // Re-enable input inside the popup; the parent table sits inside BeginDisabled.
            ImGui::EndDisabled();

            const ImGuiStyle& Style = ImGui::GetStyle();
            TexturePickerFilter.Draw("##Search", ImGui::GetContentRegionAvail().x - Style.FramePadding.x);

            if (ImGui::BeginChild("##TextureList", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None))
            {
                static const FName TextureClassName = "CTexture";

                TVector<FAssetData*> TextureAssets = FAssetRegistry::Get().FindByPredicate([](const FAssetData& Data)
                {
                    return Data.AssetClass == TextureClassName;
                });

                if (ImGui::BeginTable("##TexTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner))
                {
                    ImGui::TableSetupColumn("##Thumb", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                    ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthStretch);

                    for (FAssetData* AssetData : TextureAssets)
                    {
                        if (!TexturePickerFilter.PassFilter(AssetData->AssetName.c_str()))
                        {
                            continue;
                        }

                        ImGui::PushID(AssetData);
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, 42.0f);
                        ImGui::TableSetColumnIndex(0);

                        const bool bSelected = ImGui::Selectable("##sel", false,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 42.0f));

                        ImGuiX::TextTooltip("{}", AssetData->Path);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(AssetData->AssetName.c_str());

                        if (bSelected)
                        {
                            if (CObject* Loaded = LoadObject<CObject>(AssetData->AssetGUID))
                            {
                                if (CTexture* PickedTexture = Cast<CTexture>(Loaded))
                                {
                                    Instance->SetTextureValue(Param.ParameterName, PickedTexture);
                                    Asset->GetPackage()->MarkDirty();
                                }
                            }
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            // Restore disabled state expected by the surrounding row.
            ImGui::BeginDisabled(!bEnabled);
            ImGui::EndPopup();
        }
    }

    void FMaterialInstanceEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.4f, &rightDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MaterialInstanceParametersName).c_str(), rightDockID);
    }
}
