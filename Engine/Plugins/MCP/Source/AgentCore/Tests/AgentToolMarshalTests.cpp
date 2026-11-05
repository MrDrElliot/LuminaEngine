#include <gtest/gtest.h>
#include "Core/Reflection/PropertyTypeTraits.h"

#include "Agent/AgentTool.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolSchema.h"
#include "Animation/AnimNotifyDefaults.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Audio/AudioTypes.h"
#include "Scripting/EntityScript.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Containers/ContainerOps.h"
#include "Containers/Optional.h"
#include "Core/Object/PropertyArena.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/StringProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/MeshComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/World.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace AgentToolMarshalTestsInternal
{
    FJsonValue ParseJson(const char* Text)
    {
        FJsonValue Out;
        EXPECT_TRUE(FJsonValue::Parse(Text, Out));
        return Out;
    }

    // Reflection is off for this module, so every fixture is a type the engine already reflects and exports.
    FMarshalResult ReadInto(FStructInstance& Instance, const char* Json)
    {
        return ReadStruct(ParseJson(Json), Instance.GetType(), Instance.Get());
    }
}

using namespace AgentToolMarshalTestsInternal;

TEST(AgentToolMarshal, AScalarIsApplied)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Lifetime":2.5})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_FLOAT_EQ(static_cast<SLifetimeComponent*>(Instance.Get())->Lifetime, 2.5f);
}

TEST(AgentToolMarshal, AStringIsApplied)
{
    FStructInstance Instance(SNameComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Name":"Torch"})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(static_cast<SNameComponent*>(Instance.Get())->Name, FName("Torch"));
}

TEST(AgentToolMarshal, ABoolIsApplied)
{
    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"bCastShadow":false})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_FALSE(static_cast<SMeshComponent*>(Instance.Get())->bCastShadow);
}

// An omitted field is how a caller says "leave it alone", so the constructed default has to survive.
TEST(AgentToolMarshal, AnOmittedFieldKeepsItsDefault)
{
    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const bool Before = static_cast<SMeshComponent*>(Instance.Get())->bReceiveShadow;

    const FMarshalResult Result = ReadInto(Instance, R"({"bCastShadow":false})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(static_cast<SMeshComponent*>(Instance.Get())->bReceiveShadow, Before);
}

TEST(AgentToolMarshal, AnEmptyObjectLeavesEverythingAtItsDefault)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, "{}");
    EXPECT_TRUE(Result.IsValid()) << *Result.Error;
}

TEST(AgentToolMarshal, ANestedStructIsApplied)
{
    FStructInstance Instance(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"bEnabled":true,"LowPassFrequency":900.0})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;

    const SAudioOcclusion* Typed = static_cast<SAudioOcclusion*>(Instance.Get());
    EXPECT_TRUE(Typed->bEnabled);
    EXPECT_FLOAT_EQ(Typed->LowPassFrequency, 900.0f);
}

// A misspelled field would otherwise be dropped, leaving the caller sure it had been applied.
TEST(AgentToolMarshal, AnUnknownFieldIsRefusedByName)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Lifetim":2.5})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Lifetim"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AWrongTypeIsRefusedByPath)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Lifetime":"soon"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Lifetime"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, ANonObjectIsRefused)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, "[1,2,3]");
    EXPECT_FALSE(Result.IsValid());
}

TEST(AgentToolMarshal, ANullInstanceIsRefused)
{
    const FMarshalResult Result = ReadStruct(FJsonValue::MakeObject(), nullptr, nullptr);
    EXPECT_FALSE(Result.IsValid());
}

// A float field must not silently swallow a string, which is what the raw archive would do.
TEST(AgentToolMarshal, AWrongTypeInANestedStructNamesTheWholePath)
{
    FStructInstance Instance(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"LowPassFrequency":true})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("LowPassFrequency"), Constants::kIndexNone);
}

// An invalid enum name would otherwise resolve to whatever the lookup falls back to.
TEST(AgentToolMarshal, AnUnknownEnumNameIsRefusedAndListsTheChoices)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Bus":"NotARealBus"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Bus"), Constants::kIndexNone);
    EXPECT_NE(Result.Error.Find("SFX"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AValidEnumNameIsApplied)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Bus":"Music"})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(static_cast<SAudioSourceComponent*>(Instance.Get())->Bus, EAudioBus::Music);
}

