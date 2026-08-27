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
    ASSERT_FALSE(Token.empty());

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;
    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << Error.c_str();
    EXPECT_EQ(Resolved, Entity);
}

// Nothing about the token should invite an agent to do arithmetic on it.
TEST(AgentEntityToken, ATokenIsNotJustTheRawHandle)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    EXPECT_NE(Token.find("ent_"), FString::npos);
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
    EXPECT_TRUE(FEntityTokens::Mint(Registry, ECS::NullEntity).empty());
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
    EXPECT_FALSE(Error.empty());
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
    EXPECT_FALSE(Error.empty());
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

    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << Error.c_str();
    EXPECT_EQ(Resolved, Recycled);
}

// A world swap has to retire every outstanding token, or one could land in the wrong world.
TEST(AgentEntityToken, InvalidatingRetiresOutstandingTokens)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);

    FEntityTokens::InvalidateAll();

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error));
    EXPECT_FALSE(Error.empty());
}

TEST(AgentEntityToken, TokensMintedAfterInvalidationStillWork)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    FEntityTokens::InvalidateAll();

    const FString Token = FEntityTokens::Mint(Registry, Entity);

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << Error.c_str();
    EXPECT_EQ(Resolved, Entity);
}

TEST(AgentEntityToken, InvalidatingMovesTheEpochForward)
{
    const uint64 Before = FEntityTokens::GetEpoch();
    FEntityTokens::InvalidateAll();

    EXPECT_GT(FEntityTokens::GetEpoch(), Before);
}

// A token from one world must not resolve against another, even at the same slot.
TEST(AgentEntityToken, ATokenFromAnotherRegistryDoesNotResolveAfterAWorldSwap)
{
    ECS::FRegistry First;
    const ECS::FEntity Entity = First.Create();
    const FString Token = FEntityTokens::Mint(First, Entity);

    FEntityTokens::InvalidateAll();

    ECS::FRegistry Second;
    const ECS::FEntity Occupant = Second.Create();
    ASSERT_EQ(static_cast<uint32>(Occupant), static_cast<uint32>(Entity));

    ECS::FEntity Resolved = ECS::NullEntity;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Second, FStringView(Token), Resolved, Error));
}
