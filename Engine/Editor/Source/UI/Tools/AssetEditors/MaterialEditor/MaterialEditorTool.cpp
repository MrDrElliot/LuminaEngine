#include "MaterialEditorTool.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Settings/EditorSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_CustomSlang.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/RmlUiBridge.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "world/entity/components/cameracomponent.h"
#include "world/entity/components/environmentcomponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/entity/components/lightcomponent.h"
#include "World/entity/components/staticmeshcomponent.h"
#include "Log/Log.h"

namespace Lumina
{
    static const char* MaterialGraphName           = "Material Graph";
    static const char* MaterialPropertiesName      = "Material Properties";
    static const char* ShaderStatsName             = "Shader Stats";
    static const char* CustomCodeName              = "Custom Code";

    FMaterialEditorTool::FMaterialEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , MeshEntity()
        , DirectionalLightEntity()
        , CompilationResult()
        , NodeGraph(nullptr)
        , DebugMesh()
    {
    }


    void FMaterialEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();
        
        CreateToolWindow(MaterialGraphName, [&](bool bFocused)
        {
            DrawMaterialGraph();
        });

        CreateToolWindow(MaterialPropertiesName, [&](bool bFocused)
        {
            DrawMaterialProperties();
        });

        CreateToolWindow(ShaderStatsName, [&](bool bFocused)
        {
            DrawShaderStats();
        });

        CreateToolWindow(CustomCodeName, [&](bool bFocused)
        {
            DrawCustomCodeEditor();
        });

        FString GraphName = "AssetMaterialGraph";
        NodeGraph = Cast<CMaterialNodeGraph>(Asset->GetPackage()->LoadObjectByName(GraphName));
        
        if (NodeGraph == nullptr)
        {
            NodeGraph = NewObject<CMaterialNodeGraph>(Asset->GetPackage(), GraphName);
        }
        
        NodeGraph->SetMaterial(Cast<CMaterial>(Asset.Get()));
        NodeGraph->Initialize();
        NodeGraph->SetNodeSelectedCallback( [this] (CEdGraphNode* Node)
        {
            if (Node != SelectedNode)
            {
                SelectedNode = Node;

                if (SelectedNode == nullptr)
                {
                    GetPropertyTable()->SetObject(Asset, Asset->GetClass());
                }
                else
                {
                    GetPropertyTable()->SetObject(Node, Node->GetClass());
                }
            }
        });

        NodeGraph->SetPreNodeDeletedCallback([this](const CEdGraphNode* Node)
        {
            if (Node == SelectedNode)
            {
                GetPropertyTable()->SetObject(nullptr, nullptr);
            }
        });
        
