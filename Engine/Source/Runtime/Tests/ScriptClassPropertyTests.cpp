#include <gtest/gtest.h>

#include <cstring>

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"
#include "ScriptTestUtil.h"

using namespace Lumina;

// A minted CClass declares a larger size and carries real FPropertys in the trailing block.

// The schema is built by hand here because a live .NET host normally produces it.

namespace
{
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

    CScriptClass* MintWithSchema(const char* ClassName, const Scripting::FScriptExportSchema& Schema, uint32& OutShimSize)
    {
        CScriptClass* Minted = FScriptableRegistry::Mint(ClassName, "CScriptableTest");
        if (Minted == nullptr)
        {
            return nullptr;
        }
        OutShimSize = Minted->GetSize();
        Scripting::AppendScriptPropertiesToClass(Minted, Schema);

        // Register the reflected types first, since Link latches and would lose the base's properties.
        RegisterPendingTypes();
        Minted->GetDefaultObject();   // Link (chains supers) + CDO at the enlarged size
        return Minted;
    }
}

TEST(ScriptClassProperties, SchemaFieldsBecomeRealPropertiesPastTheShim)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));
    Schema.Fields.Add(ScriptTest::ScalarField("Enabled", EPropertyTypeFlags::Bool));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptProps_Layout", Schema, ShimSize);
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
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptProps_Serialize", Schema, ShimSize);
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
        Sub->SerializeProperties(Ar, Source);
    }
    ASSERT_FALSE(Bytes.IsEmpty());

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeProperties(Ar, Restored);
    }

    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Restored), 3.5f) << "script property did not round-trip";
    EXPECT_EQ(*Health->GetValuePtr<int32>(Restored), 42);
    EXPECT_FLOAT_EQ(Cast<CScriptableTest>(Restored)->NativeValue, 8.0f) << "native property did not round-trip";
}

// Constructed, not just zeroed, since an FString over memzeroed bytes corrupts on assignment.
TEST(ScriptClassProperties, StorageOwningFieldsAreConstructedPerInstance)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.Add(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));
    Schema.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptProps_Strings", Schema, ShimSize);
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
    EXPECT_TRUE(Label->GetValuePtr<FString>(First)->IsEmpty());
    *Label->GetValuePtr<FString>(First) = "a string long enough to have heap storage rather than SSO";
    *Label->GetValuePtr<FString>(Second) = "another";

    EXPECT_STREQ(*(*Label->GetValuePtr<FString>(First)), "a string long enough to have heap storage rather than SSO");
    EXPECT_STREQ(*(*Label->GetValuePtr<FString>(Second)), "another") << "instances share storage";

    // The shim half is untouched.
    ASSERT_NE(Cast<CScriptableTest>(First), nullptr);
    EXPECT_FLOAT_EQ(Cast<CScriptableTest>(First)->NativeValue, 1.5f);
}

// Strings round-trip through the stock tagged serializer, with nothing special-cased.
TEST(ScriptClassProperties, StringPropertiesRoundTripThroughTheStockSerializer)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));
    Schema.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptProps_StringSerialize", Schema, ShimSize);
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
        Sub->SerializeProperties(Ar, Source);
    }
    ASSERT_FALSE(Bytes.IsEmpty());

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeProperties(Ar, Restored);
    }

    EXPECT_STREQ(*(*Label->GetValuePtr<FString>(Restored)), "round trip me");
    EXPECT_EQ(*Health->GetValuePtr<int32>(Restored), 17);
}

// Declared defaults live on the CDO and every instance is copied from it, by value.
TEST(ScriptClassProperties, ValuesOnTheDefaultObjectSeedEveryInstance)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields.Add(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptProps_Defaults", Schema, ShimSize);
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
    EXPECT_STREQ(*(*Label->GetValuePtr<FString>(Instance)), "declared default");

    // Seeded by value, not shared, so writing the instance leaves the CDO alone.
    *Label->GetValuePtr<FString>(Instance) = "mutated";
    EXPECT_STREQ(*(*Label->GetValuePtr<FString>(Cdo)), "declared default");
}

