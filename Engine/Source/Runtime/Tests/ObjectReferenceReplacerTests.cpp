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
#include "Core/Object/ObjectHandleTyped.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// Retargeting references used to be a serialize pass, which could not see a NoSerialize property or a holder
// that is not a CObject. It walks the reflected shape now, so these cover both directions it has to get right.

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

    CScriptClass* MintWith(const char* ClassName, const Scripting::FScriptExportSchema& Schema)
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
}

TEST(ObjectReferenceReplacer, RepointsAHardReferenceAndClearsWhenTheReplacementIsNull)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Ref", EPropertyTypeFlags::Object));

    CScriptClass* Holder = MintWith("Replacer_Holder", Schema);
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
    Schema.Fields.push_back(MakeScalarField("Soft", EPropertyTypeFlags::SoftObject));

    CScriptClass* Holder = MintWith("Replacer_SoftHolder", Schema);
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
    EXPECT_EQ(FString(Result->GetPath().data(), Result->GetPath().size()), FString("/Game/New"));
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
    Schema.Fields.push_back(Field);

    CScriptClass* Holder = MintWith("Replacer_ListHolder", Schema);
    ASSERT_NE(Holder, nullptr);

    CObject* Owner  = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Target = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Other  = NewObject(Holder, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Owner, nullptr);

    auto* Array = static_cast<FArrayProperty*>(Holder->GetProperty(FName("Refs")));
    ASSERT_NE(Array, nullptr);

    void* Container = Array->GetValuePtr<void>(Owner);
    Array->Resize(Container, 1);
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
    Schema.Fields.push_back(MakeScalarField("Unused", EPropertyTypeFlags::Int32));

    CScriptClass* Holder = MintWith("Replacer_ProviderHolder", Schema);
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
    Schema.Fields.push_back(MakeScalarField("Unused", EPropertyTypeFlags::Int32));

    CScriptClass* Retiring = MintWith("Replacer_RetiringClass", Schema);
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
