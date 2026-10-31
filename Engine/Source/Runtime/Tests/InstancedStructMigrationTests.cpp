#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Versioning/CoreVersion.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Scripting/ScriptStruct.h"
#include "ScriptTestUtil.h"

using namespace Lumina;
using namespace Lumina::Scripting;

namespace
{
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
    Before.Fields.push_back(ScriptTest::ScalarField("Alpha", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScriptTest::ScalarField("Beta", EPropertyTypeFlags::Float));
    Before.Fields.push_back(ScriptTest::ScalarField("Gamma", EPropertyTypeFlags::Int32));

    // Same names, deliberately different order, which is what inserting a field upstream produces.
    FScriptExportSchema After;
    After.Fields.push_back(ScriptTest::ScalarField("Gamma", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScriptTest::ScalarField("Alpha", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScriptTest::ScalarField("Beta", EPropertyTypeFlags::Float));

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

// A number that changed kind converts by value, the way loading a saved file does, rather than having its bits reinterpreted.
TEST(InstancedStructMigration, ARetypedNumberConvertsRatherThanReinterpreting)
{
    FScriptExportSchema Before;
    Before.Fields.push_back(ScriptTest::ScalarField("Value", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScriptTest::ScalarField("Kept", EPropertyTypeFlags::Int32));

    FScriptExportSchema After;
    After.Fields.push_back(ScriptTest::ScalarField("Value", EPropertyTypeFlags::Float));
    After.Fields.push_back(ScriptTest::ScalarField("Kept", EPropertyTypeFlags::Int32));

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

    EXPECT_FLOAT_EQ(*New->GetProperty(FName("Value"))->GetValuePtr<float>(NewBytes.data()), 1078530011.0f)
        << "the int's value must arrive as a float, not its bits";
    EXPECT_EQ(*New->GetProperty(FName("Kept"))->GetValuePtr<int32>(NewBytes.data()), 7)
        << "a field that did not change still migrates";

    Old->DestroyStruct(OldBytes.data());
    New->DestroyStruct(NewBytes.data());
}

// A field the new layout dropped, and one it added, must both be handled without disturbing the other.
TEST(InstancedStructMigration, AddedAndRemovedFieldsAreHandled)
{
    FScriptExportSchema Before;
    Before.Fields.push_back(ScriptTest::ScalarField("Survives", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScriptTest::ScalarField("Removed", EPropertyTypeFlags::Int32));

    FScriptExportSchema After;
    After.Fields.push_back(ScriptTest::ScalarField("Added", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScriptTest::ScalarField("Survives", EPropertyTypeFlags::Int32));

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
    Schema.Fields.push_back(ScriptTest::ScalarField("Pad", EPropertyTypeFlags::Int32));
    Schema.Fields.push_back(ScriptTest::ScalarField("Text", EPropertyTypeFlags::String));

    FScriptExportSchema Reordered;
    Reordered.Fields.push_back(ScriptTest::ScalarField("Text", EPropertyTypeFlags::String));
    Reordered.Fields.push_back(ScriptTest::ScalarField("Pad", EPropertyTypeFlags::Int32));

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

// A reload drops the registry's ref first, so a value nobody touched since still has to destroy through its old type.
TEST(InstancedStructMigration, ValueOutlivesItsTypesRegistryRef)
{
    FScriptExportSchema Schema;
    Schema.Fields.push_back(ScriptTest::ScalarField("Text", EPropertyTypeFlags::String));

    TStrongObjectPtr<CScriptStruct> RegistryRef = BuildLayout(Schema);
    ASSERT_TRUE(RegistryRef.IsValid());
    RegistryRef->Metadata.AddValue("ScriptTypeName", "ReloadProbeRow");

    {
        FInstancedStruct Value;
        Value.InitializeAs(RegistryRef.Get());
        *RegistryRef->GetProperty(FName("Text"))->GetValuePtr<FString>(Value.GetMutableMemory()) =
            "long enough to live on the heap rather than inline";

        RegistryRef = nullptr;
    }

    TStrongObjectPtr<CScriptStruct> MigratedRef = BuildLayout(Schema);
    ASSERT_TRUE(MigratedRef.IsValid());
    MigratedRef->Metadata.AddValue("ScriptTypeName", "ReloadProbeRow");

    FInstancedStruct Value;
    Value.InitializeAs(MigratedRef.Get());
    MigratedRef = nullptr;
    FScriptStructRegistry::Get().Clear();

    // The type is gone from the registry, so the value migrates to nothing, destroying its old contents on the way.
    EXPECT_FALSE(Value.IsValid());
}

namespace InstancedStructMigrationTestsInternal
{
    TSharedPtr<FScriptExportType> MakeType(EPropertyTypeFlags Kind)
    {
        TSharedPtr<FScriptExportType> Type = MakeShared<FScriptExportType>();
        Type->Kind = Kind;
        return Type;
    }

    FScriptExportField MakeField(const char* Name, TSharedPtr<FScriptExportType> Type)
    {
        FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = std::move(Type);
        return Field;
    }

    FScriptExportField ListField(const char* Name, TSharedPtr<FScriptExportType> Element)
    {
        TSharedPtr<FScriptExportType> List = MakeType(EPropertyTypeFlags::Vector);
        List->ElementType = std::move(Element);
        return MakeField(Name, std::move(List));
    }

    TSharedPtr<FScriptExportType> StructType(const char* StructName)
    {
        TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
        Type->NativeName = FName(StructName);
        return Type;
    }

    struct FMigratedPair
    {
        CScriptStruct*  Old = nullptr;
        CScriptStruct*  New = nullptr;
        TVector<uint8>  OldBytes;
        TVector<uint8>  NewBytes;

        ~FMigratedPair()
        {
            if (Old != nullptr) { Old->DestroyStruct(OldBytes.data()); }
            if (New != nullptr) { New->DestroyStruct(NewBytes.data()); }
        }
    };
}

TEST(InstancedStructMigration, AWidenedNumberKeepsItsValueAndANarrowedOneThatDoesNotFitIsLeftAlone)
{
    FScriptExportSchema Before;
    Before.Fields.push_back(ScriptTest::ScalarField("Small", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(ScriptTest::ScalarField("Huge", EPropertyTypeFlags::Int64));
    Before.Fields.push_back(ScriptTest::ScalarField("Exact", EPropertyTypeFlags::Int64));

    FScriptExportSchema After;
    After.Fields.push_back(ScriptTest::ScalarField("Small", EPropertyTypeFlags::Int64));
    After.Fields.push_back(ScriptTest::ScalarField("Huge", EPropertyTypeFlags::Int32));
    After.Fields.push_back(ScriptTest::ScalarField("Exact", EPropertyTypeFlags::UInt64));

    InstancedStructMigrationTestsInternal::FMigratedPair Pair;
    Pair.Old = BuildLayout(Before);
    Pair.New = BuildLayout(After);
    ASSERT_NE(Pair.Old, nullptr);
    ASSERT_NE(Pair.New, nullptr);
    Pair.OldBytes.resize(Pair.Old->GetAlignedSize());
    Pair.NewBytes.resize(Pair.New->GetAlignedSize());
    Pair.Old->InitializeStruct(Pair.OldBytes.data());
    Pair.New->InitializeStruct(Pair.NewBytes.data());

    *Pair.Old->GetProperty(FName("Small"))->GetValuePtr<int32>(Pair.OldBytes.data()) = 42;
    *Pair.Old->GetProperty(FName("Huge"))->GetValuePtr<int64>(Pair.OldBytes.data()) = 3948752093457239045ll;
    *Pair.Old->GetProperty(FName("Exact"))->GetValuePtr<int64>(Pair.OldBytes.data()) = 9007199254740993ll;

    MigrateStructByFieldName(Pair.Old, Pair.OldBytes.data(), Pair.New, Pair.NewBytes.data());

    EXPECT_EQ(*Pair.New->GetProperty(FName("Small"))->GetValuePtr<int64>(Pair.NewBytes.data()), 42);
    EXPECT_EQ(*Pair.New->GetProperty(FName("Huge"))->GetValuePtr<int32>(Pair.NewBytes.data()), 0) << "a value that does not fit must not wrap";
    EXPECT_EQ(*Pair.New->GetProperty(FName("Exact"))->GetValuePtr<uint64>(Pair.NewBytes.data()), 9007199254740993ull) << "an integer must not round through a double";
}

TEST(InstancedStructMigration, AListWhoseElementWidenedConvertsEachElement)
{
    using namespace InstancedStructMigrationTestsInternal;

    FScriptExportSchema Before;
    Before.Fields.push_back(ListField("Numbers", MakeType(EPropertyTypeFlags::Int32)));
    FScriptExportSchema After;
    After.Fields.push_back(ListField("Numbers", MakeType(EPropertyTypeFlags::Int64)));

    FMigratedPair Pair;
    Pair.Old = BuildLayout(Before);
    Pair.New = BuildLayout(After);
    ASSERT_NE(Pair.Old, nullptr);
    ASSERT_NE(Pair.New, nullptr);
    Pair.OldBytes.resize(Pair.Old->GetAlignedSize());
    Pair.NewBytes.resize(Pair.New->GetAlignedSize());
    Pair.Old->InitializeStruct(Pair.OldBytes.data());
    Pair.New->InitializeStruct(Pair.NewBytes.data());

    const FArrayProperty* OldList = static_cast<const FArrayProperty*>(Pair.Old->GetProperty(FName("Numbers")));
    const FArrayProperty* NewList = static_cast<const FArrayProperty*>(Pair.New->GetProperty(FName("Numbers")));
    void* OldValue = OldList->GetValuePtr<void>(Pair.OldBytes.data());
    void* NewValue = NewList->GetValuePtr<void>(Pair.NewBytes.data());
    for (int32 Value : { 1, 2, -3, 4, 5 })
    {
        OldList->PushBack(OldValue, &Value);
    }

    MigrateStructByFieldName(Pair.Old, Pair.OldBytes.data(), Pair.New, Pair.NewBytes.data());

    ASSERT_EQ(NewList->GetNum(NewValue), 5u) << "the count must come from the old list, not from its bytes read at the new stride";
    const int64 Expected[] = { 1, 2, -3, 4, 5 };
    for (SIZE_T Index = 0; Index < 5; ++Index)
    {
        EXPECT_EQ(*static_cast<int64*>(NewList->GetAt(NewValue, Index)), Expected[Index]) << "element " << Index;
    }
}

TEST(InstancedStructMigration, AReshapedStructInAListAndInAFieldMigratesByName)
{
    using namespace InstancedStructMigrationTestsInternal;

    FScriptExportSchema ItemBefore;
    ItemBefore.Fields.push_back(ScriptTest::ScalarField("Count", EPropertyTypeFlags::Int32));
    ItemBefore.Fields.push_back(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));
    ScriptTest::PublishStructs({ ScriptTest::StructDefinition("MigrationTest_Item", std::move(ItemBefore)) });

    FScriptExportSchema OuterSchema;
    OuterSchema.Fields.push_back(ListField("Items", StructType("MigrationTest_Item")));
    OuterSchema.Fields.push_back(MakeField("Nested", StructType("MigrationTest_Item")));

    FMigratedPair Pair;
    Pair.Old = BuildLayout(OuterSchema);
    ASSERT_NE(Pair.Old, nullptr);
    Pair.OldBytes.resize(Pair.Old->GetAlignedSize());
    Pair.Old->InitializeStruct(Pair.OldBytes.data());

    const CStruct* OldItem = FScriptStructRegistry::Get().Find(FName("MigrationTest_Item"));
    ASSERT_NE(OldItem, nullptr);
    const FArrayProperty* OldItems = static_cast<const FArrayProperty*>(Pair.Old->GetProperty(FName("Items")));
    void* OldItemsValue = OldItems->GetValuePtr<void>(Pair.OldBytes.data());
    OldItems->Resize(OldItemsValue, 2);
    for (SIZE_T Index = 0; Index < 2; ++Index)
    {
        void* Element = OldItems->GetAt(OldItemsValue, Index);
        *OldItem->GetProperty(FName("Count"))->GetValuePtr<int32>(Element) = static_cast<int32>(Index + 7);
        *OldItem->GetProperty(FName("Label"))->GetValuePtr<FString>(Element) = Index == 0 ? "first label long enough for the heap" : "second";
    }
    void* OldNested = Pair.Old->GetProperty(FName("Nested"))->GetValuePtr<void>(Pair.OldBytes.data());
    *OldItem->GetProperty(FName("Count"))->GetValuePtr<int32>(OldNested) = 9;
    *OldItem->GetProperty(FName("Label"))->GetValuePtr<FString>(OldNested) = "nested";

    // A field is prepended, so every old offset is wrong under the new layout.
    FScriptExportSchema ItemAfter;
    ItemAfter.Fields.push_back(ScriptTest::ScalarField("Extra", EPropertyTypeFlags::Int64));
    ItemAfter.Fields.push_back(ScriptTest::ScalarField("Count", EPropertyTypeFlags::Int32));
    ItemAfter.Fields.push_back(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));
    ItemAfter.Fields.push_back(ScriptTest::ScalarField("Weight", EPropertyTypeFlags::Double));
    ScriptTest::PublishStructs({ ScriptTest::StructDefinition("MigrationTest_Item", std::move(ItemAfter)) });

    Pair.New = BuildLayout(OuterSchema);
    ASSERT_NE(Pair.New, nullptr);
    Pair.NewBytes.resize(Pair.New->GetAlignedSize());
    Pair.New->InitializeStruct(Pair.NewBytes.data());

    const CStruct* NewItem = FScriptStructRegistry::Get().Find(FName("MigrationTest_Item"));
    ASSERT_NE(NewItem, nullptr);
    ASSERT_NE(NewItem, OldItem) << "the reshape must have minted a new layout for this to test anything";

    MigrateStructByFieldName(Pair.Old, Pair.OldBytes.data(), Pair.New, Pair.NewBytes.data());

    const FArrayProperty* NewItems = static_cast<const FArrayProperty*>(Pair.New->GetProperty(FName("Items")));
    void* NewItemsValue = NewItems->GetValuePtr<void>(Pair.NewBytes.data());
    ASSERT_EQ(NewItems->GetNum(NewItemsValue), 2u);
    for (SIZE_T Index = 0; Index < 2; ++Index)
    {
        void* Element = NewItems->GetAt(NewItemsValue, Index);
        EXPECT_EQ(*NewItem->GetProperty(FName("Count"))->GetValuePtr<int32>(Element), static_cast<int32>(Index + 7));
        EXPECT_EQ(*NewItem->GetProperty(FName("Label"))->GetValuePtr<FString>(Element), Index == 0 ? FString("first label long enough for the heap") : FString("second"));
        EXPECT_EQ(*NewItem->GetProperty(FName("Extra"))->GetValuePtr<int64>(Element), 0);
    }

    void* NewNested = Pair.New->GetProperty(FName("Nested"))->GetValuePtr<void>(Pair.NewBytes.data());
    EXPECT_EQ(*NewItem->GetProperty(FName("Count"))->GetValuePtr<int32>(NewNested), 9);
    EXPECT_EQ(*NewItem->GetProperty(FName("Label"))->GetValuePtr<FString>(NewNested), FString("nested"));
}

namespace InstancedStructMigrationTestsInternal
{
    // Saves a list of ints under one layout and loads it into a layout whose list holds another kind.
    TVector<uint8> SaveIntList(CScriptStruct* Layout, std::initializer_list<int32> Values, int32 FileVersion)
    {
        TVector<uint8> Memory(Layout->GetAlignedSize());
        Layout->InitializeStruct(Memory.data());
        const FArrayProperty* List = static_cast<const FArrayProperty*>(Layout->GetProperty(FName("Numbers")));
        void* ListValue = List->GetValuePtr<void>(Memory.data());
        for (int32 Value : Values)
        {
            List->PushBack(ListValue, &Value);
        }

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        Writer.SetFileVersion(FileVersion);
        Layout->SerializeProperties(Writer, Memory.data());
        Layout->DestroyStruct(Memory.data());
        return Bytes;
    }
}

TEST(ArrayElementKind, AListSavedAsIntsLoadsIntoLongsAndFloatsByValue)
{
    using namespace InstancedStructMigrationTestsInternal;

    FScriptExportSchema Ints;
    Ints.Fields.push_back(ListField("Numbers", MakeType(EPropertyTypeFlags::Int32)));
    CScriptStruct* Saved = BuildLayout(Ints);
    ASSERT_NE(Saved, nullptr);
    const TVector<uint8> Bytes = SaveIntList(Saved, { 1, -2, 300000 }, GPackageFileLuminaVersion.FileVersion);

    for (EPropertyTypeFlags Kind : { EPropertyTypeFlags::Int64, EPropertyTypeFlags::Float })
    {
        FScriptExportSchema Retyped;
        Retyped.Fields.push_back(ListField("Numbers", MakeType(Kind)));
        CScriptStruct* Loaded = BuildLayout(Retyped);
        ASSERT_NE(Loaded, nullptr);

        TVector<uint8> Memory(Loaded->GetAlignedSize());
        Loaded->InitializeStruct(Memory.data());
        FMemoryReader Reader(Bytes);
        Loaded->SerializeProperties(Reader, Memory.data());

        const FArrayProperty* List = static_cast<const FArrayProperty*>(Loaded->GetProperty(FName("Numbers")));
        void* ListValue = List->GetValuePtr<void>(Memory.data());
        ASSERT_EQ(List->GetNum(ListValue), 3u);
        const double Expected[] = { 1.0, -2.0, 300000.0 };
        for (SIZE_T Index = 0; Index < 3; ++Index)
        {
            const double Read = Kind == EPropertyTypeFlags::Int64 ? static_cast<double>(*static_cast<int64*>(List->GetAt(ListValue, Index)))
                                                                    : static_cast<double>(*static_cast<float*>(List->GetAt(ListValue, Index)));
            EXPECT_EQ(Read, Expected[Index]) << PropertyTypeToString(Kind) << " element " << Index;
        }
        Loaded->DestroyStruct(Memory.data());
    }
}

TEST(ArrayElementKind, AnOlderFileWithoutTheKindStillWidensWithinItsFamily)
{
    using namespace InstancedStructMigrationTestsInternal;

    const int32 OlderVersion = static_cast<int32>(ELuminaEngineVersion::ARRAY_ELEMENT_KIND) - 1;

    FScriptExportSchema Ints;
    Ints.Fields.push_back(ListField("Numbers", MakeType(EPropertyTypeFlags::Int32)));
    CScriptStruct* Saved = BuildLayout(Ints);
    ASSERT_NE(Saved, nullptr);
    const TVector<uint8> Bytes = SaveIntList(Saved, { 5, 6, -7, 8 }, OlderVersion);

    FScriptExportSchema Longs;
    Longs.Fields.push_back(ListField("Numbers", MakeType(EPropertyTypeFlags::Int64)));
    CScriptStruct* Loaded = BuildLayout(Longs);
    ASSERT_NE(Loaded, nullptr);

    TVector<uint8> Memory(Loaded->GetAlignedSize());
    Loaded->InitializeStruct(Memory.data());
    FMemoryReader Reader(Bytes);
    Reader.SetFileVersion(OlderVersion);
    Loaded->SerializeProperties(Reader, Memory.data());

    const FArrayProperty* List = static_cast<const FArrayProperty*>(Loaded->GetProperty(FName("Numbers")));
    void* ListValue = List->GetValuePtr<void>(Memory.data());
    ASSERT_EQ(List->GetNum(ListValue), 4u);
    const int64 Expected[] = { 5, 6, -7, 8 };
    for (SIZE_T Index = 0; Index < 4; ++Index)
    {
        EXPECT_EQ(*static_cast<int64*>(List->GetAt(ListValue, Index)), Expected[Index]) << "element " << Index;
    }
    Loaded->DestroyStruct(Memory.data());
}