// The marshaller has to refuse exactly what the schema refuses, or the two halves drift apart.
TEST(AgentToolMarshal, AnUnsupportedFieldIsRefusedEvenWhenSupplied)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":"/Game/Audio/Beep"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Sound"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, WritingProducesTheFieldsBack)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    static_cast<SLifetimeComponent*>(Instance.Get())->Lifetime = 7.25f;

    FJsonValue Out;
    const FMarshalResult Result = WriteStruct(Instance.GetType(), Instance.Get(), Out);

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    ASSERT_TRUE(Out.Contains("Lifetime"));
    EXPECT_FLOAT_EQ(Out["Lifetime"].AsFloat(), 7.25f);
}

// Read then write then read has to land on the same values, or a result cannot be trusted.
TEST(AgentToolMarshal, AStructSurvivesARoundTrip)
{
    FStructInstance First(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(First.IsValid());

    ASSERT_TRUE(ReadInto(First, R"({"bEnabled":true,"LowPassFrequency":1234.0,"InterpTime":0.75})").IsValid());

    FJsonValue Written;
    ASSERT_TRUE(WriteStruct(First.GetType(), First.Get(), Written).IsValid());

    FStructInstance Second(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(Second.IsValid());
    ASSERT_TRUE(ReadStruct(Written, Second.GetType(), Second.Get()).IsValid());

    const SAudioOcclusion* A = static_cast<SAudioOcclusion*>(First.Get());
    const SAudioOcclusion* B = static_cast<SAudioOcclusion*>(Second.Get());

    EXPECT_EQ(A->bEnabled, B->bEnabled);
    EXPECT_FLOAT_EQ(A->LowPassFrequency, B->LowPassFrequency);
    EXPECT_FLOAT_EQ(A->InterpTime, B->InterpTime);
}

TEST(AgentToolMarshal, WritingANullInstanceIsRefused)
{
    FJsonValue Out;
    const FMarshalResult Result = WriteStruct(nullptr, nullptr, Out);

    EXPECT_FALSE(Result.IsValid());
}

// An object reference travels as its asset GUID, so a caller can look the asset back up.
TEST(AgentToolMarshal, AnObjectReferenceIsSchemaExpressible)
{
    const FSchemaResult Result = GeneratePropertySchema(
        SAudioSourceComponent::StaticStruct()->GetProperty(FName("Sound")));

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "string");

    ASSERT_TRUE(Result.Schema.Contains("description"));
    const FStringView Description = Result.Schema["description"].AsString();
    EXPECT_NE(Description.Find("GUID"), Constants::kIndexNone);
    EXPECT_NE(Description.Find("CSoundBase"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AStructHoldingAnObjectReferenceIsNowDescribable)
{
    const FSchemaResult Result = GenerateSchema(SAudioSourceComponent::StaticStruct());

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_TRUE(Result.Schema["properties"].Contains("Sound"));
}

// An empty string is how a caller clears a reference, and it must not be read as a bad GUID.
TEST(AgentToolMarshal, AnEmptyObjectReferenceClearsIt)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":""})");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(static_cast<SAudioSourceComponent*>(Instance.Get())->Sound.Get(), nullptr);
}

TEST(AgentToolMarshal, AMalformedGuidIsRefusedByPath)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":"not-a-guid"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Sound"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, ANonStringObjectReferenceIsRefused)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":12345})");
    EXPECT_FALSE(Result.IsValid());
}

// A well formed GUID naming nothing has to say so rather than silently leaving the reference null.
TEST(AgentToolMarshal, AnUnresolvableGuidIsReported)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance,
        R"({"Sound":"1D9B7A44-0000-4000-8000-000000000001"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Sound"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, ANullReferenceWritesAsAnEmptyString)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    FJsonValue Out;
    ASSERT_TRUE(WriteStruct(Instance.GetType(), Instance.Get(), Out).IsValid());

    ASSERT_TRUE(Out.Contains("Sound"));
    ASSERT_TRUE(Out["Sound"].IsString());
    EXPECT_TRUE(Out["Sound"].AsString().IsEmpty());
}

