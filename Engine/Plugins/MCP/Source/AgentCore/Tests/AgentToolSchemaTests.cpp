#include <gtest/gtest.h>
#include "Core/Reflection/PropertyTypeTraits.h"

#include "Agent/AgentToolSchema.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Scripting/EntityScript.h"
#include "Core/Object/PropertyArena.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "World/World.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/MeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/NameComponent.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace AgentToolSchemaTestsInternal
{
    // Reflection is off for this module, so every fixture is a type the engine already reflects and exports.
    FSchemaResult SchemaOfProperty(CStruct* Struct, const char* PropertyName)
    {
        return GeneratePropertySchema(Struct->GetProperty(FName(PropertyName)));
    }
}

using namespace AgentToolSchemaTestsInternal;

TEST(AgentToolSchema, ANullStructIsRejected)
{
    const FSchemaResult Result = GenerateSchema(nullptr);

    EXPECT_FALSE(Result.IsValid());
    EXPECT_FALSE(Result.Error.IsEmpty());
}

TEST(AgentToolSchema, ANullPropertyIsRejected)
{
    const FSchemaResult Result = GeneratePropertySchema(nullptr);
    EXPECT_FALSE(Result.IsValid());
}

TEST(AgentToolSchema, AStructBecomesAnObjectWithProperties)
{
    const FSchemaResult Result = GenerateSchema(SLifetimeComponent::StaticStruct());

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "object");
    ASSERT_TRUE(Result.Schema.Contains("properties"));
    EXPECT_TRUE(Result.Schema["properties"].Contains("Lifetime"));
}

TEST(AgentToolSchema, AFloatBecomesANumber)
{
    const FSchemaResult Result = SchemaOfProperty(SLifetimeComponent::StaticStruct(), "Lifetime");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "number");
}

TEST(AgentToolSchema, ABoolBecomesABoolean)
{
    const FSchemaResult Result = SchemaOfProperty(SMeshComponent::StaticStruct(), "bCastShadow");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "boolean");
}

TEST(AgentToolSchema, ANameBecomesAString)
{
    const FSchemaResult Result = SchemaOfProperty(SNameComponent::StaticStruct(), "Name");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "string");
}

// A doc comment above a PROPERTY lands in ToolTip, which is what documents the field to a model.
TEST(AgentToolSchema, ADocCommentBecomesTheDescription)
{
    const FSchemaResult Result = SchemaOfProperty(SNameComponent::StaticStruct(), "Name");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    ASSERT_TRUE(Result.Schema.Contains("description"));
    EXPECT_FALSE(Result.Schema["description"].AsString().IsEmpty());
}

TEST(AgentToolSchema, AnEnumBecomesAConstrainedString)
{
    const FSchemaResult Result = SchemaOfProperty(SAudioSourceComponent::StaticStruct(), "Bus");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "string");

    ASSERT_TRUE(Result.Schema.Contains("enum"));
    ASSERT_TRUE(Result.Schema["enum"].IsArray());

    bool bFoundKnownValue = false;
    for (const FJsonValue& Name : Result.Schema["enum"].AsArray())
    {
        bFoundKnownValue = bFoundKnownValue || Name == "SFX";
    }

    EXPECT_TRUE(bFoundKnownValue);
}

TEST(AgentToolSchema, ANestedStructBecomesANestedObject)
{
    const FSchemaResult Result = SchemaOfProperty(SAudioSourceComponent::StaticStruct(), "Occlusion");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "object");

    // The nesting has to reach real leaves rather than stop at an empty object.
    ASSERT_TRUE(Result.Schema.Contains("properties"));
    EXPECT_FALSE(Result.Schema["properties"].IsEmpty());
}

// An object reference travels as its asset GUID, so the schema has to say which class is wanted.
TEST(AgentToolSchema, AnObjectReferenceBecomesAGuidString)
{
    const FSchemaResult Result = SchemaOfProperty(SAudioSourceComponent::StaticStruct(), "Sound");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "string");
    EXPECT_NE(Result.Schema["description"].AsString().Find("CSoundBase"), Constants::kIndexNone);
}