        GetPropertyTable()->SetPostEditCallback([this](const FPropertyChangedEvent& Event)
        {
            if (Asset.IsValid())
            {
                Asset->GetPackage()->MarkDirty();
            }

            // A property edit (a constant's value, a texture reference, ...) changes the shader just as
            // much as rewiring does, so it counts toward "needs compile" the same way.
            if (NodeGraph != nullptr)
            {
                NodeGraph->NotifyContentChanged();
            }

            if (Event.PropertyName == FName("MaterialType"))
            {
                if (CMaterial* Material = Cast<CMaterial>(Asset.Get()))
                {
                    Material->SetReadyForRender(false);
                }
                Compile();
            }
        });
    }
    
    void FMaterialEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        if (NodeGraph)
        {
            NodeGraph->Shutdown();
            NodeGraph = nullptr;
        }
    }

    void FMaterialEditorTool::SetupWorldForTool()
    {
        FAssetEditorTool::SetupWorldForTool();
        World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        auto& Directional = World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        auto& Environment = World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        DirectionalEditor = MakeUnique<FPropertyTable>(&Directional, SDirectionalLightComponent::StaticStruct());
        EnvironmentEditor = MakeUnique<FPropertyTable>(&Environment, SEnvironmentComponent::StaticStruct());

        MeshEntity = World->ConstructEntity("MeshEntity");
        SStaticMeshComponent& StaticMeshComponent = World->EmplaceComponent<SStaticMeshComponent>(MeshEntity);
        StaticMeshComponent.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);

        const STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        SetOrbitTarget(MeshTransform.GetLocation(), 4.0f);
        SetCameraMode(EEditorCameraMode::Orbit);

        ApplyMaterialToPreview();
    }

    void FMaterialEditorTool::ApplyMaterialToPreview()
    {
        CMaterialInterface* MaterialInterface = CastAsserted<CMaterialInterface>(Asset.Get());
        const EMaterialType MaterialType = MaterialInterface ? MaterialInterface->GetMaterialType() : EMaterialType::None;

        SStaticMeshComponent& StaticMeshComponent = World->GetComponent<SStaticMeshComponent>(MeshEntity);
        StaticMeshComponent.MaterialOverrides.clear();

        SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera)
        {
            Camera->PostProcessMaterials.clear();
        }
        
        if (MaterialType != EMaterialType::UI)
        {
            RmlUi::SetWorldInlineDocument(World, FStringView(), FStringView());
            if (StaticMeshComponent.GetStaticMesh() == nullptr)
            {
                StaticMeshComponent.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);
            }
        }

        if (MaterialType == EMaterialType::PostProcess)
        {
            if (Camera)
            {
                Camera->PostProcessMaterials.push_back(MaterialInterface);
            }
        }
        else if (MaterialType == EMaterialType::UI)
        {
            StaticMeshComponent.SetStaticMesh(nullptr);

            const FAssetData* Data = MaterialInterface
                ? FAssetRegistry::Get().GetAssetByGUID(MaterialInterface->GetGUID())
                : nullptr;
            if (Data != nullptr)
            {
                FString Body;
                Body.append("<rml><head><style>"
                            "body{margin:0;padding:0;width:100%;height:100%;background-color:#000000;"
                            "display:flex;align-items:center;justify-content:center;}"
                            "img{width:75%;height:75%;}"
                            "</style></head>");
                Body.append("<body><img src=\"material:");
                Body.append(Data->Path.c_str());
                Body.append("\"/></body></rml>");
                RmlUi::SetWorldInlineDocument(World, FStringView(Body.c_str(), Body.size()), FStringView("material_preview.rml"));
            }
            else
            {
                LOG_WARN("[MaterialEditor] UI material preview: no registry path (save the asset first).");
            }
        }
        else if (MaterialType == EMaterialType::Decal)
        {
            StaticMeshComponent.MaterialOverrides.clear();
        }
        else
        {
            StaticMeshComponent.MaterialOverrides.push_back(MaterialInterface);
        }
    }

    void FMaterialEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Graph",
            "Right-click empty space to spawn nodes. Drag from a pin to wire it; types must match. "
            "Shift-drag from a pin to drop a Reroute. Delete or Backspace removes selection.");
        DrawHelpTextRow("Compile",
            "Saving compiles the graph and uploads it to all material instances using this asset. "
            "Compile errors surface in the log and on the failing node.");
        DrawHelpTextRow("Preview",
            "Use the Mesh menu to swap the preview between sphere/cube/plane/cylinder/cone. "
            "Camera controls match the world editor (RMB + WASD).");
        DrawHelpTextRow("Instances",
            "Make derived materials via Content Browser > New > Material Instance. Instances inherit "
            "the master graph and only override exposed parameters.");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Debug Node IDs");
        ImGui::TableNextColumn();
        ImGui::Checkbox("##DebugID", &NodeGraph->bDebug);
    }

    void FMaterialEditorTool::SetDebugMesh(EDebugMesh Mesh, FStringView Path)
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

    bool FMaterialEditorTool::DrawViewport(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture)
    {
        const ImVec2 ContentRegion = ImGui::GetContentRegionAvail();
        const ImVec2 ViewportSize(eastl::max(ContentRegion.x, 64.0f), eastl::max(ContentRegion.y, 64.0f));
        const ImVec2 CursorScreenPos = ImGui::GetCursorScreenPos();
        const ImVec2 WindowBottomRight = { CursorScreenPos.x + ViewportSize.x, CursorScreenPos.y + ViewportSize.y };

        if (IRenderScene* Scene = World ? World->GetRenderer() : nullptr)
        {
            FSceneRenderSettings& Settings = Scene->GetSceneRenderSettings();
            Settings.bDrawBillboards = false;
            Settings.bDrawAABB       = false;
            bWorldGridEnabled        = false;
        }

        if (SCameraComponent* CameraComponent = World->GetActiveCamera())
        {
            CameraComponent->SetFOV(60.0f);
        }

        ImGui::GetWindowDrawList()->AddRectFilled(CursorScreenPos, WindowBottomRight, IM_COL32(0, 0, 0, 255));

        ImGui::GetWindowDrawList()->AddImage(
            ViewportTexture,
            CursorScreenPos,
            WindowBottomRight,
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32_WHITE
        );

        const ImGuiStyle& ImStyle = ImGui::GetStyle();

        ImVec2 Origin = ImGui::GetCursorStartPos();

        ImGui::Dummy(ImStyle.ItemSpacing);
        ImGui::SetCursorPos(Origin + ImStyle.ItemSpacing);
        DrawViewportOverlayElements(UpdateContext, ViewportTexture, ViewportSize);

        Origin = ImGui::GetCursorStartPos();

        ImGui::Dummy(ImStyle.ItemSpacing);
        ImGui::SetCursorPos(Origin + ImStyle.ItemSpacing);
        DrawViewportToolbar(UpdateContext);
        
        if (ImGuiDockNode* pDockNode = ImGui::GetWindowDockNode())
        {
           pDockNode->LocalFlags = 0;
           pDockNode->LocalFlags |= ImGuiDockNodeFlags_NoDockingOverMe;
        }

        return false;
    }

    void FMaterialEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
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

    void FMaterialEditorTool::OnAssetLoadFinished()
    {
    }
    
    bool FMaterialEditorTool::NeedsCompile() const
    {
        return NodeGraph != nullptr && (!bHasCompiledOnce || NodeGraph->GetContentVersion() != CompiledContentVersion);
    }

    void FMaterialEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        const bool bPending = NeedsCompile();
        if (bPending)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Warning());
        }

        if (ImGui::MenuItem(bPending ? LE_ICON_ALERT_CIRCLE " Compile*" : LE_ICON_RECEIPT_TEXT " Compile"))
        {
            Compile();
            OnSave();
        }

        if (bPending)
        {
            ImGui::PopStyleColor();
            ImGuiX::TextTooltip("{}", "This material has changes that are not in its compiled shader yet.");
        }
    }

    void FMaterialEditorTool::DrawMaterialGraph()
    {
        NodeGraph->DrawGraph();
    }

    void FMaterialEditorTool::DrawMaterialProperties()
    {
        GetPropertyTable()->DrawTree();
        
        if (EnvironmentEditor && DirectionalEditor)
        {
            ImGui::Spacing();
            ImGui::SeparatorText("Preview Editor");
            ImGui::Spacing();

            DirectionalEditor->DrawTree();
            EnvironmentEditor->DrawTree();
        }
    }
    
    void FMaterialEditorTool::DrawCustomCodeEditor()
    {
        CMaterialExpression_CustomSlang* Node = Cast<CMaterialExpression_CustomSlang>(SelectedNode);

        if (Node == nullptr)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("Select a Custom Slang node to edit its code.\n\n"
                               "Declare the node's inputs and outputs in Material Properties; they become pins "
                               "and named locals your code can read and assign.");
            ImGui::PopStyleColor();
            CodeEditorBoundNode = nullptr;
            return;
        }

        // (Re)bind on selection change. Only pull from the node here -- pulling every frame would
        // fight the editor's own undo history and cursor state.
        if (CodeEditorBoundNode != Node)
        {
            CodeEditorBoundNode = Node;
            CodeEditor.SetText(std::string_view(Node->Code.c_str(), Node->Code.size()));
            LastCodeEditorUndoIndex = CodeEditor.GetUndoIndex();
        }

        // Signature reference: what the body can actually read and must assign.
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::Accent(), "%s", Node->Title.c_str());
        ImGui::SameLine(0.0f, 12.0f);

        FFixedString Signature;
        for (const FCustomSlangPin& In : Node->Inputs)
        {
            Signature += (Signature.empty() ? "in: " : ", ");
            Signature += In.Name.c_str();
        }
        if (!Node->Outputs.empty())
        {
            Signature += Signature.empty() ? "out: " : "  |  out: ";
            bool bFirst = true;
            for (const FCustomSlangPin& Out : Node->Outputs)
            {
                if (!bFirst) { Signature += ", "; }
                Signature += Out.Name.c_str();
                bFirst = false;
            }
        }
        ImGui::TextColored(EditorColors::TextDim(), "%s", Signature.empty() ? "no pins declared" : Signature.c_str());

        // Live validation, so a stray brace is caught here rather than as a wall of shader errors.
        TVector<FString> Problems;
        const bool bValid = Node->Validate(Problems);
        if (!bValid)
        {
            for (const FString& Problem : Problems)
            {
                ImGui::TextColored(EditorColors::Warning(), LE_ICON_EXCLAMATION_THICK " %s", Problem.c_str());
            }
        }

        ImGui::Separator();

        // Same monospaced face the RmlUi code editor uses -- column alignment is the whole point of a
        // code view, and the proportional UI font makes indented shader code unreadable.
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Mono);
        CodeEditor.Render("##CustomSlangCode", ImGui::GetContentRegionAvail(), false);
        ImGuiX::Font::PopFont();

        // Write back only on a real edit; the graph's auto-compile picks it up on the next frame. The
        // undo cursor is just a cheap gate -- the text is compared before writing, because the cursor
        // also moves on undo/redo (and back onto a value we already stored).
        if (CodeEditor.GetUndoIndex() != LastCodeEditorUndoIndex)
        {
            LastCodeEditorUndoIndex = CodeEditor.GetUndoIndex();

            const std::string Text = CodeEditor.GetText();
            if (Node->Code.size() != Text.size() || memcmp(Node->Code.data(), Text.data(), Text.size()) != 0)
            {
                Node->Code.assign(Text.c_str(), Text.size());

                if (CPackage* Package = Node->GetPackage())
                {
                    Package->MarkDirty();
                }
            }
        }
    }

    void FMaterialEditorTool::DrawShaderStats()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

        if (CompilationResult.bIsError)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.10f, 0.10f, 1.0f));
            ImGui::BeginChild("##stats_error", ImVec2(0, 0), true);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::Text("Compilation failed (%d error%s)",
                static_cast<int>(CompilationResult.Errors.size()),
                CompilationResult.Errors.size() == 1 ? "" : "s");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();

            constexpr ImVec4 TitleColor (1.00f, 0.55f, 0.55f, 1.0f);
            constexpr ImVec4 NodeColor  (1.00f, 0.85f, 0.55f, 1.0f);
            constexpr ImVec4 BodyColor  (1.00f, 0.80f, 0.80f, 1.0f);
            constexpr ImVec4 HintColor  (0.65f, 0.65f, 0.70f, 1.0f);

            for (size_t i = 0; i < CompilationResult.Errors.size(); ++i)
            {
                const FCompilationError& Err = CompilationResult.Errors[i];

                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.13f, 0.13f, 1.0f));
                ImGui::BeginChild("##err_row", ImVec2(0, 0), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);

                ImGui::PushStyleColor(ImGuiCol_Text, TitleColor);
                ImGui::Text("[%s]", Err.Title.c_str());
                ImGui::PopStyleColor();

                if (Err.Node != nullptr)
                {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, NodeColor);
                    ImGui::Text("%s", Err.Node->GetNodeFullName().c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::PushStyleColor(ImGuiCol_Text, BodyColor);
                ImGui::TextWrapped("%s", Err.Description.c_str());
                ImGui::PopStyleColor();

                if (Err.Node != nullptr)
                {
                    ImGui::Spacing();
                    if (ImGui::SmallButton("Focus Node"))
                    {
                        FocusGraphNode(Err.Node);
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, HintColor);
                    ImGui::TextUnformatted("(selects and centers in graph)");
                    ImGui::PopStyleColor();
                }

                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();

                ImGui::Spacing();
            }

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            return;
        }

        if (!bHasCompiledOnce)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.16f, 1.0f));
            ImGui::BeginChild("##stats_empty", ImVec2(0, 0), true);

            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const ImVec2 Size  = ImGui::CalcTextSize("Compile to see shader stats");
            ImGui::SetCursorPos(ImVec2((Avail.x - Size.x) * 0.5f, (Avail.y - Size.y) * 0.5f));

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1.0f));
            ImGui::TextUnformatted("Compile to see shader stats");
            ImGui::PopStyleColor();

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
        ImGui::BeginChild("##stats_root", ImVec2(0, 0), true);

        const ImVec4 LabelColor (0.65f, 0.65f, 0.72f, 1.0f);
        const ImVec4 ValueColor (1.00f, 1.00f, 1.00f, 1.0f);
        const ImVec4 HeaderColor(0.70f, 0.85f, 1.00f, 1.0f);

        // Cost color thresholds.
        ImVec4 CostColor;
        const uint32 Cost = ShaderStats.EstimatedCost;
        if      (Cost < 50)   CostColor = ImVec4(0.40f, 1.00f, 0.45f, 1.0f);
        else if (Cost < 150)  CostColor = ImVec4(0.95f, 0.95f, 0.40f, 1.0f);
        else if (Cost < 300)  CostColor = ImVec4(1.00f, 0.65f, 0.30f, 1.0f);
        else                  CostColor = ImVec4(1.00f, 0.40f, 0.40f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, HeaderColor);
        ImGui::TextUnformatted("Shader Complexity");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        const float LabelWidth = 220.0f;

        auto Row = [&](const char* Label, const char* Fmt, auto Value, ImVec4 ValColor)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, LabelColor);
            ImGui::TextUnformatted(Label);
            ImGui::PopStyleColor();
            ImGui::SameLine(LabelWidth);
            ImGui::PushStyleColor(ImGuiCol_Text, ValColor);
            ImGui::Text(Fmt, Value);
            ImGui::PopStyleColor();
        };

        Row("Estimated Cost",          "%u", ShaderStats.EstimatedCost,      CostColor);
        Row("Pixel Instructions",      "%u", ShaderStats.PixelInstructions,  ValueColor);
        if (ShaderStats.bUsesVertexStage)
        {
            Row("Vertex Instructions", "%u", ShaderStats.VertexInstructions, ValueColor);
        }
        Row("Texture Samples",         "%u", ShaderStats.TextureSamples,     ValueColor);
        Row("Math Operations",         "%u", ShaderStats.MathOps,            ValueColor);
        Row("Noise Operations",        "%u", ShaderStats.NoiseOps,           ValueColor);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, HeaderColor);
        ImGui::TextUnformatted("Resources");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        Row("Bound Textures",          "%u", ShaderStats.BoundTextures,      ValueColor);
        Row("Texture Parameters",      "%u", ShaderStats.TextureParameters,  ValueColor);
        Row("Scalar Parameters",       "%u", ShaderStats.ScalarParameters,   ValueColor);
        Row("Vector Parameters",       "%u", ShaderStats.VectorParameters,   ValueColor);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, HeaderColor);
        ImGui::TextUnformatted("Stages");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        Row("Uses Vertex Stage (WPO)", "%s", ShaderStats.bUsesVertexStage ? "Yes" : "No",
            ShaderStats.bUsesVertexStage ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ValueColor);
        Row("Pixel Source Size",       "%u chars", ShaderStats.PixelCharacters, ValueColor);
        if (ShaderStats.bUsesVertexStage)
        {
            Row("Vertex Source Size",  "%u chars", ShaderStats.VertexCharacters, ValueColor);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Generated source is intentionally tucked behind a collapsing header so it isn't part
        // of the main stats view -- the user has to opt in to see the raw HLSL.
        if (ImGui::CollapsingHeader("Generated Pixel Shader (HLSL)"))
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
            ImGui::BeginChild("##hlsl_pixel", ImVec2(0, 320), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.92f, 1.0f));
            ImGui::TextUnformatted(Tree.c_str());
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        if (ShaderStats.bUsesVertexStage && !VertexTree.empty())
        {
            if (ImGui::CollapsingHeader("Generated Vertex Shader (HLSL)"))
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
                ImGui::BeginChild("##hlsl_vertex", ImVec2(0, 320), true, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.92f, 1.0f));
                ImGui::TextUnformatted(VertexTree.c_str());
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void FMaterialEditorTool::FocusGraphNode(CEdGraphNode* Node)
    {
        if (Node == nullptr || NodeGraph == nullptr)
        {
            return;
        }

        // May be called outside DrawGraph scope (e.g. stats panel); safe to set-act-clear manually.
        ax::NodeEditor::EditorContext* PrevCtx = ax::NodeEditor::GetCurrentEditor();
        ax::NodeEditor::EditorContext* OurCtx  = NodeGraph->GetEditorContext();
        if (OurCtx == nullptr)
        {
            return;
        }

        ax::NodeEditor::SetCurrentEditor(OurCtx);
        ax::NodeEditor::SelectNode(Node->GetNodeID(), false);
        ax::NodeEditor::NavigateToSelection(false, 0.25f);
        ax::NodeEditor::SetCurrentEditor(PrevCtx);
    }

    void FMaterialEditorTool::Compile()
    {
        CompilationResult = FCompilationResultInfo();
        CMaterial* Material = Cast<CMaterial>(Asset.Get());

        // All the heavy lifting (graph compile -> every shader stage -> params/textures -> PostLoad) lives in
        // the shared CompileMaterialGraph so the importer's procedural materials compile identically.
        const FMaterialGraphCompileResult CompileResult = CompileMaterialGraph(Material, NodeGraph);

        ShaderStats       = CompileResult.Stats;
        bHasCompiledOnce  = true;
        bGLSLPreviewDirty = true;

        // Stamped even on failure: the compile ran against this exact graph, so nagging about it again
        // until something else changes would just be noise on top of the errors already surfaced.
        CompiledContentVersion = NodeGraph != nullptr ? NodeGraph->GetContentVersion() : 0;

        if (!CompileResult.bSuccess)
        {
            for (const EdNodeGraph::FError& Error : CompileResult.Errors)
            {
                CompilationResult.CompilationLog += "ERROR - [" + Error.Name + "]: " + Error.Description + "\n";

                FCompilationError Structured;
                Structured.Title       = Error.Name;
                Structured.Description = Error.Description;
                Structured.Node        = Error.Node;
                CompilationResult.Errors.push_back(Move(Structured));
            }

            CompilationResult.bIsError = true;
            return;
        }

        Tree       = CompileResult.PixelSource;
        VertexTree = CompileResult.VertexSource;

        // ReplacementStart/End power the GLSL preview highlight band; recompute against the pixel shader tree.
        ReplacementStart = Tree.find("$MATERIAL_INPUTS");
        ReplacementEnd   = ReplacementStart;

        CompilationResult.CompilationLog = "Generated GLSL: \n \n \n";
        CompilationResult.bIsError = false;

        Material->GetPackage()->MarkDirty();

        // Re-route asset to preview in case MaterialType changed during compile.
        ApplyMaterialToPreview();
    }

    void FMaterialEditorTool::OnSave()
    {
        // Before the base save, not after: Compile writes the shaders, parameters and texture table back
        // onto the material, so saving first would persist the pre-compile state and leave the asset on
        // disk disagreeing with the graph it contains.
        const CMaterialEditorSettings* Settings = GetDefault<CMaterialEditorSettings>();
        if (Settings != nullptr && Settings->bCompileOnSave && NeedsCompile())
        {
            Compile();
        }

        FAssetEditorTool::OnSave();
    }

    void FMaterialEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, rightBottomDockID = 0;

        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(rightDockID, ImGuiDir_Down, 0.3f, &rightBottomDockID, &rightDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(MaterialGraphName).c_str(),       leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(),      rightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(ShaderStatsName).c_str(),         rightBottomDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MaterialPropertiesName).c_str(),  rightBottomDockID);
        // Tabs behind the graph: editing a node's code is the same task as editing the graph.
        ImGui::DockBuilderDockWindow(GetToolWindowName(CustomCodeName).c_str(),          leftDockID);
    }
}