// An array of references is the common case for material overrides, so it travels element by element.
TEST(AgentToolMarshal, AnArrayOfObjectReferencesIsDescribableAndEmptyByDefault)
{
    const FSchemaResult Schema = GeneratePropertySchema(
        SMeshComponent::StaticStruct()->GetProperty(FName("MaterialOverrides")));

    ASSERT_TRUE(Schema.IsValid()) << *Schema.Error;
    EXPECT_EQ(Schema.Schema["type"], "array");
    EXPECT_EQ(Schema.Schema["items"]["type"], "string");

    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"MaterialOverrides":[]})");
    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(static_cast<SMeshComponent*>(Instance.Get())->MaterialOverrides.Num(), 0u);
}

TEST(AgentToolMarshal, AMalformedGuidInsideAnArrayNamesItsIndex)
{
    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"MaterialOverrides":["","nope"]})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("[1]"), Constants::kIndexNone);
}

// Container and reference kinds, built from descriptors the way generated code builds them.

namespace AgentToolMarshalTestsInternal
{
    constexpr FPropertyDesc GNameToIntMap{ .Name = "Table", .Type = PropertyTypeOf<THashMap<FName, int32>> };
    constexpr FPropertyDesc GOptionalFloat{ .Name = "Regen", .Type = PropertyTypeOf<TOptional<float>> };
    constexpr FPropertyDesc GNotifySubStruct{ .Name = "NotifyType", .Type = PropertyTypeOf<TSubStructOf<SAnimNotify>> };
    constexpr FPropertyDesc GStartupMap{ .Name = "GameStartupMap", .Type = PropertyTypeOf<TSoftObjectPtr<CWorld>> };
    constexpr FPropertyDesc GScriptClass{ .Name = "ScriptClass", .Type = PropertyTypeOf<TSubclassOf<CEntityScript>> };

    struct FContainerFixtures
    {
        FPropertyArena      Arena;
        TVector<FProperty*> Collected;

        template<typename TProperty>
        TProperty* Build(const FPropertyDesc& Desc)
        {
            return static_cast<TProperty*>(ConstructProperty(FPropertyOwner{ &Arena, nullptr, nullptr, &Collected }, Desc));
        }

        FMapProperty* NameToIntMap()              { return Build<FMapProperty>(GNameToIntMap); }
        FOptionalProperty* OptionalFloat()        { return Build<FOptionalProperty>(GOptionalFloat); }
        FSubStructProperty* NotifySubStruct()     { return Build<FSubStructProperty>(GNotifySubStruct); }
        FSoftObjectProperty* StartupMap()         { return Build<FSoftObjectProperty>(GStartupMap); }
        FClassProperty* ScriptClass()             { return Build<FClassProperty>(GScriptClass); }
    };
}

using namespace AgentToolMarshalTestsInternal;

TEST(AgentToolMarshal, ASoftObjectPathIsAppliedAndWrittenBack)
{
    FContainerFixtures Fixtures;
    FSoftObjectProperty* Property = Fixtures.StartupMap();

    FSoftObjectPath Path;
    const FMarshalResult Read = ReadProperty("/Game/Content/Maps/GameWorld.lasset", Property, &Path, "GameStartupMap");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    EXPECT_EQ(Path.GetPath(), FStringView("/Game/Content/Maps/GameWorld.lasset"));

    FJsonValue Out;
    ASSERT_TRUE(WriteProperty(Property, &Path, Out).IsValid());
    EXPECT_EQ(Out, "/Game/Content/Maps/GameWorld.lasset");
}

TEST(AgentToolMarshal, ASoftObjectRefusesANonString)
{
    FContainerFixtures Fixtures;
    FSoftObjectProperty* Property = Fixtures.StartupMap();

    const FMarshalResult Result = ValidatePropertyValue(42, Property, "GameStartupMap");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("GameStartupMap"), Constants::kIndexNone);
}