// Which kinds are supported is whatever the property types themselves implement.

namespace
{
    // A schema exercising one field of every kind a C# script can declare, including the recursive ones.
    Scripting::FScriptExportSchema MakeEveryKindSchema()
    {
        using namespace Scripting;
        FScriptExportSchema Schema;

        Schema.Fields.Add(ScriptTest::ScalarField("AFloat",  EPropertyTypeFlags::Float));
        Schema.Fields.Add(ScriptTest::ScalarField("ABool",   EPropertyTypeFlags::Bool));
        Schema.Fields.Add(ScriptTest::ScalarField("AnInt",   EPropertyTypeFlags::Int64));
        Schema.Fields.Add(ScriptTest::ScalarField("AString", EPropertyTypeFlags::String));
        Schema.Fields.Add(ScriptTest::ScalarField("AName",   EPropertyTypeFlags::Name));

        // An enum is its own reflected type, which the field names.
        {
            ScriptTest::PublishEnum("EveryKind_Mode", EPropertyTypeFlags::Int32, { { FName("Off"), 0 }, { FName("On"), 1 } });
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Enum);
            Type->EnumName = FName("EveryKind_Mode");
            Schema.Fields.Add(MakeField("AnEnum", Type));
        }

        // Soft object (an asset reference) and a hard object reference.
        Schema.Fields.Add(MakeField("AnAsset",  MakeType(EPropertyTypeFlags::SoftObject)));
        Schema.Fields.Add(MakeField("AnObject", MakeType(EPropertyTypeFlags::Object)));

