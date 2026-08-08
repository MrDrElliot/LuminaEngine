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

        ConfigureCodeEditor();

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

        DrawViewGizmo(CursorScreenPos, ViewportSize);

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
    
    namespace
    {
        // Slang for the Custom Slang node's body.
        //
        // Built by COPYING the editor's HLSL definition rather than authoring one from scratch: Slang is
        // HLSL plus extensions, and the copy carries the pieces that are not data -- isPunctuation,
        // getIdentifier, getNumber -- which are file-static inside TextEditor.cpp and cannot be reached
        // from here. Only the additions below are ours.
        //
        // The split between `keywords` and `identifiers` is the reason this is worth doing at all: keywords
        // colour as language syntax, identifiers as knownIdentifier. Putting the intrinsics and the node's
        // own pin names in the second bucket is what makes "this is a thing the engine gave me" visually
        // distinct from "this is Slang", which a plain HLSL definition cannot express.
        const TextEditor::Language& GetSlangLanguageBase()
        {
            static bool bInitialized = false;
            static TextEditor::Language Language;

            if (bInitialized)
            {
                return Language;
            }

            Language = *TextEditor::Language::Hlsl();
            Language.name = "Slang";

            // Slang-only syntax the HLSL set has no reason to know about.
            static const char* const SlangKeywords[] =
            {
                "__generic", "__init", "__subscript", "__exported", "associatedtype", "extension",
                "interface", "property", "typealias", "func", "let", "var", "enum", "is", "as",
                "where", "This", "no_diff", "nodiff", "differentiable", "bwd_diff", "fwd_diff",
                "module", "import", "implementing", "public", "internal", "private", "override",
                "dynamic_uniform", "groupshared", "ConstantBuffer", "ParameterBlock", "StructuredBuffer",
                "Ptr", "Optional", "none", "each", "expand", "reinterpret", "spirv_asm",
                "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                "float16_t", "float32_t", "float64_t", "matrix", "vector",
            };
            for (const char* Keyword : SlangKeywords)
            {
                Language.keywords.insert(Keyword);
            }

            // Intrinsics. The HLSL definition ships keywords only, so before this every call in a custom
            // node body -- saturate, lerp, dot -- rendered as plain text.
            static const char* const Intrinsics[] =
            {
                "abs", "acos", "all", "any", "asin", "atan", "atan2", "ceil", "clamp", "cos", "cosh",
                "cross", "ddx", "ddy", "degrees", "determinant", "distance", "dot", "exp", "exp2",
                "faceforward", "floor", "fmod", "frac", "frexp", "fwidth", "isfinite", "isinf", "isnan",
                "ldexp", "length", "lerp", "log", "log2", "log10", "mad", "max", "min", "modf", "mul",
                "normalize", "pow", "radians", "rcp", "reflect", "refract", "round", "rsqrt", "saturate",
                "sign", "sin", "sincos", "sinh", "smoothstep", "sqrt", "step", "tan", "tanh", "transpose",
                "trunc", "clip", "countbits", "firstbithigh", "firstbitlow", "reversebits", "asfloat",
                "asint", "asuint", "f16tof32", "f32tof16", "select", "InterlockedAdd", "InterlockedOr",
                "GroupMemoryBarrierWithGroupSync", "AllMemoryBarrierWithGroupSync",
                "WaveActiveSum", "WaveActiveMax", "WaveActiveMin", "WaveActiveCountBits",
                "WavePrefixCountBits", "WaveReadLaneFirst", "WaveIsFirstLane", "WaveGetLaneIndex",
            };
            for (const char* Intrinsic : Intrinsics)
            {
                Language.identifiers.insert(Intrinsic);
            }

            // Engine-side names reachable from a material body. These are what the material compiler
            // substitutes around the node, so they are in scope whether or not the author declared them.
            static const char* const EngineIdentifiers[] =
            {
                "MaterialIndex", "EntityID", "UV0", "UV0_DDX", "UV0_DDY", "WorldPosition", "WorldNormal",
                "WorldTangent", "ViewPosition", "VertexColor", "Material", "Inst", "ModelMatrix",
                "SampleTexture2D", "SampleTexture2DGrad", "SampleTexture2DLevel", "SampleTexture2DArray",
                "SampleTextureCube", "GetMaterialTexture", "GetMaterialScalar", "GetMaterialVector",
                "GetCameraPosition", "GetCameraForward", "GetTime", "Scene",
            };
            for (const char* Identifier : EngineIdentifiers)
            {
                Language.identifiers.insert(Identifier);
            }

            bInitialized = true;
            return Language;
        }

        // Change detector for the highlight only, so it deliberately covers pin NAMES and nothing else --
        // the node's own signature hash is private, and it also folds in pin types, which would rebuild
        // the language on edits that cannot change a single colour.
        uint64 HashPinNames(const CMaterialExpression_CustomSlang* Node)
        {
            uint64 Hash = 1469598103934665603ull;   // FNV-1a
            auto Mix = [&Hash](const char* Text)
            {
                for (const char* C = Text; *C != '\0'; ++C)
                {
                    Hash ^= (uint64)(uint8)*C;
                    Hash *= 1099511628211ull;
                }
                Hash ^= 0xFFull;   // separator, so {"AB","C"} and {"A","BC"} differ
                Hash *= 1099511628211ull;
            };

            for (const FCustomSlangPin& Pin : Node->Inputs)  { Mix(Pin.Name.c_str()); }
            Mix("|");
            for (const FCustomSlangPin& Pin : Node->Outputs) { Mix(Pin.Name.c_str()); }
            return Hash;
        }
    }

    void FMaterialEditorTool::ConfigureCodeEditor()
    {
        // SetTabSize is only honoured while the document is empty and has no transactions, so this has to
        // run before the first SetText -- hence OnInitialize rather than the first draw.
        CodeEditor.SetTabSize(4);
        CodeEditor.SetInsertSpacesOnTabs(true);
        CodeEditor.SetAutoIndentEnabled(true);
        CodeEditor.SetShowLineNumbersEnabled(true);
        CodeEditor.SetShowMatchingBrackets(true);
        CodeEditor.SetCompletePairedGlyphs(true);
        CodeEditor.SetShowScrollbarMiniMapEnabled(true);
        CodeEditor.SetPalette(TextEditor::GetDarkPalette());

        CodeEditorLanguage = GetSlangLanguageBase();
        CodeEditor.SetLanguage(&CodeEditorLanguage);
    }

    void FMaterialEditorTool::RebuildCodeEditorLanguage(const CMaterialExpression_CustomSlang* Node)
    {
        CodeEditorLanguage = GetSlangLanguageBase();

        // The node's own pins, so an author sees at a glance whether they spelled a declared name right --
        // a misspelled output is otherwise a silent no-op that only surfaces as "nothing changed".
        if (Node != nullptr)
        {
            for (const FCustomSlangPin& Pin : Node->Inputs)
            {
                CodeEditorLanguage.identifiers.insert(Pin.Name.c_str());
            }
            for (const FCustomSlangPin& Pin : Node->Outputs)
            {
                CodeEditorLanguage.identifiers.insert(Pin.Name.c_str());
            }
        }

        // Re-point even though the address is unchanged: SetLanguage raises the dirty flag that makes the
        // colorizer re-run over the whole document, which is what picks up the new identifier set.
        CodeEditor.SetLanguage(&CodeEditorLanguage);
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
            RebuildCodeEditorLanguage(Node);
            CodeEditor.SetText(std::string_view(Node->Code.c_str(), Node->Code.size()));
            LastCodeEditorUndoIndex = CodeEditor.GetUndoIndex();
            CodeEditorPinSignature = HashPinNames(Node);
        }
        else if (const uint64 Signature = HashPinNames(Node); Signature != CodeEditorPinSignature)
        {
            // Pins are edited in the details panel while this tab stays bound, so the highlight has to
            // follow them -- otherwise a freshly renamed output keeps colouring under its old name.
            CodeEditorPinSignature = Signature;
            RebuildCodeEditorLanguage(Node);
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

        // Captured at the START of the toolbar line: SameLine's offset is measured from the line start, so
        // reading the remaining width after the buttons would place the status by the wrong origin.
        const float ToolbarWidth = ImGui::GetContentRegionAvail().x;

        // Toolbar. Find/replace is built into the editor but has no discoverable entry point, so it gets a
        // button as well as its shortcut.
        if (ImGui::SmallButton(LE_ICON_MAGNIFY " Find"))
        {
            CodeEditor.OpenFindReplaceWindow();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Find / replace (Ctrl+F)");
        }

        ImGui::SameLine();
        bool bShowWhitespace = CodeEditor.IsShowWhitespacesEnabled();
        if (ImGui::Checkbox("Whitespace", &bShowWhitespace))
        {
            CodeEditor.SetShowWhitespacesEnabled(bShowWhitespace);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Tabs " LE_ICON_ARROW_RIGHT " Spaces"))
        {
            CodeEditor.TabsToSpaces();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Converts leading tabs to spaces at the editor's tab size.");
        }

        // Right-aligned status: language and cursor. The language name is worth showing because the
        // highlight includes THIS node's pin names, so it is not a generic Slang mode.
        const TextEditor::CursorPosition Cursor = CodeEditor.GetCursorPosition(0);
        FFixedString Status;
        Status.sprintf("%s  |  Ln %d, Col %d  |  %d lines",
            CodeEditor.GetLanguageName().c_str(), Cursor.line + 1, Cursor.column + 1, CodeEditor.GetLineCount());

        const float StatusWidth = ImGui::CalcTextSize(Status.c_str()).x;
        const float StatusX     = ToolbarWidth - StatusWidth;
        // A narrow panel would push the status left of the buttons and overlap them; let it wrap instead.
        if (StatusX > 0.0f)
        {
            ImGui::SameLine(StatusX);
            ImGui::TextColored(EditorColors::TextDim(), "%s", Status.c_str());
        }

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

    // Hardware-truth counterpart to the source-derived stats above: local-memory arrays (from the compiled
    // SPIR-V) and the driver's own register count / occupancy (from VK_KHR_pipeline_executable_properties).
    //
    // These exist because both are invisible while authoring and only show up in a GPU capture. Register
    // count in particular is a STEP function on occupancy, not a gradient -- on Ampere 128 regs caps a
    // shader at 16 resident warps, 96 gets 20, 80 gets 24 -- so a material can sit one register over a
    // cliff and lose a quarter of its latency hiding with nothing in the graph looking different.
    namespace
    {
        // Register-limited warp occupancy on an NVIDIA SM.
        //
        // Occupancy is a STEP function of the per-thread register count: an SM partition has a fixed
        // register budget and hands it out in fixed-size blocks, so shedding registers buys EXACTLY NOTHING
        // until the count crosses into the next block. That is the whole reason this exists -- a raw
        // "84 registers" tells an author nothing they can act on, while "4 fewer registers gains a warp"
        // (or "you are 1 register into a new block, 7 more are free") does.
        //
        // Assumes the layout every NVIDIA part since Turing shares: 65536 32-bit registers per SM across 4
        // partitions, allocated per warp in blocks of 8 registers/thread. AMD's register file and wave
        // sizing are different enough that this model does not transfer, which is why it is only applied to
        // a statistic the driver named like an NVIDIA register count (AMD names its "VGPRs"/"SGPRs" and
        // reports an occupancy statistic of its own, which is displayed verbatim).
        struct FOccupancyStep
        {
            bool   bValid            = false;
            uint32 WarpsPerSM        = 0;
            uint32 NextStepRegs      = 0;   // register count that reaches the next step (0 = none better)
            uint32 NextStepWarpsPerSM = 0;
        };

        constexpr uint32 kRegsPerPartition   = 16384u;   // 65536 per SM / 4 partitions
        constexpr uint32 kRegAllocGranularity = 8u;
        constexpr uint32 kThreadsPerWarp     = 32u;
        // Slot cap, not a register cap. Ampere/Ada consumer parts top out at 48 warps/SM; Turing at 32.
        // Taking the higher value keeps this an upper bound on the REGISTER-limited figure, which is the
        // component the author actually controls.
        constexpr uint32 kMaxWarpsPerSM      = 48u;

        uint32 WarpsPerSMForRegs(uint32 RegsPerThread)
        {
            if (RegsPerThread == 0u)
            {
                return kMaxWarpsPerSM;
            }
            const uint32 Rounded = ((RegsPerThread + kRegAllocGranularity - 1u) / kRegAllocGranularity) * kRegAllocGranularity;
            const uint32 PerWarp = Rounded * kThreadsPerWarp;
            const uint32 Warps   = (kRegsPerPartition / PerWarp) * 4u;
            return Warps < kMaxWarpsPerSM ? Warps : kMaxWarpsPerSM;
        }

        FOccupancyStep ComputeOccupancyStep(uint32 RegsPerThread)
        {
            FOccupancyStep Out;
            if (RegsPerThread == 0u || RegsPerThread > 255u)
            {
                return Out;
            }

            Out.bValid     = true;
            Out.WarpsPerSM = WarpsPerSMForRegs(RegsPerThread);

            // Walk down to the first register count that yields MORE warps. Stepping by the allocation
            // granularity is what makes the answer honest: any target that is not on a block boundary
            // would be advice that cannot pay off.
            for (uint32 Candidate = (RegsPerThread / kRegAllocGranularity) * kRegAllocGranularity;
                 Candidate >= kRegAllocGranularity; Candidate -= kRegAllocGranularity)
            {
                const uint32 Warps = WarpsPerSMForRegs(Candidate);
                if (Warps > Out.WarpsPerSM)
                {
                    Out.NextStepRegs       = Candidate;
                    Out.NextStepWarpsPerSM = Warps;
                    break;
                }
            }
            return Out;
        }

        // NVIDIA names it "Register Count"; AMD uses "VGPRs"/"SGPRs", which this deliberately does not match.
        bool IsNvidiaRegisterStat(const FString& Name)
        {
            FString Lower = Name;
            for (char& C : Lower)
            {
                C = (char)eastl::CharToLower(C);
            }
            return Lower.find("register") != FString::npos;
        }
    }

    void FMaterialEditorTool::DrawGPUStats(float LabelWidth, const ImVec4& HeaderColor,
                                           const ImVec4& LabelColor, const ImVec4& ValueColor)
    {
        CMaterial* Material = Cast<CMaterial>(Asset.Get());
        if (Material == nullptr)
        {
            return;
        }

        // The DEFERRED (VisBuffer) permutation first: it is the one that shades opaque geometry, so it is
        // the one whose register count matters. The forward pixel shader is listed too because translucent
        // and capture views still use it, and the two can differ (different spec constants survive).
        const struct { const char* Label; const FShaderEntry* Entry; } Lanes[] =
        {
            { "Deferred (VisBuffer)", Material->GetDeferredShader() },
            { "Forward Pixel",        Material->GetPixelShader()    },
        };

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, HeaderColor);
        ImGui::TextUnformatted("GPU (compiled)");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

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

        const ImVec4 WarnColor(1.00f, 0.65f, 0.30f, 1.0f);
        const ImVec4 MutedColor(0.55f, 0.55f, 0.60f, 1.0f);

        bool bAnyPipelineStats = false;

        for (const auto& Lane : Lanes)
        {
            if (Lane.Entry == nullptr || !Lane.Entry->IsValid())
            {
                continue;
            }

            const FShaderEntry::FGPUStats Stats = FShaderLibrary::GetGPUStats(Lane.Entry);

            ImGui::PushStyleColor(ImGuiCol_Text, ValueColor);
            ImGui::TextUnformatted(Lane.Label);
            ImGui::PopStyleColor();
            ImGui::Indent(12.0f);

            // Non-zero is a real finding, not a style note: a function-scope array does not live in
            // registers, so each access is a local-memory round trip that stalls on the long scoreboard.
            Row("Local Memory Arrays", "%u", Stats.LocalArrayCount,
                Stats.LocalArrayCount > 0 ? WarnColor : ValueColor);
            if (Stats.LocalArrayCount > 0)
            {
                Row("  Scalars Spilled", "%u", Stats.LocalArrayScalars, WarnColor);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Arrays declared in function scope do not promote to registers.\n"
                                      "The driver backs them with local memory, so every indexed read is\n"
                                      "a memory round trip. Rewriting the indexing as constant-foldable\n"
                                      "selects normally removes them entirely.");
                }
            }

            for (const RHI::FPipelineStat& Stat : Stats.Pipeline)
            {
                bAnyPipelineStats = true;
                const FString Label = FString("  ") + Stat.Stage + " / " + Stat.Name;
                if (Stat.bIsFloat)
                {
                    Row(Label.c_str(), "%.2f", Stat.Value, ValueColor);
                }
                else
                {
                    Row(Label.c_str(), "%lld", (long long)Stat.Value, ValueColor);
                }

                // Turn the raw count into the thing an author can act on. Only for a fragment stage: the
                // step matters everywhere, but the deferred/forward PS is the shader a material controls.
                if (!Stat.bIsFloat && IsNvidiaRegisterStat(Stat.Name))
                {
                    const FOccupancyStep Step = ComputeOccupancyStep((uint32)Stat.Value);
                    if (Step.bValid)
                    {
                        // Under ~4 warps/SM the shader is starved regardless of what else is going on.
                        const ImVec4 OccColor = Step.WarpsPerSM <= 16u ? WarnColor : ValueColor;
                        Row("    Occupancy (reg-limited)", "%u warps/SM", Step.WarpsPerSM, OccColor);

                        if (Step.NextStepRegs > 0u)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, MutedColor);
                            ImGui::TextWrapped("      Next step at %u registers (-%u) -> %u warps/SM. "
                                               "Shedding fewer than that gains nothing.",
                                               Step.NextStepRegs,
                                               (uint32)Stat.Value - Step.NextStepRegs,
                                               Step.NextStepWarpsPerSM);
                            ImGui::PopStyleColor();
                        }

                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("Occupancy is a step function of register count: an SM\n"
                                              "partition allocates registers in fixed blocks, so a\n"
                                              "reduction that does not cross a block boundary buys\n"
                                              "nothing. Estimated for NVIDIA (64K registers/SM, 4\n"
                                              "partitions, 8-register granularity).");
                        }
                    }
                }
            }

            ImGui::Unindent(12.0f);
            ImGui::Spacing();
        }

        if (!bAnyPipelineStats)
        {
            // Two distinct causes, and the user can only act on the first: the pipeline is built lazily on
            // first draw, so a material that has never been rendered simply has nothing to report yet.
            ImGui::PushStyleColor(ImGuiCol_Text, MutedColor);
            ImGui::TextWrapped("Driver pipeline statistics unavailable. They are captured the first time this "
                               "material is drawn -- open a scene using it, or use the preview viewport. If they "
                               "never appear, this GPU does not support VK_KHR_pipeline_executable_properties.");
            ImGui::PopStyleColor();
        }
    }

    void FMaterialEditorTool::DrawDiagnosticRows(const TVector<FCompilationError>& Diagnostics, bool bIsError)
    {
        const ImVec4 RowBg      = bIsError ? ImVec4(0.20f, 0.13f, 0.13f, 1.0f) : ImVec4(0.20f, 0.17f, 0.11f, 1.0f);
        const ImVec4 TitleColor = bIsError ? ImVec4(1.00f, 0.55f, 0.55f, 1.0f) : ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
        const ImVec4 BodyColor  = bIsError ? ImVec4(1.00f, 0.80f, 0.80f, 1.0f) : ImVec4(0.95f, 0.90f, 0.78f, 1.0f);
        constexpr ImVec4 NodeColor(1.00f, 0.85f, 0.55f, 1.0f);
        constexpr ImVec4 HintColor(0.65f, 0.65f, 0.70f, 1.0f);

        for (size_t i = 0; i < Diagnostics.size(); ++i)
        {
            const FCompilationError& Diag = Diagnostics[i];

            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, RowBg);
            // Height follows the wrapped text; AlwaysAutoResize is a child flag, not a window flag.
            ImGui::BeginChild("##diag_row", ImVec2(0, 0),
                ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                ImGuiWindowFlags_NoScrollbar);

            ImGui::PushStyleColor(ImGuiCol_Text, TitleColor);
            ImGui::Text("[%s]", Diag.Title.c_str());
            ImGui::PopStyleColor();

            if (Diag.Node != nullptr)
            {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, NodeColor);
                ImGui::Text("%s", Diag.Node->GetNodeFullName().c_str());
                ImGui::PopStyleColor();
            }

            ImGui::PushStyleColor(ImGuiCol_Text, BodyColor);
            ImGui::TextWrapped("%s", Diag.Description.c_str());
            ImGui::PopStyleColor();

            if (Diag.Node != nullptr)
            {
                ImGui::Spacing();
                if (ImGui::SmallButton("Focus Node"))
                {
                    FocusGraphNode(Diag.Node);
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

            DrawDiagnosticRows(CompilationResult.Errors, /*bIsError*/ true);

            // A failed compile can still have produced warnings; dropping them here would make them appear
            // only once the error is fixed, which reads as the fix having caused them.
            if (!CompilationResult.Warnings.empty())
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.72f, 0.30f, 1.0f));
                ImGui::Text("%d warning%s", static_cast<int>(CompilationResult.Warnings.size()),
                    CompilationResult.Warnings.size() == 1 ? "" : "s");
                ImGui::PopStyleColor();
                ImGui::Separator();
                ImGui::Spacing();
                DrawDiagnosticRows(CompilationResult.Warnings, /*bIsError*/ false);
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

        // FIRST, above the stats. These describe a material that compiled and renders, so nothing else
        // flags them -- put anywhere lower they would sit below a fold nobody scrolls past.
        if (!CompilationResult.Warnings.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.72f, 0.30f, 1.0f));
            ImGui::Text("%d warning%s", static_cast<int>(CompilationResult.Warnings.size()),
                CompilationResult.Warnings.size() == 1 ? "" : "s");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();

            DrawDiagnosticRows(CompilationResult.Warnings, /*bIsError*/ false);

            ImGui::Spacing();
        }

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

        // Everything above is derived from the GENERATED SOURCE -- op counts, parameter counts, a weighted
        // cost estimate. Useful for comparing materials, but blind to the two things that actually decide
        // what a pixel shader costs on the hardware, both of which only appear once the driver has compiled
        // it. That is what this section is.
        DrawGPUStats(LabelWidth, HeaderColor, LabelColor, ValueColor);

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

        // Before the failure branch: warnings survive a failed compile, and the log should carry them
        // whether or not the material built.
        for (const EdNodeGraph::FError& Warning : CompileResult.Warnings)
        {
            CompilationResult.CompilationLog += "WARNING - [" + Warning.Name + "]: " + Warning.Description + "\n";

            FCompilationError Structured;
            Structured.Title       = Warning.Name;
            Structured.Description = Warning.Description;
            Structured.Node        = Warning.Node;
            CompilationResult.Warnings.push_back(Move(Structured));
        }

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
