#include "AnimationGraphEditorTool.h"

#include <cstdio>
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/Customizations/BonePickerContext.h"
#include "UI/Properties/Customizations/ParameterPickerContext.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_State.h"
#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "World/WorldManager.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/BlackboardComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"

namespace Lumina
{
    static const char* AnimationGraphWindowName = "Animation Graph";
    static const char* GraphPropertiesWindowName = "Graph Properties";
    static const char* GraphParametersWindowName = "Parameters";

    FAnimationGraphEditorTool::FAnimationGraphEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , NodeGraph(nullptr)
    {
    }

    void FAnimationGraphEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(AnimationGraphWindowName, [this](bool /*bFocused*/)
        {
            DrawGraphWindow();
        });

        CreateToolWindow(GraphPropertiesWindowName, [this](bool /*bFocused*/)
        {
            DrawPropertiesWindow();
        });

        CreateToolWindow(GraphParametersWindowName, [this](bool /*bFocused*/)
        {
            DrawParametersWindow();
        });

        // Editor node graph is a sibling sub-object in the asset's package (like the
        // material editor); created on first open, reloaded thereafter.
        FString GraphName = "AssetAnimationGraph";
        NodeGraph = Cast<CAnimationGraphNodeGraph>(Asset->GetPackage()->LoadObjectByName(GraphName));

        if (NodeGraph == nullptr)
        {
            NodeGraph = NewObject<CAnimationGraphNodeGraph>(Asset->GetPackage(), GraphName);
        }

        NodeGraph->SetAnimationGraph(Cast<CAnimationGraph>(Asset.Get()));

        // Seed the navigation stack with the top-level graph. EnterGraph readies
        // it (creates its context, wires callbacks) and pushes it.
        EnterGraph(NodeGraph, "Animation Graph");

