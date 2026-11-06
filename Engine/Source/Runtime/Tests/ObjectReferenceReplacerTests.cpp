#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include "Core/Object/ObjectReferenceReplacer.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"
#include "ScriptTestUtil.h"

using namespace Lumina;

// Retargeting references used to be a serialize pass, which could not see a NoSerialize property or a holder
// that is not a CObject. It walks the reflected shape now, so these cover both directions it has to get right.

TEST(ObjectReferenceReplacer, RepointsAHardReferenceAndClearsWhenTheReplacementIsNull)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Ref", EPropertyTypeFlags::Object));

    CScriptClass* Holder = ScriptTest::MintWith("Replacer_Holder", Schema);
    ASSERT_NE(Holder, nullptr);

    CObject* Owner  = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Target = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Other  = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Owner, nullptr);

    FProperty* Ref = Holder->GetProperty(FName("Ref"));
    ASSERT_NE(Ref, nullptr);
    *Ref->GetValuePtr<TStrongObjectPtr<CObject>>(Owner) = Target;

    {
        FObjectReferenceReplacer Replacer(Target, Other);
        EXPECT_GT(Replacer.ApplyToAllObjects(), 0u);
    }
    EXPECT_EQ(Ref->GetValuePtr<TStrongObjectPtr<CObject>>(Owner)->Get(), Other);

    {
        // A null replacement clears rather than repoints, which is how an unloaded export is let go.
        FObjectReferenceReplacer Replacer(Other, nullptr);
        Replacer.ApplyToAllObjects();
    }
    EXPECT_EQ(Ref->GetValuePtr<TStrongObjectPtr<CObject>>(Owner)->Get(), nullptr);
}

TEST(ObjectReferenceReplacer, RewritesASoftReferenceByGuidWithoutLoadingIt)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Soft", EPropertyTypeFlags::SoftObject));

    CScriptClass* Holder = ScriptTest::MintWith("Replacer_SoftHolder", Schema);
    ASSERT_NE(Holder, nullptr);

    CObject* Owner = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Owner, nullptr);

    FProperty* Soft = Holder->GetProperty(FName("Soft"));
    ASSERT_NE(Soft, nullptr);

    const FGuid OldGuid = FGuid::New();
    const FGuid NewGuid = FGuid::New();
    *Soft->GetValuePtr<FSoftObjectPath>(Owner) = FSoftObjectPath(FStringView("/Game/Old"), OldGuid);

    FObjectReferenceReplacer Replacer;
    Replacer.AddSoftReplacement(OldGuid, FStringView("/Game/Old"), NewGuid, FStringView("/Game/New"));
    EXPECT_GT(Replacer.ApplyToAllObjects(), 0u);

    const FSoftObjectPath* Result = Soft->GetValuePtr<FSoftObjectPath>(Owner);
    EXPECT_EQ(Result->GetCachedGUID(), NewGuid);
    EXPECT_EQ(FString(Result->GetPath().GetData(), Result->GetPath().Len()), FString("/Game/New"));
}

// A deleted skeletal mesh and its skeleton own each other, so clearing the skeleton's preview mesh freed the skeleton mid-visit.
TEST(ObjectReferenceReplacer, SurvivesFreeingTheObjectItIsVisitingThroughACycle)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Ref", EPropertyTypeFlags::Object));

    CScriptClass* Holder = ScriptTest::MintWith("Replacer_CycleHolder", Schema);
    ASSERT_NE(Holder, nullptr);

    CObject* Skeleton = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Mesh     = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Skeleton, nullptr);
    ASSERT_NE(Mesh, nullptr);

    FProperty* Ref = Holder->GetProperty(FName("Ref"));
    ASSERT_NE(Ref, nullptr);
    *Ref->GetValuePtr<TStrongObjectPtr<CObject>>(Skeleton) = Mesh;
    *Ref->GetValuePtr<TStrongObjectPtr<CObject>>(Mesh) = Skeleton;

    const FObjectHandle SkeletonHandle = Object::Internal::GetHandleByObject(Skeleton);
    const FObjectHandle MeshHandle     = Object::Internal::GetHandleByObject(Mesh);

    FObjectReferenceReplacer Replacer(Mesh, nullptr);
    EXPECT_EQ(Replacer.ApplyTo(Skeleton), 1u);

    EXPECT_EQ(Object::Internal::ResolveHandle(MeshHandle), nullptr) << "the mesh lost its only owner";
    EXPECT_EQ(Object::Internal::ResolveHandle(SkeletonHandle), nullptr) << "the skeleton went with it, and the visit stopped touching it";
}

