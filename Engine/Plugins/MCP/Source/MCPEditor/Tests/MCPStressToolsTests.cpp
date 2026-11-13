#include <gtest/gtest.h>

#include "Containers/HashTable.h"
#include "MCPStressTools.h"

using namespace Lumina;
using namespace Lumina::MCP;

TEST(MCPStressTools, RespawnKeepsTheNewEntityWhenItSpawned)
{
    THashMap<int32, ECS::FEntity> Cells;
    Cells.Add(3, ECS::FEntity(1u, 0u));

    const ECS::FEntity Replacement(2u, 0u);
    EXPECT_TRUE(StoreRespawnedCell(Cells, 3, Replacement));
    ASSERT_TRUE(Cells.Contains(3));
    EXPECT_EQ(*Cells.Find(3), Replacement);
}

TEST(MCPStressTools, RespawnDropsTheCellWhenTheReplacementFailsToSpawn)
{
    THashMap<int32, ECS::FEntity> Cells;
    Cells.Add(5, ECS::FEntity(1u, 0u));

    EXPECT_FALSE(StoreRespawnedCell(Cells, 5, ECS::NullEntity));
    EXPECT_FALSE(Cells.Contains(5));
}
