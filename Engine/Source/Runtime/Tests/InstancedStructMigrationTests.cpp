#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptStruct.h"

using namespace Lumina;
using namespace Lumina::Scripting;

namespace
{
    FScriptExportField ScalarField(const char* Name, EPropertyTypeFlags Kind)
    {
        FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<FScriptExportType>();
        Field.Type->Kind = Kind;
        return Field;
    }

    // A standalone layout, which is what a script reload mints fresh for every live type.
    CScriptStruct* BuildLayout(const FScriptExportSchema& Schema)
    {
        CScriptStruct* Layout = NewObject<CScriptStruct>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        if (Layout == nullptr || !Layout->BuildFromSchema(Schema))
        {
            return nullptr;
        }
        return Layout;
    }
}

// The old blob has the old layout, so reading it at the new layout's offsets silently shifts every field.
TEST(InstancedStructMigration, ReorderedFieldsMigrateByNameNotByOffset)
{
    FScriptExportSchema Before;
    Before.Fields.push_back(ScalarField("Alpha", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScalarField("Beta", EPropertyTypeFlags::Float));
    Before.Fields.push_back(ScalarField("Gamma", EPropertyTypeFlags::Int32));

    // Same names, deliberately different order, which is what inserting a field upstream produces.
    FScriptExportSchema After;
    After.Fields.push_back(ScalarField("Gamma", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScalarField("Alpha", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScalarField("Beta", EPropertyTypeFlags::Float));

    CScriptStruct* Old = BuildLayout(Before);
    CScriptStruct* New = BuildLayout(After);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    TVector<uint8> OldBytes(Old->GetAlignedSize(), 0);
    TVector<uint8> NewBytes(New->GetAlignedSize(), 0);
    Old->InitializeStruct(OldBytes.data());
    New->InitializeStruct(NewBytes.data());

    *Old->GetProperty(FName("Alpha"))->GetValuePtr<int32>(OldBytes.data()) = 11;
    *Old->GetProperty(FName("Beta"))->GetValuePtr<float>(OldBytes.data())  = 2.5f;
    *Old->GetProperty(FName("Gamma"))->GetValuePtr<int32>(OldBytes.data()) = 33;

    MigrateStructByFieldName(Old, OldBytes.data(), New, NewBytes.data());

    EXPECT_EQ(*New->GetProperty(FName("Alpha"))->GetValuePtr<int32>(NewBytes.data()), 11)
        << "Alpha must follow its name across the reorder";
    EXPECT_FLOAT_EQ(*New->GetProperty(FName("Beta"))->GetValuePtr<float>(NewBytes.data()), 2.5f);
    EXPECT_EQ(*New->GetProperty(FName("Gamma"))->GetValuePtr<int32>(NewBytes.data()), 33);

    Old->DestroyStruct(OldBytes.data());
    New->DestroyStruct(NewBytes.data());
}

// A field that changed type shares only its name, so copying its bytes across would reinterpret them.
TEST(InstancedStructMigration, ARetypedFieldIsSkippedRatherThanReinterpreted)
{
    FScriptExportSchema Before;
    Before.Fields.push_back(ScalarField("Value", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScalarField("Kept", EPropertyTypeFlags::Int32));

    FScriptExportSchema After;
    After.Fields.push_back(ScalarField("Value", EPropertyTypeFlags::Float));
    After.Fields.push_back(ScalarField("Kept", EPropertyTypeFlags::Int32));

    CScriptStruct* Old = BuildLayout(Before);
    CScriptStruct* New = BuildLayout(After);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    TVector<uint8> OldBytes(Old->GetAlignedSize(), 0);
    TVector<uint8> NewBytes(New->GetAlignedSize(), 0);
    Old->InitializeStruct(OldBytes.data());
    New->InitializeStruct(NewBytes.data());

    *Old->GetProperty(FName("Value"))->GetValuePtr<int32>(OldBytes.data()) = 1078530011;
    *Old->GetProperty(FName("Kept"))->GetValuePtr<int32>(OldBytes.data())  = 7;
    *New->GetProperty(FName("Value"))->GetValuePtr<float>(NewBytes.data()) = 0.0f;

    MigrateStructByFieldName(Old, OldBytes.data(), New, NewBytes.data());

    EXPECT_FLOAT_EQ(*New->GetProperty(FName("Value"))->GetValuePtr<float>(NewBytes.data()), 0.0f)
        << "the bits of an int must not arrive reinterpreted as a float";
    EXPECT_EQ(*New->GetProperty(FName("Kept"))->GetValuePtr<int32>(NewBytes.data()), 7)
        << "a field that did not change still migrates";

    Old->DestroyStruct(OldBytes.data());
    New->DestroyStruct(NewBytes.data());
}

// A field the new layout dropped, and one it added, must both be handled without disturbing the other.
TEST(InstancedStructMigration, AddedAndRemovedFieldsAreHandled)
{
    FScriptExportSchema Before;
    Before.Fields.push_back(ScalarField("Survives", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScalarField("Removed", EPropertyTypeFlags::Int32));

    FScriptExportSchema After;
    After.Fields.push_back(ScalarField("Added", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScalarField("Survives", EPropertyTypeFlags::Int32));

    CScriptStruct* Old = BuildLayout(Before);
    CScriptStruct* New = BuildLayout(After);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    TVector<uint8> OldBytes(Old->GetAlignedSize(), 0);
    TVector<uint8> NewBytes(New->GetAlignedSize(), 0);
    Old->InitializeStruct(OldBytes.data());
    New->InitializeStruct(NewBytes.data());

    *Old->GetProperty(FName("Survives"))->GetValuePtr<int32>(OldBytes.data()) = 42;
    *Old->GetProperty(FName("Removed"))->GetValuePtr<int32>(OldBytes.data())  = 99;
    *New->GetProperty(FName("Added"))->GetValuePtr<int32>(NewBytes.data())    = 5;

    MigrateStructByFieldName(Old, OldBytes.data(), New, NewBytes.data());

    EXPECT_EQ(*New->GetProperty(FName("Survives"))->GetValuePtr<int32>(NewBytes.data()), 42);
    EXPECT_EQ(*New->GetProperty(FName("Added"))->GetValuePtr<int32>(NewBytes.data()), 5)
        << "a field the old layout never had keeps whatever the new layout initialized it to";
    EXPECT_EQ(New->GetProperty(FName("Removed")), nullptr);

    Old->DestroyStruct(OldBytes.data());
    New->DestroyStruct(NewBytes.data());
}

// A storage-owning field has to deep copy, or the two layouts would end up sharing one buffer.
TEST(InstancedStructMigration, StorageOwningFieldsDeepCopy)
{
    FScriptExportSchema Schema;
    Schema.Fields.push_back(ScalarField("Pad", EPropertyTypeFlags::Int32));
    Schema.Fields.push_back(ScalarField("Text", EPropertyTypeFlags::String));

    FScriptExportSchema Reordered;
    Reordered.Fields.push_back(ScalarField("Text", EPropertyTypeFlags::String));
    Reordered.Fields.push_back(ScalarField("Pad", EPropertyTypeFlags::Int32));

    CScriptStruct* Old = BuildLayout(Schema);
    CScriptStruct* New = BuildLayout(Reordered);
    ASSERT_NE(Old, nullptr);
    ASSERT_NE(New, nullptr);

    TVector<uint8> OldBytes(Old->GetAlignedSize(), 0);
    TVector<uint8> NewBytes(New->GetAlignedSize(), 0);
    Old->InitializeStruct(OldBytes.data());
    New->InitializeStruct(NewBytes.data());

    *Old->GetProperty(FName("Text"))->GetValuePtr<FString>(OldBytes.data()) = "carried across";

    MigrateStructByFieldName(Old, OldBytes.data(), New, NewBytes.data());

    EXPECT_EQ(*New->GetProperty(FName("Text"))->GetValuePtr<FString>(NewBytes.data()), FString("carried across"));

    // Destroying the source must leave the destination's own copy intact.
    Old->DestroyStruct(OldBytes.data());
    EXPECT_EQ(*New->GetProperty(FName("Text"))->GetValuePtr<FString>(NewBytes.data()), FString("carried across"))
        << "the migrated string must be its own allocation, not a shared buffer";

    New->DestroyStruct(NewBytes.data());
}