// A refusal has to name the offending property, or nobody can tell which field broke registration.
TEST(AgentToolSchema, ARefusalNamesTheOffendingProperty)
{
    const FSchemaResult Result = SchemaOfProperty(SRigidBodyComponent::StaticStruct(), "CollisionProfile");

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("CollisionProfile"), Constants::kIndexNone);
}

// One bad field has to sink the whole struct, or a tool would advertise a shape missing a parameter.
TEST(AgentToolSchema, OneUnsupportedFieldRejectsTheWholeStruct)
{
    const FSchemaResult Result = GenerateSchema(SRigidBodyComponent::StaticStruct());

    ASSERT_FALSE(Result.IsValid());
    EXPECT_NE(Result.Error.Find("cannot be described"), Constants::kIndexNone);
}

TEST(AgentToolSchema, ARefusedStructYieldsNoPartialSchema)
{
    const FSchemaResult Result = GenerateSchema(SRigidBodyComponent::StaticStruct());

    ASSERT_FALSE(Result.IsValid());
    EXPECT_TRUE(Result.Schema.IsNull());
}

TEST(AgentToolSchema, TheSameStructGeneratesTheSameSchemaTwice)
{
    const FSchemaResult First  = GenerateSchema(SLifetimeComponent::StaticStruct());
    const FSchemaResult Second = GenerateSchema(SLifetimeComponent::StaticStruct());

    ASSERT_TRUE(First.IsValid());
    ASSERT_TRUE(Second.IsValid());
    EXPECT_EQ(First.Schema, Second.Schema);
}

// Container and reference kinds, described so a model knows the shape before it calls.

namespace AgentToolSchemaTestsInternal
{
    constexpr FPropertyDesc GSchemaStartupMap{ .Name = "GameStartupMap", .Type = PropertyTypeOf<TSoftObjectPtr<CWorld>> };
    constexpr FPropertyDesc GSchemaScriptClass{ .Name = "ScriptClass", .Type = PropertyTypeOf<TSubclassOf<CEntityScript>> };
}

using namespace AgentToolSchemaTestsInternal;

TEST(AgentToolSchema, ASoftObjectIsAStringNamingItsClass)
{
    FPropertyArena Arena;
    TVector<FProperty*> Collected;
    const FPropertyOwner Owner{ &Arena, nullptr, nullptr, &Collected };

    const FSchemaResult Result = GeneratePropertySchema(Owner.Build<FSoftObjectProperty>(GSchemaStartupMap));

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "string");
    EXPECT_NE(Result.Schema["description"].AsString().Find("CWorld"), Constants::kIndexNone);
}

TEST(AgentToolSchema, AClassIsAStringNamingItsBase)
{
    FPropertyArena Arena;
    TVector<FProperty*> Collected;
    const FPropertyOwner Owner{ &Arena, nullptr, nullptr, &Collected };

    const FSchemaResult Result = GeneratePropertySchema(Owner.Build<FClassProperty>(GSchemaScriptClass));

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "string");
    EXPECT_NE(Result.Schema["description"].AsString().Find("CEntityScript"), Constants::kIndexNone);
}

TEST(AgentToolSchema, AnInstancedStructDescribesTypeAndData)
{
    const FSchemaResult Result = SchemaOfProperty(SAnimMontageNotify::StaticStruct(), "Notify");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_EQ(Result.Schema["type"], "object");
    EXPECT_EQ(Result.Schema["properties"]["StructType"]["type"], "string");
    EXPECT_EQ(Result.Schema["properties"]["Data"]["type"], "object");
}

// A delegate is not data, so it drops out of the struct instead of making the struct undescribable.
TEST(AgentToolSchema, ADelegateIsSkippedNotRejected)
{
    const FSchemaResult Result = SchemaOfProperty(SRigidBodyComponent::StaticStruct(), "OnWake");

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_TRUE(Result.Schema.IsNull());
}
