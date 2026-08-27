#include <gtest/gtest.h>

#include "Agent/AgentEntityToken.h"

using namespace Lumina;
using namespace Lumina::Agent;

TEST(AgentEntityToken, AMintedTokenResolvesBackToItsEntity)
{
    FEntityRegistry Registry;
    const FEntity Entity = Registry.create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    ASSERT_FALSE(Token.empty());

    FEntity Resolved = entt::null;
    FString Error;
    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << Error.c_str();
    EXPECT_EQ(Resolved, Entity);
}

// Nothing about the token should invite an agent to do arithmetic on it.
TEST(AgentEntityToken, ATokenIsNotJustTheRawHandle)
{
    FEntityRegistry Registry;
    const FEntity Entity = Registry.create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    EXPECT_NE(Token.find("ent_"), FString::npos);
    EXPECT_EQ(Token, FEntityTokens::Mint(Registry, Entity));
}

TEST(AgentEntityToken, DifferentEntitiesGetDifferentTokens)
{
    FEntityRegistry Registry;

    const FString First  = FEntityTokens::Mint(Registry, Registry.create());
    const FString Second = FEntityTokens::Mint(Registry, Registry.create());

    EXPECT_NE(First, Second);
}

TEST(AgentEntityToken, AnInvalidEntityMintsNothing)
{
    FEntityRegistry Registry;
    EXPECT_TRUE(FEntityTokens::Mint(Registry, entt::null).empty());
}

TEST(AgentEntityToken, GarbageIsRefused)
{
    FEntityRegistry Registry;

    FEntity Resolved = entt::null;
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
    FEntityRegistry Registry;
    const FEntity Entity = Registry.create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);
    Registry.destroy(Entity);

    FEntity Resolved = entt::null;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error));
    EXPECT_FALSE(Registry.valid(Resolved));
    EXPECT_FALSE(Error.empty());
}

// This is the whole reason tokens exist. A recycled slot must not answer to the old occupant's name.
TEST(AgentEntityToken, ARecycledSlotDoesNotAnswerToTheOldToken)
{
    FEntityRegistry Registry;

    const FEntity Original = Registry.create();
    const FString Token = FEntityTokens::Mint(Registry, Original);

    Registry.destroy(Original);

    const FEntity Recycled = Registry.create();

    // The slot came back, which is exactly the case a bare index would silently resolve to.
    ASSERT_TRUE(Registry.valid(Recycled));

    FEntity Resolved = entt::null;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error));
    EXPECT_NE(Resolved, Recycled);
}

TEST(AgentEntityToken, TheRecycledEntityGetsAWorkingTokenOfItsOwn)
{
    FEntityRegistry Registry;

    const FEntity Original = Registry.create();
    Registry.destroy(Original);

    const FEntity Recycled = Registry.create();
    const FString Token = FEntityTokens::Mint(Registry, Recycled);

    FEntity Resolved = entt::null;
    FString Error;

    ASSERT_TRUE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error)) << Error.c_str();
    EXPECT_EQ(Resolved, Recycled);
}

// A world swap has to retire every outstanding token, or one could land in the wrong world.
TEST(AgentEntityToken, InvalidatingRetiresOutstandingTokens)
{
    FEntityRegistry Registry;
    const FEntity Entity = Registry.create();

    const FString Token = FEntityTokens::Mint(Registry, Entity);

    FEntityTokens::InvalidateAll();

    FEntity Resolved = entt::null;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Registry, FStringView(Token), Resolved, Error));
    EXPECT_FALSE(Error.empty());
}

TEST(AgentEntityToken, TokensMintedAfterInvalidationStillWork)
{
    FEntityRegistry Registry;
    const FEntity Entity = Registry.create();

    FEntityTokens::InvalidateAll();

    const FString Token = FEntityTokens::Mint(Registry, Entity);

    FEntity Resolved = entt::null;
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
    FEntityRegistry First;
    const FEntity Entity = First.create();
    const FString Token = FEntityTokens::Mint(First, Entity);

    FEntityTokens::InvalidateAll();

    FEntityRegistry Second;
    const FEntity Occupant = Second.create();
    ASSERT_EQ(static_cast<uint32>(Occupant), static_cast<uint32>(Entity));

    FEntity Resolved = entt::null;
    FString Error;

    EXPECT_FALSE(FEntityTokens::Resolve(Second, FStringView(Token), Resolved, Error));
}