        // Native struct, resolved by registered name.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
            Type->NativeName = FName("FVector3");
            Schema.Fields.Add(MakeField("ANativeStruct", Type));
        }

        // A C# struct is published once as its own reflected type, and named like a native one.
        {
            FScriptExportSchema Inner;
            Inner.Fields.Add(ScriptTest::ScalarField("Inner", EPropertyTypeFlags::Int32));
            Inner.Fields.Add(ScriptTest::ScalarField("InnerText", EPropertyTypeFlags::String));
            FScriptExportSchema Base;
            Base.Fields.Add(ScriptTest::ScalarField("Payload", EPropertyTypeFlags::Int32));
            ScriptTest::PublishStructs({ ScriptTest::StructDefinition("EveryKind_Struct", std::move(Inner)),
                                         ScriptTest::StructDefinition("EveryKind_Base", std::move(Base)) });

            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
            Type->NativeName = FName("EveryKind_Struct");
            Schema.Fields.Add(MakeField("AScriptStruct", Type));
        }

        // An array of a storage-owning element needs its element description wired in by ConstructValue.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Vector);
            Type->ElementType = MakeType(EPropertyTypeFlags::String);
            Schema.Fields.Add(MakeField("AStringArray", Type));
        }
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Vector);
            Type->ElementType = MakeType(EPropertyTypeFlags::Int32);
            Schema.Fields.Add(MakeField("AnIntArray", Type));
        }

        // Map, with a storage-owning key.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Map);
            Type->KeyType = MakeType(EPropertyTypeFlags::String);
            Type->ValueType = MakeType(EPropertyTypeFlags::Float);
            Schema.Fields.Add(MakeField("AMap", Type));
        }

        // Instanced struct, whose base is any reflected struct.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::InstancedStruct);
            Type->BaseName = FName("EveryKind_Base");
            Schema.Fields.Add(MakeField("AnInstance", Type));
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
    CScriptClass* Sub = MintWithSchema("ScriptProps_EveryKind", MakeEveryKindSchema(), ShimSize);
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
    CScriptClass* Sub = MintWithSchema("ScriptProps_Containers", MakeEveryKindSchema(), ShimSize);
    ASSERT_NE(Sub, nullptr);

    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);

    FArrayProperty* Strings = static_cast<FArrayProperty*>(Sub->GetProperty(FName("AStringArray")));
    ASSERT_NE(Strings, nullptr);
    void* ArrayValue = Strings->GetValuePtr<uint8>(Object);

    EXPECT_EQ(Strings->Num(ArrayValue), 0);
    Strings->SetNum(ArrayValue, 2);
    ASSERT_EQ(Strings->Num(ArrayValue), 2);

    // Each element was constructed, so assigning a heap-length string is safe.
    *static_cast<FString*>(Strings->GetAt(ArrayValue, 0)) = "element zero, long enough to be heap allocated";
    *static_cast<FString*>(Strings->GetAt(ArrayValue, 1)) = "element one";
    EXPECT_STREQ(*(*static_cast<FString*>(Strings->GetAt(ArrayValue, 0))), "element zero, long enough to be heap allocated");

    Strings->RemoveAt(ArrayValue, 0);
    ASSERT_EQ(Strings->Num(ArrayValue), 1);
    EXPECT_STREQ(*(*static_cast<FString*>(Strings->GetAt(ArrayValue, 0))), "element one");

    FMapProperty* Map = static_cast<FMapProperty*>(Sub->GetProperty(FName("AMap")));
    ASSERT_NE(Map, nullptr);
    void* MapValue = Map->GetValuePtr<uint8>(Object);

    EXPECT_EQ(Map->Num(MapValue), 0);
    {
        const FString Key = "speed";
        const float Value = 2.5f;
        Map->Add(MapValue, &Key, &Value);
    }
    ASSERT_EQ(Map->Num(MapValue), 1);
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
    CScriptClass* Sub = MintWithSchema("ScriptProps_ContainerSerialize", MakeEveryKindSchema(), ShimSize);
    ASSERT_NE(Sub, nullptr);

    FArrayProperty* Ints = static_cast<FArrayProperty*>(Sub->GetProperty(FName("AnIntArray")));
    FProperty* Text = Sub->GetProperty(FName("AString"));
    ASSERT_NE(Ints, nullptr);
    ASSERT_NE(Text, nullptr);

    CObject* Source = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Source, nullptr);

    void* SourceArray = Ints->GetValuePtr<uint8>(Source);
    Ints->SetNum(SourceArray, 3);
    *static_cast<int32*>(Ints->GetAt(SourceArray, 0)) = 10;
    *static_cast<int32*>(Ints->GetAt(SourceArray, 1)) = 20;
    *static_cast<int32*>(Ints->GetAt(SourceArray, 2)) = 30;
    *Text->GetValuePtr<FString>(Source) = "carried alongside";

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Sub->SerializeProperties(Ar, Source);
    }
    ASSERT_FALSE(Bytes.IsEmpty());

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeProperties(Ar, Restored);
    }

    void* RestoredArray = Ints->GetValuePtr<uint8>(Restored);
    ASSERT_EQ(Ints->Num(RestoredArray), 3) << "the array did not round-trip";
    EXPECT_EQ(*static_cast<int32*>(Ints->GetAt(RestoredArray, 0)), 10);
    EXPECT_EQ(*static_cast<int32*>(Ints->GetAt(RestoredArray, 2)), 30);
    EXPECT_STREQ(*(*Text->GetValuePtr<FString>(Restored)), "carried alongside");
}

//~ Hot reload rebuilds the appended block, since a changed set cannot be patched in place.

TEST(ScriptClassReload, AnUnchangedSchemaNeedsNoRebuild)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Unchanged", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    // An identical schema must compare equal, or every reload pays a rebuild for nothing.
    Scripting::FScriptExportSchema Same;
    Same.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    EXPECT_TRUE(Scripting::ScriptClassLayoutMatches(Sub, Same));

    // Metadata is not layout, so retitling a field must not force a rebuild either.
    Scripting::FScriptExportSchema Retitled;
    Retitled.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Retitled.Fields[0].Meta.Set(FName("Tooltip"), "now with a tooltip");
    EXPECT_TRUE(Scripting::ScriptClassLayoutMatches(Sub, Retitled));

    // Any real shape change does not.
    Scripting::FScriptExportSchema Added;
    Added.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Added.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));
    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Added));

    Scripting::FScriptExportSchema Retyped;
    Retyped.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Double));
    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Retyped));

    Scripting::FScriptExportSchema Renamed;
    Renamed.Fields.Add(ScriptTest::ScalarField("Velocity", EPropertyTypeFlags::Float));
    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Renamed));
}

