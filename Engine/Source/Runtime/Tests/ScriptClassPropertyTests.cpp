#include <gtest/gtest.h>

#include <cstring>

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// A minted CClass declares a larger size and carries real FPropertys in the trailing block.

// The schema is built by hand here because a live .NET host normally produces it.

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

    TSharedPtr<Scripting::FScriptExportType> MakeType(EPropertyTypeFlags Kind)
    {
        TSharedPtr<Scripting::FScriptExportType> Type = MakeShared<Scripting::FScriptExportType>();
        Type->Kind = Kind;
        return Type;
    }

    Scripting::FScriptExportField MakeField(const char* Name, TSharedPtr<Scripting::FScriptExportType> Type)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = std::move(Type);
        return Field;
    }

    CClass* MintWithSchema(const char* ClassName, const Scripting::FScriptExportSchema& Schema, uint32& OutShimSize)
    {
        CClass* Minted = FScriptableRegistry::Mint(ClassName, "CScriptableTest", 0);
        if (Minted == nullptr)
        {
            return nullptr;
        }
        OutShimSize = Minted->GetSize();
        Scripting::AppendScriptPropertiesToClass(Minted, Schema);

        // Drain deferred registration FIRST, since Link latches and would lose the base's properties.
        ProcessNewlyLoadedCObjects();
        Minted->GetDefaultObject();   // Link (chains supers) + CDO at the enlarged size
        return Minted;
    }
}

TEST(ScriptClassProperties, SchemaFieldsBecomeRealPropertiesPastTheShim)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));
    Schema.Fields.push_back(MakeScalarField("Enabled", EPropertyTypeFlags::Bool));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_Layout", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    FProperty* Speed = Sub->GetProperty(FName("Speed"));
    FProperty* Health = Sub->GetProperty(FName("Health"));
    FProperty* Enabled = Sub->GetProperty(FName("Enabled"));
    ASSERT_NE(Speed, nullptr);
    ASSERT_NE(Health, nullptr);
    ASSERT_NE(Enabled, nullptr);

    // Every appended property sits past the C++ shim, so none can alias a native member.
    EXPECT_GE(Speed->Offset, ShimSize);
    EXPECT_GE(Health->Offset, ShimSize);
    EXPECT_GE(Enabled->Offset, ShimSize);

    // And they do not overlap each other.
    EXPECT_NE(Speed->Offset, Health->Offset);
    EXPECT_NE(Health->Offset, Enabled->Offset);

    // The native base property is still reachable through the chain.
    ASSERT_NE(Sub->GetProperty(FName("NativeValue")), nullptr);

    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);

    // Zero-initialized by StaticAllocateObject's memzero over the enlarged size.
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Object), 0.0f);
    EXPECT_EQ(*Health->GetValuePtr<int32>(Object), 0);

    Speed->SetValue<float>(Object, 12.5f);
    Health->SetValue<int32>(Object, 99);
    Enabled->SetValue<bool>(Object, true);

    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Object), 12.5f);
    EXPECT_EQ(*Health->GetValuePtr<int32>(Object), 99);
    EXPECT_TRUE(*Enabled->GetValuePtr<bool>(Object));

    // The shim half is untouched by the appended writes.
    CScriptableTest* Typed = Cast<CScriptableTest>(Object);
    ASSERT_NE(Typed, nullptr);
    EXPECT_FLOAT_EQ(Typed->NativeValue, 1.5f);
    EXPECT_EQ(Typed->OnTest(5), 10);
}