        // Seed the runtime asset with bytecode so the preview viewport has
        // something to evaluate on the very first frame.
        Compile(false);
    }

    void FAnimationGraphEditorTool::OnDeinitialize(const FUpdateContext& /*UpdateContext*/)
    {
        // Tear down every node-editor context this tool created (the top graph
        // plus any nested state machine / blend-tree canvases that were opened).
        for (CEdNodeGraph* Graph : InitializedGraphs)
        {
            if (Graph != nullptr)
            {
                Graph->Shutdown();
            }
        }
        InitializedGraphs.clear();
        GraphStack.clear();
        NodeGraph = nullptr;
    }

    void FAnimationGraphEditorTool::WireGraphCallbacks(CEdNodeGraph* Graph)
    {
        Graph->SetNodeSelectedCallback([this](CEdGraphNode* Node)
        {
            if (Node != nullptr)
            {
                if (Node != SelectedNode || SelectedTransition != nullptr)
                {
                    SelectedNode = Node;
                    SelectedTransition = nullptr;
                    GetPropertyTable()->SetObject(Node, Node->GetClass());
                }
            }
            else if (SelectedNode != nullptr)
            {
                SelectedNode = nullptr;
                // A transition may be (re)selected later this same frame by the
                // link callback; only fall back to the asset if not.
                if (SelectedTransition == nullptr)
                {
                    GetPropertyTable()->SetObject(Asset, Asset->GetClass());
                }
            }
        });

        Graph->SetPreNodeDeletedCallback([this](const CEdGraphNode* Node)
        {
            // Deleting a State node also drops its transitions, so clear any
            // inspected transition defensively rather than risk a stale pointer.
            if (Node == SelectedNode || SelectedTransition != nullptr)
            {
                SelectedNode = nullptr;
                SelectedTransition = nullptr;
                GetPropertyTable()->SetObject(Asset, Asset->GetClass());
            }
        });

        Graph->SetNodeDoubleClickedCallback([this](CEdGraphNode* Node)
        {
            if (Node == nullptr)
            {
                return;
            }
            if (CEdNodeGraph* SubGraph = Node->GetEnterableSubGraph())
            {
                FString Label = Node->GetNodeDisplayName();
                if (CAnimGraphNode_State* StateNode = Cast<CAnimGraphNode_State>(Node))
                {
                    Label = StateNode->StateName.IsNone() ? FString("State") : StateNode->StateName.ToString();
                }
                EnterGraph(SubGraph, Label);
            }
        });

        Graph->SetLinkSelectedCallback([this](CEdNodeGraphPin* PinA, CEdNodeGraphPin* PinB)
        {
            // No single link selected -> drop a previously inspected transition.
            if (PinA == nullptr || PinB == nullptr)
            {
                if (SelectedTransition != nullptr)
                {
                    SelectedTransition = nullptr;
                    GetPropertyTable()->SetObject(Asset, Asset->GetClass());
                }
                return;
            }

            // Links are emitted as (input pin, connected output pin); be order
            // agnostic anyway.
            CEdNodeGraphPin* InPin  = PinA->bInputPin ? PinA : PinB;
            CEdNodeGraphPin* OutPin = PinA->bInputPin ? PinB : PinA;

            CAnimGraphNode_State* ToState   = Cast<CAnimGraphNode_State>(InPin->GetOwningNode());
            CAnimGraphNode_State* FromState = Cast<CAnimGraphNode_State>(OutPin->GetOwningNode());
            if (ToState == nullptr || FromState == nullptr)
            {
                // Not a transition wire (e.g. the Entry link) -- leave as-is.
                return;
            }

            CAnimStateMachineGraph* SMGraph = GraphStack.empty()
                ? nullptr
                : Cast<CAnimStateMachineGraph>(GraphStack.back().Graph);
            if (SMGraph == nullptr)
            {
                return;
            }

            CAnimStateTransition* Transition = SMGraph->FindTransition(FromState->GetNodeID(), ToState->GetNodeID());
            if (Transition != nullptr && Transition != SelectedTransition)
            {
                SelectedTransition = Transition;
                SelectedNode = nullptr;
                GetPropertyTable()->SetObject(Transition, Transition->GetClass());
            }
        });
    }

    void FAnimationGraphEditorTool::EnsureGraphReady(CEdNodeGraph* Graph)
    {
        if (Graph == nullptr || InitializedGraphs.find(Graph) != InitializedGraphs.end())
        {
            return;
        }
        InitializedGraphs.insert(Graph);
        Graph->Initialize();          // guarded internally; creates the context
        WireGraphCallbacks(Graph);
    }

    void FAnimationGraphEditorTool::EnterGraph(CEdNodeGraph* Graph, const FString& Label)
    {
        if (Graph == nullptr)
        {
            return;
        }

        EnsureGraphReady(Graph);
        GraphStack.push_back({ Graph, Label });

        // Reset the inspector on descent so it doesn't show a node from the graph we left;
        // cached transition tables are tied to the outer canvas and would dangle.
        SelectedNode = nullptr;
        SelectedTransition = nullptr;
        TransitionTables.clear();
        GetPropertyTable()->SetObject(Asset, Asset->GetClass());
    }

    void FAnimationGraphEditorTool::PopToLevel(int32 Index)
    {
        if (Index < 0 || Index >= (int32)GraphStack.size() - 1)
        {
            return;
        }
        GraphStack.resize(Index + 1);

        SelectedNode = nullptr;
        SelectedTransition = nullptr;
        GetPropertyTable()->SetObject(Asset, Asset->GetClass());
    }

    void FAnimationGraphEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        CreateFloorPlane();

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        CameraState.Speed = 5.0f;

        // The skeleton may already be set (reopening a configured asset) or not
        // (a fresh graph). SyncPreviewMesh handles both, and re-runs every frame.
        SyncPreviewMesh();
    }

    void FAnimationGraphEditorTool::SyncPreviewMesh()
    {
        if (!World.IsValid())
        {
            return;
        }

        CAnimationGraph* Graph = Cast<CAnimationGraph>(Asset.Get());
        const bool bHasPreview = Graph != nullptr
            && Graph->Skeleton.IsValid()
            && Graph->Skeleton->PreviewMesh.IsValid();

        const bool bMeshEntityValid = MeshEntity != entt::null && World->IsValidEntity(MeshEntity);

        // Skeleton cleared while the tool is open -> tear the preview down.
        if (!bHasPreview)
        {
            if (bMeshEntityValid)
            {
                World->DestroyEntity(MeshEntity);
            }
            MeshEntity = entt::null;
            return;
        }

        CSkeletalMesh* PreviewMesh = Graph->Skeleton->PreviewMesh;

        if (!bMeshEntityValid)
        {
            MeshEntity = World->ConstructEntity("Preview Mesh");
            World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SkeletalMesh = PreviewMesh;
            World->EmplaceComponent<SAnimationGraphComponent>(MeshEntity).Graph = Graph;
            // Drive parameter values through a blackboard instance, exactly like
            // a real entity would; the Parameters panel writes into it.
            World->EmplaceComponent<SBlackboardComponent>(MeshEntity).Blackboard = Graph->Blackboard;

            STransformComponent& MeshTransform   = World->GetComponent<STransformComponent>(MeshEntity);
            STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

            FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetLocation() + FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
            EditorTransform.SetRotation(Rotation);
            return;
        }

        // Entity exists -> keep its mesh / graph references current in case the
        // skeleton's preview mesh or the graph asset changed underneath us.
        SSkeletalMeshComponent& MeshComp = World->GetComponent<SSkeletalMeshComponent>(MeshEntity);
        if (MeshComp.SkeletalMesh.Get() != PreviewMesh)
        {
            MeshComp.SkeletalMesh = PreviewMesh;
        }

        SAnimationGraphComponent& GraphComp = World->GetComponent<SAnimationGraphComponent>(MeshEntity);
        if (GraphComp.Graph.Get() != Graph)
        {
            GraphComp.Graph = Graph;
        }

        // Keep the preview blackboard instance pointed at the graph's schema.
        SBlackboardComponent& BlackboardComp = World->GetOrEmplaceComponent<SBlackboardComponent>(MeshEntity);
        if (BlackboardComp.Blackboard.Get() != Graph->Blackboard.Get())
        {
            BlackboardComp.Blackboard = Graph->Blackboard;
        }
    }

    void FAnimationGraphEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        // The skeleton is frequently assigned after the tool is already open;
        // pick that up here rather than only at world setup.
        SyncPreviewMesh();

        // Live preview: keep the runtime asset's bytecode in sync with the node
        // graph so edits resolve in the viewport without a manual compile.
        if (bAutoCompile)
        {
            Compile(false);
        }

        // Drive the preview mesh's parameters from the Parameters panel so
        // transition conditions and Get Parameter nodes actually do something.
        PushParameterOverrides();
    }

    void FAnimationGraphEditorTool::PushParameterOverrides()
    {
        if (!World.IsValid() || MeshEntity == entt::null)
        {
            return;
        }

        if (!World->IsValidEntity(MeshEntity))
        {
            return;
        }

        SBlackboardComponent* BlackboardComp = World->TryGetComponent<SBlackboardComponent>(MeshEntity);
        if (BlackboardComp == nullptr)
        {
            return;
        }

        // Push live panel values into the preview entity's blackboard; the anim system
        // resolves them into the VM each frame as for gameplay. Unknown keys are created.
        for (const auto& [Name, Value] : ParameterOverrides)
        {
            BlackboardComp->SetFloat(Name, Value);
        }
    }

    void FAnimationGraphEditorTool::DrawToolMenu(const FUpdateContext& /*UpdateContext*/)
    {
        if (ImGui::MenuItem(LE_ICON_COG " Compile"))
        {
            Compile();
        }
    }

    void FAnimationGraphEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, bottomDockID = 0;

        // Right pane: properties. Left pane splits into a preview viewport on
        // top and the node graph canvas below it.
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.25f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(leftDockID, ImGuiDir_Up, 0.45f, &leftDockID, &bottomDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(),            leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(AnimationGraphWindowName).c_str(),      bottomDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(GraphPropertiesWindowName).c_str(),     rightDockID);
        // Parameters share the right pane, tabbed behind Properties.
        ImGui::DockBuilderDockWindow(GetToolWindowName(GraphParametersWindowName).c_str(),     rightDockID);
    }

    void FAnimationGraphEditorTool::DrawBreadcrumbBar()
    {
        for (int32 i = 0; i < (int32)GraphStack.size(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextUnformatted(">");
                ImGui::SameLine(0.0f, 4.0f);
            }

            const bool bIsCurrent = (i == (int32)GraphStack.size() - 1);
            ImGui::BeginDisabled(bIsCurrent);
            if (ImGui::Button(GraphStack[i].Label.c_str()))
            {
                PopToLevel(i);
            }
            ImGui::EndDisabled();
        }

        ImGui::Separator();
    }

    void FAnimationGraphEditorTool::DrawGraphWindow()
    {
        DrawBreadcrumbBar();

        UpdateDebugOverlay();

        if (!GraphStack.empty() && GraphStack.back().Graph != nullptr)
        {
            GraphStack.back().Graph->DrawGraph();
        }
    }

    void FAnimationGraphEditorTool::DrawPreviewControls()
    {
        CAnimationGraph* Graph = Cast<CAnimationGraph>(Asset.Get());
        if (Graph == nullptr)
        {
            return;
        }

        if (!Graph->Skeleton.IsValid())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "No skeleton assigned -- set one on the graph asset to see a preview mesh.");
        }
        else if (!Graph->Skeleton->PreviewMesh.IsValid())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "Skeleton has no preview mesh assigned.");
        }
    }

    void FAnimationGraphEditorTool::DrawPropertiesWindow()
    {
        CAnimationGraph* Graph = Cast<CAnimationGraph>(Asset.Get());
        const FSkeletonResource* ActiveSkeleton = (Graph != nullptr && Graph->Skeleton.IsValid())
            ? Graph->Skeleton->GetSkeletonResource()
            : nullptr;
        BonePickerContext::FScope      BonePickerScope(ActiveSkeleton);
        ParameterPickerContext::FScope ParamPickerScope(Graph);

        if (ImGui::Button(LE_ICON_COG " Compile"))
        {
            Compile();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto Compile", &bAutoCompile);

        ImGui::SameLine();
        ImGui::Checkbox("Debug", &bDebugEnabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Animate flow, show live pin values, and highlight the active state from the selected target");
        }

        if (bDebugEnabled)
        {
            DrawDebugTargetCombo();
        }

        ImGui::SameLine();
        if (bHasCompilationErrors)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Compile failed");
        }
        else if (!CompilationLog.empty())
        {
            ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "Compiled");
        }

        ImGui::Separator();

        DrawPreviewControls();

        ImGui::Separator();

        // Contextual hint: on the state machine canvas, transitions are edited
        // by selecting their wire -- not obvious without a nudge.
        if (SelectedNode == nullptr && SelectedTransition == nullptr &&
            !GraphStack.empty() && Cast<CAnimStateMachineGraph>(GraphStack.back().Graph) != nullptr)
        {
            ImGui::TextWrapped("Tip: click a transition wire between two States to edit its "
                "condition here. Click a State node to rename it; double-click to edit its blend tree.");
            ImGui::Separator();
        }

        GetPropertyTable()->DrawTree();

        // When a State node is selected, inline-list its outgoing transitions
        // so the user can edit conditions without having to click each wire.
        if (CAnimGraphNode_State* StateNode = Cast<CAnimGraphNode_State>(SelectedNode))
        {
            DrawOutgoingTransitionsForState(StateNode);
        }

        if (!CompilationLog.empty())
        {
            ImGui::Separator();
            ImGui::TextUnformatted(CompilationLog.c_str());
        }
    }

    void FAnimationGraphEditorTool::DrawOutgoingTransitionsForState(CAnimGraphNode_State* State)
    {
        if (State == nullptr || GraphStack.empty())
        {
            return;
        }

        CAnimStateMachineGraph* SMGraph = Cast<CAnimStateMachineGraph>(GraphStack.back().Graph);
        if (SMGraph == nullptr)
        {
            return;
        }

        TVector<CAnimStateTransition*> Outgoing;
        for (const TObjectPtr<CAnimStateTransition>& T : SMGraph->GetTransitions())
        {
            if (T.IsValid() && T->FromStateNodeID == State->GetNodeID())
            {
                Outgoing.push_back(T.Get());
            }
        }

        // Drop cached property tables whose backing transition was removed.
        for (auto It = TransitionTables.begin(); It != TransitionTables.end(); )
        {
            const bool bAlive = eastl::find(Outgoing.begin(), Outgoing.end(), It->first) != Outgoing.end();
            if (!bAlive)
            {
                It = TransitionTables.erase(It);
            }
            else
            {
                ++It;
            }
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 220, 222, 255));
        ImGui::TextUnformatted(LE_ICON_ARROW_RIGHT_BOLD " Outgoing Transitions");
        ImGui::PopStyleColor();

        if (Outgoing.empty())
        {
            ImGui::TextDisabled("Drag from this State's Out pin to another State to create one.");
            return;
        }

        for (CAnimStateTransition* Transition : Outgoing)
        {
            ImGui::PushID(Transition);

            CAnimGraphNode_State* ToState = nullptr;
            for (CEdGraphNode* N : SMGraph->Nodes)
            {
                CAnimGraphNode_State* S = Cast<CAnimGraphNode_State>(N);
                if (S != nullptr && S->GetNodeID() == Transition->ToStateNodeID)
                {
                    ToState = S;
                    break;
                }
            }

            const FString ToLabel = (ToState != nullptr && !ToState->StateName.IsNone())
                ? ToState->StateName.ToString()
                : FString("(unnamed)");
            const FString Header = FString("\xE2\x86\x92 ") + ToLabel;

            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader(Header.c_str()))
            {
                auto It = TransitionTables.find(Transition);
                if (It == TransitionTables.end())
                {
                    TUniquePtr<FPropertyTable> NewTable = MakeUnique<FPropertyTable>(Transition);
                    It = TransitionTables.emplace(Transition, Move(NewTable)).first;
                }
                It->second->DrawTree();
            }

            ImGui::PopID();
        }
    }

    CEnum* FAnimationGraphEditorTool::ResolveReflectedEnum(const FName& Name)
    {
        if (!bEnumCacheBuilt)
        {
            bEnumCacheBuilt = true;
            GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
            {
                if (Object != nullptr && Object->IsA<CEnum>())
                {
                    CEnum* Enum = static_cast<CEnum*>(Object);
                    ReflectedEnumCache[Enum->GetName()] = Enum;
                }
            });
        }

        auto It = ReflectedEnumCache.find(Name);
        return It == ReflectedEnumCache.end() ? nullptr : It->second;
    }

    void FAnimationGraphEditorTool::DrawParametersWindow()
    {
        CAnimationGraph* Graph = Cast<CAnimationGraph>(Asset.Get());
        if (Graph == nullptr)
        {
            return;
        }

        ImGui::TextWrapped("Live values for the assigned Blackboard's keys. Set them here to test the "
            "graph in the preview viewport; at runtime an entity's Blackboard Component supplies these.");
        ImGui::Separator();

        CBlackboard* Blackboard = Graph->Blackboard.Get();
        if (Blackboard == nullptr)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "No Blackboard assigned. Set one on the graph asset");
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "(Graph Properties) to declare parameters.");
            return;
        }

        if (Blackboard->Keys.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "Blackboard has no keys. Open it and add some.");
            return;
        }

        for (const FBlackboardKey& Key : Blackboard->Keys)
        {
            if (Key.Name.IsNone())
            {
                continue;
            }

            // Numeric keys (Float / Int / Bool / Enum) drive the animation VM and
            // are live-editable here. Vector / Object exist for the AI system.
            if (Key.Type == EBlackboardKeyType::Vector || Key.Type == EBlackboardKeyType::Object)
            {
                ImGui::TextDisabled("%s (%s)", Key.Name.c_str(),
                    Key.Type == EBlackboardKeyType::Vector ? "Vector" : "Object");
                continue;
            }

            // Seed the live value from the key's default the first time we see it.
            auto It = ParameterOverrides.find(Key.Name);
            if (It == ParameterOverrides.end())
            {
                float Seed = Key.DefaultFloat;
                switch (Key.Type)
                {
                case EBlackboardKeyType::Int:
                case EBlackboardKeyType::Enum: Seed = (float)Key.DefaultInt;             break;
                case EBlackboardKeyType::Bool: Seed = Key.DefaultBool ? 1.0f : 0.0f;     break;
                default: break;
                }
                It = ParameterOverrides.emplace(Key.Name, Seed).first;
            }

            float& Value = It->second;
            const char* Name = Key.Name.c_str();

            const bool bReadOnly = EnumHasAnyFlags(Key.Flags, EBlackboardKeyFlags::ReadOnly);

            ImGui::PushID(Name);
            ImGui::BeginDisabled(bReadOnly);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            switch (Key.Type)
            {
            case EBlackboardKeyType::Bool:
            {
                bool bValue = Value != 0.0f;
                if (ImGui::Checkbox(Name, &bValue)) Value = bValue ? 1.0f : 0.0f;
                break;
            }
            case EBlackboardKeyType::Int:
            {
                int IntValue = (int)Math::Round(Value);
                if (ImGui::DragInt(Name, &IntValue)) Value = (float)IntValue;
                break;
            }
            case EBlackboardKeyType::Enum:
            {
                CEnum* Enum = ResolveReflectedEnum(Key.EnumType);
                if (Enum != nullptr)
                {
                    const int Current = (int)Math::Round(Value);

                    int32 CurrentIndex = INDEX_NONE;
                    for (int64 e = 0; e < (int64)Enum->Names.size(); ++e)
                    {
                        if ((int)Enum->GetValueAtIndex(e) == Current)
                        {
                            CurrentIndex = (int32)e;
                            break;
                        }
                    }

                    const FFixedString Preview = Enum->GetNameAtValue((uint64)Current).c_str();
                    const int32 Picked = ImGuiX::SearchableCombo(Name, Preview.c_str(), (int32)Enum->Names.size(), CurrentIndex,
                        [Enum](int32 Index) { return FFixedString(Enum->GetNameAtIndex(Index).c_str()); }, LE_ICON_RHOMBUS_OUTLINE);

                    if (Picked != INDEX_NONE)
                    {
                        Value = (float)(int)Enum->GetValueAtIndex(Picked);
                    }
                }
                else
                {
                    int IntValue = (int)Math::Round(Value);
                    if (ImGui::DragInt(Name, &IntValue)) Value = (float)IntValue;
                }
                break;
            }
            default:
                ImGui::DragFloat(Name, &Value, 0.01f);
                break;
            }
            ImGui::EndDisabled();
            if (bReadOnly && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGuiX::TextTooltip_Internal("Read-only key");
            }
            ImGui::PopID();
        }
    }

    void FAnimationGraphEditorTool::Compile(bool bMarkPackageDirty)
    {
        CompilationLog.clear();
        bHasCompilationErrors = false;

        CAnimationGraph* Graph = Cast<CAnimationGraph>(Asset.Get());
        if (Graph == nullptr || NodeGraph == nullptr)
        {
            return;
        }

        FAnimationGraphCompiler Compiler;

        // Resolve bone-mask names to per-bone weight arrays up front so Layered Blend
        // Per Bone nodes can look up by name during GenerateBytecode.
        if (Graph->Skeleton.IsValid())
        {
            Compiler.ResolveBoneMasks(Graph->BoneMaskDefs, Graph->Skeleton->GetSkeletonResource());
        }

        // Seed the compiler with parameters already declared on the asset so they persist
        // across compiles even if no node/transition references them yet.
        for (const FAnimGraphParameter& Param : Graph->Parameters)
        {
            Compiler.AddParameter(Param.Name, Param.Type, Param.DefaultValue);
        }

        // Give the compiler the blackboard schema so it can warn when a node
        // references a key that's been renamed / removed / retyped.
        Compiler.SetBlackboard(Graph->Blackboard.Get());

        NodeGraph->CompileGraph(Compiler);

        // Non-fatal diagnostics first, so they're visible whether or not the
        // compile also produced hard errors.
        for (const EdNodeGraph::FError& Warning : Compiler.GetWarnings())
        {
            CompilationLog += "WARNING - [" + Warning.Name + "]: " + Warning.Description + "\n";
        }

        if (Compiler.HasErrors())
        {
            bHasCompilationErrors = true;
            for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
            {
                CompilationLog += "ERROR - [" + Error.Name + "]: " + Error.Description + "\n";
            }
            return;
        }

        Compiler.BuildGraph(Graph);

        // Snapshot pin->register and state-node mappings so the debug overlay can read live
        // VM values back onto the graph; pin pointers stay valid (re-run every frame).
        DebugPinRegisters = Compiler.GetPinRegisters();
        DebugStateNodes   = Compiler.GetDebugStateNodes();

        if (bMarkPackageDirty)
        {
            Asset->GetPackage()->MarkDirty();
        }

        CompilationLog += "Animation graph compiled successfully.\n";
    }

    void FAnimationGraphEditorTool::UpdateDebugOverlay()
    {
        DebugPinValues.clear();
        DebugActiveNodes.clear();

        CEdNodeGraph* Graph = GraphStack.empty() ? nullptr : GraphStack.back().Graph;
        if (Graph == nullptr)
        {
            return;
        }

        if (!bDebugEnabled)
        {
            Graph->ClearDebugContext();
            return;
        }

        // Resolve the debug target: null world = editor preview, else a live entity from the
        // dropdown. A stale target (world/entity gone, e.g. PIE ended) reverts to preview.
        CWorld* TargetWorld = DebugTargetWorld.Get();
        entt::entity TargetEntity = DebugTargetEntity;
        if (TargetWorld == nullptr)
        {
            TargetWorld  = World.Get();
            TargetEntity = MeshEntity;
        }

        const FAnimGraphVMState* VMState = nullptr;
        if (TargetWorld != nullptr && TargetEntity != entt::null)
        {
            if (TargetWorld->IsValidEntity(TargetEntity))
            {
                if (SAnimationGraphComponent* Comp = TargetWorld->TryGetComponent<SAnimationGraphComponent>(TargetEntity))
                {
                    VMState = &Comp->VMState;
                }
            }
        }

        if (VMState != nullptr)
        {
            // Live scalar values onto value pins.
            const TVector<float>& Scalars = VMState->ScalarRegisters;
            for (const auto& [Pin, Reg] : DebugPinRegisters)
            {
                CAnimGraphPin* AnimPin = Cast<CAnimGraphPin>(const_cast<CEdNodeGraphPin*>(Pin));
                if (AnimPin == nullptr || AnimPin->GetPinType() != EAnimPinType::Value || Reg >= Scalars.size())
                {
                    continue;
                }

                char Buffer[32];
                snprintf(Buffer, sizeof(Buffer), "%.2f", Scalars[Reg]);
                DebugPinValues[const_cast<CEdNodeGraphPin*>(Pin)] = Buffer;
            }

            // Highlight whichever State node the VM is currently in.
            const TVector<float>& Slots = VMState->StateSlots;
            for (const FAnimGraphDebugStateNode& Entry : DebugStateNodes)
            {
                if (Entry.CurrentStateSlot < Slots.size() &&
                    (int32)(Slots[Entry.CurrentStateSlot] + 0.5f) == Entry.StateIndex)
                {
                    DebugActiveNodes.insert(Entry.Node);
                }
            }
        }

        CEdNodeGraph::FGraphDebugContext Context;
        Context.bEnabled    = true;
        Context.bFlowLinks  = true;
        Context.PinValues   = &DebugPinValues;
        Context.ActiveNodes = &DebugActiveNodes;
        Graph->SetDebugContext(Context);
    }

    void FAnimationGraphEditorTool::DrawDebugTargetCombo()
    {
        static auto WorldTypeLabel = [](EWorldType Type) -> const char*
        {
            switch (Type)
            {
            case EWorldType::Game:       return "Game";
            case EWorldType::Simulation: return "Sim";
            case EWorldType::Editor:     return "Editor";
            default:                     return "World";
            }
        };

        CAnimationGraph* AssetGraph = Cast<CAnimationGraph>(Asset.Get());

        // Resolve the current selection's label; revert to preview if it's gone.
        FString CurrentLabel = "Preview";
        if (CWorld* CurWorld = DebugTargetWorld.Get())
        {
            if (CurWorld->IsValidEntity(DebugTargetEntity))
            {
                const SNameComponent* Name = CurWorld->TryGetComponent<SNameComponent>(DebugTargetEntity);
                CurrentLabel = Name ? Name->Name.ToString() : FString("Entity");
            }
            else
            {
                DebugTargetWorld  = nullptr;
                DebugTargetEntity = entt::null;
            }
        }

        // Flatten candidates ("Preview" + every live entity across worlds running this graph)
        // into one indexable list so the searchable picker can select by index.
        TVector<FString> Labels;
        TVector<TPair<CWorld*, entt::entity>> Targets;
        Labels.push_back("Preview");
        Targets.push_back({ nullptr, static_cast<entt::entity>(entt::null) });

        int32 CurrentIndex = DebugTargetWorld.IsValid() ? INDEX_NONE : 0;

        if (GWorldManager != nullptr && AssetGraph != nullptr)
        {
            for (const TUniquePtr<FWorldContext>& Ctx : GWorldManager->GetContexts())
            {
                CWorld* CandidateWorld = Ctx->World.Get();
                if (CandidateWorld == nullptr || CandidateWorld == World.Get())
                {
                    continue;
                }

                for (entt::entity Entity : CandidateWorld->View<SAnimationGraphComponent>())
                {
                    if (CandidateWorld->GetComponent<SAnimationGraphComponent>(Entity).Graph.Get() != AssetGraph)
                    {
                        continue;
                    }

                    const SNameComponent* Name = CandidateWorld->TryGetComponent<SNameComponent>(Entity);
                    Labels.push_back((Name ? Name->Name.ToString() : FString("Entity"))
                        + "  (" + WorldTypeLabel(Ctx->Type) + ")");
                    Targets.push_back({ CandidateWorld, Entity });

                    if (DebugTargetWorld.Get() == CandidateWorld && DebugTargetEntity == Entity)
                    {
                        CurrentIndex = (int32)Targets.size() - 1;
                    }
                }
            }
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        const int32 Picked = ImGuiX::SearchableCombo("##DebugTarget", CurrentLabel.c_str(), (int32)Labels.size(), CurrentIndex,
            [&Labels](int32 Index) { return FFixedString(Labels[Index].c_str()); });

        if (Picked != INDEX_NONE)
        {
            DebugTargetWorld  = Targets[Picked].first;
            DebugTargetEntity = Targets[Picked].second;
        }
    }

    void FAnimationGraphEditorTool::OnSave()
    {
        Compile();
        FAssetEditorTool::OnSave();
    }
}