// A reload has to say WHAT changed, not just that something did: a reworded tooltip must reach the editor
// without a rebuild, which refuses while instances are live and so used to drop the edit on the floor.
TEST(ScriptClassReload, ADiffSeparatesLayoutFromMetadataAndDefaults)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields[0].Meta.Set(FName("Tooltip"), "how fast");

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Diff", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    EXPECT_EQ(Scripting::DiffScriptClassLayout(Sub, Schema), EScriptTypeDirty::None)
        << "an identical schema must be clean, or every reload pays for nothing";

    Scripting::FScriptExportSchema Retitled;
    Retitled.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Retitled.Fields[0].Meta.Set(FName("Tooltip"), "how fast it goes");
    EXPECT_EQ(Scripting::DiffScriptClassLayout(Sub, Retitled), EScriptTypeDirty::Metadata);

    Scripting::FScriptExportSchema Retyped;
    Retyped.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Double));
    EXPECT_TRUE(EnumHasAnyFlags(Scripting::DiffScriptClassLayout(Sub, Retyped), EScriptTypeDirty::Layout));
}

// The metadata-only remedy: reapplied against the properties already in place, with the block untouched.
TEST(ScriptClassReload, RefreshingMetadataKeepsTheSamePropertiesAndReachesTheEditor)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Schema.Fields[0].Meta.Set(FName("Tooltip"), "how fast");

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_MetaRefresh", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    FProperty* Before = Sub->GetProperty(FName("Speed"));
    ASSERT_NE(Before, nullptr);
    EXPECT_EQ(Before->GetMetadata("Tooltip").View(), FStringView("how fast"));

    Scripting::FScriptExportSchema Retitled;
    Retitled.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Retitled.Fields[0].Meta.Set(FName("Tooltip"), "how fast it goes");
    Retitled.Fields[0].Meta.Set(FName("Category"), "Movement");

    Scripting::RefreshScriptPropertyMetadata(Sub, Retitled);

    FProperty* After = Sub->GetProperty(FName("Speed"));
    EXPECT_EQ(After, Before) << "a metadata refresh must not rebuild the block";
    EXPECT_EQ(After->GetMetadata("Tooltip").View(), FStringView("how fast it goes"));
    EXPECT_EQ(After->GetMetadata("Category").View(), FStringView("Movement"));

    // And the record now agrees, so the next reload sees no change rather than the same one again.
    EXPECT_EQ(Scripting::DiffScriptClassLayout(Sub, Retitled), EScriptTypeDirty::None);
}

