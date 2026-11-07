#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Agent/AgentEntityToken.h"

using namespace Lumina;
using namespace Lumina::Agent;

TEST(AgentEntityToken, AMintedTokenResolvesBackToItsEntity)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    ASSERT_FALSE(Token.IsEmpty());

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;
    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << *Error;
    EXPECT_EQ(Resolved, Entity);
}

// Nothing about the token should invite an agent to do arithmetic on it.
TEST(AgentEntityToken, ATokenIsNotJustTheRawHandle)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    EXPECT_NE(Token.Find("ent_"), Constants::kIndexNone);
    EXPECT_EQ(Token, FEntityTokens::Mint(Registry, Entity));
}

TEST(AgentEntityToken, DifferentEntitiesGetDifferentTokens)
{
    ECS::FRegistry Registry;

    const FString First  = FEntityTokens::Mint(Registry, Registry.Create());
    const FString Second = FEntityTokens::Mint(Registry, Registry.Create());

    EXPECT_NE(First, Second);
}

TEST(AgentEntityToken, AnInvalidEntityMintsNothing)
{
    ECS::FRegistry Registry;
    EXPECT_TRUE(FEntityTokens::Mint(Registry, ECS::NullEntity).IsEmpty());
}

TEST(AgentEntityToken, GarbageIsRefused)
{
    ECS::FRegistry Registry;

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, "", Resolved, Error));
    EXPECT_FALSE(FEntityTokens::Resolve(Registry, "42", Resolved, Error));
    EXPECT_FALSE(FEntityTokens::Resolve(Registry, "ent_", Resolved, Error));
    EXPECT_FALSE(FEntityTokens::Resolve(Registry, "ent_zz_zz", Resolved, Error));
    EXPECT_FALSE(FEntityTokens::Resolve(Registry, "notatoken", Resolved, Error));
    EXPECT_FALSE(Error.IsEmpty());
}

TEST(AgentEntityToken, ADestroyedEntityIsRefused)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    Registry.Destroy(Entity);

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error));
    EXPECT_FALSE(Registry.IsValid(Resolved));
    EXPECT_FALSE(Error.IsEmpty());
}

// This is the whole reason tokens exist. A recycled slot must not answer to the old occupant's name.
TEST(AgentEntityToken, ARecycledSlotDoesNotAnswerToTheOldToken)
{
    ECS::FRegistry Registry;

    const ECS::FEntity Original = Registry.Create();
    const FString Token = FEntityTokens::Mint(Registry, Original);

    Registry.Destroy(Original);

    const ECS::FEntity Recycled = Registry.Create();

    // The slot came back, which is exactly the case a bare index would silently resolve to.
    ASSERT_TRUE(Registry.IsValid(Recycled));

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error));
    EXPECT_NE(Resolved, Recycled);
}

TEST(AgentEntityToken, TheRecycledEntityGetsAWorkingTokenOfItsOwn)
{
    ECS::FRegistry Registry;

    const ECS::FEntity Original = Registry.Create();
    Registry.Destroy(Original);

    const ECS::FEntity Recycled = Registry.Create();
    const FString Token = FEntityTokens::Mint(Registry, Recycled);

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << *Error;
    EXPECT_EQ(Resolved, Recycled);
}

// A token from one world must not resolve against another, even at the same slot and version.
TEST(AgentEntityToken, ATokenFromAnotherRegistryDoesNotResolveThere)
{
    ECS::FRegistry First;
    const ECS::FEntity Entity = First.Create();
    const FString Token = FEntityTokens::Mint(First, Entity);

    ECS::FRegistry Second;
    const ECS::FEntity Occupant = Second.Create();
    ASSERT_EQ(static_cast<uint32>(Occupant), static_cast<uint32>(Entity));

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Second, FStringView(Token), Resolved, Error));
    EXPECT_NE(Error.Find("no longer open"), Constants::kIndexNone);
    EXPECT_EQ(Resolved, ECS::NullEntity);
}

// Both registries have minted, so each holds a scope, and neither answers to the other's tokens.
TEST(AgentEntityToken, TwoMintingRegistriesRefuseEachOthersTokens)
{
    ECS::FRegistry First;
    ECS::FRegistry Second;
    const ECS::FEntity InFirst  = First.Create();
    const ECS::FEntity InSecond = Second.Create();
    ASSERT_EQ(static_cast<uint32>(InFirst), static_cast<uint32>(InSecond));

    const FString FirstToken  = FEntityTokens::Mint(First, InFirst);
    const FString SecondToken = FEntityTokens::Mint(Second, InSecond);
    EXPECT_NE(FirstToken, SecondToken);

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Second, FStringView(FirstToken), Resolved, Error));
    EXPECT_FALSE(FEntityTokens::Resolve(First, FStringView(SecondToken), Resolved, Error));
    ASSERT_TRUE(FEntityTokens::Resolve(First, FStringView(FirstToken), Resolved, Error)) << *Error;
    EXPECT_EQ(Resolved, InFirst);
}

// Edits elsewhere in the same world, including destroying and recreating other entities, keep a token valid.
TEST(AgentEntityToken, ATokenSurvivesUnrelatedEditsInItsOwnRegistry)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    const FString Token = FEntityTokens::Mint(Registry, Entity);

    const ECS::FEntity Other = Registry.Create();
    const FString OtherToken = FEntityTokens::Mint(Registry, Other);
    Registry.Destroy(Other);
    const ECS::FEntity Recycled = Registry.Create();
    ASSERT_TRUE(Registry.IsValid(Recycled));
    EXPECT_NE(Recycled, Other);

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << *Error;
    EXPECT_EQ(Resolved, Entity);
    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(OtherToken), Resolved, Error));
}