// A GUID that names nothing must say so, since the path form would otherwise be stored as-is.
TEST(AgentToolMarshal, ASoftObjectGuidNamingNothingIsReported)
{
    FContainerFixtures Fixtures;
    FSoftObjectProperty* Property = Fixtures.StartupMap();

    const FMarshalResult Result = ValidatePropertyValue("1D9B7A44-0000-4000-8000-000000000001", Property, "GameStartupMap");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("GUID"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AClassIsResolvedByName)
{
    // The lookup goes by name, so the class has to exist before it runs.
    StaticClass<CEntityScript>();

    FContainerFixtures Fixtures;
    FClassProperty* Property = Fixtures.ScriptClass();

    CClass* Value = nullptr;
    const FMarshalResult Read = ReadProperty("CEntityScript", Property, &Value, "ScriptClass");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    ASSERT_NE(Value, nullptr);
    EXPECT_EQ(Value->GetName(), FName("CEntityScript"));

    FJsonValue Out;
    ASSERT_TRUE(WriteProperty(Property, &Value, Out).IsValid());
    EXPECT_EQ(Out, "CEntityScript");
}

TEST(AgentToolMarshal, AClassOutsideTheConstraintIsRefused)
{
    StaticClass<CEntityScript>();
    StaticClass<CWorld>();

    FContainerFixtures Fixtures;
    FClassProperty* Property = Fixtures.ScriptClass();

    const FMarshalResult Result = ValidatePropertyValue("CWorld", Property, "ScriptClass");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("subclass"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AnUnknownClassNameIsRefused)
{
    FContainerFixtures Fixtures;
    FClassProperty* Property = Fixtures.ScriptClass();

    const FMarshalResult Result = ValidatePropertyValue("CNoSuchClass", Property, "ScriptClass");
    EXPECT_FALSE(Result.IsValid());
}

TEST(AgentToolMarshal, ASubStructIsResolvedByName)
{
    // Reflected types register on first use, and nothing else in this process has asked for this one yet.
    SAnimNotify_Log::StaticStruct();

    FContainerFixtures Fixtures;
    FSubStructProperty* Property = Fixtures.NotifySubStruct();

    CStruct* Value = nullptr;
    const FMarshalResult Read = ReadProperty("SAnimNotify_Log", Property, &Value, "NotifyType");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    EXPECT_EQ(Value, SAnimNotify_Log::StaticStruct());
}

TEST(AgentToolMarshal, ASubStructOutsideTheConstraintIsRefused)
{
    SLifetimeComponent::StaticStruct();

    FContainerFixtures Fixtures;
    FSubStructProperty* Property = Fixtures.NotifySubStruct();

    const FMarshalResult Result = ValidatePropertyValue("SLifetimeComponent", Property, "NotifyType");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("derive"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AnOptionalTakesNullAsReset)
{
    FContainerFixtures Fixtures;
    FOptionalProperty* Property = Fixtures.OptionalFloat();

    TOptional<float> Value(1.0f);
    const FMarshalResult Read = ReadProperty(nullptr, Property, &Value, "Regen");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    EXPECT_FALSE(Value.IsSet());
}

TEST(AgentToolMarshal, AnOptionalTakesABareValueAsEngaged)
{
    FContainerFixtures Fixtures;
    FOptionalProperty* Property = Fixtures.OptionalFloat();

    TOptional<float> Value;
    const FMarshalResult Read = ReadProperty(2.5, Property, &Value, "Regen");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    ASSERT_TRUE(Value.IsSet());
    EXPECT_FLOAT_EQ(*Value, 2.5f);
}

TEST(AgentToolMarshal, AnOptionalRoundTripsItsRecordForm)
{
    FContainerFixtures Fixtures;
    FOptionalProperty* Property = Fixtures.OptionalFloat();

    TOptional<float> Value;
    const FMarshalResult Read = ReadProperty(ParseJson(R"({"Engaged":true,"Value":3.0})"), Property, &Value, "Regen");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    ASSERT_TRUE(Value.IsSet());
    EXPECT_FLOAT_EQ(*Value, 3.0f);

    FJsonValue Out;
    ASSERT_TRUE(WriteProperty(Property, &Value, Out).IsValid());
    EXPECT_EQ(Out["Engaged"], true);
    EXPECT_FLOAT_EQ(Out["Value"].AsFloat(), 3.0f);
}

TEST(AgentToolMarshal, AnOptionalWithAWrongInnerTypeNamesThePath)
{
    FContainerFixtures Fixtures;
    FOptionalProperty* Property = Fixtures.OptionalFloat();

    const FMarshalResult Result = ValidatePropertyValue("soon", Property, "Regen");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Regen.Value"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AMapTakesAnObject)
{
    FContainerFixtures Fixtures;
    FMapProperty* Property = Fixtures.NameToIntMap();

    THashMap<FName, int32> Value;
    const FMarshalResult Read = ReadProperty(ParseJson(R"({"A":1,"B":2})"), Property, &Value, "Table");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    ASSERT_EQ(Value.Num(), 2u);
    EXPECT_EQ(Value[FName("A")], 1);
    EXPECT_EQ(Value[FName("B")], 2);
}

TEST(AgentToolMarshal, AMapTakesAFlatArrayAndWritesOneBack)
{
    FContainerFixtures Fixtures;
    FMapProperty* Property = Fixtures.NameToIntMap();

    THashMap<FName, int32> Value;
    const FMarshalResult Read = ReadProperty(ParseJson(R"(["C",3])"), Property, &Value, "Table");
    ASSERT_TRUE(Read.IsValid()) << *Read.Error;
    ASSERT_EQ(Value.Num(), 1u);
    EXPECT_EQ(Value[FName("C")], 3);

    FJsonValue Out;
    ASSERT_TRUE(WriteProperty(Property, &Value, Out).IsValid());
    ASSERT_TRUE(Out.IsArray());
    ASSERT_EQ(Out.Num(), 2);
    EXPECT_EQ(Out[0], "C");
    EXPECT_EQ(Out[1], 3);
}

TEST(AgentToolMarshal, AMapWithAnOddArrayIsRefused)
{
    FContainerFixtures Fixtures;
    FMapProperty* Property = Fixtures.NameToIntMap();

    const FMarshalResult Result = ValidatePropertyValue(ParseJson(R"(["C"])"), Property, "Table");
    EXPECT_FALSE(Result.IsValid());
}

TEST(AgentToolMarshal, AMapWithAWrongValueTypeNamesItsIndex)
{
    FContainerFixtures Fixtures;
    FMapProperty* Property = Fixtures.NameToIntMap();

    const FMarshalResult Result = ValidatePropertyValue(ParseJson(R"({"A":"one"})"), Property, "Table");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Table[1]"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, AnInstancedStructIsAppliedByTypeName)
{
    SAnimNotify_Log::StaticStruct();

    FStructInstance Instance(SAnimMontageNotify::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance,
        R"({"Notify":{"StructType":"SAnimNotify_Log","Data":{"Message":"hello"}}})");
    ASSERT_TRUE(Result.IsValid()) << *Result.Error;

    const SAnimMontageNotify* Typed = static_cast<SAnimMontageNotify*>(Instance.Get());
    ASSERT_EQ(Typed->Notify.GetScriptStruct(), SAnimNotify_Log::StaticStruct());
    EXPECT_EQ(static_cast<const SAnimNotify_Log*>(Typed->Notify.GetMemory())->Message, FString("hello"));

    FJsonValue Out;
    ASSERT_TRUE(WriteStruct(Instance.GetType(), Instance.Get(), Out).IsValid());
    EXPECT_EQ(Out["Notify"]["StructType"], "SAnimNotify_Log");
    EXPECT_EQ(Out["Notify"]["Data"]["Message"], "hello");
}

TEST(AgentToolMarshal, AnInstancedStructWithAnUnknownTypeIsRefused)
{
    FStructInstance Instance(SAnimMontageNotify::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Notify":{"StructType":"SNoSuchNotify","Data":{}}})");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Notify"), Constants::kIndexNone);
}

// The struct the type picks is validated field by field, so a typo inside Data is caught before the archive.
TEST(AgentToolMarshal, AnInstancedStructValidatesItsDataFields)
{
    SAnimNotify_Log::StaticStruct();

    FStructInstance Instance(SAnimMontageNotify::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance,
        R"({"Notify":{"StructType":"SAnimNotify_Log","Data":{"Mesage":"typo"}}})");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("Notify.Data.Mesage"), Constants::kIndexNone);
}

TEST(AgentToolMarshal, ADelegateIsRefusedAsNotSettable)
{
    FProperty* Property = SRigidBodyComponent::StaticStruct()->GetProperty(FName("OnWake"));
    ASSERT_NE(Property, nullptr);

    const FMarshalResult Result = ValidatePropertyValue(FJsonValue::MakeObject(), Property, "OnWake");
    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("delegate"), Constants::kIndexNone);
}