TEST(ScriptClassReload, AddingAPropertyRebuildsTheBlock)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Added", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Speed")), nullptr);
    ASSERT_EQ(Sub->GetProperty(FName("Health")), nullptr);

    Scripting::FScriptExportSchema After;
    After.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    After.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));

    Sub = ScriptTest::Reshape(Sub, "ScriptReshape_V1", After);
    ASSERT_NE(Sub, nullptr);

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
    Before.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Before.Fields.Add(ScriptTest::ScalarField("Doomed", EPropertyTypeFlags::Int32));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Removed", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Doomed")), nullptr);

    // Drop one and widen the other in the same reload.
    Scripting::FScriptExportSchema After;
    After.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Double));

    Sub = ScriptTest::Reshape(Sub, "ScriptReshape_V2", After);
    ASSERT_NE(Sub, nullptr);

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
    Before.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Before.Fields.Add(ScriptTest::ScalarField("Doomed", EPropertyTypeFlags::Int32));
    Before.Fields.Add(MakeField("Label", MakeType(EPropertyTypeFlags::String)));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Values", Before, ShimSize);
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
        Sub->SerializeProperties(Ar, Authored);
    }

    // The instance has to be gone before the class can be rebuilt under it.
    Authored->ForceDestroyNow();

    Scripting::FScriptExportSchema After;
    After.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    After.Fields.Add(MakeField("Label", MakeType(EPropertyTypeFlags::String)));
    After.Fields.Add(ScriptTest::ScalarField("Added", EPropertyTypeFlags::Int32));

    Sub = ScriptTest::Reshape(Sub, "ScriptReshape_V3", After);
    ASSERT_NE(Sub, nullptr);

    CObject* Restored = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Restored, nullptr);
    {
        FMemoryReader Reader(Buffer);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Sub->SerializeProperties(Ar, Restored);
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

TEST(ScriptClassReload, ALiveInstanceIsCarriedOntoTheReshapedClass)
{
    Scripting::FScriptExportSchema Before;
    Before.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Live", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);

    CObject* Live = NewObject(Sub, nullptr, FName("ReshapeSubject"), FGuid::New(), OF_Transient);
    ASSERT_NE(Live, nullptr);
    Sub->GetProperty(FName("Speed"))->SetValue<float>(Live, 3.0f);

    Scripting::FScriptExportSchema After;
    After.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    After.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));

    // A live instance used to refuse the reshape outright; now it is the case the reinstancer exists for.
    Sub = ScriptTest::Reshape(Sub, "ScriptReload_Live_V2", After);
    ASSERT_NE(Sub, nullptr);

    CObject* Carried = FindObject<CObject>(FName("ReshapeSubject"));
    ASSERT_NE(Carried, nullptr) << "the instance did not come across to the reshaped class";
    EXPECT_EQ(Carried->GetClass(), Sub);

    FProperty* Speed = Sub->GetProperty(FName("Speed"));
    ASSERT_NE(Speed, nullptr);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Carried), 3.0f) << "a value in both shapes survives the reshape";

    ASSERT_NE(Sub->GetProperty(FName("Health")), nullptr);
    EXPECT_EQ(*Sub->GetProperty(FName("Health"))->GetValuePtr<int32>(Carried), 0);
}

TEST(ScriptClassReload, ContainersSurviveARebuild)
{
    // Containers own side data, so the layout record is retired rather than freed.
    Scripting::FScriptExportSchema Before;
    TSharedPtr<Scripting::FScriptExportType> List = MakeType(EPropertyTypeFlags::Vector);
    List->ElementType = MakeType(EPropertyTypeFlags::Int32);
    Before.Fields.Add(MakeField("Values", List));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_Containers", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Values")), nullptr);

    // Changing the ELEMENT type only is the case a shallow comparison would lay out wrong.
    Scripting::FScriptExportSchema After;
    TSharedPtr<Scripting::FScriptExportType> Floats = MakeType(EPropertyTypeFlags::Vector);
    Floats->ElementType = MakeType(EPropertyTypeFlags::Float);
    After.Fields.Add(MakeField("Values", Floats));

    EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, After)) << "an element-type change was not noticed";
    Sub = ScriptTest::Reshape(Sub, "ScriptReshape_V4", After);
    ASSERT_NE(Sub, nullptr);

    FArrayProperty* Values = static_cast<FArrayProperty*>(Sub->GetProperty(FName("Values")));
    ASSERT_NE(Values, nullptr);
    ASSERT_NE(Values->GetOps(), nullptr);
    EXPECT_EQ(Values->GetOps()->ElementSize, sizeof(float)) << "the rebuilt array kept the old element size";

    // Usable through the stock API, which proves the new element description was wired in.
    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    void* Array = Values->GetValuePtr<void>(Object);
    ASSERT_NE(Array, nullptr);
    EXPECT_EQ(Values->GetOps()->Num(Array), 0);
    const float One = 1.0f;
    Values->GetOps()->Add(Array, &One);
    EXPECT_EQ(Values->GetOps()->Num(Array), 1);
}

