#include <gtest/gtest.h>

#include "Assets/AssetTypes/Prefabs/PrefabOverride.h"
#include "Assets/AssetTypes/Prefabs/PrefabOverrideTestTypes.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

using namespace Lumina;

namespace
{
    CStruct* DerivedLayout()
    {
        ProcessNewlyLoadedCObjects();
        return SPrefabLeafTestDerived::StaticStruct();
    }
}

// CStruct::Link splices the super onto the tail, so walking it again reported each inherited leaf twice.
TEST(PrefabOverrideDiff, AnInheritedLeafIsReportedOnce)
{
    CStruct* Layout = DerivedLayout();
    ASSERT_NE(Layout, nullptr);

    SPrefabLeafTestDerived Prefab;
    SPrefabLeafTestDerived Instance;

    TVector<FName> Paths;
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Prefab, Paths);
    EXPECT_TRUE(Paths.empty()) << "an untouched instance overrides nothing";

    Instance.Inherited = 7;
    Paths.clear();
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Prefab, Paths);
    ASSERT_EQ(Paths.size(), size_t(1)) << "an inherited leaf must be reported once, not once per level";
    EXPECT_EQ(Paths[0], FName("Inherited"));

    Instance.Own = 3;
    Paths.clear();
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Prefab, Paths);
    EXPECT_EQ(Paths.size(), size_t(2)) << "own and inherited leaves are both visible to the diff";
}

// The inherited half of the chain has to round-trip through apply as well as through the diff.
TEST(PrefabOverrideDiff, AnInheritedLeafInheritsAndOverridesCorrectly)
{
    CStruct* Layout = DerivedLayout();
    ASSERT_NE(Layout, nullptr);

    SPrefabLeafTestDerived Prefab;
    Prefab.Inherited = 1;
    Prefab.Own = 2;

    SPrefabLeafTestDerived Instance;
    Instance.Inherited = 99;
    Instance.Own = 42;

    THashSet<FName> Overridden;
    Overridden.insert(FName("Inherited"));

    PrefabOverride::ApplyInheritedLeaves(Layout, &Instance, &Prefab, Overridden);
    EXPECT_EQ(Instance.Inherited, 99) << "an overridden inherited leaf keeps the instance value";
    EXPECT_EQ(Instance.Own, 2) << "a non-overridden leaf takes the prefab value";

    SPrefabLeafTestDerived Dest;
    PrefabOverride::ApplyOverriddenLeaves(Layout, &Dest, &Instance, Overridden);
    EXPECT_EQ(Dest.Inherited, 99) << "the mirror copies only the overridden leaves";
    EXPECT_EQ(Dest.Own, 0);
}