TEST(ScriptClassProperties, ScriptPropertiesRoundTripThroughTheStockSerializer)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_Serialize", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    FProperty* Speed = Sub->GetProperty(FName("Speed"));
    FProperty* Health = Sub->GetProperty(FName("Health"));
    ASSERT_NE(Speed, nullptr);
    ASSERT_NE(Health, nullptr);

    CObject* Source = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Source, nullptr);
    Speed->SetValue<float>(Source, 3.5f);
    Health->SetValue<int32>(Source, 42);
    Cast<CScriptableTest>(Source)->NativeValue = 8.0f;

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Sub->SerializeTaggedProperties(Ar, Source);
    }
    ASSERT_FALSE(Bytes.empty());

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeTaggedProperties(Ar, Restored);
    }

    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Restored), 3.5f) << "script property did not round-trip";
    EXPECT_EQ(*Health->GetValuePtr<int32>(Restored), 42);
    EXPECT_FLOAT_EQ(Cast<CScriptableTest>(Restored)->NativeValue, 8.0f) << "native property did not round-trip";
}

// Constructed, not just zeroed, since an FString over memzeroed bytes corrupts on assignment.
TEST(ScriptClassProperties, StorageOwningFieldsAreConstructedPerInstance)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.push_back(MakeScalarField("Label", EPropertyTypeFlags::String));
    Schema.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_Strings", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    FProperty* Label = Sub->GetProperty(FName("Label"));
    ASSERT_NE(Label, nullptr) << "a storage-owning field must be appended, not dropped";
    EXPECT_NE(Sub->GetProperty(FName("Speed")), nullptr);
    EXPECT_NE(Sub->GetProperty(FName("Health")), nullptr);
    EXPECT_TRUE(Label->OwnsStorage());
    EXPECT_GE(Label->Offset, ShimSize);

    CObject* First  = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Second = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);

    // Constructed, so assigning is safe; over memzeroed bytes an FString corrupts the heap.
    EXPECT_TRUE(Label->GetValuePtr<FString>(First)->empty());
    *Label->GetValuePtr<FString>(First) = "a string long enough to have heap storage rather than SSO";
    *Label->GetValuePtr<FString>(Second) = "another";

    EXPECT_STREQ(Label->GetValuePtr<FString>(First)->c_str(), "a string long enough to have heap storage rather than SSO");
    EXPECT_STREQ(Label->GetValuePtr<FString>(Second)->c_str(), "another") << "instances share storage";

    // The shim half is untouched.
    ASSERT_NE(Cast<CScriptableTest>(First), nullptr);
    EXPECT_FLOAT_EQ(Cast<CScriptableTest>(First)->NativeValue, 1.5f);
}

// Strings round-trip through the stock tagged serializer, with nothing special-cased.
TEST(ScriptClassProperties, StringPropertiesRoundTripThroughTheStockSerializer)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Label", EPropertyTypeFlags::String));
    Schema.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_StringSerialize", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    FProperty* Label = Sub->GetProperty(FName("Label"));
    FProperty* Health = Sub->GetProperty(FName("Health"));
    ASSERT_NE(Label, nullptr);
    ASSERT_NE(Health, nullptr);

    CObject* Source = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Source, nullptr);
    *Label->GetValuePtr<FString>(Source) = "round trip me";
    Health->SetValue<int32>(Source, 17);

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Sub->SerializeTaggedProperties(Ar, Source);
    }
    ASSERT_FALSE(Bytes.empty());

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeTaggedProperties(Ar, Restored);
    }

    EXPECT_STREQ(Label->GetValuePtr<FString>(Restored)->c_str(), "round trip me");
    EXPECT_EQ(*Health->GetValuePtr<int32>(Restored), 17);
}

// Declared defaults live on the CDO and every instance is copied from it, by value.
TEST(ScriptClassProperties, ValuesOnTheDefaultObjectSeedEveryInstance)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.push_back(MakeScalarField("Label", EPropertyTypeFlags::String));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_Defaults", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    FProperty* Speed = Sub->GetProperty(FName("Speed"));
    FProperty* Label = Sub->GetProperty(FName("Label"));
    ASSERT_NE(Speed, nullptr);
    ASSERT_NE(Label, nullptr);

    CObject* Cdo = Sub->GetDefaultObjectIfCreated();
    ASSERT_NE(Cdo, nullptr) << "the CDO must exist and carry the appended block";

    // The CDO's own trailing block is in-bounds only because Size was bumped before it existed.
    Speed->SetValue<float>(Cdo, 6.5f);
    *Label->GetValuePtr<FString>(Cdo) = "declared default";

    CObject* Instance = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Instance), 6.5f) << "instance was not seeded from the CDO";
    EXPECT_STREQ(Label->GetValuePtr<FString>(Instance)->c_str(), "declared default");

    // Seeded by value, not shared, so writing the instance leaves the CDO alone.
    *Label->GetValuePtr<FString>(Instance) = "mutated";
    EXPECT_STREQ(Label->GetValuePtr<FString>(Cdo)->c_str(), "declared default");
}

