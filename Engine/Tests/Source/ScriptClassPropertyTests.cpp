#include <gtest/gtest.h>

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
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

// Script properties on a runtime-minted CClass: the C#-facing half of the rewrite
// (Docs/CSharpScriptingRewrite.md, Pillar 3).
//
// The premise these pin: a minted CClass declares a LARGER size than the C++ shim it was minted from and
// carries real FPropertys in the trailing block. That is what lets a script's properties be drawn by the
// stock FPropertyTable, carried by SerializeTaggedProperties, and covered by undo / prefab overrides /
// replication -- instead of a parallel minted CStruct fed by a value blob.
//
// Two facts they lean on, both verified in the engine rather than assumed:
//   * StaticAllocateObject sizes BOTH the allocation and the memzero from Class->GetSize() (ObjectCore.cpp),
//     so bumping Size before the CDO exists makes every instance carry the trailing block, zeroed.
//   * CClass::CreateDefaultObject calls Link() itself, and CStruct::Link chains the super's property list
//     onto this class's -- and latches. So properties must be appended BEFORE the CDO is created, and the
//     super's own registration must already have been processed or its properties are lost permanently.

// The schema is built by hand in these tests because a live .NET host is what normally produces it; the code
// under test is identical either way.

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

        // Drain deferred registration FIRST. CStruct::Link chains the super's property list onto ours, but
        // only if the super already has one -- and a compiled-in class gets its properties when its deferred
        // registration is processed. Link latches (bLinked), so linking against an unregistered base loses
        // the base's properties permanently. In the live engine registration is long done by the time a
        // script class mints; in a test process it is not.
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

    // Zero-initialized by StaticAllocateObject's memzero over the enlarged size -- this is exactly why the
    // first increment is restricted to trivially-constructible fields.
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

// Storage-owning kinds are appended like any other, because the value lifecycle is the PROPERTY's
// (ConstructValue / DestructValue / OwnsStorage) and the class just drives whichever of its appended
// properties said they own storage. An FString field over memzeroed bytes would read as a plausible empty
// string and corrupt on the first assignment, so "constructed, not just zeroed" is the whole contract here.
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

    // Constructed, so assigning is safe: over memzeroed bytes this is where an FString corrupts the heap.
    EXPECT_TRUE(Label->GetValuePtr<FString>(First)->empty());
    *Label->GetValuePtr<FString>(First) = "a string long enough to have heap storage rather than SSO";
    *Label->GetValuePtr<FString>(Second) = "another";

    EXPECT_STREQ(Label->GetValuePtr<FString>(First)->c_str(), "a string long enough to have heap storage rather than SSO");
    EXPECT_STREQ(Label->GetValuePtr<FString>(Second)->c_str(), "another") << "instances share storage";

    // The shim half is untouched.
    ASSERT_NE(Cast<CScriptableTest>(First), nullptr);
    EXPECT_FLOAT_EQ(Cast<CScriptableTest>(First)->NativeValue, 1.5f);
}

// Strings round-trip through the stock tagged serializer, same as the scalars: nothing about the appended
// block is special-cased in the serializer.
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

// A script class has no C++ constructor, so its declared defaults are written once to the class default
// object and every instance is copied from it inside ConstructScriptProperties. The write itself comes from
// managed code replaying the C# initializers (DotNet::ApplyScriptableDefaults); what is pinned here is the
// half that has to hold regardless of who wrote them -- CDO to instance, by value, for storage-owning kinds
// as much as for memcpy-able ones.
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

    // The CDO's own trailing block is in-bounds and constructed -- if Size were bumped after the CDO was
    // created, this write would be past the end of its allocation.
    Speed->SetValue<float>(Cdo, 6.5f);
    *Label->GetValuePtr<FString>(Cdo) = "declared default";

    CObject* Instance = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(Instance), 6.5f) << "instance was not seeded from the CDO";
    EXPECT_STREQ(Label->GetValuePtr<FString>(Instance)->c_str(), "declared default");

    // Seeded by value, not shared: writing the instance leaves the CDO alone.
    *Label->GetValuePtr<FString>(Instance) = "mutated";
    EXPECT_STREQ(Label->GetValuePtr<FString>(Cdo)->c_str(), "declared default");
}

