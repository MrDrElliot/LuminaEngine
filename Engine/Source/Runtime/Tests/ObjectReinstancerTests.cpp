#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectReinstancer.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// The reinstancer moves live instances onto a replacement class and repoints whatever pointed at them, which
// is what lets a script type change shape with a world running.

namespace
{
    Scripting::FScriptExportField MakeScalarField(const char* Name, EPropertyTypeFlags Kind)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = Kind;
        return Field;
    }

    CScriptClass* MintWithFields(const char* ClassName, const Scripting::FScriptExportSchema& Schema)
    {
        CScriptClass* Minted = FScriptableRegistry::Mint(ClassName, "CScriptableTest");
        if (Minted == nullptr)
        {
            return nullptr;
        }
        Scripting::AppendScriptPropertiesToClass(Minted, Schema);
        ProcessNewlyLoadedCObjects();
        Minted->GetDefaultObject();
        return Minted;
    }

    int32* FindInt(CObject* Object, const char* FieldName)
    {
        for (FProperty* Property : Object->GetClass()->GetProperties())
        {
            if (Property->Name == FName(FieldName))
            {
                return Property->GetValuePtr<int32>(Object);
            }
        }
        return nullptr;
    }

    // Stands in for any holder the reflected object graph cannot reach, which is the case the providers exist for.
    class FTestReferenceProvider final : public IObjectReferenceProvider
    {
    public:

        CObject* Held = nullptr;

        const char* GetReferenceProviderName() const override { return "test"; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override
        {
            Held = Func(Held);
        }
    };
}

TEST(ObjectReinstancer, MovesInstancesOntoTheReplacementAndKeepsMatchingValues)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Kept", EPropertyTypeFlags::Int32));
    CScriptClass* Old = MintWithFields("Reinst_Before", Before);
    ASSERT_NE(Old, nullptr);

    CObject* Instance = NewObject(Old, nullptr, FName("ReinstSubject"), FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);
    int32* KeptBefore = FindInt(Instance, "Kept");
    ASSERT_NE(KeptBefore, nullptr);
    *KeptBefore = 4321;

    const FName OriginalName = Instance->GetName();
    const FGuid OriginalGuid = Instance->GetGUID();

    // The replacement adds a field, which is exactly the change an in-place rebuild could not make.
    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Kept", EPropertyTypeFlags::Int32));
    After.Fields.push_back(MakeScalarField("Added", EPropertyTypeFlags::Int32));
    CScriptClass* New = MintWithFields("Reinst_After", After);
    ASSERT_NE(New, nullptr);

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    const FReinstanceResult Result = Reinstancer.Commit();

    EXPECT_EQ(Result.InstancesReplaced, 1);

    CObject* Replaced = FindObject<CObject>(OriginalName);
    ASSERT_NE(Replaced, nullptr) << "the replacement must take the original's name";
    EXPECT_EQ(Replaced->GetClass(), New);
    EXPECT_NE(Replaced, Instance);
    EXPECT_EQ(Replaced->GetGUID(), OriginalGuid) << "a reference saved by GUID must still resolve after a reshape";
    EXPECT_EQ(FindObject<CObject>(OriginalGuid), Replaced) << "the GUID must resolve to the replacement, not the original";

    const int32* KeptAfter = FindInt(Replaced, "Kept");
    ASSERT_NE(KeptAfter, nullptr);
    EXPECT_EQ(*KeptAfter, 4321) << "a field present in both shapes keeps its value across the reinstance";

    const int32* Added = FindInt(Replaced, "Added");
    ASSERT_NE(Added, nullptr) << "the replacement carries the new shape";
}