// Which kinds are supported is whatever the property types themselves implement.

namespace
{
    // A schema exercising one field of every kind a C# script can declare, including the recursive ones.
    Scripting::FScriptExportSchema MakeEveryKindSchema()
    {
        using namespace Scripting;
        FScriptExportSchema Schema;

        Schema.Fields.push_back(MakeScalarField("AFloat",  EPropertyTypeFlags::Float));
        Schema.Fields.push_back(MakeScalarField("ABool",   EPropertyTypeFlags::Bool));
        Schema.Fields.push_back(MakeScalarField("AnInt",   EPropertyTypeFlags::Int64));
        Schema.Fields.push_back(MakeScalarField("AString", EPropertyTypeFlags::String));
        Schema.Fields.push_back(MakeScalarField("AName",   EPropertyTypeFlags::Name));

        // Enum minted from the entries the C# type reported.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Enum);
            Type->EnumName = FName("EveryKind_Mode");
            Type->EnumEntries.push_back({ FName("Off"), 0 });
            Type->EnumEntries.push_back({ FName("On"),  1 });
            Schema.Fields.push_back(MakeField("AnEnum", Type));
        }

        // Soft object (an asset reference) and a hard object reference.
        Schema.Fields.push_back(MakeField("AnAsset",  MakeType(EPropertyTypeFlags::SoftObject)));
        Schema.Fields.push_back(MakeField("AnObject", MakeType(EPropertyTypeFlags::Object)));

        // Native struct, resolved by registered name.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
            Type->NativeName = FName("FVector3");
            Schema.Fields.push_back(MakeField("ANativeStruct", Type));
        }

        // Script struct, a nested C#-declared struct minted as its own CScriptStruct.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
            Type->Fields.push_back(MakeScalarField("Inner", EPropertyTypeFlags::Int32));
            Type->Fields.push_back(MakeScalarField("InnerText", EPropertyTypeFlags::String));
            Schema.Fields.push_back(MakeField("AScriptStruct", Type));
        }

        // An array of a storage-owning element needs its element description wired in by ConstructValue.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Vector);
            Type->ElementType = MakeType(EPropertyTypeFlags::String);
            Schema.Fields.push_back(MakeField("AStringArray", Type));
        }
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Vector);
            Type->ElementType = MakeType(EPropertyTypeFlags::Int32);
            Schema.Fields.push_back(MakeField("AnIntArray", Type));
        }

        // Map, with a storage-owning key.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Map);
            Type->KeyType = MakeType(EPropertyTypeFlags::String);
            Type->ValueType = MakeType(EPropertyTypeFlags::Float);
            Schema.Fields.push_back(MakeField("AMap", Type));
        }

        // Instanced struct, an empty base plus one selectable candidate.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::InstancedStruct);
            Type->BaseName = FName("EveryKind_Base");
            FScriptExportInstanceCandidate& Candidate = Type->Candidates.emplace_back();
            Candidate.TypeName = FName("EveryKind_Candidate");
            Candidate.Fields.push_back(MakeScalarField("Payload", EPropertyTypeFlags::Int32));
            Schema.Fields.push_back(MakeField("AnInstance", Type));
        }

        return Schema;
    }

    const char* const GEveryKindNames[] =
    {
        "AFloat", "ABool", "AnInt", "AString", "AName", "AnEnum", "AnAsset", "AnObject",
        "ANativeStruct", "AScriptStruct", "AStringArray", "AnIntArray", "AMap", "AnInstance",
    };
}