// The superseded-layout grace existed only for an in-place rebuild. A reshape now mints a new class, so the
// invariant that matters is that the old one is left empty and can retire.
TEST(ScriptClassReload, AReshapeLeavesTheOldClassWithNoInstances)
{
    Scripting::FScriptExportSchema Initial;
    Initial.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CScriptClass* Old = MintWithSchema("ScriptReload_Bounded", Initial, ShimSize);
    ASSERT_NE(Old, nullptr);

    for (int32 Index = 0; Index < 4; ++Index)
    {
        NewObject(Old, nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    auto CountInstancesOf = [](const CClass* Class)
    {
        size_t Count = 0;
        ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base != nullptr && Base->GetClass() == Class
                && !Base->HasAnyFlag(OF_MarkedDestroy) && !Base->HasAnyFlag(OF_DefaultObject))
            {
                ++Count;
            }
        });
        return Count;
    };

    ASSERT_EQ(CountInstancesOf(Old), 4u);

    Scripting::FScriptExportSchema Next;
    Next.Fields.Add(ScriptTest::ScalarField("Speed", EPropertyTypeFlags::Float));
    Next.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Int32));

    CScriptClass* New = ScriptTest::Reshape(Old, "ScriptReload_Bounded_V2", Next);
    ASSERT_NE(New, nullptr);

    EXPECT_EQ(CountInstancesOf(Old), 0u) << "an instance left on the old class would block its retirement";
    EXPECT_EQ(CountInstancesOf(New), 4u) << "every instance must land on the replacement";
}

namespace
{
    Scripting::FScriptExportField MakeSameShapeStructField(const char* Name)
    {
        TSharedPtr<Scripting::FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
        Type->NativeName = FName("SharedMint_Point");
        return MakeField(Name, Type);
    }

    Scripting::FScriptExportField MakeSharedEnumField(const char* Name)
    {
        TSharedPtr<Scripting::FScriptExportType> Type = MakeType(EPropertyTypeFlags::Enum);
        Type->EnumName = FName("SharedMint_Mode");
        Type->EnumUnderlying = EPropertyTypeFlags::Int32;
        return MakeField(Name, Type);
    }
}

// Every field of a type names the one reflected type, so none mints a layout of its own.
TEST(ScriptClassProperties, IdenticalTypesShareOneMintedLayout)
{
    Scripting::FScriptExportSchema Point;
    Point.Fields.Add(ScriptTest::ScalarField("X", EPropertyTypeFlags::Float));
    Point.Fields.Add(ScriptTest::ScalarField("Y", EPropertyTypeFlags::Float));
    ScriptTest::PublishStructs({ ScriptTest::StructDefinition("SharedMint_Point", std::move(Point)) });
    ScriptTest::PublishEnum("SharedMint_Mode", EPropertyTypeFlags::Int32, { { FName("Off"), 0 }, { FName("On"), 1 } });

    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(MakeSameShapeStructField("First"));
    Schema.Fields.Add(MakeSameShapeStructField("Second"));
    Schema.Fields.Add(MakeSameShapeStructField("Third"));
    Schema.Fields.Add(MakeSharedEnumField("ModeA"));
    Schema.Fields.Add(MakeSharedEnumField("ModeB"));

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptProps_SharedMint", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    auto StructOf = [&](const char* FieldName) -> CStruct*
    {
        FProperty* Property = Sub->GetProperty(FName(FieldName));
        return Property != nullptr ? static_cast<FStructProperty*>(Property)->GetStruct() : nullptr;
    };

    CStruct* First = StructOf("First");
    ASSERT_NE(First, nullptr);
    EXPECT_EQ(StructOf("Second"), First) << "a second field of the same type minted its own layout";
    EXPECT_EQ(StructOf("Third"),  First);

    auto EnumOf = [&](const char* FieldName) -> CEnum*
    {
        FProperty* Property = Sub->GetProperty(FName(FieldName));
        return Property != nullptr ? static_cast<FEnumProperty*>(Property)->GetEnum() : nullptr;
    };

    CEnum* ModeA = EnumOf("ModeA");
    ASSERT_NE(ModeA, nullptr);
    EXPECT_EQ(EnumOf("ModeB"), ModeA) << "a second field of the same enum minted its own CEnum";
}