TEST(ObjectReinstancer, RepointsAReferenceHeldOutsideTheObjectGraph)
{
    Scripting::FScriptExportSchema Empty;
    Empty.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));

    CScriptClass* Old = MintWithFields("ReinstRef_Before", Empty);
    CScriptClass* New = MintWithFields("ReinstRef_After", Empty);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    CObject* Instance = NewObject(Old, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);

    FTestReferenceProvider Provider;
    Provider.Held = Instance;
    FObjectReferenceProviders::Register(&Provider);

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    const FReinstanceResult Result = Reinstancer.Commit();

    FObjectReferenceProviders::Unregister(&Provider);

    EXPECT_EQ(Result.InstancesReplaced, 1);
    ASSERT_NE(Provider.Held, nullptr);
    EXPECT_NE(Provider.Held, Instance) << "a holder outside the graph must be repointed, not left dangling";
    EXPECT_EQ(Provider.Held->GetClass(), New);
}

TEST(ObjectReinstancer, AWeakReferenceFollowsTheInstanceOntoTheReplacement)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));

    CScriptClass* Old = MintWithFields("ReinstWeak_Before", Schema);
    CScriptClass* New = MintWithFields("ReinstWeak_After", Schema);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    CObject* Instance = NewObject(Old, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);

    TWeakObjectPtr<CObject> Weak(Instance);
    ASSERT_TRUE(Weak.IsValid());

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    ASSERT_EQ(Reinstancer.Commit().InstancesReplaced, 1);

    ASSERT_TRUE(Weak.IsValid()) << "a reinstanced object did not die, so a weak reference must not read as dead";
    EXPECT_EQ(Weak.Get()->GetClass(), New) << "the weak reference must resolve to the replacement";
}

TEST(ObjectReinstancer, DoesNothingWithoutAMapping)
{
    FObjectReinstancer Reinstancer;
    EXPECT_TRUE(Reinstancer.IsEmpty());

    const FReinstanceResult Result = Reinstancer.Commit();
    EXPECT_EQ(Result.InstancesReplaced, 0);
    EXPECT_EQ(Result.ReferencesPatched, 0);
}

// The seam a language uses to say which of its properties must not survive the migration. Without it the
// reinstancer would have to know what a script attribute means.
TEST(ObjectReinstancer, ThePostReplaceHookSeesEveryReplacementBeforeAnythingPointsAtIt)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));
    CScriptClass* Old = MintWithFields("Reinst_HookBefore", Before);
    ASSERT_NE(Old, nullptr);

    CObject* First  = NewObject(Old, nullptr, FName("HookOne"), FGuid::New(), OF_Transient);
    CObject* Second = NewObject(Old, nullptr, FName("HookTwo"), FGuid::New(), OF_Transient);
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);
    *FindInt(First, "Value")  = 11;
    *FindInt(Second, "Value") = 22;

    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));
    After.Fields.push_back(MakeScalarField("Added", EPropertyTypeFlags::Int32));
    CScriptClass* New = MintWithFields("Reinst_HookAfter", After);
    ASSERT_NE(New, nullptr);

    static int32 GHookCalls = 0;
    GHookCalls = 0;

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    Reinstancer.SetPostReplaceHook([](CObject*, CObject* Replacement)
    {
        ++GHookCalls;
        // Zeroed here, so a value that did carry over proves the hook ran after the copy rather than before.
        if (int32* Value = FindInt(Replacement, "Value"))
        {
            *Value = 0;
        }
    });

    const FReinstanceResult Result = Reinstancer.Commit();

    EXPECT_EQ(Result.InstancesReplaced, 2);
    EXPECT_EQ(GHookCalls, 2) << "the hook runs once per replacement, not once per commit";

    CObject* NewFirst = FindObject<CObject>(FName("HookOne"));
    ASSERT_NE(NewFirst, nullptr);
    EXPECT_EQ(NewFirst->GetClass(), New);
    EXPECT_EQ(*FindInt(NewFirst, "Value"), 0) << "the hook must run after the values are carried over";
}

