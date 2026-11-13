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

        constexpr float CardIndent       = 18.0f;
        constexpr float CardPadding      = 5.0f;
        constexpr float CardSpacing      = 4.0f;
        constexpr float CardRounding     = 6.0f;
        constexpr float CardMinWidth     = 160.0f;
        constexpr float ChipSpacing      = 4.0f;
        constexpr float ContainerPadding = 4.0f;
        constexpr float SelectionGlow    = 5.0f;
        constexpr float ArrowLaneWidth   = 7.0f;
        constexpr float ArrivalSpacing   = 6.0f;
        constexpr int32 MaxIssueRows     = 6;

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

        FString ConditionLabel(const FInstancedStruct& Node, int32 Index)
        {
            const SStateTreeConditionBase* Condition = Node.GetPtr<SStateTreeConditionBase>();
            const char* Join = Index == 0 ? "if" : Condition != nullptr && Condition->Operand == EStateTreeConditionOperand::Or ? "or" : "and";
            return Format("{}{} {}", Join, Condition != nullptr && Condition->bInvert ? " not" : "", NodeLabel(Node));
        }

        // A label on a card row that selects Target, gathered before drawing so the card can size to its widest row.
        struct FCardChip
        {
            FString             Label;
            FStateTreeSelection Target;
            ImVec4              Color;
            bool                bLit = false;
        };

        struct FCardRow
        {
            const char*        Icon = nullptr;
            TVector<FCardChip> Chips;
        };

        // The transition, then a badge for each setting that changes when or whether it fires, then its conditions.
        void AddTransitionChips(FCardRow& Row, const SStateTreeTransition& Transition, const FStateTreeSelection& Target, bool bLastTaken)
        {
            const FString Trigger = Transition.Trigger != EStateTreeTransitionTrigger::OnEvent ? FString(TriggerLabels[(int32)Transition.Trigger])
                : Format("On {} {}", LE_ICON_TAG_OUTLINE, Transition.EventTag.IsValid() ? Transition.EventTag.TagName.ToString() : FString("(no tag)"));
            const FString Destination = Transition.Target == EStateTreeTransitionTarget::State ? Transition.TargetState.ToString() : FString(TargetLabels[(int32)Transition.Target]);
            Row.Chips.Add({ Format("{} {} {}", Trigger, LE_ICON_ARROW_RIGHT_THIN, Destination), Target, TriggerColor(Transition.Trigger), bLastTaken });
            if (Transition.Delay > 0.0f || Transition.DelayRandomRange > 0.0f)
            {
                const FString Range = Transition.DelayRandomRange > 0.0f ? Format(" {} {:g}", LE_ICON_PLUS_MINUS, Transition.DelayRandomRange) : FString();
                Row.Chips.Add({ Format("{} {:g} s{}", LE_ICON_TIMER_OUTLINE, Transition.Delay, Range), Target, EditorColors::TextDim() });
            }
            if (Transition.Priority != 0)
            {
                Row.Chips.Add({ Format("priority {}", Transition.Priority), Target, EditorColors::TextDim() });
            }
            for (int32 Index = 0; Index < Transition.Conditions.Num(); ++Index)
            {
                Row.Chips.Add({ ConditionLabel(Transition.Conditions[Index], Index), { Target.State, EStateTreeSlot::TransitionCondition, Target.Transition, Index }, EditorColors::Warning() });
            }
        }

        float ChipWidth(const FString& Label)
        {
            return ImGui::CalcTextSize(*Label).x + CardPadding * 2.0f;
        }

        float RowWidth(const FCardRow& Row)
        {
            float Width = Row.Icon != nullptr ? ImGui::CalcTextSize(Row.Icon).x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
            for (const FCardChip& Chip : Row.Chips)
            {
                Width += ChipWidth(Chip.Label) + ChipSpacing;
            }
            return Width;
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

        // A rounded label drawn at Pos, which then moves past it along the row, filled strongly while lit by the debugger.
        bool DrawChip(ImVec2& Pos, int32 Id, const FCardChip& Chip, bool bSelected)
        {
            const ImVec2 Size(ChipWidth(Chip.Label), ImGui::GetTextLineHeight() + 2.0f);
            const ImVec2 Min = Pos;
            Pos.x += Size.x + ChipSpacing;
            ImGui::SetCursorScreenPos(Min);
            ImGui::PushID(Id);
            const bool bClicked = ImGui::InvisibleButton("##Chip", Size);
            const bool bHovered = ImGui::IsItemHovered();
            ImGui::PopID();

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);
            DrawList->AddRectFilled(Min, Max, EditorColors::U32(EditorColors::WithAlpha(Chip.Color, (Chip.bLit ? 0.5f : 0.2f) + (bHovered ? 0.15f : 0.0f))), CardRounding);
            if (bSelected || Chip.bLit)
            {
                DrawList->AddRect(Min, Max, EditorColors::U32(bSelected ? EditorColors::Accent() : Chip.Color), CardRounding, 1.5f);
            }
            DrawList->AddText(ImVec2(Min.x + CardPadding, Min.y + 1.0f), EditorColors::U32(EditorColors::TextPrimary()), *Chip.Label);
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
        ImGuiID OutlineDockID = 0, RightDockID = 0, RightBottomDockID = 0;
        SplitDockingLayoutLeftRight(InDockspaceID, 0.4f, OutlineDockID, RightDockID);
        const ImGuiID DetailsDockID = ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Up, 0.6f, nullptr, &RightBottomDockID);

        ImGui::DockBuilderDockWindow(*GetToolWindowName(OutlineWindowName), OutlineDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(DetailsWindowName), DetailsDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(DebuggerWindowName), RightBottomDockID);
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
        DrawValidation();

        if (ImGui::BeginChild("##Cards"))
        {
            Cards.Reset();
            Cards.SetNum(Tree->States.Num());
            LastTransition = Debugger.GetLastTransition(*Tree);
            TVector<bool> Drawn(Tree->States.Num(), false);

            // Container frames go on the channel under the cards, since their size is known only after the children are drawn.
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->ChannelsSplit(2);
            DrawList->ChannelsSetCurrent(1);
            DrawStates(*Tree, FName(), 0, Drawn, true);

            // A state whose parent is missing or loops still gets a card, at the top level, so it can be fixed.
            for (int32 State = 0; State < Drawn.Num(); ++State)
            {
                if (!Drawn[State])
                {
                    DrawStateCard(*Tree, State, 0);
                }
            }
            DrawList->ChannelsMerge();
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

    ImVec2 FStateTreeEditorTool::DrawStates(CStateTree& Tree, FName Parent, int32 Depth, TVector<bool>& Drawn, bool bShown)
    {
        ImVec2 Extent(-FLT_MAX, -FLT_MAX);
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
                Extent = ImMax(Extent, Cards[State].Max);
            }
            const ImVec2 Children = DrawStates(Tree, Tree.States[State].Name, Depth + 1, Drawn, bShown && !Algo::Contains(CollapsedStates, Tree.States[State].Name));
            if (!bShown || Children.y < Cards[State].Max.y)
            {
                continue;
            }

            // The card heads a frame around its children, so a group reads as the container it is.
            const FCardLayout& Card = Cards[State];
            const ImVec2 FrameMin(Card.Min.x - ContainerPadding, Card.Min.y - ContainerPadding);
            const ImVec2 FrameMax(Math::Max(Card.Max.x, Children.x) + ContainerPadding, Children.y + ContainerPadding);
            const ImVec4 Color = KindColor(Tree.States[State].Kind);
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->ChannelsSetCurrent(0);
            DrawList->AddRectFilled(FrameMin, FrameMax, EditorColors::U32(EditorColors::WithAlpha(Color, 0.05f)), CardRounding + ContainerPadding);
            DrawList->AddRect(FrameMin, FrameMax, EditorColors::U32(EditorColors::WithAlpha(Color, 0.3f)), CardRounding + ContainerPadding);
            DrawList->ChannelsSetCurrent(1);
            Extent = ImMax(Extent, FrameMax);

            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, FrameMax.y + CardSpacing));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        return Extent;
    }

    float FStateTreeEditorTool::GetGutterWidth() const
    {
        return ArrowLaneWidth * (float)ArrowLanes + 10.0f;
    }

    void FStateTreeEditorTool::DrawStateCard(CStateTree& Tree, int32 State, int32 Depth)
    {
        const SStateTreeState& Source = Tree.States[State];
        const TOptional<EStateTreeRunStatus> Active = Debugger.GetStateStatus(Tree, State);
        const ImVec4 ActiveColor = Active ? FStateTreeDebugger::GetStatusColor(*Active) : EditorColors::Accent();

        TVector<FCardRow> Rows;
        auto AddNodeRow = [&](const auto& Nodes, EStateTreeSlot Slot, const char* Icon, const ImVec4& Color)
        {
            if (Nodes.IsEmpty())
            {
                return;
            }
            FCardRow& Row = Rows.EmplaceGetRef();
            Row.Icon = Icon;
            for (int32 Index = 0; Index < Nodes.Num(); ++Index)
            {
                const bool bTask = Slot == EStateTreeSlot::Task;
                Row.Chips.Add({ bTask ? NodeLabel(Nodes[Index]) : ConditionLabel(Nodes[Index], Index), { State, Slot, Constants::kIndexNone, Index }, Color, bTask && Active.IsSet() });
            }
        };
        AddNodeRow(Source.EnterConditions, EStateTreeSlot::EnterCondition, LE_ICON_FILTER, EditorColors::Warning());
        AddNodeRow(Source.Tasks, EStateTreeSlot::Task, LE_ICON_PLAY, ActiveColor);
        for (int32 Transition = 0; Transition < Source.Transitions.Num(); ++Transition)
        {
            const FStateTreeSelection Target = { State, EStateTreeSlot::Transition, Transition };
            AddTransitionChips(Rows.EmplaceGetRef(), Source.Transitions[Transition], Target, LastTransition == Target);
        }

        const ImGuiStyle& Style = ImGui::GetStyle();
        const CStateTree* Linked = Source.Kind == EStateTreeStateKind::LinkedSubtree ? Source.LinkedSubtree.Get() : nullptr;
        const FString LinkedLabel = Linked != nullptr ? Format("{} {}", LE_ICON_ARROW_RIGHT_THIN, Linked->GetName()) : FString();
        float ContentWidth = ImGui::GetTextLineHeight() + 2.0f + ImGui::CalcTextSize(KindIcon(Source.Kind)).x + Style.ItemSpacing.x + ImGui::CalcTextSize(*Source.Name.ToString()).x
            + (Linked != nullptr ? Style.ItemSpacing.x + ImGui::CalcTextSize(*LinkedLabel).x : 0.0f) + (Active ? Style.ItemSpacing.x + ImGui::CalcTextSize(LE_ICON_LIGHTNING_BOLT).x : 0.0f);
        for (const FCardRow& Row : Rows)
        {
            ContentWidth = Math::Max(ContentWidth, RowWidth(Row));
        }

        const float LineHeight = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 Start = ImGui::GetCursorScreenPos();
        const ImVec2 Min(Start.x + GetGutterWidth() + Depth * CardIndent, Start.y);
        const float Width = Math::Max(CardMinWidth, ContentWidth + CardPadding * 2.0f + 4.0f);
        const ImVec2 Max(Min.x + Math::Min(Width, Math::Max(CardMinWidth, Start.x + ImGui::GetContentRegionAvail().x - Min.x)), Start.y + (1 + Rows.Num()) * LineHeight + CardPadding * 2.0f);

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

        const bool bSelectedState = Selection.State == State && Selection.Slot == EStateTreeSlot::State;
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(Min, Max, EditorColors::U32(bSelectedState ? EditorColors::RowBgActive() : bHovered ? EditorColors::RowBgHovered() : EditorColors::RowBg()), CardRounding);
        if (Active)
        {
            DrawList->AddRectFilled(Min, Max, EditorColors::U32(EditorColors::WithAlpha(ActiveColor, 0.16f)), CardRounding);
        }
        DrawList->AddRectFilled(Min, ImVec2(Min.x + 3.0f, Max.y), EditorColors::U32(KindColor(Source.Kind)), CardRounding, ImDrawFlags_RoundCornersLeft);
        const bool bOutlined = Active || Selection.State == State;
        DrawList->AddRect(Min, Max, EditorColors::U32(Active ? ActiveColor : bOutlined ? EditorColors::Accent() : EditorColors::Border()), CardRounding, bOutlined ? 2.0f : 1.0f);
        if (bSelectedState)
        {
            const float Glow = 1.0f + SelectionGlow * 0.5f;
            DrawList->AddRect(Min - ImVec2(Glow, Glow), Max + ImVec2(Glow, Glow), EditorColors::ScaleAlpha(EditorColors::U32(EditorColors::Accent()), 0.22f), CardRounding + Glow, SelectionGlow);
        }

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
            if (Linked != nullptr)
            {
                ImGui::SameLine();
                ImGui::TextColored(EditorColors::TextDim(), "%s", *LinkedLabel);
            }
            if (Active)
            {
                ImGui::SameLine();
                ImGui::TextColored(ActiveColor, "%s", LE_ICON_LIGHTNING_BOLT);
            }
        }

        Card.TransitionY.Reset();
        int32 ChipId = 0;
        for (const FCardRow& Row : Rows)
        {
            Y += LineHeight;
            ImVec2 Pos(X, Y);
            if (Row.Icon != nullptr)
            {
                ImGui::SetCursorScreenPos(Pos);
                ImGui::TextColored(EditorColors::TextDim(), "%s", Row.Icon);
                Pos.x = ImGui::GetItemRectMax().x + Style.ItemSpacing.x;
            }
            if (Row.Chips[0].Target.Slot == EStateTreeSlot::Transition)
            {
                Card.TransitionY.Add(Y + LineHeight * 0.5f);
            }
            for (const FCardChip& Chip : Row.Chips)
            {
                if (DrawChip(Pos, ChipId++, Chip, Selection == Chip.Target))
                {
                    Select(Chip.Target);
                }
            }
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
        Arrows.Reset();
        TVector<int32> Arrivals(Tree.States.Num(), 0);
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
                if (!Cards[Target].bVisible)
                {
                    continue;
                }
                const FStateTreeSelection Selected = { State, EStateTreeSlot::Transition, Transition };
                Arrows.Add({ Card.Min.x, Card.TransitionY[Transition], 0.0f, 4.0f, Target, Constants::kIndexNone, EditorColors::U32(TriggerColor(Transitions[Transition].Trigger)),
                    Selection == Selected, LastTransition == Selected });
                ++Arrivals[Target];
            }
        }

        // Each arrow lands at its own height around the target's header, in the order the arrows leave, so none shares a segment.
        Algo::Sort(Arrows, [](const FArrow& A, const FArrow& B) { return A.FromY < B.FromY; });
        TVector<int32> Landed(Tree.States.Num(), 0);
        for (FArrow& Arrow : Arrows)
        {
            const int32 Count = Arrivals[Arrow.Target];
            const float Spacing = Math::Min(ArrivalSpacing, (ImGui::GetTextLineHeight() + CardPadding) / (float)Count);
            Arrow.ToY = Cards[Arrow.Target].HeaderY + ((float)Landed[Arrow.Target]++ - (float)(Count - 1) * 0.5f) * Spacing;
            Arrow.HeadSize = Math::Min(4.0f, Spacing * 0.5f);
        }

        // Shorter arrows take the lanes nearest the cards and longer ones go around them, each lane holding arrows whose spans do not touch.
        auto Top = [](const FArrow& Arrow) { return Math::Min(Arrow.FromY, Arrow.ToY) - 2.0f; };
        auto Bottom = [](const FArrow& Arrow) { return Math::Max(Arrow.FromY, Arrow.ToY) + 2.0f; };
        Algo::Sort(Arrows, [&](const FArrow& A, const FArrow& B) { return Bottom(A) - Top(A) < Bottom(B) - Top(B); });
        ArrowLanes = 1;
        for (int32 Index = 0; Index < Arrows.Num(); ++Index)
        {
            FArrow& Arrow = Arrows[Index];
            auto Collides = [&](const FArrow& Other) { return Other.Lane == Arrow.Lane && Top(Other) < Bottom(Arrow) && Top(Arrow) < Bottom(Other); };
            Arrow.Lane = 0;
            while (Algo::AnyOf(Arrows.begin(), Arrows.begin() + Index, Collides))
            {
                ++Arrow.Lane;
            }
            ArrowLanes = Math::Max(ArrowLanes, Arrow.Lane + 1);
        }

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const float GutterRight = ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x - ImGui::GetScrollX() + GetGutterWidth();
        const ImU32 Casing = EditorColors::U32(EditorColors::WindowBg());
        const ImU32 SelectedGlow = EditorColors::ScaleAlpha(EditorColors::U32(EditorColors::Accent()), 0.85f);
        const ImU32 LastTakenGlow = EditorColors::U32(EditorColors::WithAlpha(EditorColors::Warning(), 0.5f));
        // Highlighted arrows go last so they cross over the rest, and every arrow is cased so the one on top at a crossing is plain.
        for (const bool bHighlightedPass : { false, true })
        {
            for (const FArrow& Arrow : Arrows)
            {
                const bool bHighlighted = Arrow.bSelected || Arrow.bLastTaken;
                if (bHighlighted != bHighlightedPass)
                {
                    continue;
                }
                const float LaneX = GutterRight - ArrowLaneWidth * (float)(Arrow.Lane + 1);
                const float ToX = Cards[Arrow.Target].Min.x - 1.0f;
                const ImVec2 Points[] = { ImVec2(Arrow.FromX, Arrow.FromY), ImVec2(LaneX, Arrow.FromY), ImVec2(LaneX, Arrow.ToY), ImVec2(ToX - Arrow.HeadSize, Arrow.ToY) };
                const float Thickness = bHighlighted ? 2.0f : 1.5f;
                DrawList->AddPolyline(Points, IM_ARRAYSIZE(Points), Casing, Thickness + 3.0f);
                if (bHighlighted)
                {
                    DrawList->AddPolyline(Points, IM_ARRAYSIZE(Points), Arrow.bSelected ? SelectedGlow : LastTakenGlow, Thickness + 3.0f);
                }
                DrawList->AddPolyline(Points, IM_ARRAYSIZE(Points), Arrow.Color, Thickness);
                DrawList->AddCircleFilled(Points[0], 2.5f, Arrow.Color);
                DrawList->AddTriangleFilled(ImVec2(ToX - Arrow.HeadSize * 1.5f, Arrow.ToY - Arrow.HeadSize), ImVec2(ToX - Arrow.HeadSize * 1.5f, Arrow.ToY + Arrow.HeadSize), ImVec2(ToX, Arrow.ToY), Arrow.Color);
            }
        }
    }

    void FStateTreeEditorTool::DrawValidation()
    {
        // The status closes the toolbar row on its right, and any issues list under it, above the cards they point at.
        const int32 NumErrors = (int32)Algo::CountIf(Issues, [](const FStateTreeIssue& Issue) { return Issue.bError; });
        const FString Status = Issues.IsEmpty() ? Format("{} Compiled", LE_ICON_CHECK_CIRCLE) : Format("{} {} errors, {} warnings", LE_ICON_ALERT_CIRCLE, NumErrors, Issues.Num() - NumErrors);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Math::Max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(*Status).x));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Issues.IsEmpty() ? EditorColors::Success() : NumErrors > 0 ? EditorColors::Danger() : EditorColors::Warning(), "%s", *Status);
        ImGui::Separator();
        if (Issues.IsEmpty())
        {
            return;
        }

        if (ImGui::BeginChild("##Issues", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * Math::Min(Issues.Num(), MaxIssueRows))))
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
            DrawEmptyState(LE_ICON_STATE_MACHINE, "Nothing selected", "Select a state, a node or a transition in the outline.");
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
