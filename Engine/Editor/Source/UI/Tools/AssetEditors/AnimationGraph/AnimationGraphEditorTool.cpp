#include "AnimationGraphEditorTool.h"

#include <cfloat>
#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Containers/StringFormat.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_ClipPlayer.h"
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
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_State.h"
#include "World/WorldManager.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    static const char* AnimationGraphWindowName = "Animation Graph";
    static const char* GraphPropertiesWindowName = "Graph Properties";
    static const char* GraphParametersWindowName = "Parameters";
    static const char* GraphTasksWindowName = "Task Graph";
    static const char* GraphClipsWindowName = "Animation Clips";

    // Presentation helpers for the Task Graph window. Internal linkage: these names are generic
    // enough to collide with other translation units' Detail helpers.
    namespace
    {
        namespace AnimGraphDetail
        {
            const char* TaskTypeName(EAnimTaskType Type)
            {
                switch (Type)
                {
                case EAnimTaskType::ReferencePose:      return "Reference Pose";
                case EAnimTaskType::SampleClip:         return "Sample Clip";
                case EAnimTaskType::Blend:              return "Blend";
                case EAnimTaskType::BlendMasked:        return "Layered Blend";
                case EAnimTaskType::MakeAdditive:       return "Make Additive";
                case EAnimTaskType::ApplyAdditive:      return "Apply Additive";
                case EAnimTaskType::StateMachineOutput: return "State Machine";
                case EAnimTaskType::BoneTransform:      return "Bone Transform";
                case EAnimTaskType::TwoBoneIK:          return "Two Bone IK";
                }
                return "Unknown";
            }

            // Category hue so the shape of a recipe reads without parsing labels: pose sources green,
            // blends blue, additive violet, state machine amber, bone ops teal.
            ImVec4 TaskTypeColor(EAnimTaskType Type)
            {
                switch (Type)
                {
                case EAnimTaskType::ReferencePose:      return ImVec4(0.56f, 0.59f, 0.64f, 1.0f);
                case EAnimTaskType::SampleClip:         return ImVec4(0.38f, 0.76f, 0.47f, 1.0f);
                case EAnimTaskType::Blend:
                case EAnimTaskType::BlendMasked:        return ImVec4(0.36f, 0.62f, 0.92f, 1.0f);
                case EAnimTaskType::MakeAdditive:
                case EAnimTaskType::ApplyAdditive:      return ImVec4(0.70f, 0.52f, 0.90f, 1.0f);
                case EAnimTaskType::StateMachineOutput: return ImVec4(0.93f, 0.68f, 0.33f, 1.0f);
                case EAnimTaskType::BoneTransform:
                case EAnimTaskType::TwoBoneIK:          return ImVec4(0.40f, 0.80f, 0.80f, 1.0f);
                }
                return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
            }

            FString BuildTaskDetail(const FAnimTaskDebugEntry& Entry)
            {
                switch (Entry.Type)
                {
                case EAnimTaskType::SampleClip:
                    // Time first: it's the per-frame liveness signal, and clip names are long enough
                    // that the tail is what gets ellipsized on a narrow box.
                    return Format("t={:.3f}s   {}", Entry.Time,
                                  Entry.ClipName.IsNone() ? "<no clip>" : Entry.ClipName.c_str());

                case EAnimTaskType::BlendMasked:
                    return Format("alpha {:.2f}   mask {}/{} bones",
                                  Entry.Alpha, Entry.MaskWeightedBones, Entry.MaskTotalBones);

                case EAnimTaskType::Blend:
                case EAnimTaskType::ApplyAdditive:
                case EAnimTaskType::BoneTransform:
                case EAnimTaskType::TwoBoneIK:
                    return Format("alpha {:.2f}", Entry.Alpha);

                case EAnimTaskType::StateMachineOutput:
                    return Format("inertialization t={:.3f}s", Entry.Time);

                case EAnimTaskType::MakeAdditive:
                    return FString("relative to bind pose");

                case EAnimTaskType::ReferencePose:
                    return FString("skeleton bind pose");
                }
                return FString();
            }

            // Trims Text to MaxWidth with a trailing ellipsis. Measured with the same font and size
            // used to draw it, so boxes stay clean at any zoom or editor DPI scale rather than
            // hard-clipping mid-glyph.
            FString FitText(ImFont* Font, float FontSize, const char* Text, float MaxWidth)
            {
                if (Text == nullptr || Text[0] == '\0')
                {
                    return FString();
                }
                if (Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Text).x <= MaxWidth)
                {
                    return FString(Text);
                }

                const char* Ellipsis = "...";
                const float Budget = MaxWidth - Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Ellipsis).x;
                if (Budget <= 0.0f)
                {
                    return FString(Ellipsis);
                }

                FString Result(Text);
                while (!Result.empty() &&
                       Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Result.c_str()).x > Budget)
                {
                    Result.pop_back();
                }
                Result += Ellipsis;
                return Result;
            }

            FString BuildDepText(const FAnimTaskDebugEntry& Entry)
            {
                if (Entry.DepA < 0 && Entry.DepB < 0)
                {
                    return FString("none (source task)");
                }
                if (Entry.DepB < 0)
                {
                    return Format("task {}", (int32)Entry.DepA);
                }
                return Format("task {}, task {}", (int32)Entry.DepA, (int32)Entry.DepB);
            }
        }
    }

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

        CreateToolWindow(GraphTasksWindowName, [this](bool /*bFocused*/)
        {
            DrawTaskGraphWindow();
        });

        CreateToolWindow(GraphClipsWindowName, [this](bool /*bFocused*/)
        {
            DrawClipBrowserWindow();
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

        // Duplicating a state used to hand the copy the original's sub-graph; this splits those apart.
        THashSet<CEdNodeGraph*> VisitedGraphs;
        VisitedGraphs.insert(NodeGraph);
        if (const uint32 Repaired = NodeGraph->UnaliasSubGraphs(VisitedGraphs))
        {
            Asset->GetPackage()->MarkDirty();
            ImGuiX::Notifications::NotifyWarning("Gave {0} duplicated node(s) their own sub-graph. Save to keep the split.", Repaired);
        }

        GetPropertyTable()->SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            if (Asset.IsValid())
            {
                Asset->GetPackage()->MarkDirty();
            }

            // A clip swap or a transition condition changes the compiled graph as much as rewiring does.
            if (NodeGraph != nullptr)
            {
                NodeGraph->NotifyContentChanged();
            }
        });

        // Seed the navigation stack with the top-level graph. EnterGraph readies
        // it (creates its context, wires callbacks) and pushes it.
        EnterGraph(NodeGraph, "Animation Graph");

        // Seed the runtime asset with bytecode so the preview viewport has
        // something to evaluate on the very first frame.
        Compile(false);
    }

    void FAnimationGraphEditorTool::OnDeinitialize(const FUpdateContext& /*UpdateContext*/)
    {
        // Never leave the runtime capture pointing at a component this tool no longer watches.
        Anim::DisarmTaskCapture();

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
        NavForwardStack.clear();
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
            // A deleted node takes its sub-graph with it, which forward history may still point at.
            NavForwardStack.clear();

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
                EnterGraph(SubGraph, Node->GetNodeTitleText());
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

            CAnimGraphNode_State* ToState = Cast<CAnimGraphNode_State>(InPin->GetOwningNode());
            CEdGraphNode* FromNode = OutPin->GetOwningNode();
            const bool bTransitionSource = FromNode != nullptr &&
                (FromNode->IsA<CAnimGraphNode_State>() || FromNode->IsA<CAnimGraphNode_StateAny>());
            if (ToState == nullptr || !bTransitionSource)
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

            CAnimStateTransition* Transition = SMGraph->FindTransition(FromNode->GetNodeID(), ToState->GetNodeID());
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

        // Descending somewhere new is what abandons the levels forward was going to retrace.
        NavForwardStack.clear();

        ClearGraphSelection();
    }

    void FAnimationGraphEditorTool::PopToLevel(int32 Index)
    {
        if (Index < 0 || Index >= (int32)GraphStack.size() - 1)
        {
            return;
        }

        // Deepest first, so the next forward step lands on the level just below Index.
        for (int32 i = (int32)GraphStack.size() - 1; i > Index; --i)
        {
            NavForwardStack.push_back(GraphStack[i]);
        }
        GraphStack.resize(Index + 1);

        ClearGraphSelection();
    }

    void FAnimationGraphEditorTool::NavigateBack()
    {
        if (GraphStack.size() < 2)
        {
            return;
        }

        NavForwardStack.push_back(GraphStack.back());
        GraphStack.pop_back();

        ClearGraphSelection();
    }

    void FAnimationGraphEditorTool::NavigateForward()
    {
        if (NavForwardStack.empty())
        {
            return;
        }

        FGraphStackEntry Entry = NavForwardStack.back();
        NavForwardStack.pop_back();

        EnsureGraphReady(Entry.Graph);
        GraphStack.push_back(Move(Entry));

        ClearGraphSelection();
    }

    void FAnimationGraphEditorTool::ClearGraphSelection()
    {
        // Cached transition tables are tied to the canvas being left and would dangle.
        SelectedNode = nullptr;
        SelectedTransition = nullptr;
        TransitionTables.clear();
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
            World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SetSkeletalMesh(PreviewMesh);
            World->EmplaceComponent<SAnimationGraphComponent>(MeshEntity).Graph = Graph;

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
            MeshComp.SetSkeletalMesh(PreviewMesh);
        }

        SAnimationGraphComponent& GraphComp = World->GetComponent<SAnimationGraphComponent>(MeshEntity);
        if (GraphComp.Graph.Get() != Graph)
        {
            GraphComp.Graph = Graph;
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
        if (bAutoCompile && NeedsCompile())
        {
            Compile(false);
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
        // Task Graph tabs behind the node canvas: same "what is this graph doing" workspace.
        ImGui::DockBuilderDockWindow(GetToolWindowName(GraphTasksWindowName).c_str(),          bottomDockID);
        // Clips sit with the other pickers on the right so they can be dragged onto the canvas.
        ImGui::DockBuilderDockWindow(GetToolWindowName(GraphClipsWindowName).c_str(),          rightDockID);
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

            // Nesting repeats labels, and a button IDs off its label.
            ImGui::PushID(i);
            ImGui::BeginDisabled(bIsCurrent);
            if (ImGui::Button(GraphStack[i].Label.c_str()))
            {
                PopToLevel(i);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }

        ImGui::Separator();
    }

    void FAnimationGraphEditorTool::DrawGraphWindow()
    {
        DrawBreadcrumbBar();

        UpdateDebugOverlay();

        // Thumb buttons, which GLFW reports as 3 and 4. Hovered, not focused, so no click-in first.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        {
            if (ImGui::IsMouseClicked(3))
            {
                NavigateBack();
            }
            else if (ImGui::IsMouseClicked(4))
            {
                NavigateForward();
            }
        }

        if (!GraphStack.empty() && GraphStack.back().Graph != nullptr)
        {
            const ImVec2 CanvasMin = ImGui::GetCursorScreenPos();
            const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
            GraphCanvasCenter = ImVec2(CanvasMin.x + CanvasSize.x * 0.5f, CanvasMin.y + CanvasSize.y * 0.5f);

            GraphStack.back().Graph->DrawGraph();

            // Clip drops onto the canvas. The node editor consumes the region itself, so the target
            // is registered over the whole window rect (same pattern as the outliner's empty area).
            if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->Rect(), ImGui::GetCurrentWindow()->ID))
            {
                if (CAnimation* DroppedClip = DragDrop::AcceptAsset<CAnimation>())
                {
                    SpawnClipPlayerNode(DroppedClip, ImGui::GetMousePos());
                }
                ImGui::EndDragDropTarget();
            }
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
        // by selecting their arrow -- not obvious without a nudge.
        if (SelectedNode == nullptr && SelectedTransition == nullptr &&
            !GraphStack.empty() && Cast<CAnimStateMachineGraph>(GraphStack.back().Graph) != nullptr)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("Click a transition's condition badge to edit it here. Drag from a State's right band "
                "onto another State's left band to add one. Click a State to rename it, double-click to edit its "
                "blend tree, and right-click the canvas to add a State or an Any State source.");
            ImGui::PopStyleColor();
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
        SMGraph->GetOutgoingTransitions(State->GetNodeID(), Outgoing);

        // Drop cached property tables whose backing transition was removed.
        for (auto It = TransitionTables.begin(); It != TransitionTables.end(); )
        {
            const bool bAlive = Algo::Find(Outgoing.begin(), Outgoing.end(), It->first) != Outgoing.end();
            if (!bAlive)
            {
                It = TransitionTables.erase(It);
            }
            else
            {
                ++It;
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::SectionHeader());
        ImGui::SeparatorText(LE_ICON_ARROW_RIGHT_BOLD " Outgoing Transitions");
        ImGui::PopStyleColor();

        if (Outgoing.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("None. Drag from this State's right edge onto another State to add one.");
            ImGui::PopStyleColor();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
        ImGui::TextWrapped("Tested top to bottom while this state is active; the first passing condition wins. "
                           "Reorder with Priority.");
        ImGui::PopStyleColor();

        for (int32 Index = 0; Index < (int32)Outgoing.size(); ++Index)
        {
            CAnimStateTransition* Transition = Outgoing[Index];
            ImGui::PushID(Transition);

            const FString ToLabel = SMGraph->GetEndpointLabel(Transition->ToStateNodeID);
            const FString Header = FString(LE_ICON_ARROW_RIGHT_BOLD " ") + ToLabel;

            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            const bool bOpen = ImGui::CollapsingHeader(Header.c_str());

            // Condition and blend length on the header row: the two things worth scanning without
            // expanding every entry.
            ImGui::SameLine();
            ImGui::TextColored(Transition->ConditionParameter.IsNone() ? EditorColors::Warning() : EditorColors::TextDim(),
                               "   %s", Transition->GetConditionText().c_str());
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::TextMuted(), "   %.2fs", Transition->BlendDuration);

            if (bOpen)
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

        DrawParameters(Graph);
        DrawLiveCurveValues(Graph);
    }

    void FAnimationGraphEditorTool::DrawLiveCurveValues(CAnimationGraph* Graph)
    {
        if (Graph->CurveNames.empty())
        {
            return;
        }

        ImGui::Spacing();
        if (!ImGui::CollapsingHeader(LE_ICON_CHART_BELL_CURVE " Curves", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        const FAnimGraphVMState* VMState = nullptr;
        CWorld* TargetWorld = nullptr;
        entt::entity TargetEntity = entt::null;
        if (ResolveDebugTarget(TargetWorld, TargetEntity))
        {
            if (SAnimationGraphComponent* Comp = TargetWorld->TryGetComponent<SAnimationGraphComponent>(TargetEntity))
            {
                VMState = &Comp->VMState;
            }
        }

        for (int32 i = 0; i < (int32)Graph->CurveNames.size(); ++i)
        {
            const bool bHasValue = VMState != nullptr && i < (int32)VMState->CurveValues.size();
            if (bHasValue)
            {
                ImGui::Text("%s", Graph->CurveNames[i].c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.6f);
                ImGui::Text("%.3f", VMState->CurveValues[i]);
            }
            else
            {
                ImGui::TextDisabled("%s", Graph->CurveNames[i].c_str());
            }
        }
    }

    void FAnimationGraphEditorTool::DrawParameters(CAnimationGraph* Graph)
    {
        ImGui::TextWrapped("Read-only view of the preview entity's live parameter block. At runtime gameplay "
            "code writes these directly on the Animation Graph Component.");
        ImGui::Separator();

        CStruct* Struct = Graph->GetParameterStruct();
        if (Struct == nullptr)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "No parameter struct assigned. Set one on the");
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "graph asset (Graph Properties) to declare parameters.");
            return;
        }

        SAnimationGraphComponent* Comp = (World.IsValid() && MeshEntity != entt::null && World->IsValidEntity(MeshEntity))
            ? World->TryGetComponent<SAnimationGraphComponent>(MeshEntity)
            : nullptr;

        void* Base = Comp != nullptr ? Comp->GetParameterMemory() : nullptr;
        if (Base == nullptr)
        {
            ParameterTable.Reset();
            ImGui::TextDisabled("Preview entity has no instance yet.");
            return;
        }

        if (ParameterTable == nullptr)
        {
            ParameterTable = MakeUnique<FPropertyTable>(Base, Struct);
            ParameterTable->SetShowSearchBar(false);
        }
        else if (ParameterTable->GetObject() != Base || ParameterTable->GetType() != Struct)
        {
            ParameterTable->SetObject(Base, Struct);
        }

        if (!ParameterTable->PrepareAndTestFilter())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                "Struct declares no readable fields.");
            return;
        }

        ParameterTable->DrawTree(true);
    }

    bool FAnimationGraphEditorTool::NeedsCompile() const
    {
        return NodeGraph != nullptr && (!bHasCompiledOnce || NodeGraph->GetContentVersion() != CompiledContentVersion);
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

        // Stamped before the early-outs below so a failing compile does not re-run every frame.
        CompiledContentVersion = NodeGraph->GetContentVersion();
        bHasCompiledOnce = true;

        FAnimationGraphCompiler Compiler;

        // Resolve bone-mask names to per-bone weight arrays up front so Layered Blend
        // Per Bone nodes can look up by name during GenerateBytecode.
        if (Graph->Skeleton.IsValid())
        {
            Compiler.ResolveBoneMasks(Graph->BoneMaskDefs, Graph->Skeleton->GetSkeletonResource());
        }

        // Registered before the node walk so a runtime-chosen clip has slots to write into.
        for (const FName& CurveName : Graph->DeclaredCurves)
        {
            Compiler.AddCurve(CurveName);
        }

        // Give the compiler the parameter struct so it can warn about renamed or retyped fields.
        Compiler.SetDataStruct(Graph->GetParameterStruct());

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

        CAnimStateMachineGraph* SMGraph = Cast<CAnimStateMachineGraph>(Graph);
        if (SMGraph != nullptr)
        {
            SMGraph->SetDebugTransition(nullptr, 0.0f);
        }

        if (!bDebugEnabled)
        {
            Graph->ClearDebugContext();
            return;
        }

        CWorld* TargetWorld = nullptr;
        entt::entity TargetEntity = entt::null;

        const FAnimGraphVMState* VMState = nullptr;
        if (ResolveDebugTarget(TargetWorld, TargetEntity))
        {
            if (SAnimationGraphComponent* Comp = TargetWorld->TryGetComponent<SAnimationGraphComponent>(TargetEntity))
            {
                VMState = &Comp->VMState;
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

                DebugPinValues[const_cast<CEdNodeGraphPin*>(Pin)] = Format("{:.2f}", Scalars[Reg]);
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

            if (SMGraph != nullptr)
            {
                UpdateDebugTransition(SMGraph, *VMState);
            }
        }

        CEdNodeGraph::FGraphDebugContext Context;
        Context.bEnabled    = true;
        Context.bFlowLinks  = true;
        Context.PinValues   = &DebugPinValues;
        Context.ActiveNodes = &DebugActiveNodes;
        Graph->SetDebugContext(Context);
    }

    void FAnimationGraphEditorTool::UpdateDebugTransition(CAnimStateMachineGraph* SMGraph, const FAnimGraphVMState& VMState)
    {
        // Every entry on this canvas describes the same machine, so the last one seen carries its slots.
        THashMap<int32, CEdGraphNode*> StateNodesByIndex;
        uint16 CurrentSlot  = 0;
        uint16 FromSlot     = 0;
        uint16 MachineIndex = 0;
        bool bFoundMachine  = false;

        for (const FAnimGraphDebugStateNode& Entry : DebugStateNodes)
        {
            if (Entry.Node == nullptr || Entry.Node->GetOwningGraph() != SMGraph)
            {
                continue;
            }

            StateNodesByIndex[Entry.StateIndex] = Entry.Node;
            CurrentSlot   = Entry.CurrentStateSlot;
            FromSlot      = Entry.FromStateSlot;
            MachineIndex  = Entry.MachineIndex;
            bFoundMachine = true;
        }

        const TVector<float>& Slots = VMState.StateSlots;
        if (!bFoundMachine || CurrentSlot >= Slots.size() || FromSlot >= Slots.size())
        {
            return;
        }

        // Negative means settled on one state, so nothing is blending.
        if (Slots[FromSlot] < 0.0f)
        {
            return;
        }

        auto FromItr = StateNodesByIndex.find((int32)(Slots[FromSlot] + 0.5f));
        auto ToItr   = StateNodesByIndex.find((int32)(Slots[CurrentSlot] + 0.5f));
        if (FromItr == StateNodesByIndex.end() || ToItr == StateNodesByIndex.end())
        {
            return;
        }

        const int64 ToNodeID = ToItr->second->GetNodeID();
        CAnimStateTransition* Active = SMGraph->FindTransition(FromItr->second->GetNodeID(), ToNodeID);
        if (Active == nullptr)
        {
            Active = SMGraph->FindAnyStateTransitionTo(ToNodeID);
        }

        if (Active == nullptr || MachineIndex >= VMState.Inertializers.size())
        {
            return;
        }

        // An instant transition has no blend to report, so it reads as fully arrived.
        const FAnimInertializer& Inert = VMState.Inertializers[MachineIndex];
        const float Weight = Inert.Duration > 1e-5f ? Inert.Elapsed / Inert.Duration : 1.0f;

        SMGraph->SetDebugTransition(Active, Weight);
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

    void FAnimationGraphEditorTool::SpawnClipPlayerNode(CAnimation* Clip, ImVec2 ScreenPos)
    {
        if (Clip == nullptr || GraphStack.empty())
        {
            return;
        }

        CEdNodeGraph* Canvas = GraphStack.back().Graph;
        if (Canvas == nullptr)
        {
            return;
        }

        CEdGraphNode* Node = Canvas->CreateNode(CAnimGraphNode_ClipPlayer::StaticClass());
        if (CAnimGraphNode_ClipPlayer* ClipNode = Cast<CAnimGraphNode_ClipPlayer>(Node))
        {
            ClipNode->Clip = Clip;
        }

        // Screen->canvas needs the node-editor context, which is only current inside DrawGraph.
        Canvas->QueueNodePlacement(Node, ScreenPos);
        Canvas->ValidateGraph();

        if (Asset.IsValid() && Asset->GetPackage() != nullptr)
        {
            Asset->GetPackage()->MarkDirty();
        }
    }

    void FAnimationGraphEditorTool::DrawClipBrowserWindow()
    {
        CAnimationGraph* Graph = Cast<CAnimationGraph>(Asset.Get());
        CSkeleton* Skeleton = (Graph != nullptr && Graph->Skeleton.IsValid()) ? Graph->Skeleton.Get() : nullptr;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Clips");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 140.0f);
        ClipFilter.Draw("##ClipFilter");

        ImGui::SameLine();
        if (ImGui::Checkbox("Any skeleton", &bClipBrowserAnySkeleton))
        {
            ClipCacheAssetCount = -1; // force a rebuild with the new filter
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("List every animation clip, not just the ones authored against this graph's skeleton");
        }

        if (Skeleton == nullptr && !bClipBrowserAnySkeleton)
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("Assign a skeleton to this graph to list its clips, or tick \"Any skeleton\".");
            ImGui::PopStyleColor();
            return;
        }

        // Rebuild the list when the skeleton or the registry's asset count changes. Matching runs off
        // the registry's dependency table, so clips are filtered without loading a single one.
        TVector<FAssetData*> AllAssets = FAssetRegistry::Get().FindByPredicate([](const FAssetData&) { return true; });
        if (ClipCacheSkeleton != Skeleton || ClipCacheAssetCount != (int32)AllAssets.size())
        {
            ClipCacheSkeleton   = Skeleton;
            ClipCacheAssetCount = (int32)AllAssets.size();
            ClipEntries.clear();

            static const FName AnimationClassName = CAnimation::StaticClass()->GetName();
            const FGuid SkeletonGUID = Skeleton != nullptr ? Skeleton->GetGUID() : FGuid();

            for (const FAssetData* Data : AllAssets)
            {
                if (Data->AssetClass != AnimationClassName)
                {
                    continue;
                }

                if (!bClipBrowserAnySkeleton && Skeleton != nullptr)
                {
                    bool bReferencesSkeleton = false;
                    for (const FAssetDependency& Dependency : Data->Dependencies)
                    {
                        if (Dependency.TargetGUID == SkeletonGUID)
                        {
                            bReferencesSkeleton = true;
                            break;
                        }
                    }
                    if (!bReferencesSkeleton)
                    {
                        continue;
                    }
                }

                FClipEntry Entry;
                Entry.Path        = Data->Path.c_str();
                Entry.DisplayName = Data->AssetName.ToString();
                ClipEntries.push_back(Move(Entry));
            }

            Algo::Sort(ClipEntries.begin(), ClipEntries.end(),
                        [](const FClipEntry& A, const FClipEntry& B) { return A.DisplayName < B.DisplayName; });
        }

        ImGui::Separator();

        if (ClipEntries.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped(bClipBrowserAnySkeleton
                ? "No animation clips found in the project."
                : "No clips reference this skeleton. Import an animation against it, or tick \"Any skeleton\".");
            ImGui::PopStyleColor();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
        ImGui::TextWrapped("Drag a clip onto the graph canvas to add a Play Animation Clip node, or double-click to drop one in the middle.");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (ImGui::BeginChild("##ClipList", ImVec2(0.0f, 0.0f)))
        {
            for (const FClipEntry& Entry : ClipEntries)
            {
                if (!ImGuiX::PassSearchFilter(ClipFilter, Entry.DisplayName.c_str()) && !ImGuiX::PassSearchFilter(ClipFilter, Entry.Path.c_str()))
                {
                    continue;
                }

                ImGui::PushID(Entry.Path.c_str());

                FFixedString Label = LE_ICON_RUN_FAST "  ";
                Label += Entry.DisplayName.c_str();
                ImGui::Selectable(Label.c_str());

                // Drag source: the shared LumDD asset channel, so this row also drops onto any
                // CAnimation property slot in the details panel.
                if (ImGui::BeginDragDropSource())
                {
                    const FStringView Path(Entry.Path.c_str(), Entry.Path.size());
                    if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
                    {
                        DragDrop::SetAssetPayload(*Data);
                    }
                    ImGui::TextUnformatted(Entry.DisplayName.c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::IsItemHovered())
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        const FStringView Path(Entry.Path.c_str(), Entry.Path.size());
                        if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
                        {
                            SpawnClipPlayerNode(LoadObject<CAnimation>(Data->AssetGUID), GraphCanvasCenter);
                        }
                    }
                    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    {
                        ImGuiX::TextTooltip_Internal(Entry.Path.c_str());
                    }
                }

                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    bool FAnimationGraphEditorTool::ResolveDebugTarget(CWorld*& OutWorld, entt::entity& OutEntity) const
    {
        // Null selected world = the editor preview; a stale selection (world/entity gone, e.g. PIE
        // ended) falls back to it too.
        OutWorld  = DebugTargetWorld.Get();
        OutEntity = DebugTargetEntity;

        if (OutWorld == nullptr || !OutWorld->IsValidEntity(OutEntity))
        {
            OutWorld  = World.Get();
            OutEntity = MeshEntity;
        }

        return OutWorld != nullptr && OutEntity != entt::null && OutWorld->IsValidEntity(OutEntity);
    }

    void FAnimationGraphEditorTool::DrawTaskGraphWindow()
    {
        CWorld* TargetWorld = nullptr;
        entt::entity TargetEntity = entt::null;
        SSkeletalMeshComponent* MeshComp = nullptr;
        if (ResolveDebugTarget(TargetWorld, TargetEntity))
        {
            MeshComp = TargetWorld->TryGetComponent<SSkeletalMeshComponent>(TargetEntity);
        }

        // Arm the runtime capture for exactly this component while the window is visible; the next
        // animation tick fills the snapshot read below. Disarmed everywhere else, so populated
        // worlds pay one null compare per mesh.
        Anim::ArmTaskCapture(MeshComp);
        if (MeshComp != nullptr)
        {
            Anim::GetTaskCapture(TaskSnapshot);
        }
        else
        {
            TaskSnapshot.Reset();
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Target");
        DrawDebugTargetCombo();

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##TaskZoom", &TaskGraphZoom, 0.6f, 1.6f, "zoom %.2fx");

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::Checkbox("Show skipped", &bTaskGraphShowSkipped);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Include tasks the executor skipped this frame (branches not reachable from the output)");
        }

        const FAnimTaskSnapshot& Snap = TaskSnapshot;

        if (!Snap.bValid || Snap.Entries.empty())
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped(
                MeshComp == nullptr
                    ? "No debug target. Assign a skeleton so the preview mesh spawns, or pick a live entity above."
                    : "No task list captured yet.\n\n"
                      "The target only records tasks on frames it actually evaluates: the graph must be compiled, "
                      "and the mesh must be visible (off-screen meshes freeze their pose, and distant ones "
                      "evaluate every 2-4 frames).");
            ImGui::PopStyleColor();
            return;
        }

        // Summary line: what the recipe cost and how much of it was live.
        const int32 SkippedCount = (int32)Snap.Entries.size() - Snap.ReachableCount;

        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::TextPrimary(), "%d tasks", (int32)Snap.Entries.size());
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextMuted(), "|");
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::Success(), "%d ran", Snap.ReachableCount);
        if (SkippedCount > 0)
        {
            ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextMuted(), "|");
            ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::Warning(), "%d skipped", SkippedCount);
        }
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextMuted(), "|");
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextDim(), "%d levels", Snap.NumLevels);
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextMuted(), "|");
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextDim(), "peak %d pose buffers", Snap.PeakLiveBuffers);
        ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextMuted(), "|");
        ImGui::SameLine(0.0f, 6.0f);
        if (Snap.ActiveBoneCount > 0 && Snap.ActiveBoneCount < Snap.NumBones)
        {
            ImGui::TextColored(EditorColors::Warning(), "%d/%d bones (LOD cut)", Snap.ActiveBoneCount, Snap.NumBones);
        }
        else
        {
            ImGui::TextColored(EditorColors::TextDim(), "%d bones", Snap.NumBones);
        }
        if (Snap.bLockRoot)
        {
            ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextMuted(), "|");
            ImGui::SameLine(0.0f, 6.0f); ImGui::TextColored(EditorColors::TextDim(), "root pinned");
        }

        // The threading reality, stated where it can't be misread: one list = one thread.
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
        ImGui::TextWrapped("This whole list runs start-to-finish on a single worker thread; the animation system's "
                           "parallelism is across meshes, not within one graph. Columns are dependency levels: tasks "
                           "in the same column have no dependency between them.");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // ---- Layout -------------------------------------------------------------------------

        const float Scale = TaskGraphZoom;

        // Box metrics derive from the font rather than fixed pixels: the editor's DPI scale changes
        // the font size independently of zoom, and a hardcoded height clipped the last line.
        ImFont*     Font      = ImGui::GetFont();
        const float FontSize  = ImGui::GetFontSize() * Scale;
        const float SmallFont = FontSize * 0.86f;

        const float InnerPad = 10.0f * Scale;
        const float PadY     = 8.0f * Scale;
        const float LineGap  = 5.0f * Scale;

        // Title row + detail row + buffer row, and wide enough that typical labels don't truncate.
        const float NodeH   = PadY * 2.0f + FontSize + LineGap + SmallFont + LineGap + SmallFont;
        const float NodeW   = Math::Max(190.0f * Scale, FontSize * 11.5f);
        const float ColGap  = 56.0f * Scale;
        const float RowGap  = 16.0f * Scale;
        const float HeaderH = SmallFont + 10.0f * Scale;
        const float Pad     = 14.0f * Scale;

        const int32 NumEntries = (int32)Snap.Entries.size();
        const int32 NumLevels  = Math::Max(Snap.NumLevels, 1);

        TVector<TVector<int32>> Columns;
        Columns.resize(NumLevels);
        for (int32 i = 0; i < NumEntries; ++i)
        {
            const FAnimTaskDebugEntry& Entry = Snap.Entries[i];
            if (!Entry.bReachable && !bTaskGraphShowSkipped)
            {
                continue;
            }
            const int32 Level = Math::Clamp((int32)Entry.Level, 0, NumLevels - 1);
            Columns[Level].push_back(i);
        }

        float MaxColumnHeight = 0.0f;
        for (const TVector<int32>& Column : Columns)
        {
            const float Height = Column.empty()
                ? 0.0f
                : (float)Column.size() * NodeH + (float)(Column.size() - 1) * RowGap;
            MaxColumnHeight = Math::Max(MaxColumnHeight, Height);
        }

        // Column-local positions (canvas space); each column is vertically centered.
        TVector<ImVec2> Positions;
        Positions.resize(NumEntries, ImVec2(0.0f, 0.0f));
        for (int32 L = 0; L < NumLevels; ++L)
        {
            const TVector<int32>& Column = Columns[L];
            if (Column.empty())
            {
                continue;
            }
            const float Height = (float)Column.size() * NodeH + (float)(Column.size() - 1) * RowGap;
            float Y = (MaxColumnHeight - Height) * 0.5f;
            for (int32 Index : Column)
            {
                Positions[Index] = ImVec2((float)L * (NodeW + ColGap), Y);
                Y += NodeH + RowGap;
            }
        }

        const float TotalW = (float)NumLevels * NodeW + (float)(NumLevels - 1) * ColGap;
        const float TotalH = MaxColumnHeight;

        // ---- Canvas -------------------------------------------------------------------------

        ImGui::BeginChild("##TaskCanvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

        const ImVec2 CanvasCursor = ImGui::GetCursorScreenPos();
        const ImVec2 Origin(CanvasCursor.x + Pad, CanvasCursor.y + Pad + HeaderH);
        ImDrawList* DL = ImGui::GetWindowDrawList();
        const bool bWindowHovered = ImGui::IsWindowHovered();

        // Column bands + level headers: the visual answer to "what is grouped with what".
        for (int32 L = 0; L < NumLevels; ++L)
        {
            const float BandX = Origin.x + (float)L * (NodeW + ColGap);
            const ImVec2 BandMin(BandX - RowGap * 0.5f, Origin.y - HeaderH);
            const ImVec2 BandMax(BandX + NodeW + RowGap * 0.5f, Origin.y + TotalH + Pad * 0.5f);

            DL->AddRectFilled(BandMin, BandMax,
                              EditorColors::U32(EditorColors::WithAlpha(EditorColors::PanelBg(), (L % 2) ? 0.55f : 0.28f)),
                              6.0f * Scale);

            const int32 Count = (int32)Columns[L].size();
            const FString Header = Format("Level {}  ({} task{})", L, Count, Count == 1 ? "" : "s");
            DL->AddText(Font, SmallFont, ImVec2(BandMin.x + 8.0f * Scale, Origin.y - HeaderH + 4.0f * Scale),
                        EditorColors::U32(EditorColors::TextMuted()), Header.c_str());
        }

        // Dependency links behind the boxes. Curves flow left (producer) to right (consumer),
        // matching the node canvas's reading direction.
        for (int32 i = 0; i < NumEntries; ++i)
        {
            const FAnimTaskDebugEntry& Entry = Snap.Entries[i];
            if (!Entry.bReachable && !bTaskGraphShowSkipped)
            {
                continue;
            }

            const int16 Deps[2] = { Entry.DepA, Entry.DepB };
            for (int32 D = 0; D < 2; ++D)
            {
                const int16 Dep = Deps[D];
                if (Dep < 0 || Dep >= NumEntries)
                {
                    continue;
                }
                const FAnimTaskDebugEntry& DepEntry = Snap.Entries[Dep];
                if (!DepEntry.bReachable && !bTaskGraphShowSkipped)
                {
                    continue;
                }

                const ImVec2 From(Origin.x + Positions[Dep].x + NodeW, Origin.y + Positions[Dep].y + NodeH * 0.5f);
                const ImVec2 To(Origin.x + Positions[i].x, Origin.y + Positions[i].y + NodeH * 0.5f);
                const float Curve = (To.x - From.x) * 0.5f;

                // A stolen buffer means this consumer writes in place over the producer's pose:
                // draw it solid and accented, since that edge is where the zero-copy path happens.
                const bool bLive  = Entry.bReachable && DepEntry.bReachable;
                const bool bSteal = bLive && Entry.bStoleBuffer && D == 0;

                ImVec4 LinkColor = bSteal ? EditorColors::AccentAlt() : EditorColors::TextDim();
                LinkColor = EditorColors::WithAlpha(LinkColor, bLive ? (bSteal ? 0.95f : 0.55f) : 0.18f);

                DL->AddBezierCubic(From, ImVec2(From.x + Curve, From.y), ImVec2(To.x - Curve, To.y), To,
                                   EditorColors::U32(LinkColor), (bSteal ? 2.6f : 1.8f) * Scale);
            }
        }

        // Task boxes.
        for (int32 i = 0; i < NumEntries; ++i)
        {
            const FAnimTaskDebugEntry& Entry = Snap.Entries[i];
            if (!Entry.bReachable && !bTaskGraphShowSkipped)
            {
                continue;
            }

            const ImVec2 Min(Origin.x + Positions[i].x, Origin.y + Positions[i].y);
            const ImVec2 Max(Min.x + NodeW, Min.y + NodeH);
            const bool bIsOutput = i == (int32)Snap.OutputTask;
            const float Alpha    = Entry.bReachable ? 1.0f : 0.34f;
            const float Rounding = 6.0f * Scale;

            const ImVec4 Category = AnimGraphDetail::TaskTypeColor(Entry.Type);

            DL->AddRectFilled(Min, Max,
                              EditorColors::U32(EditorColors::WithAlpha(EditorColors::FrameBg(), Alpha)), Rounding);
            DL->AddRect(Min, Max,
                        EditorColors::U32(EditorColors::WithAlpha(bIsOutput ? EditorColors::Success() : Category, Alpha)),
                        Rounding, 0, (bIsOutput ? 2.4f : 1.4f) * Scale);

            // Clip is a safety net only; every string below is measured and ellipsized to fit.
            DL->PushClipRect(Min, Max, true);

            const float TextX     = Min.x + InnerPad;
            const float DotOffset = 13.0f * Scale;
            const float BodyMaxW  = NodeW - InnerPad * 2.0f;
            float TextY = Min.y + PadY;

            // Execution order (or "skipped"), right-aligned; measured first so the title can
            // reserve room for it instead of running underneath.
            const FString Order = Entry.bReachable ? Format("#{}", (int32)Entry.ExecOrder) : FString("skipped");
            const ImVec2 OrderSize = Font->CalcTextSizeA(SmallFont, FLT_MAX, 0.0f, Order.c_str());

            // Category dot + title.
            DL->AddCircleFilled(ImVec2(TextX + 3.0f * Scale, TextY + FontSize * 0.5f), 3.5f * Scale,
                                EditorColors::U32(EditorColors::WithAlpha(Category, Alpha)));

            const float TitleMaxW = BodyMaxW - DotOffset - OrderSize.x - 6.0f * Scale;
            const FString Title = AnimGraphDetail::FitText(Font, FontSize, AnimGraphDetail::TaskTypeName(Entry.Type), TitleMaxW);
            DL->AddText(Font, FontSize, ImVec2(TextX + DotOffset, TextY),
                        EditorColors::U32(EditorColors::WithAlpha(EditorColors::TextPrimary(), Alpha)), Title.c_str());

            DL->AddText(Font, SmallFont, ImVec2(Max.x - InnerPad - OrderSize.x, TextY + 1.0f * Scale),
                        EditorColors::U32(EditorColors::WithAlpha(
                            Entry.bReachable ? EditorColors::TextMuted() : EditorColors::Warning(), Alpha)), Order.c_str());

            TextY += FontSize + LineGap;

            const FString DetailText = AnimGraphDetail::FitText(Font, SmallFont, AnimGraphDetail::BuildTaskDetail(Entry).c_str(), BodyMaxW);
            DL->AddText(Font, SmallFont, ImVec2(TextX, TextY),
                        EditorColors::U32(EditorColors::WithAlpha(EditorColors::TextDim(), Alpha)), DetailText.c_str());

            TextY += SmallFont + LineGap;

            // Buffer chip: where this task's pose lives, and whether it reused its input's buffer.
            if (Entry.bReachable && Entry.BufferIndex >= 0)
            {
                const FString Where = Format("buffer {}  ({})", (int32)Entry.BufferIndex,
                                             Entry.bStoleBuffer ? "in place" : "new");
                const FString Chip = AnimGraphDetail::FitText(Font, SmallFont, Where.c_str(), BodyMaxW);
                DL->AddText(Font, SmallFont, ImVec2(TextX, TextY),
                            EditorColors::U32(EditorColors::WithAlpha(
                                Entry.bStoleBuffer ? EditorColors::AccentAlt() : EditorColors::TextMuted(), Alpha)),
                            Chip.c_str());
            }

            DL->PopClipRect();

            if (bWindowHovered && ImGui::IsMouseHoveringRect(Min, Max))
            {
                ImGui::BeginTooltip();
                ImGui::TextColored(Category, "%s", AnimGraphDetail::TaskTypeName(Entry.Type));
                ImGui::Separator();
                ImGui::Text("Task index    %d%s", i, bIsOutput ? "   (graph output)" : "");
                ImGui::Text("Level         %d", (int32)Entry.Level);
                ImGui::Text("Dependencies  %s", AnimGraphDetail::BuildDepText(Entry).c_str());
                ImGui::Separator();
                if (Entry.bReachable)
                {
                    ImGui::Text("Executed      yes, position %d", (int32)Entry.ExecOrder);
                    ImGui::Text("Pose buffer   %d %s", (int32)Entry.BufferIndex,
                                Entry.bStoleBuffer ? "(reused its input's, zero copy)" : "(fresh from the pool)");
                    ImGui::Text("Buffers live  %d", (int32)Entry.LiveBuffers);
                }
                else
                {
                    ImGui::TextColored(EditorColors::Warning(), "Skipped: not reachable from the output task.");
                    ImGui::TextColored(EditorColors::TextMuted(), "Inactive branches cost nothing to evaluate.");
                }
                const FString DetailLine = AnimGraphDetail::BuildTaskDetail(Entry);
                if (!DetailLine.empty())
                {
                    ImGui::Separator();
                    ImGui::TextColored(EditorColors::TextDim(), "%s", DetailLine.c_str());
                }
                ImGui::EndTooltip();
            }
        }

        ImGui::Dummy(ImVec2(TotalW + Pad * 2.0f, TotalH + HeaderH + Pad * 2.0f));
        ImGui::EndChild();
    }

    void FAnimationGraphEditorTool::OnSave()
    {
        Compile();
        FAssetEditorTool::OnSave();
    }
}