TEST(ScriptClassProperties, EveryReflectedKindIsAppendedPastTheShim)
{
    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_EveryKind", MakeEveryKindSchema(), ShimSize);
    ASSERT_NE(Sub, nullptr);

    for (const char* Name : GEveryKindNames)
    {
        FProperty* Property = Sub->GetProperty(FName(Name));
        ASSERT_NE(Property, nullptr) << "kind '" << Name << "' was dropped instead of appended";
        EXPECT_GE(Property->Offset, ShimSize) << Name << " overlaps the C++ shim";
    }

    // The native base property still resolves through the chain, so nothing about the append disturbed it.
    ASSERT_NE(Sub->GetProperty(FName("NativeValue")), nullptr);

    CObject* First  = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    CObject* Second = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);

    for (const char* Name : GEveryKindNames)
    {
        FProperty* Property = Sub->GetProperty(FName(Name));
        ASSERT_NE(Property, nullptr);
        if (!Property->OwnsStorage())
        {
            continue;
        }
        // Distinct addresses, so each instance carries its own value rather than one aliased buffer.
        EXPECT_NE(Property->GetValuePtr<uint8>(First), Property->GetValuePtr<uint8>(Second))
            << Name << " shares storage between instances";
    }

    // The shim half survived every append.
    CScriptableTest* Typed = Cast<CScriptableTest>(First);
    ASSERT_NE(Typed, nullptr);
    EXPECT_FLOAT_EQ(Typed->NativeValue, 1.5f);
    EXPECT_EQ(Typed->OnTest(5), 10);
}

// Containers need their element description wired in, or every later op is a null deref.
TEST(ScriptClassProperties, AppendedContainersAreUsableThroughTheStockPropertyApi)
{
    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_Containers", MakeEveryKindSchema(), ShimSize);
    ASSERT_NE(Sub, nullptr);

    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);

    FArrayProperty* Strings = static_cast<FArrayProperty*>(Sub->GetProperty(FName("AStringArray")));
    ASSERT_NE(Strings, nullptr);
    void* ArrayValue = Strings->GetValuePtr<uint8>(Object);

    EXPECT_EQ(Strings->GetNum(ArrayValue), 0u);
    Strings->Resize(ArrayValue, 2);
    ASSERT_EQ(Strings->GetNum(ArrayValue), 2u);

    // Each element was constructed, so assigning a heap-length string is safe.
    *static_cast<FString*>(Strings->GetAt(ArrayValue, 0)) = "element zero, long enough to be heap allocated";
    *static_cast<FString*>(Strings->GetAt(ArrayValue, 1)) = "element one";
    EXPECT_STREQ(static_cast<FString*>(Strings->GetAt(ArrayValue, 0))->c_str(), "element zero, long enough to be heap allocated");

    Strings->RemoveAt(ArrayValue, 0);
    ASSERT_EQ(Strings->GetNum(ArrayValue), 1u);
    EXPECT_STREQ(static_cast<FString*>(Strings->GetAt(ArrayValue, 0))->c_str(), "element one");

    FMapProperty* Map = static_cast<FMapProperty*>(Sub->GetProperty(FName("AMap")));
    ASSERT_NE(Map, nullptr);
    void* MapValue = Map->GetValuePtr<uint8>(Object);

    EXPECT_EQ(Map->GetNum(MapValue), 0u);
    {
        const FString Key = "speed";
        const float Value = 2.5f;
        Map->Insert(MapValue, &Key, &Value);
    }
    ASSERT_EQ(Map->GetNum(MapValue), 1u);
    {
        const FString Key = "speed";
        void* Found = Map->Find(MapValue, &Key);
        ASSERT_NE(Found, nullptr) << "the key was stored but cannot be found (key Identical wired wrong?)";
        EXPECT_FLOAT_EQ(*static_cast<float*>(Found), 2.5f);
    }
}

