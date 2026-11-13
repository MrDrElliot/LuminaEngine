#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "AI/StateTree/StateTreeTestTypes.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/UpdateContext.h"
#include "UI/Tools/AssetEditors/StateTree/StateTreeEditorTool.h"

namespace Lumina
{
    struct FStateTreeEditorToolTestAccess
    {
        static void AddState(FStateTreeEditorTool& Tool, FName Parent) { Tool.AddState(Parent, Constants::kIndexNone); }
        static const TVector<FStateTreeIssue>& GetIssues(const FStateTreeEditorTool& Tool) { return Tool.Issues; }
        static void ActivateIssue(FStateTreeEditorTool& Tool, int32 Index) { Tool.ActivateIssue(Index); }
        static const FStateTreeSelection& GetSelection(const FStateTreeEditorTool& Tool) { return Tool.Selection; }
        static void Select(FStateTreeEditorTool& Tool, const FStateTreeSelection& Selection) { Tool.Select(Selection); }
        static void Collapse(FStateTreeEditorTool& Tool, FName State) { Tool.CollapsedStates.Add(State); }
        static bool IsCardVisible(const FStateTreeEditorTool& Tool, int32 State) { return Tool.Cards.IsValidIndex(State) && Tool.Cards[State].bVisible; }
        static const auto& GetCard(const FStateTreeEditorTool& Tool, int32 State) { return Tool.Cards[State]; }
        static const auto& GetArrows(const FStateTreeEditorTool& Tool) { return Tool.Arrows; }

