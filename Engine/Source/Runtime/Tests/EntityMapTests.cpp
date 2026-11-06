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
    EXPECT_EQ(Map.Num(), 1);
}

TEST(EntityMapTests, FindCheckedReturnsTheStoredValue)
{
    ECS::TEntityMap<int32> Map;
    const ECS::FEntity Entity(11, 4);
    Map.FindOrAdd(Entity) = 5;

    Map.FindChecked(Entity) += 1;
    const ECS::TEntityMap<int32>& ConstMap = Map;
    static_assert(std::is_same_v<decltype(ConstMap.FindChecked(Entity)), const int32&>);
    EXPECT_EQ(ConstMap.FindChecked(Entity), 6);
    EXPECT_EQ(&Map.FindChecked(Entity), Map.Find(Entity));
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
    EXPECT_EQ(Map.Num(), 2);

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

TEST(EntityMapTests, ReusedIndexReplacesTheStaleEntry)
{
    ECS::TEntityMap<int32> Map;
    const ECS::FEntity Other(3, 1);
    const ECS::FEntity Old(7, 1);
    const ECS::FEntity Reused(7, 2);

    Map.FindOrAdd(Old) = 1;
    Map.FindOrAdd(Other) = 3;
    Map.FindOrAdd(Reused) = 2;

    EXPECT_EQ(Map.Num(), 2u);
    EXPECT_EQ(Map.Find(Old), nullptr);
    ASSERT_NE(Map.Find(Reused), nullptr);
    EXPECT_EQ(*Map.Find(Reused), 2);
    ASSERT_NE(Map.Find(Other), nullptr);
    EXPECT_EQ(*Map.Find(Other), 3);

    EXPECT_TRUE(Map.Remove(Other));
    ASSERT_NE(Map.Find(Reused), nullptr);
    EXPECT_EQ(*Map.Find(Reused), 2);
}

TEST(EntityMapTests, RemoveAtKeepsTheMovedEntryReachable)
{
    ECS::TEntityMap<int32> Map;
    for (uint32 Index = 0; Index < 4; ++Index)
    {
        Map.FindOrAdd(ECS::FEntity(Index, 1)) = (int32)Index;
    }

    Map.RemoveAt(1);

    EXPECT_EQ(Map.Num(), 3u);
    EXPECT_EQ(Map.Find(ECS::FEntity(1, 1)), nullptr);
    for (uint32 Index : { 0u, 2u, 3u })
    {
        ASSERT_NE(Map.Find(ECS::FEntity(Index, 1)), nullptr);
        EXPECT_EQ(*Map.Find(ECS::FEntity(Index, 1)), (int32)Index);
    }
}