// A struct keeps its object while its shape holds, and a reshape supersedes it so every holder is rebuilt.
TEST(ScriptClassReload, EditingAStructShapeSupersedesTheOldType)
{
    auto PublishInner = [](int32 InnerCount)
    {
        Scripting::FScriptExportSchema Inner;
        for (int32 Index = 0; Index < InnerCount; ++Index)
        {
            FString FieldName = "Field";
            FieldName += *Format("{}", Index);
            Inner.Fields.Add(ScriptTest::ScalarField(*FieldName, EPropertyTypeFlags::Float));
        }
        ScriptTest::PublishStructs({ ScriptTest::StructDefinition("ShapeChurn_Inner", std::move(Inner)) });
        return FScriptStructRegistry::Get().Find(FName("ShapeChurn_Inner"));
    };

    Scripting::FScriptExportSchema Schema;
    {
        TSharedPtr<Scripting::FScriptExportType> Nested = MakeType(EPropertyTypeFlags::Struct);
        Nested->NativeName = FName("ShapeChurn_Inner");
        Schema.Fields.Add(MakeField("Nested", Nested));
    }

    CScriptStruct* Current = PublishInner(1);
    ASSERT_NE(Current, nullptr);
    EXPECT_EQ(PublishInner(1), Current) << "an unchanged struct must keep the object every holder points at";

    uint32 ShimSize = 0;
    CScriptClass* Sub = MintWithSchema("ScriptReload_ShapeChurn", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);
    EXPECT_TRUE(Scripting::ScriptClassLayoutMatches(Sub, Schema));

    for (int32 Pass = 2; Pass <= 5; ++Pass)
    {
        CScriptStruct* Previous = Current;
        Current = PublishInner(Pass);
        ASSERT_NE(Current, nullptr);
        ASSERT_NE(Current, Previous);
        EXPECT_TRUE(Previous->IsSuperseded());
        EXPECT_EQ(FindObject<CStruct>(FName("ShapeChurn_Inner")), Current) << "the name must resolve to the current shape";

        EXPECT_FALSE(Scripting::ScriptClassLayoutMatches(Sub, Schema)) << "a class holding a superseded struct must be rebuilt";
        Sub = ScriptTest::Reshape(Sub, *Format("ScriptReload_ShapeChurn{}", Pass), Schema);
        ASSERT_NE(Sub, nullptr);

        FProperty* Property = Sub->GetProperty(FName("Nested"));
        ASSERT_NE(Property, nullptr);
        EXPECT_EQ(static_cast<FStructProperty*>(Property)->GetStruct(), Current);
    }
}

// An enum deleted or renamed without an alias must stop resolving once a reload leaves it out.
TEST(ScriptClassReload, ReloadWithoutAnEnumRetiresIt)
{
    ScriptTest::PublishEnum("EnumChurn_Dropped", EPropertyTypeFlags::Int32, { { FName("Off"), 0 } });
    // Pinned, since a retired enum nothing else holds is freed by the next reload.
    TStrongObjectPtr<CEnum> Dropped(FindObject<CEnum>(FName("EnumChurn_Dropped")));
    ASSERT_TRUE(Dropped.IsValid());

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        ScriptTest::PublishEnum("EnumChurn_Kept", EPropertyTypeFlags::Int32, { { FName("On"), 1 } });
        EXPECT_EQ(FindObject<CEnum>(FName("EnumChurn_Dropped")), nullptr) << "a dropped enum must not resolve by its old name";
        EXPECT_NE(FindObject<CEnum>(FName("EnumChurn_Kept")), nullptr);
    }
    EXPECT_TRUE(Dropped->GetName().ToString().StartsWith("REPLACED_"));
}

