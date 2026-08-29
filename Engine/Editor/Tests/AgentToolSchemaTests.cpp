#include <gtest/gtest.h>

#include "Agent/AgentToolSchema.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/MeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/NameComponent.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace
{
    // Reflection is off for this module, so every fixture is a type the engine already reflects and exports.
    FSchemaResult SchemaOfProperty(CStruct* Struct, const char* PropertyName)
    {
        return GeneratePropertySchema(Struct->GetProperty(FName(PropertyName)));
    }
}

TEST(AgentToolSchema, ANullStructIsRejected)
{
    const FSchemaResult Result = GenerateSchema(nullptr);

    EXPECT_FALSE(Result.IsValid());
    EXPECT_FALSE(Result.Error.empty());
}

TEST(AgentToolSchema, ANullPropertyIsRejected)
{
    const FSchemaResult Result = GeneratePropertySchema(nullptr);
    EXPECT_FALSE(Result.IsValid());
}

TEST(AgentToolSchema, AStructBecomesAnObjectWithProperties)
{
    const FSchemaResult Result = GenerateSchema(SLifetimeComponent::StaticStruct());

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "object");
    ASSERT_TRUE(Result.Schema.contains("properties"));
    EXPECT_TRUE(Result.Schema["properties"].contains("Lifetime"));
}

TEST(AgentToolSchema, AFloatBecomesANumber)
{
    const FSchemaResult Result = SchemaOfProperty(SLifetimeComponent::StaticStruct(), "Lifetime");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "number");
}

TEST(AgentToolSchema, ABoolBecomesABoolean)
{
    const FSchemaResult Result = SchemaOfProperty(SMeshComponent::StaticStruct(), "bCastShadow");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "boolean");
}

TEST(AgentToolSchema, ANameBecomesAString)
{
    const FSchemaResult Result = SchemaOfProperty(SNameComponent::StaticStruct(), "Name");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "string");
}

// A doc comment above a PROPERTY lands in ToolTip, which is what documents the field to a model.
TEST(AgentToolSchema, ADocCommentBecomesTheDescription)
{
    const FSchemaResult Result = SchemaOfProperty(SNameComponent::StaticStruct(), "Name");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    ASSERT_TRUE(Result.Schema.contains("description"));
    EXPECT_FALSE(Result.Schema["description"].get<std::string>().empty());
}

TEST(AgentToolSchema, AnEnumBecomesAConstrainedString)
{
    const FSchemaResult Result = SchemaOfProperty(SAudioSourceComponent::StaticStruct(), "Bus");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "string");

    ASSERT_TRUE(Result.Schema.contains("enum"));
    ASSERT_TRUE(Result.Schema["enum"].is_array());

    bool bFoundKnownValue = false;
    for (const nlohmann::json& Name : Result.Schema["enum"])
    {
        bFoundKnownValue = bFoundKnownValue || Name == "SFX";
    }

    EXPECT_TRUE(bFoundKnownValue);
}

TEST(AgentToolSchema, ANestedStructBecomesANestedObject)
{
    const FSchemaResult Result = SchemaOfProperty(SAudioSourceComponent::StaticStruct(), "Occlusion");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "object");

    // The nesting has to reach real leaves rather than stop at an empty object.
    ASSERT_TRUE(Result.Schema.contains("properties"));
    EXPECT_FALSE(Result.Schema["properties"].empty());
}

// An object reference travels as its asset GUID, so the schema has to say which class is wanted.
TEST(AgentToolSchema, AnObjectReferenceBecomesAGuidString)
{
    const FSchemaResult Result = SchemaOfProperty(SAudioSourceComponent::StaticStruct(), "Sound");

    ASSERT_TRUE(Result.IsValid()) << Result.Error.c_str();
    EXPECT_EQ(Result.Schema["type"], "string");
    EXPECT_NE(Result.Schema["description"].get<std::string>().find("CSoundBase"), std::string::npos);
}

// A refusal has to name the offending property, or nobody can tell which field broke registration.
TEST(AgentToolSchema, ARefusalNamesTheOffendingProperty)
{
    const FSchemaResult Result = SchemaOfProperty(SRigidBodyComponent::StaticStruct(), "OnContactBegin");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("OnContactBegin"), FString::npos);
}

// One bad field has to sink the whole struct, or a tool would advertise a shape missing a parameter.
TEST(AgentToolSchema, OneUnsupportedFieldRejectsTheWholeStruct)
{
    const FSchemaResult Result = GenerateSchema(SRigidBodyComponent::StaticStruct());

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.find("cannot be described"), FString::npos);
}

TEST(AgentToolSchema, ARefusedStructYieldsNoPartialSchema)
{
    const FSchemaResult Result = GenerateSchema(SRigidBodyComponent::StaticStruct());

    ASSERT_FALSE(Result.IsValid());
    EXPECT_TRUE(Result.Schema.is_null());
}

TEST(AgentToolSchema, TheSameStructGeneratesTheSameSchemaTwice)
{
    const FSchemaResult First  = GenerateSchema(SLifetimeComponent::StaticStruct());
    const FSchemaResult Second = GenerateSchema(SLifetimeComponent::StaticStruct());

    ASSERT_TRUE(First.IsValid());
    ASSERT_TRUE(Second.IsValid());
    EXPECT_EQ(First.Schema, Second.Schema);
}
