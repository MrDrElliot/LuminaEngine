#include <gtest/gtest.h>
#include "Tools/UI/ImGui/Widgets/TreeListView.h"

using namespace Lumina;

namespace
{
    struct FSelectionLog
    {
        TVector<int32> Selected;
        int32 LastReported = -1;
    };

    // Mirrors the outliner, which replaces its selection on a clearing report and toggles otherwise.
    FTreeListViewContext MakeContext(FSelectionLog& Log)
    {
        FTreeListViewContext Context;
        Context.bAllowRangeSelect = true;
        Context.ItemSelectedFunction = [&Log](FTreeListView&, FTreeNodeID Item, bool bShouldClear)
        {
            if (bShouldClear)
            {
                Log.Selected.Reset();
            }
            if (!Item.IsValid())
            {
                return;
            }
            if (const int32 Found = Log.Selected.Find(Item.Index); Found != Constants::kIndexNone)
            {
                Log.Selected.RemoveAt(Found);
            }
            else
            {
                Log.Selected.Add(Item.Index);
            }
            Log.LastReported = Item.Index;
        };
        return Context;
    }

    TVector<FTreeNodeID> MakeRows(FTreeListView& Tree, int32 Count)
    {
        TVector<FTreeNodeID> Rows;
        for (int32 i = 0; i < Count; ++i)
        {
            Rows.Add(Tree.CreateNode(InvalidTreeNode, "Row", (uint64)(i + 1)));
        }
        return Rows;
    }

    TVector<int32> SelectedRows(FTreeListView& Tree, const TVector<FTreeNodeID>& Rows)
    {
        TVector<int32> Out;
        for (int32 i = 0; i < Rows.Num(); ++i)
        {
            if (Tree.Get<FTreeNodeState>(Rows[i]).bSelected)
            {
                Out.Add(i);
            }
        }
        return Out;
    }
}

TEST(TreeListViewSelection, ShiftClickSelectsTheRunFromTheLastPlainClick)
{
    FTreeListView Tree;
    FSelectionLog Log;
    const FTreeListViewContext Context = MakeContext(Log);
    const TVector<FTreeNodeID> Rows = MakeRows(Tree, 5);

    Tree.ClickRow(Rows[1], false, false, Context);
    Tree.ClickRow(Rows[3], true, false, Context);

    EXPECT_EQ(SelectedRows(Tree, Rows), (TVector<int32>{ 1, 2, 3 }));
    EXPECT_EQ(Log.Selected.Num(), 3);
    EXPECT_EQ(Log.LastReported, Rows[3].Index) << "the clicked row is the one the details panel ends on";
}

// The viewport or an undo selects by writing row state, and the first shift-click used to act as a plain click.
TEST(TreeListViewSelection, ShiftClickRangesFromASelectionMadeElsewhere)
{
    FTreeListView Tree;
    FSelectionLog Log;
    const FTreeListViewContext Context = MakeContext(Log);
    const TVector<FTreeNodeID> Rows = MakeRows(Tree, 5);

    Tree.Get<FTreeNodeState>(Rows[4]).bSelected = true;
    Log.Selected = { Rows[4].Index };

    Tree.ClickRow(Rows[1], true, false, Context);

    EXPECT_EQ(SelectedRows(Tree, Rows), (TVector<int32>{ 1, 2, 3, 4 }));
    EXPECT_EQ(Log.Selected.Num(), 4);
    EXPECT_EQ(Log.LastReported, Rows[1].Index);
}

// A rebuild recreates every row, which used to drop the anchor.
TEST(TreeListViewSelection, ShiftClickSurvivesARebuild)
{
    FTreeListView Tree;
    FSelectionLog Log;
    const FTreeListViewContext Context = MakeContext(Log);
    TVector<FTreeNodeID> Rows = MakeRows(Tree, 5);

    Tree.ClickRow(Rows[0], false, false, Context);
    Tree.ClearTree();
    Rows = MakeRows(Tree, 5);
    Tree.Get<FTreeNodeState>(Rows[0]).bSelected = true;

    Tree.ClickRow(Rows[2], true, false, Context);
    EXPECT_EQ(SelectedRows(Tree, Rows), (TVector<int32>{ 0, 1, 2 }));
}

TEST(TreeListViewSelection, CtrlShiftExtendsWithoutDroppingTheRest)
{
    FTreeListView Tree;
    FSelectionLog Log;
    const FTreeListViewContext Context = MakeContext(Log);
    const TVector<FTreeNodeID> Rows = MakeRows(Tree, 6);

    Tree.ClickRow(Rows[0], false, false, Context);
    Tree.ClickRow(Rows[3], false, true, Context);
    Tree.ClickRow(Rows[5], true, true, Context);

    EXPECT_EQ(SelectedRows(Tree, Rows), (TVector<int32>{ 0, 3, 4, 5 }));
    EXPECT_EQ(Log.Selected.Num(), 4) << "rows already selected are not reported again, which would toggle them off";
}
