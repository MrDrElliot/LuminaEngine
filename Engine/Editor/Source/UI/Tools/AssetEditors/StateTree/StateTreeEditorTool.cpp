#include "StateTreeEditorTool.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/Factories/Factory.h"
#include "Containers/Algorithm.h"
#include "Containers/Format.h"
#include "Core/Math/Math.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Memory/Memcpy.h"
#include "Scripting/ScriptStruct.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/EditorAssetActions.h"

namespace Lumina
{
    namespace
    {
        constexpr const char* OutlineWindowName    = "Outline";
        constexpr const char* DetailsWindowName    = "Details";
        constexpr const char* ParametersWindowName = "Parameters";
        constexpr const char* EvaluatorsWindowName = "Evaluators";
        constexpr const char* DebuggerWindowName   = "Debugger";
        constexpr const char* StatePayload         = "STATE_TREE_STATE";

        constexpr float CardIndent     = 18.0f;
        constexpr float CardPadding    = 5.0f;
        constexpr float CardSpacing    = 4.0f;
        constexpr float CardRounding   = 6.0f;
        constexpr float ArrowLaneWidth = 7.0f;
        constexpr int32 ArrowLanes     = 6;
        constexpr float GutterWidth    = ArrowLaneWidth * ArrowLanes + 10.0f;
        constexpr int32 MaxIssueRows   = 6;

        constexpr const char* TriggerLabels[] = { "On Completed", "On Succeeded", "On Failed", "On Event", "On Tick" };
        constexpr const char* TargetLabels[]  = { "State", "Next Sibling", "Parent", "Succeeded", "Failed" };

        const char* KindIcon(EStateTreeStateKind Kind)
        {
            switch (Kind)
            {
                case EStateTreeStateKind::Group:         return LE_ICON_FOLDER_OUTLINE;
                case EStateTreeStateKind::LinkedSubtree: return LE_ICON_LINK_VARIANT;
                default:                                 return LE_ICON_CIRCLE_MEDIUM;
            }
        }

        ImVec4 KindColor(EStateTreeStateKind Kind)
        {
            switch (Kind)
            {
                case EStateTreeStateKind::Group:         return EditorColors::AccentAlt();
                case EStateTreeStateKind::LinkedSubtree: return EditorColors::Warning();
                default:                                 return EditorColors::Accent();
            }
        }

        ImVec4 TriggerColor(EStateTreeTransitionTrigger Trigger)
        {
            switch (Trigger)
            {
                case EStateTreeTransitionTrigger::OnSucceeded: return EditorColors::Success();
                case EStateTreeTransitionTrigger::OnFailed:    return EditorColors::Danger();
                case EStateTreeTransitionTrigger::OnEvent:     return EditorColors::Warning();
                case EStateTreeTransitionTrigger::OnTick:      return EditorColors::AccentAlt();
                default:                                       return EditorColors::Accent();
            }
        }

        FString NodeLabel(const FInstancedStruct& Node)
        {
            const SStateTreeNodeBase* Base = Node.GetPtr<SStateTreeNodeBase>();
            if (Base == nullptr)
            {
                return "(empty)";
            }
            return Base->Name.IsNone() ? FString(*Node.GetScriptStruct()->MakeDisplayName()) : Base->Name.ToString();
        }

        FString TransitionLabel(const SStateTreeTransition& Transition)
        {
            const FString Target = Transition.Target == EStateTreeTransitionTarget::State ? Transition.TargetState.ToString() : FString(TargetLabels[(int32)Transition.Target]);
            return Format("{} {} {}", TriggerLabels[(int32)Transition.Trigger], LE_ICON_ARROW_RIGHT_THIN, Target);
        }

        // The state an arrow points at, none for a transition that completes the tree.
        int32 FindTransitionTarget(const CStateTree& Tree, int32 State, const SStateTreeTransition& Transition)
        {
            const SStateTreeState& Source = Tree.States[State];
            switch (Transition.Target)
            {
                case EStateTreeTransitionTarget::State:
                    return Transition.TargetState.IsNone() ? Constants::kIndexNone : Algo::IndexOf(Tree.States, Transition.TargetState, &SStateTreeState::Name);
                case EStateTreeTransitionTarget::Parent:
                    return Source.Parent.IsNone() ? Constants::kIndexNone : Algo::IndexOf(Tree.States, Source.Parent, &SStateTreeState::Name);
                case EStateTreeTransitionTarget::NextSibling:
                    for (int32 Next = State + 1; Next < Tree.States.Num(); ++Next)
                    {
                        if (Tree.States[Next].Parent == Source.Parent)
                        {
                            return Next;
                        }
                    }
                    return Constants::kIndexNone;
                default:
                    return Constants::kIndexNone;
            }
        }

        bool SelectionExists(CStateTree& Tree, const FStateTreeSelection& Selection)
        {
            if (Selection.IsNode())
            {
                return StateTreeEditing::FindNode(Tree, Selection) != nullptr;
            }
            const bool bState = Tree.States.IsValidIndex(Selection.State);
            return Selection.Slot == EStateTreeSlot::Transition ? bState && Tree.States[Selection.State].Transitions.IsValidIndex(Selection.Transition) : bState;
        }

        // A rounded label that selects what it names, drawn at Pos, which then moves past it along the row.
        bool DrawChip(ImVec2& Pos, int32 Id, const FString& Label, const ImVec4& Color, bool bSelected)
        {
            const ImVec2 TextSize = ImGui::CalcTextSize(*Label);
            const ImVec2 Size(TextSize.x + CardPadding * 2.0f, ImGui::GetTextLineHeight() + 2.0f);
            const ImVec2 Min = Pos;
            Pos.x += Size.x + 4.0f;
            ImGui::SetCursorScreenPos(Min);
            ImGui::PushID(Id);
            const bool bClicked = ImGui::InvisibleButton("##Chip", Size);
            const bool bHovered = ImGui::IsItemHovered();
            ImGui::PopID();

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);
            DrawList->AddRectFilled(Min, Max, EditorColors::U32(EditorColors::WithAlpha(Color, bHovered ? 0.35f : 0.2f)), CardRounding);
            if (bSelected)
            {
                DrawList->AddRect(Min, Max, EditorColors::U32(Color), CardRounding, 1.5f);
            }
            DrawList->AddText(ImVec2(Min.x + CardPadding, Min.y + 1.0f), EditorColors::U32(EditorColors::TextPrimary()), *Label);
            return bClicked;
        }