// Containers and strings survive the stock tagged serializer with no bespoke codec.
TEST(ScriptClassProperties, AppendedContainersRoundTripThroughTheStockSerializer)
{
    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptProps_ContainerSerialize", MakeEveryKindSchema(), ShimSize);
    ASSERT_NE(Sub, nullptr);

    FArrayProperty* Ints = static_cast<FArrayProperty*>(Sub->GetProperty(FName("AnIntArray")));
    FProperty* Text = Sub->GetProperty(FName("AString"));
    ASSERT_NE(Ints, nullptr);
    ASSERT_NE(Text, nullptr);

    CObject* Source = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Source, nullptr);

    void* SourceArray = Ints->GetValuePtr<uint8>(Source);
    Ints->Resize(SourceArray, 3);
    *static_cast<int32*>(Ints->GetAt(SourceArray, 0)) = 10;
    *static_cast<int32*>(Ints->GetAt(SourceArray, 1)) = 20;
    *static_cast<int32*>(Ints->GetAt(SourceArray, 2)) = 30;
    *Text->GetValuePtr<FString>(Source) = "carried alongside";

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Sub->SerializeTaggedProperties(Ar, Source);
    }
    ASSERT_FALSE(Bytes.empty());

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeTaggedProperties(Ar, Restored);
    }

    void* RestoredArray = Ints->GetValuePtr<uint8>(Restored);
    ASSERT_EQ(Ints->GetNum(RestoredArray), 3u) << "the array did not round-trip";
    EXPECT_EQ(*static_cast<int32*>(Ints->GetAt(RestoredArray, 0)), 10);
    EXPECT_EQ(*static_cast<int32*>(Ints->GetAt(RestoredArray, 2)), 30);
    EXPECT_STREQ(Text->GetValuePtr<FString>(Restored)->c_str(), "carried alongside");
}

//~ Hot reload rebuilds the appended block, since a changed set cannot be patched in place.

TEST(ScriptClassReload, AnUnchangedSchemaNeedsNoRebuild)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Unchanged", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    // An identical schema must compare equal, or every reload pays a rebuild for nothing.
    Scripting::FScriptExportSchema Same;
    Same.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    EXPECT_TRUE(Scripting::ScriptClassLayoutMatches(Sub, Same));

    // Metadata is not layout, so retitling a field must not force a rebuild either.
    Scripting::FScriptExportSchema Retitled;
    Retitled.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Retitled.Fields[0].Meta.Set(FName("Tooltip"), "now with a tooltip");
    EXPECT_TRUE(Scripting::ScriptClassLayoutMatches(Sub, Retitled));

    // Any real shape change does not.
    Scripting::FScriptExportSchema Added;
    Added.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Added.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));
    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Added));

    Scripting::FScriptExportSchema Retyped;
    Retyped.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Double));
    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Retyped));

    Scripting::FScriptExportSchema Renamed;
    Renamed.Fields.push_back(MakeScalarField("Velocity", EPropertyTypeFlags::Float));
    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Renamed));
}

TEST(ScriptClassReload, AddingAPropertyRebuildsTheBlock)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Added", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Speed")), nullptr);
    ASSERT_EQ(Sub->GetProperty(FName("Health")), nullptr);

    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    After.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));

    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Sub, After));

    FProperty* Speed  = Sub->GetProperty(FName("Speed"));
    FProperty* Health = Sub->GetProperty(FName("Health"));
    ASSERT_NE(Speed, nullptr);
    ASSERT_NE(Health, nullptr) << "the added property did not appear after the rebuild";
    EXPECT_GE(Speed->Offset, ShimSize);
    EXPECT_GE(Health->Offset, ShimSize);
    EXPECT_NE(Speed->Offset, Health->Offset);

    // Re-linking has to restore the super's chain, or the base's members disappear.
    ASSERT_NE(Sub->GetProperty(FName("NativeValue")), nullptr) << "Unlink lost the base class's properties";

    // A fresh CDO at the new size, and instances built from it carry the new field.
    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    Health->SetValue<int32>(Object, 77);
    Speed->SetValue<float>(Object, 4.5f);
    EXPECT_EQ(*Health->GetValuePtr<int32>(Object), 77);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Object), 4.5f);

    // The shim half still works, so the rebuild did not disturb the C++ subclass under the block.
    CScriptableTest* Typed = Cast<CScriptableTest>(Object);
    ASSERT_NE(Typed, nullptr);
    EXPECT_FLOAT_EQ(Typed->NativeValue, 1.5f);
    EXPECT_EQ(Typed->OnTest(5), 10);
}

