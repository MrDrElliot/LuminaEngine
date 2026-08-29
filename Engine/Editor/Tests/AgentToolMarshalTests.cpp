#include <gtest/gtest.h>

#include "Agent/AgentTool.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolSchema.h"
#include "Audio/AudioTypes.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/MeshComponent.h"
#include "World/Entity/Components/NameComponent.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace
{
    // Reflection is off for this module, so every fixture is a type the engine already reflects and exports.
    FMarshalResult ReadInto(FStructInstance& Instance, const char* Json)
    {
        return ReadStruct(nlohmann::json::parse(Json), Instance.GetType(), Instance.Get());
    }
}

TEST(AgentToolMarshal, AScalarIsApplied)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Lifetime":2.5})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_FLOAT_EQ(static_cast<SLifetimeComponent*>(Instance.Get())->Lifetime, 2.5f);
}

TEST(AgentToolMarshal, AStringIsApplied)
{
    FStructInstance Instance(SNameComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Name":"Torch"})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(static_cast<SNameComponent*>(Instance.Get())->Name, FName("Torch"));
}

TEST(AgentToolMarshal, ABoolIsApplied)
{
    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"bCastShadow":false})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_FALSE(static_cast<SMeshComponent*>(Instance.Get())->bCastShadow);
}

// An omitted field is how a caller says "leave it alone", so the constructed default has to survive.
TEST(AgentToolMarshal, AnOmittedFieldKeepsItsDefault)
{
    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const bool Before = static_cast<SMeshComponent*>(Instance.Get())->bReceiveShadow;

    const FMarshalResult Result = ReadInto(Instance, R"({"bCastShadow":false})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(static_cast<SMeshComponent*>(Instance.Get())->bReceiveShadow, Before);
}

TEST(AgentToolMarshal, AnEmptyObjectLeavesEverythingAtItsDefault)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, "{}");
    EXPECT_TRUE(Result.IsValid()) << Result.Error.c_str();
}

TEST(AgentToolMarshal, ANestedStructIsApplied)
{
    FStructInstance Instance(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"bEnabled":true,"LowPassFrequency":900.0})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();

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
    EXPECT_NE(Result.Error.find("Lifetim"), FString::npos);
}

TEST(AgentToolMarshal, AWrongTypeIsRefusedByPath)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Lifetime":"soon"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("Lifetime"), FString::npos);
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
    const FMarshalResult Result = ReadStruct(nlohmann::json::object(), nullptr, nullptr);
    EXPECT_FALSE(Result.IsValid());
}

// A float field must not silently swallow a string, which is what the raw archive would do.
TEST(AgentToolMarshal, AWrongTypeInANestedStructNamesTheWholePath)
{
    FStructInstance Instance(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"LowPassFrequency":true})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("LowPassFrequency"), FString::npos);
}

// An invalid enum name would otherwise resolve to whatever the lookup falls back to.
TEST(AgentToolMarshal, AnUnknownEnumNameIsRefusedAndListsTheChoices)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Bus":"NotARealBus"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("Bus"), FString::npos);
    EXPECT_NE(Result.Error.find("SFX"), FString::npos);
}

TEST(AgentToolMarshal, AValidEnumNameIsApplied)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Bus":"Music"})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(static_cast<SAudioSourceComponent*>(Instance.Get())->Bus, EAudioBus::Music);
}

// The marshaller has to refuse exactly what the schema refuses, or the two halves drift apart.
TEST(AgentToolMarshal, AnUnsupportedFieldIsRefusedEvenWhenSupplied)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":"/Game/Audio/Beep"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("Sound"), FString::npos);
}