        // The details and parameter panels are property tables, which need the engine a test binary does not start.
        static void DrawPanels(FStateTreeEditorTool& Tool)
        {
            Tool.DrawOutline();
            Tool.DrawEvaluators();
        }
    };
}

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeEditorTests
{
    using namespace Lumina;
    using namespace Lumina::StateTreeTest;
    using FAccess = FStateTreeEditorToolTestAccess;

    CStateTree* NewTree(const char* PackagePath = nullptr)
    {
        RegisterPendingTypes();
        if (PackagePath != nullptr)
        {
            return NewObject<CStateTree>(CPackage::CreatePackage(PackagePath), "Tree");
        }
        return NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    int32 IndexOf(const CStateTree& Tree, const char* Name)
    {
        return Algo::IndexOf(Tree.States, FName(Name), &SStateTreeState::Name);
    }

    bool HasSource(const TVector<FStateTreeBindingSource>& Sources, const char* Description)
    {
        return Algo::AnyOf(Sources, [Description](const FStateTreeBindingSource& Source) { return Source.Item.Key == Description; });
    }

    const SStateTreeBindingPath& FindSource(const TVector<FStateTreeBindingSource>& Sources, const char* Description)
    {
        return Sources.FindByPredicate([Description](const FStateTreeBindingSource& Source) { return Source.Item.Key == Description; })->Path;
    }

    TEST(StateTreeEditor, AddedStatesGetFreeNamesAndARenameCarriesParentsAndTargets)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Root");
        const int32 First = StateTreeEditing::AddState(*Tree, FName("Root"));
        const int32 Second = StateTreeEditing::AddState(*Tree, FName("Root"));
        EXPECT_NE(Tree->States[First].Name, Tree->States[Second].Name);
        AddTransition(Tree->States[Second], EStateTreeTransitionTrigger::OnCompleted, EStateTreeTransitionTarget::State, "Root");

        ASSERT_TRUE(StateTreeEditing::RenameState(*Tree, IndexOf(*Tree, "Root"), FName("Top")));
        EXPECT_EQ(Tree->States[First].Parent, FName("Top"));
        EXPECT_EQ(Tree->States[Second].Parent, FName("Top"));
        EXPECT_EQ(Tree->States[Second].Transitions[0].TargetState, FName("Top"));
        EXPECT_FALSE(StateTreeEditing::RenameState(*Tree, First, Tree->States[Second].Name));
    }

    TEST(StateTreeEditor, MovingAStateReparentsAndReordersButNeverIntoItsOwnSubtree)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Root");
        AddState(*Tree, "A", "Root");
        AddState(*Tree, "B", "Root");
        AddState(*Tree, "A1", "A");

        EXPECT_EQ(StateTreeEditing::MoveState(*Tree, IndexOf(*Tree, "A"), FName("A1"), Tree->States.Num()), Constants::kIndexNone);
        EXPECT_EQ(Tree->States[IndexOf(*Tree, "A")].Parent, FName("Root"));

        EXPECT_NE(StateTreeEditing::MoveState(*Tree, IndexOf(*Tree, "A1"), FName("B"), Tree->States.Num()), Constants::kIndexNone);
        EXPECT_EQ(Tree->States[IndexOf(*Tree, "A1")].Parent, FName("B"));

        StateTreeEditing::MoveState(*Tree, IndexOf(*Tree, "B"), FName("Root"), IndexOf(*Tree, "A"));
        EXPECT_LT(IndexOf(*Tree, "B"), IndexOf(*Tree, "A"));
        EXPECT_TRUE(Tree->Compile());
    }

    TEST(StateTreeEditor, ADuplicatedSubtreeGetsNewNamesAndKeepsItsTransitionsInside)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Root");
        AddState(*Tree, "A", "Root");
        AddState(*Tree, "A1", "A");
        AddTransition(Tree->States[IndexOf(*Tree, "A1")], EStateTreeTransitionTrigger::OnCompleted, EStateTreeTransitionTarget::State, "A");

        const int32 Copy = StateTreeEditing::DuplicateState(*Tree, IndexOf(*Tree, "A"));
        ASSERT_EQ(Tree->States.Num(), 5);
        const FName CopyName = Tree->States[Copy].Name;
        EXPECT_NE(CopyName, FName("A"));
        EXPECT_EQ(Tree->States[Copy].Parent, FName("Root"));

        const SStateTreeState* CopiedChild = Tree->States.FindByPredicate([&](const SStateTreeState& State) { return State.Parent == CopyName; });
        ASSERT_NE(CopiedChild, nullptr);
        EXPECT_NE(CopiedChild->Name, FName("A1"));
        EXPECT_EQ(CopiedChild->Transitions[0].TargetState, CopyName);
        EXPECT_TRUE(Tree->Compile());

        StateTreeEditing::DeleteState(*Tree, Copy);
        EXPECT_EQ(Tree->States.Num(), 3);
    }

    TEST(StateTreeEditor, AnOutlineEditIsOneUndoStep)
    {
        FImGuiTestHarness Harness;
        CStateTree* Tree = NewTree("/StateTreeEditorTests/Undo");
        AddState(*Tree, "Root");
        FStateTreeEditorTool Tool(nullptr, Tree);

        auto Frame = [&]
        {
            FImGuiTestHarness::BeginFrame();
            Tool.Update(FUpdateContext());
            FImGuiTestHarness::EndFrame();
        };
        Frame();

        FAccess::AddState(Tool, FName("Root"));
        ASSERT_EQ(Tree->States.Num(), 2);
        Frame();

        ASSERT_TRUE(Tool.RunUndo());
        EXPECT_EQ(Tree->States.Num(), 1);
        EXPECT_EQ(Tree->States[0].Name, FName("Root"));

        ASSERT_TRUE(Tool.RunRedo());
        EXPECT_EQ(Tree->States.Num(), 2);
    }

    TEST(StateTreeEditor, TheBindingPickerOffersOnlyVisibleSourcesOfTheFieldsType)
    {
        CStateTree* Tree = NewTree();
        (void)Tree->Parameters.InitializeAs<SStateTreeTestParameters>();
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Eval");
        AddState(*Tree, "Root");
        AddState(*Tree, "Leaf", "Root");
        AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Outer");
        AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "First");
        AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "Second");

        const FProperty& Input = *SStateTreeTestTaskData::StaticStruct()->GetProperty(FName("Input"));
        const FStateTreeSelection Second = { 1, EStateTreeSlot::Task, Constants::kIndexNone, 1 };
        TVector<FStateTreeBindingSource> Sources;
        StateTreeEditing::GatherBindingSources(*Tree, Second, Input, true, Sources);

        EXPECT_TRUE(HasSource(Sources, "Parameters.Speed"));
        EXPECT_TRUE(HasSource(Sources, "Eval.Counter"));
        EXPECT_TRUE(HasSource(Sources, "Eval.GetDoubledCounter"));
        EXPECT_TRUE(HasSource(Sources, "First.Output"));
        EXPECT_TRUE(HasSource(Sources, "Outer.Output"));
        EXPECT_TRUE(HasSource(Sources, "CStateTreeTestLibrary.AddInts()"));
        EXPECT_FALSE(HasSource(Sources, "Context.DeltaTime"));
        EXPECT_FALSE(HasSource(Sources, "First.DeltaTime"));
        EXPECT_FALSE(HasSource(Sources, "Second.Output"));
        EXPECT_FALSE(HasSource(Sources, "CStateTreeTestLibrary.ReturnsNothing()"));

        TVector<FStateTreeBindingSource> EnterSources;
        StateTreeEditing::GatherBindingSources(*Tree, { 1, EStateTreeSlot::EnterCondition, Constants::kIndexNone, 0 }, Input, true, EnterSources);
        EXPECT_FALSE(HasSource(EnterSources, "First.Output"));
        EXPECT_TRUE(HasSource(EnterSources, "Outer.Output"));

        // Every offered source here binds, the function through arguments the argument picker offers.
        for (const FStateTreeBindingSource& Source : Sources)
        {
            if (Source.Path.Source == EStateTreeBindingSource::Function && Source.Path.SourceNode != FName("CStateTreeTestLibrary"))
            {
                continue;
            }
            SStateTreeBinding& Binding = StateTreeEditing::SetBinding(*Tree, Tree->States[1].Tasks[1].GetMutable()->Bindings, FName("Input"), Source.Path);
            if (const FFunction* Function = StateTreeEditing::FindBoundFunction(*Tree, Binding))
            {
                ASSERT_EQ(Binding.Arguments.Num(), Function->GetArguments().Num());
                for (int32 Argument = 0; Argument < Binding.Arguments.Num(); ++Argument)
                {
                    TVector<FStateTreeBindingSource> ArgumentSources;
                    StateTreeEditing::GatherBindingSources(*Tree, Second, *Function->GetArguments()[Argument], false, ArgumentSources);
                    ASSERT_TRUE(HasSource(ArgumentSources, "Parameters.Speed"));
                    Binding.Arguments[Argument] = FindSource(ArgumentSources, "Parameters.Speed");
                }
            }
            EXPECT_TRUE(Tree->Compile()) << *Source.Item.Key;
            Tree->States[1].Tasks[1].GetMutable()->Bindings.Reset();
        }
    }

    TEST(StateTreeEditor, ValidationNamesTheOffendingItemAndClickingItSelectsIt)
    {
        FImGuiTestHarness Harness;
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Root");
        AddState(*Tree, "Walk", "Root");
        AddState(*Tree, "Empty", "Root", EStateTreeStateKind::Group);
        AddTransition(Tree->States[1], EStateTreeTransitionTrigger::OnCompleted, EStateTreeTransitionTarget::State, "Missing");
        Bind(AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "Mover"), "Input", EStateTreeBindingSource::Context, "", "DeltaTime");

        FStateTreeEditorTool Tool(nullptr, Tree);
        const TVector<FStateTreeIssue>& Issues = FAccess::GetIssues(Tool);
        const int32 Transition = Algo::IndexOfIf(Issues, [](const FStateTreeIssue& Issue) { return Issue.Message.Contains("missing state 'Missing'"); });
        const int32 Binding = Algo::IndexOfIf(Issues, [](const FStateTreeIssue& Issue) { return Issue.Message.Contains("DeltaTime"); });
        const int32 Group = Algo::IndexOfIf(Issues, [](const FStateTreeIssue& Issue) { return !Issue.bError; });
        ASSERT_NE(Transition, Constants::kIndexNone);
        ASSERT_NE(Binding, Constants::kIndexNone);
        ASSERT_NE(Group, Constants::kIndexNone);
        EXPECT_EQ(Issues[Group].Target.State, IndexOf(*Tree, "Empty"));

        FAccess::Collapse(Tool, FName("Root"));
        FAccess::ActivateIssue(Tool, Transition);
        EXPECT_EQ(FAccess::GetSelection(Tool), FStateTreeSelection{ IndexOf(*Tree, "Walk") });

        FAccess::ActivateIssue(Tool, Binding);
        const FStateTreeSelection Mover = { IndexOf(*Tree, "Walk"), EStateTreeSlot::Task, Constants::kIndexNone, 0 };
        EXPECT_EQ(FAccess::GetSelection(Tool), Mover);

        // Jumping to an issue opens the collapsed parent, so the selected card is on screen.
        FImGuiTestHarness::BeginFrame();
        FAccess::DrawPanels(Tool);
        FImGuiTestHarness::EndFrame();
        EXPECT_TRUE(FAccess::IsCardVisible(Tool, IndexOf(*Tree, "Walk")));
    }

    TEST(StateTreeEditor, TheOutlineDrawsEverySelectionKindWithCollapsedStatesHidden)
    {
        FImGuiTestHarness Harness;
        CStateTree* Tree = NewTree();
        (void)Tree->Parameters.InitializeAs<SStateTreeTestParameters>();
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Eval");
        AddState(*Tree, "Root");
        AddState(*Tree, "Leaf", "Root");
        AddNode<SStateTreeTestCondition>(Tree->States[1].EnterConditions, "Gate");
        Bind(AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "Task"), "Input", EStateTreeBindingSource::Parameter, "", "Speed");
        AddNode<SStateTreeTestCondition>(AddTransition(Tree->States[1], EStateTreeTransitionTrigger::OnSucceeded, EStateTreeTransitionTarget::State, "Root").Conditions, "Check");

        FStateTreeEditorTool Tool(nullptr, Tree);
        const FStateTreeSelection Selections[] =
        {
            { 0 },
            { 1, EStateTreeSlot::EnterCondition, Constants::kIndexNone, 0 },
            { 1, EStateTreeSlot::Task, Constants::kIndexNone, 0 },
            { 1, EStateTreeSlot::Transition, 0 },
            { 1, EStateTreeSlot::TransitionCondition, 0, 0 },
            { Constants::kIndexNone, EStateTreeSlot::Evaluator, Constants::kIndexNone, 0 },
        };
        for (const FStateTreeSelection& Selection : Selections)
        {
            FAccess::Select(Tool, Selection);
            for (int32 Frame = 0; Frame < 2; ++Frame)
            {
                FImGuiTestHarness::BeginFrame();
                FAccess::DrawPanels(Tool);
                FImGuiTestHarness::EndFrame();
            }
            EXPECT_TRUE(FAccess::IsCardVisible(Tool, 1));
        }

        FAccess::Collapse(Tool, FName("Root"));
        FImGuiTestHarness::BeginFrame();
        FAccess::DrawPanels(Tool);
        FImGuiTestHarness::EndFrame();
        EXPECT_TRUE(FAccess::IsCardVisible(Tool, 0));
        EXPECT_FALSE(FAccess::IsCardVisible(Tool, 1));
    }

    // Each row's chips sit on that row, not on the row above it.
    TEST(StateTreeEditor, EveryChipIsClickableOnItsOwnRow)
    {
        FImGuiTestHarness Harness;
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Root");
        AddState(*Tree, "Idle", "Root");
        AddState(*Tree, "Patrol", "Root");
        AddState(*Tree, "Alert", "Root");
        const int32 Idle = IndexOf(*Tree, "Idle");
        AddNode<SStateTreeTestCondition>(Tree->States[Idle].EnterConditions, "Gate");
        AddNode<SStateTreeTestTask>(Tree->States[Idle].Tasks, "IdleWait");
        AddTransition(Tree->States[Idle], EStateTreeTransitionTrigger::OnCompleted, EStateTreeTransitionTarget::State, "Patrol");
        AddNode<SStateTreeTestCondition>(AddTransition(Tree->States[Idle], EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionTarget::State, "Alert").Conditions, "Sensed");

        FStateTreeEditorTool Tool(nullptr, Tree);
        auto Frame = [&]
        {
            FImGuiTestHarness::BeginFrame();
            FAccess::DrawPanels(Tool);
            FImGuiTestHarness::EndFrame();
        };
        ImGui::GetIO().AddMousePosEvent(-100.0f, -100.0f);
        Frame();

        const auto Card = FAccess::GetCard(Tool, Idle);
        ASSERT_EQ(Card.TransitionY.Num(), 2);
        const float LineHeight = Card.TransitionY[1] - Card.TransitionY[0];

        // Clicks across the row, a little over the double-click distance apart, until one selects Wanted.
        auto ClickAlongRow = [&](float Y, const FStateTreeSelection& Wanted)
        {
            for (float X = Card.Min.x; X < Card.Max.x; X += 8.0f)
            {
                ImGui::GetIO().AddMousePosEvent(X, Y);
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                Frame();
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                Frame();
                if (FAccess::GetSelection(Tool) == Wanted)
                {
                    return true;
                }
            }
            return false;
        };

        EXPECT_TRUE(ClickAlongRow(Card.TransitionY[0] - LineHeight, { Idle, EStateTreeSlot::Task, Constants::kIndexNone, 0 }));
        EXPECT_TRUE(ClickAlongRow(Card.TransitionY[1], { Idle, EStateTreeSlot::TransitionCondition, 1, 0 }));
    }

    // The guard tree from the editor screenshot, with arrows into Idle from three places and across both groups.
    TEST(StateTreeEditor, ArrowsNeverShareALaneSpanOrALandingPoint)
    {
        FImGuiTestHarness Harness;
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Guard", nullptr, EStateTreeStateKind::Group);
        AddState(*Tree, "Idle", "Guard");
        AddState(*Tree, "Patrol", "Guard");
        AddState(*Tree, "Alert", "Guard");
        AddState(*Tree, "Combat", nullptr, EStateTreeStateKind::Group);
        AddState(*Tree, "Engage", "Combat");
        AddState(*Tree, "Search", "Combat");
        const struct { const char* From; EStateTreeTransitionTrigger Trigger; const char* To; } Links[] =
        {
            { "Idle", EStateTreeTransitionTrigger::OnCompleted, "Patrol" },
            { "Idle", EStateTreeTransitionTrigger::OnTick, "Alert" },
            { "Patrol", EStateTreeTransitionTrigger::OnCompleted, "Idle" },
            { "Patrol", EStateTreeTransitionTrigger::OnTick, "Alert" },
            { "Alert", EStateTreeTransitionTrigger::OnCompleted, "Engage" },
            { "Engage", EStateTreeTransitionTrigger::OnFailed, "Search" },
            { "Engage", EStateTreeTransitionTrigger::OnSucceeded, "Idle" },
            { "Search", EStateTreeTransitionTrigger::OnCompleted, "Idle" },
        };
        for (const auto& Link : Links)
        {
            AddTransition(Tree->States[IndexOf(*Tree, Link.From)], Link.Trigger, EStateTreeTransitionTarget::State, Link.To);
        }

        FStateTreeEditorTool Tool(nullptr, Tree);
        for (int32 Frame = 0; Frame < 2; ++Frame)
        {
            FImGuiTestHarness::BeginFrame();
            FAccess::DrawPanels(Tool);
            FImGuiTestHarness::EndFrame();
        }

        const auto& Arrows = FAccess::GetArrows(Tool);
        ASSERT_EQ(Arrows.Num(), (int32)std::size(Links));
        for (int32 First = 0; First < Arrows.Num(); ++First)
        {
            const auto& A = Arrows[First];
            const auto& Target = FAccess::GetCard(Tool, A.Target);
            EXPECT_GT(A.ToY, Target.Min.y);
            EXPECT_LT(A.ToY, Target.Min.y + (Target.Max.y - Target.Min.y) * 0.5f);
            for (int32 Second = First + 1; Second < Arrows.Num(); ++Second)
            {
                const auto& B = Arrows[Second];
                if (A.Lane == B.Lane)
                {
                    EXPECT_TRUE(Math::Max(A.FromY, A.ToY) < Math::Min(B.FromY, B.ToY) || Math::Max(B.FromY, B.ToY) < Math::Min(A.FromY, A.ToY)) << First << " and " << Second;
                }
                if (A.Target == B.Target)
                {
                    EXPECT_GE(Math::Abs(A.ToY - B.ToY), A.HeadSize + B.HeadSize - 0.01f) << First << " and " << Second;
                }
            }
        }
    }

    TEST(StateTreeEditor, CardsSizeToTheirContentAndGrowForTransitionBadges)
    {
        FImGuiTestHarness Harness;
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Root", nullptr, EStateTreeStateKind::Group);
        AddState(*Tree, "Search", "Root");
        const int32 Search = IndexOf(*Tree, "Search");
        AddTransition(Tree->States[Search], EStateTreeTransitionTrigger::OnCompleted, EStateTreeTransitionTarget::State, "Root");

        FStateTreeEditorTool Tool(nullptr, Tree);
        auto CardWidth = [&]
        {
            FImGuiTestHarness::BeginFrame();
            FAccess::DrawPanels(Tool);
            FImGuiTestHarness::EndFrame();
            const auto& Card = FAccess::GetCard(Tool, Search);
            return Card.Max.x - Card.Min.x;
        };

        const float Plain = CardWidth();
        EXPECT_LT(Plain, FImGuiTestHarness::kDisplaySize.x * 0.5f);

        SStateTreeTransition& Transition = Tree->States[Search].Transitions[0];
        Transition.Delay = 2.0f;
        const float Delayed = CardWidth();
        EXPECT_GT(Delayed, Plain);

        Transition.DelayRandomRange = 0.5f;
        const float Ranged = CardWidth();
        EXPECT_GT(Ranged, Delayed);

        Transition.Priority = 3;
        EXPECT_GT(CardWidth(), Ranged);
    }
}