TEST(ScriptClassReload, RemovingAndRetypingAPropertyRebuildTheBlock)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Before.Fields.push_back(MakeScalarField("Doomed", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Removed", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Doomed")), nullptr);

    // Drop one and widen the other in the same reload.
    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Double));

    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Sub, After));

    EXPECT_EQ(Sub->GetProperty(FName("Doomed")), nullptr) << "the removed property is still on the class";

    FProperty* Speed = Sub->GetProperty(FName("Speed"));
    ASSERT_NE(Speed, nullptr);
    EXPECT_EQ(Speed->TypeFlags, EPropertyTypeFlags::Double) << "the retyped property kept its old kind";

    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    Speed->SetValue<double>(Object, -2.25);
    EXPECT_DOUBLE_EQ(*Speed->GetValuePtr<double>(Object), -2.25);
}

TEST(ScriptClassReload, ValuesSurviveARebuildThroughTheStockSerializer)
{
    // The carrier is name-keyed, so a surviving property replays and an added one takes its default.
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    Before.Fields.push_back(MakeScalarField("Doomed", EPropertyTypeFlags::Int32));
    Before.Fields.push_back(MakeField("Label", MakeType(EPropertyTypeFlags::String)));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Values", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);

    CObject* Authored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Authored, nullptr);
    Sub->GetProperty(FName("Speed"))->SetValue<float>(Authored, 12.5f);
    Sub->GetProperty(FName("Doomed"))->SetValue<int32>(Authored, 7);
    *Sub->GetProperty(FName("Label"))->GetValuePtr<FString>(Authored) = "authored";

    // Evacuate exactly the way a live reload must, tagged properties into a buffer.
    TVector<uint8> Buffer;
    {
        FMemoryWriter Writer(Buffer);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Sub->SerializeTaggedProperties(Ar, Authored);
    }

    // The instance has to be gone before the class can be rebuilt under it.
    Authored->ForceDestroyNow();

    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    After.Fields.push_back(MakeField("Label", MakeType(EPropertyTypeFlags::String)));
    After.Fields.push_back(MakeScalarField("Added", EPropertyTypeFlags::Int32));

    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Sub, After));

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Buffer);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeTaggedProperties(Ar, Restored);
    }

    // Survived the rebuild.
    EXPECT_FLOAT_EQ(*Sub->GetProperty(FName("Speed"))->GetValuePtr<float>(Restored), 12.5f);
    EXPECT_EQ(*Sub->GetProperty(FName("Label"))->GetValuePtr<FString>(Restored), FString("authored"));

    // Added after the snapshot, so it sits at its default rather than reading someone else's bytes.
    ASSERT_NE(Sub->GetProperty(FName("Added")), nullptr);
    EXPECT_EQ(*Sub->GetProperty(FName("Added"))->GetValuePtr<int32>(Restored), 0);

    // And the removed one is simply not there.
    EXPECT_EQ(Sub->GetProperty(FName("Doomed")), nullptr);
}