        void GatherNodeTypes(CStruct* Base, TVector<FPickerItem>& Out)
        {
            Out.Reset();
            for (CStruct* Type : GatherInstancableStructSubtypes(Base))
            {
                const bool bManaged = Cast<CScriptStruct>(Type) != nullptr;
                FPickerItem& Item = Out.EmplaceGetRef();
                Item.Name     = FString(*Type->MakeDisplayName());
                Item.Key      = Type->GetName().ToString();
                Item.Category = Type->HasMeta("Category") ? Type->GetMeta("Category") : FString(bManaged ? "C#" : "C++");
                Item.Tooltip  = Type->HasMeta("ToolTip") ? Format("{}\n{}", Type->GetMeta("ToolTip"), Item.Key) : Item.Key;
                Item.Icon     = bManaged ? LE_ICON_LANGUAGE_CSHARP : LE_ICON_LANGUAGE_CPP;
            }
        }
    }

    FStateTreeEditorTool::FStateTreeEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, *InAsset->GetName(), InAsset)
    {
        for (FPropertyTable* Table : { &DetailsTable, &ParametersTable })
        {
            Table->SetPostEditCallback([this](const FPropertyChangedEvent&) { Edit(); });
            WirePropertyTableUndo(*Table);
        }
        TaskPicker.ItemNoun = "tasks";
        ConditionPicker.ItemNoun = "conditions";
        EvaluatorPicker.ItemNoun = "evaluators";
        BindingPicker.ItemNoun = "sources";
        Issues = StateTreeEditing::Validate(*GetTree());
    }

    void FStateTreeEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(OutlineWindowName, [this](bool) { DrawOutline(); });
        CreateToolWindow(DetailsWindowName, [this](bool) { DrawDetails(); });
        CreateToolWindow(ParametersWindowName, [this](bool) { DrawParameters(); });
        CreateToolWindow(EvaluatorsWindowName, [this](bool) { DrawEvaluators(); });
        CreateToolWindow(DebuggerWindowName, [this](bool) { Debugger.Draw(*GetTree()); });
    }

    void FStateTreeEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGuiID LeftDockID = 0, RightDockID = 0, LeftBottomDockID = 0, RightBottomDockID = 0;
        SplitDockingLayoutLeftRight(InDockspaceID, 0.45f, LeftDockID, RightDockID);
        const ImGuiID LeftTopDockID = ImGui::DockBuilderSplitNode(LeftDockID, ImGuiDir_Up, 0.7f, nullptr, &LeftBottomDockID);
        const ImGuiID RightTopDockID = ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Up, 0.6f, nullptr, &RightBottomDockID);

        ImGui::DockBuilderDockWindow(*GetToolWindowName(OutlineWindowName), LeftTopDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(DebuggerWindowName), LeftBottomDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(DetailsWindowName), RightTopDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(ParametersWindowName), RightBottomDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(EvaluatorsWindowName), RightBottomDockID);
    }

    void FStateTreeEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();
        DetailsTable.MarkDirty();
        ParametersTable.MarkDirty();
        Issues = StateTreeEditing::Validate(*GetTree());
        Select(SelectionExists(*GetTree(), Selection) ? Selection : FStateTreeSelection());
    }

    void FStateTreeEditorTool::Edit()
    {
        NotifyAssetDataChanged();
        Issues = StateTreeEditing::Validate(*GetTree());
    }

    void FStateTreeEditorTool::Select(const FStateTreeSelection& InSelection)
    {
        Selection = InSelection;
        CStateTree* Tree = GetTree();
        DetailsStateName = Tree->States.IsValidIndex(Selection.State) ? Tree->States[Selection.State].Name : FName();
    }

    void FStateTreeEditorTool::ActivateIssue(int32 IssueIndex)
    {
        CStateTree* Tree = GetTree();
        const FStateTreeSelection Target = Issues[IssueIndex].Target;
        if (Tree->States.IsValidIndex(Target.State))
        {
            for (int32 Ancestor : StateTreeEditing::GatherAncestors(*Tree, Target.State))
            {
                CollapsedStates.Remove(Tree->States[Ancestor].Name);
            }
        }
        Select(Target);
        bScrollToSelection = true;
    }

    void FStateTreeEditorTool::AddState(FName Parent, int32 InsertAt)
    {
        const int32 Index = StateTreeEditing::AddState(*GetTree(), Parent, InsertAt);
        CollapsedStates.Remove(Parent);
        Edit();
        Select({ Index });
        BeginRename(Index);
    }

    void FStateTreeEditorTool::AddNode(EStateTreeSlot Slot, CStruct* Type)
    {
        FStateTreeSelection Node = { Slot == EStateTreeSlot::Evaluator ? Constants::kIndexNone : Selection.State, Slot, Slot == EStateTreeSlot::TransitionCondition ? Selection.Transition : Constants::kIndexNone };
        Node.Index = StateTreeEditing::AddNodeOfType(*GetTree(), Node, Type);
        if (Node.Index != Constants::kIndexNone)
        {
            Edit();
            Select(Node);
        }
    }

    void FStateTreeEditorTool::AddTransition(int32 State)
    {
        CStateTree* Tree = GetTree();
        SStateTreeTransition Transition;
        Transition.Target = EStateTreeTransitionTarget::NextSibling;
        const int32 Next = FindTransitionTarget(*Tree, State, Transition);
        Transition.Target = Next != Constants::kIndexNone ? EStateTreeTransitionTarget::State : EStateTreeTransitionTarget::Succeeded;
        Transition.TargetState = Next != Constants::kIndexNone ? Tree->States[Next].Name : FName();
        Tree->States[State].Transitions.Add(Transition);
        Edit();
        Select({ State, EStateTreeSlot::Transition, Tree->States[State].Transitions.Num() - 1 });
    }

    void FStateTreeEditorTool::CopySelection()
    {
        CStateTree* Tree = GetTree();
        if (Selection.IsNode())
        {
            if (const FInstancedStruct* Node = StateTreeEditing::FindNode(*Tree, Selection))
            {
                ClipboardNode = *Node;
                ClipboardStates.Reset();
            }
        }
        else if (Selection.Slot == EStateTreeSlot::State && Tree->States.IsValidIndex(Selection.State))
        {
            ClipboardStates = StateTreeEditing::CopySubtree(*Tree, Selection.State);
            ClipboardNode.Reset();
        }
    }

    void FStateTreeEditorTool::PasteClipboard()
    {
        CStateTree* Tree = GetTree();
        const bool bHasState = Tree->States.IsValidIndex(Selection.State);
        if (!ClipboardStates.IsEmpty())
        {
            const FName Parent = bHasState ? Tree->States[Selection.State].Parent : FName();
            const int32 Index = StateTreeEditing::PasteSubtree(*Tree, ClipboardStates, Parent, bHasState ? Selection.State + 1 : Constants::kIndexNone);
            Edit();
            Select({ Index });
            return;
        }

        const CStruct* Type = ClipboardNode.GetScriptStruct();
        if (Type == nullptr)
        {
            return;
        }
        const bool bOnTransition = Selection.Slot == EStateTreeSlot::Transition || Selection.Slot == EStateTreeSlot::TransitionCondition;
        const EStateTreeSlot Slot = Type->IsChildOf(SStateTreeEvaluatorBase::StaticStruct()) ? EStateTreeSlot::Evaluator
            : Type->IsChildOf(SStateTreeTaskBase::StaticStruct()) ? EStateTreeSlot::Task
            : bOnTransition ? EStateTreeSlot::TransitionCondition : EStateTreeSlot::EnterCondition;
        if (Slot != EStateTreeSlot::Evaluator && !bHasState)
        {
            return;
        }
        FStateTreeSelection Node = { Slot == EStateTreeSlot::Evaluator ? Constants::kIndexNone : Selection.State, Slot, bOnTransition ? Selection.Transition : Constants::kIndexNone,
            Selection.Slot == Slot ? Selection.Index + 1 : Constants::kIndexNone };
        Node.Index = StateTreeEditing::InsertNode(*Tree, Node, ClipboardNode);
        Edit();
        Select(Node);
    }

    void FStateTreeEditorTool::DeleteSelection()
    {
        CStateTree* Tree = GetTree();
        if (!SelectionExists(*Tree, Selection))
        {
            return;
        }
        if (Selection.IsNode())
        {
            StateTreeEditing::RemoveNode(*Tree, Selection);
        }
        else if (Selection.Slot == EStateTreeSlot::Transition)
        {
            Tree->States[Selection.State].Transitions.RemoveAt(Selection.Transition);
        }
        else
        {
            StateTreeEditing::DeleteState(*Tree, Selection.State);
        }
        Edit();
        Select({});
    }

    void FStateTreeEditorTool::DuplicateSelection()
    {
        CStateTree* Tree = GetTree();
        if (Selection.IsNode())
        {
            if (const FInstancedStruct* Node = StateTreeEditing::FindNode(*Tree, Selection))
            {
                // Copied first, since inserting into its own list can move it.
                const FInstancedStruct Copy = *Node;
                FStateTreeSelection Duplicate = Selection;
                Duplicate.Index = StateTreeEditing::InsertNode(*Tree, { Selection.State, Selection.Slot, Selection.Transition, Selection.Index + 1 }, Copy);
                Edit();
                Select(Duplicate);
            }
        }
        else if (Selection.Slot == EStateTreeSlot::State && Tree->States.IsValidIndex(Selection.State))
        {
            const int32 Index = StateTreeEditing::DuplicateState(*Tree, Selection.State);
            Edit();
            Select({ Index });
        }
    }

    void FStateTreeEditorTool::ExtractToLinkedSubtree(int32 State)
    {
        CStateTree* Tree = GetTree();
        const FString StateName = Tree->States[State].Name.ToString();
        const FFixedString AssetPath = GetAssetVirtualPath();
        const FFixedString NewPath = MakeSiblingAssetPath(FStringView(*AssetPath, AssetPath.Len()), *Format("_{}", StateName));
        const TStrongObjectPtr<CStateTree> Linked(NewPath.IsEmpty() ? nullptr : CFactory::CreateNewOf<CStateTree>(NewPath));
        if (!Linked)
        {
            ImGuiX::Notifications::NotifyError("Could not create a state tree beside '{0}' to extract '{1}' into.", *AssetPath, StateName);
            return;
        }

        const TVector<SStateTreeState> Before = Tree->States;
        StateTreeEditing::ExtractToLinkedSubtree(*Tree, State, *Linked);
        Linked->Compile();
        if (!CPackage::SavePackage(Linked->GetPackage(), NewPath))
        {
            ImGuiX::Notifications::NotifyError("Failed to save '{0}'.", *NewPath);
            Tree->States = Before;
            CFactory::DiscardNew(Linked.Get());
            return;
        }
        FAssetRegistry::Get().AssetCreated(Linked.Get());
        Edit();
        Select({ Algo::IndexOf(Tree->States, FName(StateName), &SStateTreeState::Name) });
        ImGuiX::Notifications::NotifySuccess("Extracted '{0}' into '{1}'.", StateName, *NewPath);
    }

    void FStateTreeEditorTool::BeginRename(int32 State)
    {
        const FString Name = GetTree()->States[State].Name.ToString();
        const int32 Count = Math::Min(Name.Len(), (int32)sizeof(RenameBuffer) - 1);
        Memory::Memcpy(RenameBuffer, *Name, Count);
        RenameBuffer[Count] = '\0';
        RenamingState = State;
        bFocusRename = true;
    }

    void FStateTreeEditorTool::DrawOutline()
    {
        CStateTree* Tree = GetTree();
        if (Tree == nullptr)
        {
            return;
        }

        DrawOutlineToolbar(*Tree);

        const int32 IssueRows = Math::Min(Issues.Num(), MaxIssueRows);
        const float StripHeight = ImGui::GetFrameHeightWithSpacing() * 1.5f + ImGui::GetTextLineHeightWithSpacing() * IssueRows;
        if (ImGui::BeginChild("##Cards", ImVec2(0.0f, -StripHeight)))
        {
            Cards.Reset();
            Cards.SetNum(Tree->States.Num());
            TVector<bool> Drawn(Tree->States.Num(), false);
            DrawStates(*Tree, FName(), 0, Drawn, true);

            // A state whose parent is missing or loops still gets a card, at the top level, so it can be fixed.
            for (int32 State = 0; State < Drawn.Num(); ++State)
            {
                if (!Drawn[State])
                {
                    DrawStateCard(*Tree, State, 0);
                }
            }
            DrawTransitionArrows(*Tree);

            if (ImGui::BeginPopupContextWindow("##OutlineMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
            {
                if (ImGui::MenuItem(LE_ICON_PLUS " Add State"))
                {
                    DeferredEdit = [this] { AddState(FName(), Constants::kIndexNone); };
                }
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, !ClipboardStates.IsEmpty()))
                {
                    DeferredEdit = [this] { Select({}); PasteClipboard(); };
                }
                ImGui::EndPopup();
            }
            HandleOutlineShortcuts();
        }
        ImGui::EndChild();

        DrawValidation();

        if (DeferredEdit)
        {
            const TFunction<void()> Run = Move(DeferredEdit);
            DeferredEdit = nullptr;
            Run();
        }
    }

    void FStateTreeEditorTool::DrawOutlineToolbar(CStateTree& Tree)
    {
        const bool bHasState = Tree.States.IsValidIndex(Selection.State);
        if (ImGui::Button(LE_ICON_PLUS " State"))
        {
            AddState(bHasState ? Tree.States[Selection.State].Parent : FName(), bHasState ? Selection.State + 1 : Constants::kIndexNone);
        }
        ImGuiX::TextTooltip("Adds a state after the selected one, or at the top level.");

        ImGui::BeginDisabled(!bHasState);
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Child"))
        {
            AddState(Tree.States[Selection.State].Name, Constants::kIndexNone);
        }
        ImGui::SameLine();
        if (CStruct* Type = DrawNodePicker(TaskPicker, LE_ICON_PLUS " Task", EStateTreeSlot::Task))
        {
            AddNode(EStateTreeSlot::Task, Type);
        }
        ImGui::SameLine();
        const bool bOnTransition = Selection.Slot == EStateTreeSlot::Transition || Selection.Slot == EStateTreeSlot::TransitionCondition;
        const EStateTreeSlot ConditionSlot = bOnTransition ? EStateTreeSlot::TransitionCondition : EStateTreeSlot::EnterCondition;
        if (CStruct* Type = DrawNodePicker(ConditionPicker, LE_ICON_PLUS " Condition", ConditionSlot))
        {
            AddNode(ConditionSlot, Type);
        }
        ImGuiX::TextTooltip("Adds to the selected transition's conditions, or to the selected state's enter conditions.");
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Transition"))
        {
            AddTransition(Selection.State);
        }
        ImGui::EndDisabled();
    }

    void FStateTreeEditorTool::DrawStates(CStateTree& Tree, FName Parent, int32 Depth, TVector<bool>& Drawn, bool bShown)
    {
        for (int32 State = 0; State < Tree.States.Num(); ++State)
        {
            if (Drawn[State] || Tree.States[State].Parent != Parent)
            {
                continue;
            }
            // States under a collapsed one are still walked, so the fallback for unreachable states skips them.
            Drawn[State] = true;
            if (bShown)
            {
                DrawStateCard(Tree, State, Depth);
            }
            DrawStates(Tree, Tree.States[State].Name, Depth + 1, Drawn, bShown && !Algo::Contains(CollapsedStates, Tree.States[State].Name));
        }
    }

    void FStateTreeEditorTool::DrawStateCard(CStateTree& Tree, int32 State, int32 Depth)
    {
        const SStateTreeState& Source = Tree.States[State];
        const float LineHeight = ImGui::GetTextLineHeightWithSpacing();
        const int32 NumLines = 1 + (Source.EnterConditions.IsEmpty() ? 0 : 1) + (Source.Tasks.IsEmpty() ? 0 : 1) + Source.Transitions.Num();
        const ImVec2 Start = ImGui::GetCursorScreenPos();
        const ImVec2 Min(Start.x + GutterWidth + Depth * CardIndent, Start.y);
        const ImVec2 Max(Start.x + Math::Max(ImGui::GetContentRegionAvail().x, GutterWidth + Depth * CardIndent + 120.0f), Start.y + NumLines * LineHeight + CardPadding * 2.0f);

        FCardLayout& Card = Cards[State];
        Card.Min = Min;
        Card.Max = Max;
        Card.HeaderY = Min.y + CardPadding + LineHeight * 0.5f;
        Card.bVisible = true;

        ImGui::PushID(State);
        ImGui::SetCursorScreenPos(Min);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##Card", ImVec2(Max.x - Min.x, Max.y - Min.y));
        const bool bHovered = ImGui::IsItemHovered();
        // Opened while the card is still the last item, since the chips drawn over it replace it.
        ImGui::OpenPopupOnItemClick("##StateMenu", ImGuiPopupFlags_MouseButtonRight);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            Select({ State });
        }
        if (bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            BeginRename(State);
        }
        if (bScrollToSelection && Selection.State == State)
        {
            ImGui::SetScrollHereY(0.3f);
            bScrollToSelection = false;
        }
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(StatePayload, &State, sizeof(State));
            ImGui::TextUnformatted(*Source.Name.ToString());
            ImGui::EndDragDropSource();
        }
        AcceptStateDrop(Tree, State);
        DrawStateContextMenu(Tree, State);

        const bool bSelected = Selection.State == State;
        const TOptional<EStateTreeRunStatus> Active = Debugger.GetStateStatus(Tree, State);
        const ImVec4 ActiveColor = Active ? FStateTreeDebugger::GetStatusColor(*Active) : EditorColors::Accent();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(Min, Max, EditorColors::U32(bSelected && Selection.Slot == EStateTreeSlot::State ? EditorColors::RowBgActive() : bHovered ? EditorColors::RowBgHovered() : EditorColors::RowBg()), CardRounding);
        DrawList->AddRectFilled(Min, ImVec2(Min.x + 3.0f, Max.y), EditorColors::U32(KindColor(Source.Kind)), CardRounding, ImDrawFlags_RoundCornersLeft);
        DrawList->AddRect(Min, Max, EditorColors::U32(Active || bSelected ? ActiveColor : EditorColors::Border()), CardRounding, Active || bSelected ? 2.0f : 1.0f);

        ImGui::PushClipRect(Min, Max, true);
        const float X = Min.x + CardPadding + 4.0f;
        float Y = Min.y + CardPadding;
        ImGui::SetCursorScreenPos(ImVec2(X, Y));

        const bool bHasChildren = Algo::Contains(Tree.States, Source.Name, &SStateTreeState::Parent);
        const bool bCollapsed = Algo::Contains(CollapsedStates, Source.Name);
        const ImVec2 ChevronPos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##Expand", ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight())) && bHasChildren)
        {
            if (bCollapsed)
            {
                CollapsedStates.Remove(Source.Name);
            }
            else
            {
                CollapsedStates.Add(Source.Name);
            }
        }
        if (bHasChildren)
        {
            DrawList->AddText(ChevronPos, EditorColors::U32(EditorColors::TextDim()), bCollapsed ? LE_ICON_CHEVRON_RIGHT : LE_ICON_CHEVRON_DOWN);
        }
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextColored(KindColor(Source.Kind), "%s", KindIcon(Source.Kind));
        ImGui::SameLine();
        if (RenamingState == State)
        {
            ImGui::SetNextItemWidth(Max.x - ImGui::GetCursorScreenPos().x - CardPadding);
            if (bFocusRename)
            {
                ImGui::SetKeyboardFocusHere();
                bFocusRename = false;
            }
            const bool bCommit = ImGui::InputText("##Rename", RenameBuffer, sizeof(RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (bCommit || ImGui::IsItemDeactivated())
            {
                if (!ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    DeferredEdit = [this, State, NewName = FName(RenameBuffer)]
                    {
                        CStateTree& Edited = *GetTree();
                        if (NewName != Edited.States[State].Name && StateTreeEditing::RenameState(Edited, State, StateTreeEditing::MakeUniqueStateName(Edited, NewName)))
                        {
                            Edit();
                            Select({ State });
                        }
                    };
                }
                RenamingState = Constants::kIndexNone;
            }
        }
        else
        {
            ImGui::TextUnformatted(*Source.Name.ToString());
            if (const CStateTree* Linked = Source.LinkedSubtree.Get(); Linked != nullptr && Source.Kind == EStateTreeStateKind::LinkedSubtree)
            {
                ImGui::SameLine();
                ImGui::TextColored(EditorColors::TextDim(), "%s %s", LE_ICON_ARROW_RIGHT_THIN, *Linked->GetName());
            }
        }
        Y += LineHeight;

        auto DrawNodeLine = [&](const auto& Nodes, EStateTreeSlot Slot, const char* Icon, const ImVec4& Color)
        {
            if (Nodes.IsEmpty())
            {
                return;
            }
            ImGui::SetCursorScreenPos(ImVec2(X, Y));
            ImGui::TextColored(EditorColors::TextDim(), "%s", Icon);
            ImVec2 Chip(ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x, Y);
            for (int32 Index = 0; Index < Nodes.Num(); ++Index)
            {
                const SStateTreeConditionBase* Condition = Nodes[Index].template GetPtr<SStateTreeConditionBase>();
                const FString Label = Condition != nullptr && Condition->bInvert ? Format("not {}", NodeLabel(Nodes[Index])) : NodeLabel(Nodes[Index]);
                const FStateTreeSelection Node = { State, Slot, Constants::kIndexNone, Index };
                if (DrawChip(Chip, (int32)Slot * 1000 + Index, Label, Color, Selection == Node))
                {
                    Select(Node);
                }
            }
            Y += LineHeight;
        };
        DrawNodeLine(Source.EnterConditions, EStateTreeSlot::EnterCondition, LE_ICON_FILTER, EditorColors::Warning());
        DrawNodeLine(Source.Tasks, EStateTreeSlot::Task, LE_ICON_PLAY, ActiveColor);

        Card.TransitionY.Reset();
        for (int32 Transition = 0; Transition < Source.Transitions.Num(); ++Transition)
        {
            const SStateTreeTransition& Authored = Source.Transitions[Transition];
            ImVec2 Chip(X, Y);
            Card.TransitionY.Add(Y + LineHeight * 0.5f);
            ImGui::PushID(Transition);
            const FStateTreeSelection Selected = { State, EStateTreeSlot::Transition, Transition };
            if (DrawChip(Chip, -1, TransitionLabel(Authored), TriggerColor(Authored.Trigger), Selection == Selected))
            {
                Select(Selected);
            }
            for (int32 Index = 0; Index < Authored.Conditions.Num(); ++Index)
            {
                const FStateTreeSelection Condition = { State, EStateTreeSlot::TransitionCondition, Transition, Index };
                if (DrawChip(Chip, Index, Format("if {}", NodeLabel(Authored.Conditions[Index])), EditorColors::Warning(), Selection == Condition))
                {
                    Select(Condition);
                }
            }
            ImGui::PopID();
            Y += LineHeight;
        }
        ImGui::PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(Start.x, Max.y + CardSpacing));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }

    void FStateTreeEditorTool::DrawStateContextMenu(CStateTree& Tree, int32 State)
    {
        if (!ImGui::BeginPopup("##StateMenu"))
        {
            return;
        }
        const SStateTreeState& Source = Tree.States[State];
        const FName Name = Source.Name;
        const FName Parent = Source.Parent;
        const bool bHasChildren = Algo::Contains(Tree.States, Name, &SStateTreeState::Parent);

        if (ImGui::MenuItem(LE_ICON_PLUS " Add Child"))
        {
            DeferredEdit = [this, Name] { AddState(Name, Constants::kIndexNone); };
        }
        if (ImGui::MenuItem(LE_ICON_PLUS " Add Sibling"))
        {
            DeferredEdit = [this, Parent, State] { AddState(Parent, State + 1); };
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2"))
        {
            BeginRename(State);
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        {
            DeferredEdit = [this, State] { Select({ State }); DuplicateSelection(); };
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
        {
            Select({ State });
            CopySelection();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !ClipboardStates.IsEmpty() || ClipboardNode.IsValid()))
        {
            DeferredEdit = [this, State] { Select({ State }); PasteClipboard(); };
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Convert to Group", nullptr, false, Source.Kind != EStateTreeStateKind::Group))
        {
            DeferredEdit = [this, State] { GetTree()->States[State].Kind = EStateTreeStateKind::Group; Edit(); };
        }
        if (ImGui::MenuItem("Extract to Linked Subtree", nullptr, false, bHasChildren && HasAsset() && GetTree()->GetPackage() != nullptr))
        {
            DeferredEdit = [this, State] { ExtractToLinkedSubtree(State); };
        }
        ImGuiX::TextTooltip("Moves the children into a new state tree beside this one, which this state then links.");
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del"))
        {
            DeferredEdit = [this, State] { Select({ State }); DeleteSelection(); };
        }
        ImGui::EndPopup();
    }

    void FStateTreeEditorTool::AcceptStateDrop(CStateTree& Tree, int32 State)
    {
        if (!ImGui::BeginDragDropTarget())
        {
            return;
        }
        // The top and bottom quarters drop beside the card, the middle drops into it.
        const FCardLayout& Card = Cards[State];
        const float Fraction = (ImGui::GetMousePos().y - Card.Min.y) / Math::Max(Card.Max.y - Card.Min.y, 1.0f);
        const bool bBefore = Fraction < 0.25f;
        const bool bAfter = Fraction > 0.75f;
        const bool bChild = !bBefore && !bAfter;

        ImDrawList* Foreground = ImGui::GetForegroundDrawList();
        const ImU32 Indicator = EditorColors::U32(EditorColors::Accent());
        if (bChild)
        {
            Foreground->AddRect(Card.Min, Card.Max, Indicator, CardRounding, 2.0f);
        }
        else
        {
            const float LineY = bBefore ? Card.Min.y - CardSpacing * 0.5f : Card.Max.y + CardSpacing * 0.5f;
            Foreground->AddLine(ImVec2(Card.Min.x, LineY), ImVec2(Card.Max.x, LineY), Indicator, 2.0f);
        }

        if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(StatePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            const int32 Dragged = *static_cast<const int32*>(Payload->Data);
            const SStateTreeState& Target = Tree.States[State];
            const FName Parent = bChild ? Target.Name : Target.Parent;
            const int32 InsertBefore = bChild ? Tree.States.Num() : bBefore ? State : State + 1;
            if (Dragged != State)
            {
                DeferredEdit = [this, Dragged, Parent, InsertBefore]
                {
                    const int32 Moved = StateTreeEditing::MoveState(*GetTree(), Dragged, Parent, InsertBefore);
                    if (Moved != Constants::kIndexNone)
                    {
                        CollapsedStates.Remove(Parent);
                        Edit();
                        Select({ Moved });
                    }
                };
            }
        }
        ImGui::EndDragDropTarget();
    }

    void FStateTreeEditorTool::DrawTransitionArrows(const CStateTree& Tree)
    {
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const float GutterLeft = ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x - ImGui::GetScrollX();
        int32 Lane = 0;
        for (int32 State = 0; State < Tree.States.Num(); ++State)
        {
            const FCardLayout& Card = Cards[State];
            const TVector<SStateTreeTransition>& Transitions = Tree.States[State].Transitions;
            for (int32 Transition = 0; Card.bVisible && Transition < Transitions.Num() && Transition < Card.TransitionY.Num(); ++Transition)
            {
                int32 Target = FindTransitionTarget(Tree, State, Transitions[Transition]);
                if (Target == Constants::kIndexNone)
                {
                    continue;
                }
                // A target inside a collapsed state points at the nearest card still shown.
                for (const int32 Ancestor : StateTreeEditing::GatherAncestors(Tree, Target))
                {
                    Target = Cards[Target].bVisible ? Target : Ancestor;
                }
                const FCardLayout& TargetCard = Cards[Target];
                if (!TargetCard.bVisible)
                {
                    continue;
                }

                const bool bSelected = Selection == FStateTreeSelection{ State, EStateTreeSlot::Transition, Transition };
                const ImU32 Color = EditorColors::U32(bSelected ? EditorColors::TextPrimary() : TriggerColor(Transitions[Transition].Trigger));
                const float LaneX = GutterLeft + GutterWidth - ArrowLaneWidth * (float)(Lane++ % ArrowLanes + 1);
                const float FromY = Card.TransitionY[Transition];
                const float ToX = TargetCard.Min.x - 1.0f;
                const ImVec2 Points[] = { ImVec2(Card.Min.x, FromY), ImVec2(LaneX, FromY), ImVec2(LaneX, TargetCard.HeaderY), ImVec2(ToX - 5.0f, TargetCard.HeaderY) };
                DrawList->AddPolyline(Points, IM_ARRAYSIZE(Points), Color, bSelected ? 2.5f : 1.5f);
                DrawList->AddTriangleFilled(ImVec2(ToX - 6.0f, TargetCard.HeaderY - 4.0f), ImVec2(ToX - 6.0f, TargetCard.HeaderY + 4.0f), ImVec2(ToX, TargetCard.HeaderY), Color);
            }
        }
    }

    void FStateTreeEditorTool::DrawValidation()
    {
        ImGui::Separator();
        if (Issues.IsEmpty())
        {
            ImGui::TextColored(EditorColors::Success(), "%s Compiled with no issues", LE_ICON_CHECKBOX_MARKED);
            return;
        }

        const int32 NumErrors = (int32)Algo::CountIf(Issues, [](const FStateTreeIssue& Issue) { return Issue.bError; });
        ImGui::TextColored(NumErrors > 0 ? EditorColors::Danger() : EditorColors::Warning(), "%s %d errors, %d warnings", LE_ICON_ALERT_CIRCLE, NumErrors, Issues.Num() - NumErrors);
        if (ImGui::BeginChild("##Issues"))
        {
            for (int32 Index = 0; Index < Issues.Num(); ++Index)
            {
                const FStateTreeIssue& Issue = Issues[Index];
                ImGui::PushID(Index);
                ImGui::PushStyleColor(ImGuiCol_Text, Issue.bError ? EditorColors::Danger() : EditorColors::Warning());
                const bool bHasTarget = Issue.Target.State != Constants::kIndexNone || Issue.Target.Slot == EStateTreeSlot::Evaluator;
                if (ImGui::Selectable(*Format("{} {}", Issue.bError ? LE_ICON_ALERT_CIRCLE : LE_ICON_ALERT, Issue.Message), false) && bHasTarget)
                {
                    ActivateIssue(Index);
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void FStateTreeEditorTool::HandleOutlineShortcuts()
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsAnyItemActive())
        {
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            DeferredEdit = [this] { DeleteSelection(); };
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
        {
            DeferredEdit = [this] { DuplicateSelection(); };
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
        {
            CopySelection();
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
        {
            DeferredEdit = [this] { PasteClipboard(); };
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_F2) && Selection.Slot == EStateTreeSlot::State && GetTree()->States.IsValidIndex(Selection.State))
        {
            BeginRename(Selection.State);
        }
    }

    void FStateTreeEditorTool::DrawDetails()
    {
        CStateTree* Tree = GetTree();
        if (Tree == nullptr || !SelectionExists(*Tree, Selection))
        {
            ImGui::TextDisabled("Select a state, a node or a transition.");
            return;
        }

        if (Selection.IsNode())
        {
            FInstancedStruct& Node = *StateTreeEditing::FindNode(*Tree, Selection);
            if (DrawInstancedStructEditor("Type", Node, StateTreeEditing::GetNodeBaseStruct(Selection.Slot), DetailsTable))
            {
                Edit();
            }
            DrawBindings(*Tree, Selection);
            Debugger.DrawInstanceData(*Tree, Selection);
            return;
        }

        SStateTreeState& State = Tree->States[Selection.State];
        if (Selection.Slot == EStateTreeSlot::Transition)
        {
            SStateTreeTransition& Transition = State.Transitions[Selection.Transition];
            if (Transition.Target == EStateTreeTransitionTarget::State)
            {
                const int32 Current = Algo::IndexOf(Tree->States, Transition.TargetState, &SStateTreeState::Name);
                const int32 Picked = ImGuiX::SearchableCombo("Target State", *Transition.TargetState.ToString(), Tree->States.Num(), Current,
                    [Tree](int32 Index) { return FFixedString(*Tree->States[Index].Name); }, LE_ICON_CIRCLE_MEDIUM);
                if (Picked != Constants::kIndexNone && Picked != Current)
                {
                    Transition.TargetState = Tree->States[Picked].Name;
                    Edit();
                }
            }
            if (DetailsTable.GetObject() != &Transition)
            {
                DetailsTable.SetObject(&Transition, SStateTreeTransition::StaticStruct());
            }
            DetailsTable.DrawTree();
            return;
        }

        if (DetailsTable.GetObject() != &State)
        {
            DetailsTable.SetObject(&State, SStateTreeState::StaticStruct());
        }
        DetailsTable.DrawTree();

        // The table writes the name in place, so the rename is replayed through the edit that carries children and targets along.
        if (State.Name != DetailsStateName)
        {
            const FName NewName = State.Name;
            State.Name = DetailsStateName;
            if (StateTreeEditing::RenameState(*Tree, Selection.State, NewName))
            {
                DetailsStateName = NewName;
                Edit();
            }
            else
            {
                State.Name = NewName;
            }
        }
        DrawBindings(*Tree, Selection);
    }

    void FStateTreeEditorTool::DrawBindings(CStateTree& Tree, const FStateTreeSelection& Reader)
    {
        CStruct* TargetType = nullptr;
        TVector<SStateTreeBinding>* Bindings = StateTreeEditing::FindBindings(Tree, Reader, TargetType);
        if (Bindings == nullptr)
        {
            return;
        }

        ImGui::SeparatorText(LE_ICON_LINK_VARIANT " Bindings");
        if (!ImGui::BeginTable("##Bindings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
        {
            return;
        }
        for (const FProperty* Field : TargetType->GetProperties())
        {
            if (!FStateTreeCompiler::CanBind(*Field, *Field))
            {
                continue;
            }
            const FName FieldName = Field->GetPropertyName();
            ImGui::PushID(*FieldName);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(*FieldName);
            ImGuiX::TextTooltip("{}", Field->GetTypeName().ToString());
            ImGui::TableNextColumn();

            int32 Bound = Algo::IndexOf(*Bindings, FieldName, &SStateTreeBinding::TargetField);
            const FString Label = Bound != Constants::kIndexNone ? Format("{} {}", LE_ICON_LINK, StateTreeEditing::DescribeBinding((*Bindings)[Bound])) : Format("{} Bind...", LE_ICON_LINK_OFF);
            if (const SStateTreeBindingPath* Picked = DrawBindingPicker("##Source", Label, Tree, Reader, *Field, true))
            {
                StateTreeEditing::SetBinding(Tree, *Bindings, FieldName, *Picked);
                Bound = Algo::IndexOf(*Bindings, FieldName, &SStateTreeBinding::TargetField);
                Edit();
            }
            if (Bound != Constants::kIndexNone)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton(LE_ICON_CLOSE))
                {
                    Bindings->RemoveAt(Bound);
                    Bound = Constants::kIndexNone;
                    Edit();
                }
                ImGuiX::TextTooltip("Unbind");
            }

            const FFunction* Function = Bound != Constants::kIndexNone ? StateTreeEditing::FindBoundFunction(Tree, (*Bindings)[Bound]) : nullptr;
            const TSpan<FProperty* const> Arguments = Function != nullptr ? Function->GetArguments() : TSpan<FProperty* const>();
            for (int32 Argument = 0; Argument < Arguments.Num() && Argument < (*Bindings)[Bound].Arguments.Num(); ++Argument)
            {
                ImGui::PushID(Argument);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(EditorColors::TextDim(), "  %s %s", LE_ICON_FUNCTION, *Arguments[Argument]->GetPropertyName());
                ImGui::TableNextColumn();
                const SStateTreeBindingPath& Current = (*Bindings)[Bound].Arguments[Argument];
                const FString ArgumentLabel = Current.SourceField.IsNone() ? FString("Bind argument...") : StateTreeEditing::DescribeBinding(Current);
                if (const SStateTreeBindingPath* Picked = DrawBindingPicker("##Argument", ArgumentLabel, Tree, Reader, *Arguments[Argument], false))
                {
                    (*Bindings)[Bound].Arguments[Argument] = *Picked;
                    Edit();
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const SStateTreeBindingPath* FStateTreeEditorTool::DrawBindingPicker(const char* StrId, const FString& Label, const CStateTree& Tree, const FStateTreeSelection& Reader, const FProperty& Target, bool bAllowFunctions)
    {
        const FPickerItem* Picked = BindingPicker.DrawButton(StrId, *Label, [&](TVector<FPickerItem>& Out)
        {
            StateTreeEditing::GatherBindingSources(Tree, Reader, Target, bAllowFunctions, BindingSources);
            Out.Reset();
            for (const FStateTreeBindingSource& Source : BindingSources)
            {
                Out.Add(Source.Item);
            }
        }, -FLT_MIN);
        if (Picked == nullptr)
        {
            return nullptr;
        }
        const FStateTreeBindingSource* Source = BindingSources.FindByPredicate([Picked](const FStateTreeBindingSource& Candidate) { return Candidate.Item.Key == Picked->Key; });
        return Source != nullptr ? &Source->Path : nullptr;
    }

    CStruct* FStateTreeEditorTool::DrawNodePicker(FCategorizedPicker& Picker, const char* Label, EStateTreeSlot Slot)
    {
        CStruct* Base = StateTreeEditing::GetNodeBaseStruct(Slot);
        const FPickerItem* Picked = Picker.DrawButton(Label, Label, [Base](TVector<FPickerItem>& Out) { GatherNodeTypes(Base, Out); });
        return Picked != nullptr ? FindObject<CStruct>(FName(Picked->Key)) : nullptr;
    }

    void FStateTreeEditorTool::DrawParameters()
    {
        CStateTree* Tree = GetTree();
        if (Tree != nullptr && DrawInstancedStructEditor("Parameters", Tree->Parameters, SStateTreeParametersBase::StaticStruct(), ParametersTable))
        {
            Edit();
        }
    }

    void FStateTreeEditorTool::DrawEvaluators()
    {
        CStateTree* Tree = GetTree();
        if (Tree == nullptr)
        {
            return;
        }
        if (CStruct* Type = DrawNodePicker(EvaluatorPicker, LE_ICON_PLUS " Evaluator", EStateTreeSlot::Evaluator))
        {
            AddNode(EStateTreeSlot::Evaluator, Type);
        }
        ImGuiX::TextTooltip("Evaluators tick before the states and publish their data for bindings.");

        for (int32 Index = 0; Index < Tree->Evaluators.Num(); ++Index)
        {
            const FStateTreeSelection Node = { Constants::kIndexNone, EStateTreeSlot::Evaluator, Constants::kIndexNone, Index };
            ImGui::PushID(Index);
            const CStruct* Type = Tree->Evaluators[Index].GetScriptStruct();
            if (ImGui::Selectable(*Format("{} {}", LE_ICON_FUNCTION, NodeLabel(Tree->Evaluators[Index])), Selection == Node))
            {
                Select(Node);
            }
            ImGui::OpenPopupOnItemClick("##EvaluatorMenu", ImGuiPopupFlags_MouseButtonRight);
            if (Type != nullptr)
            {
                ImGui::SameLine();
                ImGui::TextColored(EditorColors::TextDim(), "%s", *Type->MakeDisplayName());
            }
            bool bRemoved = false;
            if (ImGui::BeginPopup("##EvaluatorMenu"))
            {
                Select(Node);
                if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
                {
                    DuplicateSelection();
                    bRemoved = true;
                }
                if (ImGui::MenuItem("Copy", "Ctrl+C"))
                {
                    CopySelection();
                }
                if (ImGui::MenuItem("Delete", "Del"))
                {
                    DeleteSelection();
                    bRemoved = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (bRemoved)
            {
                break;
            }
        }
    }
}