//=============================================================================================================
// Every reflected property kind, appended to a minted class. This is the claim the whole design rests on: the
// class-append path plans layouts with the SAME code that plans a CScriptStruct, and drives value lifetime
// through FProperty::ConstructValue / DestructValue / OwnsStorage. So "which kinds are supported" is not a
// list anyone maintains -- it is whatever the property types themselves implement.
//=============================================================================================================

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

        // Enum: minted from the entries the C# type reported.
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

        // Script struct: a nested C#-declared struct, minted as its own CScriptStruct.
        {
            TSharedPtr<FScriptExportType> Type = MakeType(EPropertyTypeFlags::Struct);
            Type->Fields.push_back(MakeScalarField("Inner", EPropertyTypeFlags::Int32));
            Type->Fields.push_back(MakeScalarField("InnerText", EPropertyTypeFlags::String));
            Schema.Fields.push_back(MakeField("AScriptStruct", Type));
        }

        // Array of a storage-owning element, which is the case that needs the element description wired
        // into the container by ConstructValue.
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

        // Instanced struct: an empty base plus one selectable candidate.
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
        // Distinct addresses: each instance carries its own value, not one aliased buffer.
        EXPECT_NE(Property->GetValuePtr<uint8>(First), Property->GetValuePtr<uint8>(Second))
            << Name << " shares storage between instances";
    }

    // The shim half survived every append.
    CScriptableTest* Typed = Cast<CScriptableTest>(First);
    ASSERT_NE(Typed, nullptr);
    EXPECT_FLOAT_EQ(Typed->NativeValue, 1.5f);
    EXPECT_EQ(Typed->OnTest(5), 10);
}

// The containers are the kinds that need more than placement-new: the element/pair description has to be
// wired into the freshly-constructed container or every later op is a null deref. Mutating one through the
// stock FArrayProperty / FMapProperty API is what proves that happened.
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

// Containers and strings survive the stock tagged serializer with no bespoke codec, which is the entire
// reason for appending real FPropertys rather than keeping a parallel value blob.
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

//~ ---------------------------------------------------------------------------------------------------------
//~ Hot reload: rebuilding the appended block when a C# reload changes the property set.
//~
//~ A minted class is reused by name across reloads and keeps its identity, but StaticAllocateObject bakes
//~ Class->GetSize() into every object at creation. So a changed property set cannot be patched in place, and
//~ MigrateMintedClassLayout tears the block down and rebuilds it instead.
//~ ---------------------------------------------------------------------------------------------------------

TEST(ScriptClassReload, AnUnchangedSchemaNeedsNoRebuild)
{
    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Unchanged", Schema, ShimSize);
    ASSERT_NE(Sub, nullptr);

    // The common reload edits a method body. An identical schema must compare equal, or every reload would
    // pay a rebuild and drop every instance for nothing.
    Scripting::FScriptExportSchema Same;
    Same.Fields.push_back(MakeScalarField("Speed", EPropertyTypeFlags::Float));
    EXPECT_TRUE(Scripting::ScriptClassLayoutMatches(Sub, Same));

    // Metadata is not layout: retitling a field must not force a rebuild either.
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

    // The super's chain was spliced on by the first Link and dropped by Unlink; re-linking has to restore it,
    // or every native member of the base silently disappears from the class.
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
    // The user-facing promise: a reload that adds a field keeps what you authored in the others. The carrier
    // is SerializeTaggedProperties, which is name-keyed, so a value whose property still exists replays, a
    // removed one is dropped, and an added one lands on its default.
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

    // Evacuate exactly the way a live reload must: tagged properties into a buffer.
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

    // Added after the snapshot was taken, so it is simply at its default rather than reading someone
    // else's bytes -- which is the whole reason the carrier is name-keyed and not a raw blob.
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

    // An instance is laid out at the old size, so rebuilding under it would move its fields. Refusing is the
    // contract; the caller evacuates first.
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
    // Containers are the kinds with side data (element descriptions, ops tables) owned by the layout record,
    // so they are what a teardown gets wrong first: the record is retired, not freed, and the new block
    // builds its own.
    Scripting::FScriptExportSchema Before;
    TSharedPtr<Scripting::FScriptExportType> List = MakeType(EPropertyTypeFlags::Vector);
    List->ElementType = MakeType(EPropertyTypeFlags::Int32);
    Before.Fields.push_back(MakeField("Values", List));

    uint32 ShimSize = 0;
    CClass* Sub = MintWithSchema("ScriptReload_Containers", Before, ShimSize);
    ASSERT_NE(Sub, nullptr);
    ASSERT_NE(Sub->GetProperty(FName("Values")), nullptr);

    // Change the ELEMENT type only. The top-level kind is Vector either way, so this is exactly the case a
    // shallow comparison would miss and then lay out wrong.
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

    // Usable through the stock API, which is what proves the new element description was wired into the
    // freshly constructed container rather than left null.
    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    void* Array = Values->GetValuePtr<void>(Object);
    ASSERT_NE(Array, nullptr);
    EXPECT_EQ(Values->GetOps()->Size(Array), 0u);
    const float One = 1.0f;
    Values->GetOps()->PushBack(Array, &One);
    EXPECT_EQ(Values->GetOps()->Size(Array), 1u);
}
