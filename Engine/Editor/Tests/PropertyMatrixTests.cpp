#include <gtest/gtest.h>

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/PropertyMatrixTool.h"

namespace Lumina
{
    struct FPropertyMatrixTestAccess
    {
        static void BeginEdit(FPropertyMatrixTool& Tool, TVector<CObject*> Targets, CObject* Source, FProperty* Property)
        {
            Tool.BeginEdit(Property->Name, Move(Targets), Source, Property);
        }

        static void SpreadEdit(FPropertyMatrixTool& Tool) { Tool.SpreadEdit(); }
        static void FinishEdit(FPropertyMatrixTool& Tool) { Tool.FinishEdit(); }
        static bool IsEditing(const FPropertyMatrixTool& Tool) { return Tool.Edit.IsSet(); }
        static const TVector<CObject*>& Targets(const FPropertyMatrixTool& Tool) { return Tool.Edit->Targets; }
        static bool IsRecording(FPropertyMatrixTool& Tool) { return Tool.GetTransactionManager().IsRecording(); }
        static bool CanUndo(const FPropertyMatrixTool& Tool) { return Tool.CanUndo(); }
    };
}

using namespace Lumina;

namespace
{
    // Two tracks open in a matrix, with Source A spreading BindingIndex to B, plus C waiting to stand in for one of them.
    struct FMatrixFixture
    {
        explicit FMatrixFixture(const char* PackagePath)
        {
            RegisterPendingTypes();
            CPackage* Package = CPackage::CreatePackage(PackagePath);
            A = NewObject<CSequenceTrack_Event>(Package, "A");
            B = NewObject<CSequenceTrack_Event>(Package, "B");
            C = NewObject<CSequenceTrack_Event>(Package, "C");
            Property = CSequenceTrack_Event::StaticClass()->GetProperty("BindingIndex");

            Tool = MakeUnique<FPropertyMatrixTool>(nullptr, TVector<FGuid>{ A->GetGUID(), B->GetGUID() });
            FPropertyMatrixTestAccess::BeginEdit(*Tool, { A, B }, A, Property);
            A->BindingIndex = 7;
        }

        void Remap(CObject* From, CObject* To)
        {
            Tool->VisitObjectReferences([From, To](CObject* Object) { return Object == From ? To : Object; });
        }

        CSequenceTrack_Event*           A = nullptr;
        CSequenceTrack_Event*           B = nullptr;
        CSequenceTrack_Event*           C = nullptr;
        FProperty*                      Property = nullptr;
        TUniquePtr<FPropertyMatrixTool> Tool;
    };
}

TEST(PropertyMatrix, ReinstancedTargetReceivesTheSpreadInsteadOfTheOldObject)
{
    FMatrixFixture Matrix("/PropertyMatrix/Retarget");
    ASSERT_NE(Matrix.Property, nullptr);

    Matrix.Remap(Matrix.B, Matrix.C);
    ASSERT_TRUE(FPropertyMatrixTestAccess::IsEditing(*Matrix.Tool));
    EXPECT_EQ(FPropertyMatrixTestAccess::Targets(*Matrix.Tool), (TVector<CObject*>{ Matrix.A, Matrix.C }));

    FPropertyMatrixTestAccess::SpreadEdit(*Matrix.Tool);
    FPropertyMatrixTestAccess::FinishEdit(*Matrix.Tool);
    EXPECT_EQ(Matrix.C->BindingIndex, 7);
    EXPECT_EQ(Matrix.B->BindingIndex, Constants::kIndexNone);
    EXPECT_TRUE(FPropertyMatrixTestAccess::CanUndo(*Matrix.Tool));
}

TEST(PropertyMatrix, NulledTargetLeavesTheEdit)
{
    FMatrixFixture Matrix("/PropertyMatrix/NullTarget");
    ASSERT_NE(Matrix.Property, nullptr);

    Matrix.Remap(Matrix.B, nullptr);
    ASSERT_TRUE(FPropertyMatrixTestAccess::IsEditing(*Matrix.Tool));
    EXPECT_EQ(FPropertyMatrixTestAccess::Targets(*Matrix.Tool), TVector<CObject*>{ Matrix.A });

    FPropertyMatrixTestAccess::SpreadEdit(*Matrix.Tool);
    FPropertyMatrixTestAccess::FinishEdit(*Matrix.Tool);
    EXPECT_EQ(Matrix.B->BindingIndex, Constants::kIndexNone);
    EXPECT_TRUE(FPropertyMatrixTestAccess::CanUndo(*Matrix.Tool));
}

TEST(PropertyMatrix, NulledSourceRollsTheEditBack)
{
    FMatrixFixture Matrix("/PropertyMatrix/NullSource");
    ASSERT_NE(Matrix.Property, nullptr);
    FPropertyMatrixTestAccess::SpreadEdit(*Matrix.Tool);
    ASSERT_EQ(Matrix.B->BindingIndex, 7);

    Matrix.Remap(Matrix.A, nullptr);
    EXPECT_FALSE(FPropertyMatrixTestAccess::IsEditing(*Matrix.Tool));
    EXPECT_EQ(Matrix.B->BindingIndex, Constants::kIndexNone);
    EXPECT_FALSE(FPropertyMatrixTestAccess::IsRecording(*Matrix.Tool));

    FPropertyMatrixTestAccess::SpreadEdit(*Matrix.Tool);
    FPropertyMatrixTestAccess::FinishEdit(*Matrix.Tool);
    EXPECT_FALSE(FPropertyMatrixTestAccess::CanUndo(*Matrix.Tool));
}