// A superseded struct is freed once no value or field is laid out by it, and kept while one still is.
TEST(ScriptClassReload, SupersededStructIsFreedOnceNothingIsLaidOutByIt)
{
    auto Publish = [](int32 FieldCount)
    {
        Scripting::FScriptExportSchema Item;
        for (int32 Index = 0; Index < FieldCount; ++Index)
        {
            Item.Fields.Add(ScriptTest::ScalarField(*Format("Field{}", Index), EPropertyTypeFlags::Float));
        }
        ScriptTest::PublishStructs({ ScriptTest::StructDefinition("ShapeRetire_Item", std::move(Item)) });
        return FScriptStructRegistry::Get().Find(FName("ShapeRetire_Item"));
    };

    FScriptStructRegistry& Registry = FScriptStructRegistry::Get();
    CScriptStruct* First = Publish(1);
    ASSERT_NE(First, nullptr);
    const int32 Baseline = Registry.NumSuperseded();

    FInstancedStruct Value(First);
    Publish(2);
    EXPECT_EQ(Registry.NumSuperseded(), Baseline + 1) << "a value still laid out by the old shape must keep it";
    Value.Reset();
    Publish(2);
    EXPECT_EQ(Registry.NumSuperseded(), Baseline) << "nothing is laid out by the old shape any more";

    for (int32 Pass = 3; Pass <= 6; ++Pass)
    {
        ASSERT_NE(Publish(Pass), nullptr);
        EXPECT_EQ(Registry.NumSuperseded(), Baseline) << "an unreferenced reshape must not accumulate, pass " << Pass;
    }

    Scripting::FScriptExportSchema Schema;
    TSharedPtr<Scripting::FScriptExportType> Nested = MakeType(EPropertyTypeFlags::Struct);
    Nested->NativeName = FName("ShapeRetire_Item");
    Schema.Fields.Add(MakeField("Nested", Nested));
    uint32 ShimSize = 0;
    ASSERT_NE(MintWithSchema("ScriptReload_ShapeRetire", Schema, ShimSize), nullptr);

    Publish(7);
    Publish(7);
    EXPECT_EQ(Registry.NumSuperseded(), Baseline + 1) << "a class field typed by the old shape must keep it";
}

// A retired enum is freed once no field is typed by it, and kept while one still is.
TEST(ScriptClassReload, RetiredEnumIsFreedOnceNoFieldIsTypedByIt)
{
    FScriptEnumRegistry& Registry = FScriptEnumRegistry::Get();
    ScriptTest::PublishEnum("EnumRetire_Dropped", EPropertyTypeFlags::Int32, { { FName("Off"), 0 } });
    const int32 Baseline = Registry.NumSuperseded();

    ScriptTest::PublishEnum("EnumRetire_Held", EPropertyTypeFlags::Int32, { { FName("On"), 1 } });
    EXPECT_EQ(Registry.NumSuperseded(), Baseline) << "an enum nothing is typed by must go with the reload that dropped it";

    Scripting::FScriptExportSchema Schema;
    TSharedPtr<Scripting::FScriptExportType> Mode = MakeType(EPropertyTypeFlags::Enum);
    Mode->EnumName = FName("EnumRetire_Held");
    Mode->EnumUnderlying = EPropertyTypeFlags::Int32;
    Schema.Fields.Add(MakeField("Mode", Mode));
    uint32 ShimSize = 0;
    ASSERT_NE(MintWithSchema("ScriptReload_EnumRetire", Schema, ShimSize), nullptr);

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        ScriptTest::PublishEnum("EnumRetire_Other", EPropertyTypeFlags::Int32, { { FName("Off"), 0 } });
        EXPECT_EQ(Registry.NumSuperseded(), Baseline + 1) << "a class field typed by the enum must keep it";
    }
}

