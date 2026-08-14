#include "MaterialInstanceEditorTool.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "UI/Properties/Customizations/CoreTypeCustomization.h"
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

    CMaterialInterface* FMaterialInstanceEditorTool::FindOverridingAncestor(CMaterialInstance* Instance, const FMaterialParameter& Param) const
    {
        for (CMaterialInterface* Level = Instance->Material.Get(); Level != nullptr; Level = Level->GetParentMaterial())
        {
            CMaterialInstance* AsInstance = Cast<CMaterialInstance>(Level);
            if (AsInstance == nullptr)
            {
                return nullptr;   // reached the root, so the value is its compiled default
            }

            if (AsInstance->IsOverrideEnabled(Param.ParameterName))
            {
                return AsInstance;
            }
        }

        return nullptr;
    }

    void FMaterialInstanceEditorTool::DrawInheritanceSection(CMaterialInstance* Instance)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Parent");
        ImGui::SameLine(120.0f);

        CMaterialInterface* Parent = Instance->Material.Get();
        FGuid ParentGUID = Parent != nullptr ? Parent->GetGUID() : FGuid();

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGuiX::AssetReferenceCombo("##ParentMaterial", CMaterialInterface::StaticClass(), ParentGUID, LE_ICON_PALETTE))
        {
            CMaterialInterface* NewParent = Cast<CMaterialInterface>(LoadObject<CObject>(ParentGUID));

            // The setter, never the property: it is what rejects a cycle and re-registers with the new parent.
            if (Instance->SetParentMaterial(NewParent))
            {
                Asset->GetPackage()->MarkDirty();
            }
            else
            {
                ImGuiX::Notifications::NotifyError("'{0}' cannot be the parent of '{1}'.",
                    NewParent != nullptr ? NewParent->GetName().c_str() : "<none>", Instance->GetName().c_str());
            }
        }

        if (Parent == nullptr)
        {
            return;
        }

        TVector<CMaterialInterface*> Chain;
        for (CMaterialInterface* Level = Instance; Level != nullptr; Level = Level->GetParentMaterial())
        {
            Chain.push_back(Level);
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Chain");
        ImGui::SameLine(120.0f);

        // Root first, the order values resolve in, so the chain reads like the override stack.
        for (size_t i = Chain.size(); i-- > 0; )
        {
            if (Chain[i] == Instance)
            {
                ImGui::TextUnformatted(Chain[i]->GetName().c_str());
            }
            else
            {
                ImGui::TextDisabled("%s", Chain[i]->GetName().c_str());
            }

            if (i > 0)
            {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled(LE_ICON_CHEVRON_RIGHT);
                ImGui::SameLine(0.0f, 4.0f);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
    }

    void FMaterialInstanceEditorTool::DrawParameterEditor(bool bFocused)
    {
        CMaterialInstance* Instance = Cast<CMaterialInstance>(Asset.Get());
        if (Instance == nullptr)
        {
            ImGui::TextUnformatted("Asset is not a material instance.");
            return;
        }

        // The asset's own properties (shading model override, etc.).
        PropertyTable.DrawTree();
        ImGui::Spacing();
        ImGui::Separator();

        DrawInheritanceSection(Instance);

        if (!Instance->Material.IsValid())
        {
            ImGui::TextUnformatted("Assign a parent material to edit parameters.");
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

                // The list belongs to the PARENT and an instance only ever diverges in values, so nothing a
                // row can do reallocates it -- no defensive copy, which here would be a heap allocation per
                // frame the panel is open.
                const TVector<FMaterialParameter>& Params = Instance->GetMaterialParams();
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

                    // Which level the shown value actually comes from, which is the whole point of a chain.
                    if (!bEnabled && ImGui::IsItemHovered())
                    {
                        const CMaterialInterface* Source = FindOverridingAncestor(Instance, Param);
                        ImGui::SetTooltip("Inherited from %s",
                            Source != nullptr ? Source->GetName().c_str() : "the base material's default");
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
        if (DisplayTexture == nullptr)
        {
            // Through the parent, not the root, so a chained instance shows the texture it inherits.
            DisplayTexture = Instance->Material->GetTextureParameterTexture(Param.ParameterName, Param.Index);
        }

        // Delegates to the engine's object picker rather than the bespoke thumbnail + drop target + browse
        // popup this used to carry: that was a second implementation of the same widget, with its own
        // asset-registry query and search box, which drifted from the standard one and had to re-solve
        // problems (disabled-state handling inside the popup) already solved there.
        if (TexturePicker == nullptr)
        {
            FProperty* TextureProp = FMaterialParameterOverride::StaticStruct()->GetProperty(FName("Texture"));
            if (TextureProp == nullptr)
            {
                return;
            }
            TexturePicker = FCObjectPropertyCustomization::MakeInstance();
            TextureHandle = MakeShared<FPropertyHandle>(&TextureScratch, TextureProp);
        }

        // Seeded before the draw: UpdateAndDraw syncs the picker from the handle first, so this is what
        // decides which texture the row displays.
        TextureScratch.Texture = DisplayTexture;

        const EPropertyChangeOp Op = TexturePicker->UpdateAndDraw(TextureHandle, !bEnabled);
        if (Op != EPropertyChangeOp::None && bEnabled)
        {
            // UpdateAndDraw leaves the write-back to its caller; commit into the scratch, then push the
            // result through the instance so the override list and uniform upload stay authoritative.
            TexturePicker->UpdatePropertyValue(TextureHandle);
            Instance->SetTextureValue(Param.ParameterName, TextureScratch.Texture.Get());
            Asset->GetPackage()->MarkDirty();
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
