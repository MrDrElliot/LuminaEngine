#include <gtest/gtest.h>

#include "Agent/AgentPropertyPath.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/MeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace AgentPropertyPathTestsInternal
{
    // Reflection is off for this module, so every fixture is a type the engine already reflects and exports.
    template<typename T>
    bool Resolve(T& Instance, FStringView Path, FResolvedProperty& Out, FString& OutError)
    {
        return ResolvePropertyPath(T::StaticStruct(), &Instance, Path, Out, OutError);
    }

    template<typename T>
    FString ResolveError(T& Instance, FStringView Path)
    {
        FResolvedProperty Out;
        FString Error;
        EXPECT_FALSE(Resolve(Instance, Path, Out, Error));
        return Error;
    }
}

using namespace AgentPropertyPathTestsInternal;

TEST(AgentPropertyPath, APlainFieldResolvesToItsValue)
{
    SLifetimeComponent Lifetime;

    FResolvedProperty Out;
    FString Error;
    ASSERT_TRUE(Resolve(Lifetime, "Lifetime", Out, Error)) << *Error;
    EXPECT_EQ(Out.ValuePtr, &Lifetime.Lifetime);
}

TEST(AgentPropertyPath, ANestedStructFieldResolvesToItsValue)
{
    SAudioSourceComponent Source;

    FResolvedProperty Out;
    FString Error;
    ASSERT_TRUE(Resolve(Source, "Occlusion.LowPassFrequency", Out, Error)) << *Error;
    EXPECT_EQ(Out.ValuePtr, &Source.Occlusion.LowPassFrequency);
}

TEST(AgentPropertyPath, AnArrayIndexResolvesToTheElement)
{
    STerrainComponent Terrain;
    Terrain.Heightmap = { 1.0f, 2.0f, 3.0f };

    FResolvedProperty Out;
    FString Error;
    ASSERT_TRUE(Resolve(Terrain, "Heightmap[2]", Out, Error)) << *Error;
    EXPECT_EQ(Out.ValuePtr, &Terrain.Heightmap[2]);
}

TEST(AgentPropertyPath, AnIndexPastTheEndIsRefused)
{
    STerrainComponent Terrain;
    Terrain.Heightmap = { 1.0f };

    EXPECT_NE(ResolveError(Terrain, "Heightmap[1]").Find("past the end"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, AnIndexOnANonArrayFieldIsRefused)
{
    SAudioSourceComponent Source;
    EXPECT_NE(ResolveError(Source, "Occlusion[0]").Find("array field"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, AnUnclosedBracketIsRefused)
{
    STerrainComponent Terrain;
    EXPECT_NE(ResolveError(Terrain, "Heightmap[0").Find("unclosed bracket"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, ANonNumericIndexIsRefused)
{
    STerrainComponent Terrain;
    EXPECT_NE(ResolveError(Terrain, "Heightmap[x]").Find("not a number"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, AnEmptyStepIsRefused)
{
    SAudioSourceComponent Source;
    EXPECT_NE(ResolveError(Source, "Occlusion..LowPassFrequency").Find("empty step"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, SteppingIntoAValueFieldNamesTheField)
{
    SLifetimeComponent Lifetime;
    EXPECT_NE(ResolveError(Lifetime, "Lifetime.X").Find("'Lifetime' holds a value"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, SteppingIntoAValueElementNamesTheFieldAndIndex)
{
    STerrainComponent Terrain;
    Terrain.Heightmap = { 1.0f };

    EXPECT_NE(ResolveError(Terrain, "Heightmap[0].X").Find("'Heightmap[0]' holds a value"), Constants::kIndexNone);
}

TEST(AgentPropertyPath, SteppingThroughANullElementNamesTheFieldAndIndex)
{
    SMeshComponent Mesh;
    Mesh.MaterialOverrides.Add(nullptr);

    EXPECT_NE(ResolveError(Mesh, "MaterialOverrides[0].Foo").Find("'MaterialOverrides[0]' is null"), Constants::kIndexNone);
}