TEST(ScriptClassReload, ARebuildIsRefusedWhileInstancesAreLive)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Refused", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);

    CObject* Live = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Live, nullptr);

    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    After.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));

    // An instance is laid out at the old size, so refusing is the contract and the caller evacuates.
    EXPECT_FALSE(Scripting::MigrateMintedClassLayout(Sub, After));

    // And the refusal changed nothing, so the live instance is still usable.
    ASSERT_EQ(Sub->GetProperty(FName("Health")), nullptr);
    FProperty* Speed = Sub->GetProperty(FName("Speed"));
    ASSERT_NE(Speed, nullptr);
    Speed->SetValue<float>(Live, 3.0f);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Live), 3.0f);

    // Once it is gone the same rebuild goes through.
    Live->ForceDestroyNow();
    EXPECT_TRUE(Scripting::MigrateMintedClassLayout(Sub, After));
    EXPECT_NE(Sub->GetProperty(FName("Health")), nullptr);
}

TEST(ScriptClassReload, ContainersSurviveARebuild)
{
    // Containers own side data, so the layout record is retired rather than freed.
    Scripting::FScriptExportSchema Before;
    TSharedPtr<Scripting::FScriptExportType> List = MakeType(EPropertyTypeFlags::Vector);
    List->ElementType = MakeType(EPropertyTypeFlags::Int32);
    Before.Fields.push_back(MakeField("Values", List));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Containers", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Values")), nullptr);

    // Changing the ELEMENT type only is the case a shallow comparison would lay out wrong.
    Scripting::FScriptExportSchema After;
    TSharedPtr<Scripting::FScriptExportType> Floats = MakeType(EPropertyTypeFlags::Vector);
    Floats->ElementType = MakeType(EPropertyTypeFlags::Float);
    After.Fields.push_back(MakeField("Values", Floats));

    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, After)) << "an element-type change was not noticed";
    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Sub, After));

    FArrayProperty* Values = static_cast<FArrayProperty*>(Sub->GetProperty(FName("Values")));
    ASSERT_NE(Values, nullptr);
    ASSERT_NE(Values->GetOps(), nullptr);
    EXPECT_EQ(Values->GetOps()->ElementSize, sizeof(float)) << "the rebuilt array kept the old element size";

    // Usable through the stock API, which proves the new element description was wired in.
    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    void* Array = Values->GetValuePtr<void>(Object);
    ASSERT_NE(Array, nullptr);
    EXPECT_EQ(Values->GetOps()->Size(Array), 0u);
    const float One = 1.0f;
    Values->GetOps()->PushBack(Array, &One);
    EXPECT_EQ(Values->GetOps()->Size(Array), 1u);
}

namespace
{
    size_t CountObjectsNamed(const char* Prefix)
    {
        const size_t PrefixLength = std::strlen(Prefix);
        size_t Count = 0;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base == nullptr || Base->HasAnyFlag(OF_MarkedDestroy))
            {
                return;
            }
            const FName Name = Base->GetName();
            if (std::strncmp(Name.c_str(), Prefix, PrefixLength) == 0)
            {
                ++Count;
            }
        });
        return Count;
    }
}

// A superseded layout used to be kept forever, so an editing session grew a record per property edit.
TEST(ScriptClassReload, RepeatedRebuildsRetainOnlyOneSupersededLayout)
{
    Scripting::FScriptExportSchema Initial;
    Initial.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Bounded", Initial, ShimSize);
    ASSERT_NE(Sub, nullptr);

    const size_t AfterFirstBuild = CountObjectsNamed("ScriptClassLayout_ScriptReload_Bounded");
    ASSERT_EQ(AfterFirstBuild, 1u);

    // Alternating shapes so every pass is a real layout change rather than a no-op.
    for (int32 Pass = 0; Pass < 8; ++Pass)
    {
        Scripting::FScriptExportSchema Next;
        Next.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
        if ((Pass % 2) == 0)
        {
            Next.Fields.push_back(MakeScalarField("Health", EPropertyTypeFlags::Int32));
        }

        ASSERT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Next));
        ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Sub, Next));
    }

    // The live layout plus at most the one it superseded, no matter how many rebuilds ran.
    const size_t Retained = CountObjectsNamed("ScriptClassLayout_ScriptReload_Bounded");
    EXPECT_LE(Retained, 2u) << "a superseded layout record is leaking once per rebuild";
}