TEST(ObjectReferenceReplacer, ReachesAReferenceInsideAContainer)
{
    Scripting::FScriptExportSchema Schema;
    TSharedPtr<Scripting::FScriptExportType> List = MakeShared<Scripting::FScriptExportType>();
    List->Kind = EPropertyTypeFlags::Vector;
    List->ElementType = MakeShared<Scripting::FScriptExportType>();
    List->ElementType->Kind = EPropertyTypeFlags::Object;

    Scripting::FScriptExportField Field;
    Field.Name = FName("Refs");
    Field.Type = List;
    Schema.Fields.Add(Field);

    CScriptClass* Holder = ScriptTest::MintWith("Replacer_ListHolder", Schema);
    ASSERT_NE(Holder, nullptr);

    CObject* Owner  = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Target = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Other  = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Owner, nullptr);

    auto* Array = static_cast<FArrayProperty*>(Holder->GetProperty(FName("Refs")));
    ASSERT_NE(Array, nullptr);

    void* Container = Array->GetValuePtr<void>(Owner);
    Array->SetNum(Container, 1);
    *static_cast<TStrongObjectPtr<CObject>*>(Array->GetAt(Container, 0)) = Target;

    FObjectReferenceReplacer Replacer(Target, Other);
    EXPECT_GT(Replacer.ApplyToAllObjects(), 0u);

    EXPECT_EQ(static_cast<TStrongObjectPtr<CObject>*>(Array->GetAt(Container, 0))->Get(), Other)
        << "a reference inside a container must be repointed like any other";
}

namespace
{
    // A holder no walk of the reflected graph can reach, which is what an ECS storage or a config map is.
    class FReplacerTestProvider final : public IObjectReferenceProvider
    {
    public:

        CObject* Held = nullptr;

        const char* GetReferenceProviderName() const override { return "replacer test"; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override { Held = Func(Held); }
    };
}

// Retiring a minted class relies on this: it used to walk only the reflected graph, so a class still named by
// a provider was freed with that provider left pointing at it.
TEST(ObjectReferenceReplacer, ClearsAReferenceHeldOnlyByAProvider)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Unused", EPropertyTypeFlags::Int32));

    CScriptClass* Holder = ScriptTest::MintWith("Replacer_ProviderHolder", Schema);
    ASSERT_NE(Holder, nullptr);

    CObject* Target = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Target, nullptr);

    FReplacerTestProvider Provider;
    Provider.Held = Target;
    FObjectReferenceProviders::Register(&Provider);

    {
        FObjectReferenceReplacer Replacer(Target, nullptr);
        EXPECT_GT(Replacer.ApplyToAllObjects(), 0u);
    }

    FObjectReferenceProviders::Unregister(&Provider);
    EXPECT_EQ(Provider.Held, nullptr) << "a provider-held reference must be cleared like any other";
}

// The class object is a CObject too, so retiring one is the same replacement with a null target.
TEST(ObjectReferenceReplacer, ClearsAClassReferenceHeldByAProvider)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Unused", EPropertyTypeFlags::Int32));

    CScriptClass* Retiring = ScriptTest::MintWith("Replacer_RetiringClass", Schema);
    ASSERT_NE(Retiring, nullptr);

    FReplacerTestProvider Provider;
    Provider.Held = Retiring;
    FObjectReferenceProviders::Register(&Provider);

    {
        FObjectReferenceReplacer Replacer(Retiring, nullptr);
        Replacer.ApplyToAllObjects();
    }

    FObjectReferenceProviders::Unregister(&Provider);
    EXPECT_EQ(Provider.Held, nullptr);
}