TEST(AgentToolMarshal, WritingProducesTheFieldsBack)
{
    FStructInstance Instance(SLifetimeComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    static_cast<SLifetimeComponent*>(Instance.Get())->Lifetime = 7.25f;

    nlohmann::json Out;
    const FMarshalResult Result = WriteStruct(Instance.GetType(), Instance.Get(), Out);

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    ASSERT_TRUE(Out.contains("Lifetime"));
    EXPECT_FLOAT_EQ(Out["Lifetime"].get<float>(), 7.25f);
}

// Read then write then read has to land on the same values, or a result cannot be trusted.
TEST(AgentToolMarshal, AStructSurvivesARoundTrip)
{
    FStructInstance First(SAudioOcclusion::StaticStruct());
    ASSERT_TRUE(First.IsValid());

    ASSERT_TRUE(ReadInto(First, R"({"bEnabled":true,"LowPassFrequency":1234.0,"InterpTime":0.75})").IsValid());

    nlohmann::json Written;
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
    nlohmann::json Out;
    const FMarshalResult Result = WriteStruct(nullptr, nullptr, Out);

    EXPECT_FALSE(Result.IsValid());
}

// An object reference travels as its asset GUID, so a caller can look the asset back up.
TEST(AgentToolMarshal, AnObjectReferenceIsSchemaExpressible)
{
    const FSchemaResult Result = GeneratePropertySchema(
        SAudioSourceComponent::StaticStruct()->GetProperty(FName("Sound")));

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "string");

    ASSERT_TRUE(Result.Schema.contains("description"));
    const std::string Description = Result.Schema["description"].get<std::string>();
    EXPECT_NE(Description.find("GUID"), std::string::npos);
    EXPECT_NE(Description.find("CSoundBase"), std::string::npos);
}

TEST(AgentToolMarshal, AStructHoldingAnObjectReferenceIsNowDescribable)
{
    const FSchemaResult Result = GenerateSchema(SAudioSourceComponent::StaticStruct());

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_TRUE(Result.Schema["properties"].contains("Sound"));
}

// An empty string is how a caller clears a reference, and it must not be read as a bad GUID.
TEST(AgentToolMarshal, AnEmptyObjectReferenceClearsIt)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":""})");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(static_cast<SAudioSourceComponent*>(Instance.Get())->Sound.Get(), nullptr);
}

TEST(AgentToolMarshal, AMalformedGuidIsRefusedByPath)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"Sound":"not-a-guid"})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("Sound"), FString::npos);
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
    EXPECT_NE(Result.Error.find("Sound"), FString::npos);
}

TEST(AgentToolMarshal, ANullReferenceWritesAsAnEmptyString)
{
    FStructInstance Instance(SAudioSourceComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    nlohmann::json Out;
    ASSERT_TRUE(WriteStruct(Instance.GetType(), Instance.Get(), Out).IsValid());

    ASSERT_TRUE(Out.contains("Sound"));
    ASSERT_TRUE(Out["Sound"].is_string());
    EXPECT_TRUE(Out["Sound"].get<std::string>().empty());
}

// An array of references is the common case for material overrides, so it travels element by element.
TEST(AgentToolMarshal, AnArrayOfObjectReferencesIsDescribableAndEmptyByDefault)
{
    const FSchemaResult Schema = GeneratePropertySchema(
        SMeshComponent::StaticStruct()->GetProperty(FName("MaterialOverrides")));

    ASSERT_TRUE(Schema.IsValid()) << Schema.Error.c_str();
    EXPECT_EQ(Schema.Schema["type"], "array");
    EXPECT_EQ(Schema.Schema["items"]["type"], "string");

    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"MaterialOverrides":[]})");
    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(static_cast<SMeshComponent*>(Instance.Get())->MaterialOverrides.size(), 0u);
}

TEST(AgentToolMarshal, AMalformedGuidInsideAnArrayNamesItsIndex)
{
    FStructInstance Instance(SMeshComponent::StaticStruct());
    ASSERT_TRUE(Instance.IsValid());

    const FMarshalResult Result = ReadInto(Instance, R"({"MaterialOverrides":["","nope"]})");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("[1]"), FString::npos);
}
