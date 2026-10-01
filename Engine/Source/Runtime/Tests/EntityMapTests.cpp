#include <gtest/gtest.h>
#include "World/ECS/EntityMap.h"

using namespace Lumina;

TEST(EntityMapTests, FindOrAddThenFind)
{
    ECS::TEntityMap<int32> Map;
    const ECS::FEntity Entity(7, 2);

    EXPECT_EQ(Map.Find(Entity), nullptr);
    Map.FindOrAdd(Entity) = 42;
    ASSERT_NE(Map.Find(Entity), nullptr);
    EXPECT_EQ(*Map.Find(Entity), 42);
    EXPECT_EQ(Map.FindOrAdd(Entity), 42);
    EXPECT_EQ(Map.Num(), 1u);
}

TEST(EntityMapTests, StaleVersionMisses)
{
    ECS::TEntityMap<int32> Map;
    Map.FindOrAdd(ECS::FEntity(3, 1)) = 1;

    EXPECT_FALSE(Map.Contains(ECS::FEntity(3, 0)));
    EXPECT_FALSE(Map.Contains(ECS::FEntity(3, 2)));
    EXPECT_FALSE(Map.Contains(ECS::NullEntity));
    EXPECT_TRUE(Map.Contains(ECS::FEntity(3, 1)));
}

TEST(EntityMapTests, RemoveKeepsMovedEntryReachable)
{
    ECS::TEntityMap<int32> Map;
    const ECS::FEntity A(1, 0), B(5000, 0), C(9000, 3);
    Map.FindOrAdd(A) = 10;
    Map.FindOrAdd(B) = 20;
    Map.FindOrAdd(C) = 30;

    EXPECT_TRUE(Map.Remove(A));
    EXPECT_FALSE(Map.Remove(A));
    EXPECT_FALSE(Map.Contains(A));
    ASSERT_NE(Map.Find(B), nullptr);
    ASSERT_NE(Map.Find(C), nullptr);
    EXPECT_EQ(*Map.Find(B), 20);
    EXPECT_EQ(*Map.Find(C), 30);
    EXPECT_EQ(Map.Num(), 2u);

    Map.FindOrAdd(A) = 11;
    EXPECT_EQ(*Map.Find(A), 11);
    EXPECT_EQ(*Map.Find(C), 30);
}

TEST(EntityMapTests, ClearForgetsEverything)
{
    ECS::TEntityMap<int32> Map;
    Map.FindOrAdd(ECS::FEntity(12, 0)) = 1;
    Map.Clear();

    EXPECT_TRUE(Map.IsEmpty());
    EXPECT_FALSE(Map.Contains(ECS::FEntity(12, 0)));
}