TEST(ObjectReinstancer, CommitWithNoHookIsUnaffected)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));
    CScriptClass* Old = MintWithFields("Reinst_NoHookBefore", Before);
    ASSERT_NE(Old, nullptr);

    CObject* Instance = NewObject(Old, nullptr, FName("NoHookSubject"), FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);
    *FindInt(Instance, "Value") = 7;

    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));
    After.Fields.push_back(MakeScalarField("Added", EPropertyTypeFlags::Int32));
    CScriptClass* New = MintWithFields("Reinst_NoHookAfter", After);
    ASSERT_NE(New, nullptr);

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    const FReinstanceResult Result = Reinstancer.Commit();

    EXPECT_EQ(Result.InstancesReplaced, 1);
    CObject* Moved = FindObject<CObject>(FName("NoHookSubject"));
    ASSERT_NE(Moved, nullptr);
    EXPECT_EQ(*FindInt(Moved, "Value"), 7);
}

namespace
{
    // Holds the instance strongly, so repointing it drops the original's last reference in the middle of the walk.
    class FStrongTestProvider final : public IObjectReferenceProvider
    {
    public:

        TStrongObjectPtr<CObject> Held;

        const char* GetReferenceProviderName() const override { return "strong test"; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override
        {
            Held = Func(Held.Get());
        }
    };
}

// The C# stress run's crash. The original died when its only holder moved, and the commit then read it.
TEST(ObjectReinstancer, AnOriginalWhoseOnlyHolderMovesDiesAfterTheWalk)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));
    CScriptClass* Old = MintWithFields("ReinstLast_Before", Schema);
    CScriptClass* New = MintWithFields("ReinstLast_After", Schema);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    FStrongTestProvider Provider;
    Provider.Held = NewObject(Old, nullptr, NAME_None, FGuid::New(), OF_Transient);
    const FObjectHandle OriginalHandle = GObjectArray.GetHandleByObject(Provider.Held.Get());
    *FindInt(Provider.Held.Get(), "Value") = 9;
    FObjectReferenceProviders::Register(&Provider);

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    const FReinstanceResult Result = Reinstancer.Commit();

    FObjectReferenceProviders::Unregister(&Provider);

    EXPECT_EQ(Result.InstancesReplaced, 1);
    EXPECT_EQ(Result.OriginalsOutlivingTheSwap, 0);
    ASSERT_NE(Provider.Held.Get(), nullptr);
    EXPECT_EQ(Provider.Held->GetClass(), New);
    EXPECT_EQ(*FindInt(Provider.Held.Get(), "Value"), 9);
    EXPECT_EQ(GObjectArray.ResolveHandle(OriginalHandle), Provider.Held.Get()) << "the original handle redirects to the replacement";
}

extern "C" void* LuminaSharp_PinObject(void* Object);
extern "C" void LuminaSharp_UnpinObject(void* Pin);

// The C# stress run's leak. A replacement for an object only C# held was left with no owner at all.
TEST(ObjectReinstancer, AManagedPinMovesOntoTheReplacement)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Value", EPropertyTypeFlags::Int32));
    CScriptClass* Old = MintWithFields("ReinstPin_Before", Schema);
    CScriptClass* New = MintWithFields("ReinstPin_After", Schema);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    CObject* Original = NewObject(Old, nullptr, NAME_None, FGuid::New(), OF_Transient);
    *FindInt(Original, "Value") = 5;
    void* Pin = LuminaSharp_PinObject(Original);
    auto* Held = static_cast<TStrongObjectPtr<CObject>*>(Pin);

    FObjectReinstancer Reinstancer;
    Reinstancer.MapClass(Old, New);
    const FReinstanceResult Result = Reinstancer.Commit();

    EXPECT_EQ(Result.OriginalsOutlivingTheSwap, 0) << "the pin kept the original alive";
    ASSERT_NE(Held->Get(), nullptr);
    EXPECT_EQ(Held->Get()->GetClass(), New);
    EXPECT_EQ(*FindInt(Held->Get(), "Value"), 5);

    const FObjectHandle Replacement = GObjectArray.GetHandleByObject(Held->Get());
    LuminaSharp_UnpinObject(Pin);
    EXPECT_EQ(GObjectArray.ResolveHandle(Replacement), nullptr) << "the replacement outlived the only reference to it";
}
