#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Assets/AssetTypes/Prefabs/PrefabOverride.h"
#include "Assets/AssetTypes/Prefabs/PrefabOverrideTestTypes.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

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

namespace
{
    CPrefab* MakePrefab()
    {
        ProcessNewlyLoadedCObjects();
        return NewObject<CPrefab>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    ECS::FEntity AddNode(ECS::FRegistry& Registry, const char* StableID)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<SPrefabComponent>(Entity).StableID = FName(StableID);
        Registry.Emplace<STransformComponent>(Entity);
        return Entity;
    }

    ECS::FEntity FindByStableID(ECS::FRegistry& Registry, const char* StableID)
    {
        ECS::FEntity Found = ECS::NullEntity;
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity E, const SPrefabComponent& Comp)
        {
            if (Comp.StableID == FName(StableID))
            {
                Found = E;
            }
        });
        return Found;
    }
}

// The delta is authored in its own id space, so a handle copied out of it verbatim names a stranger.
TEST(PrefabVariants, AnOverriddenEntityHandleSurvivesResolve)
{
    CPrefab* Parent = MakePrefab();
    ASSERT_NE(Parent, nullptr);

    const ECS::FEntity ParentRoot = AddNode(Parent->Registry, "root");
    const ECS::FEntity ParentLink = AddNode(Parent->Registry, "link");
    const ECS::FEntity ParentTarget = AddNode(Parent->Registry, "target");
    ECS::Utils::ReparentEntity(Parent->Registry, ParentLink, ParentRoot);
    ECS::Utils::ReparentEntity(Parent->Registry, ParentTarget, ParentRoot);
    Parent->Registry.Emplace<SPrefabLinkTestComponent>(ParentLink);

    CPrefab* Variant = MakePrefab();
    ASSERT_NE(Variant, nullptr);
    Variant->ParentPrefab = Parent;
    Variant->ResolveVariant();
    ASSERT_FALSE(Variant->IsUnresolvedVariant());

    // The variant points its link at a node it inherits rather than one it adds.
    const ECS::FEntity MyLink = FindByStableID(Variant->Registry, "link");
    const ECS::FEntity MyTarget = FindByStableID(Variant->Registry, "target");
    ASSERT_NE(MyLink, ECS::NullEntity);
    ASSERT_NE(MyTarget, ECS::NullEntity);
    Variant->Registry.Get<SPrefabLinkTestComponent>(MyLink).Target = MyTarget;
    Variant->Registry.Get<SPrefabLinkTestComponent>(MyLink).Value = 7;

    Variant->CaptureVariantDelta();
    Variant->ResolveVariant();
    ASSERT_FALSE(Variant->IsUnresolvedVariant());

    const ECS::FEntity ResolvedLink = FindByStableID(Variant->Registry, "link");
    const ECS::FEntity ResolvedTarget = FindByStableID(Variant->Registry, "target");
    ASSERT_NE(ResolvedLink, ECS::NullEntity);
    ASSERT_NE(ResolvedTarget, ECS::NullEntity);

    const SPrefabLinkTestComponent& Link = Variant->Registry.Get<SPrefabLinkTestComponent>(ResolvedLink);
    EXPECT_EQ(Link.Value, 7) << "the overridden leaf still lands";
    EXPECT_EQ(Link.Target, ResolvedTarget) << "the handle must name the resolved node, not a delta id";
}

// Resolving twice must not consume the delta, whose handles are the persisted authority.
TEST(PrefabVariants, ResolvingTwiceKeepsTheDeltaAddressable)
{
    CPrefab* Parent = MakePrefab();
    const ECS::FEntity ParentRoot = AddNode(Parent->Registry, "root");
    const ECS::FEntity ParentLink = AddNode(Parent->Registry, "link");
    ECS::Utils::ReparentEntity(Parent->Registry, ParentLink, ParentRoot);
    Parent->Registry.Emplace<SPrefabLinkTestComponent>(ParentLink);

    CPrefab* Variant = MakePrefab();
    Variant->ParentPrefab = Parent;
    Variant->ResolveVariant();

    const ECS::FEntity MyLink = FindByStableID(Variant->Registry, "link");
    ASSERT_NE(MyLink, ECS::NullEntity);
    Variant->Registry.Get<SPrefabLinkTestComponent>(MyLink).Target = FindByStableID(Variant->Registry, "root");
    Variant->CaptureVariantDelta();

    for (int32 Pass = 0; Pass < 3; ++Pass)
    {
        Variant->ResolveVariant();
        const ECS::FEntity Link = FindByStableID(Variant->Registry, "link");
        const ECS::FEntity Root = FindByStableID(Variant->Registry, "root");
        ASSERT_NE(Link, ECS::NullEntity);
        EXPECT_EQ(Variant->Registry.Get<SPrefabLinkTestComponent>(Link).Target, Root)
            << "pass " << Pass << " must resolve the handle the same way the first did";
    }
}

namespace
{
    CWorld* MakeWorld()
    {
        ProcessNewlyLoadedCObjects();
        return NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    // Stamps one entity as belonging to Source, the way Instantiate does.
    ECS::FEntity AddInstanceNode(ECS::FRegistry& Registry, CPrefab* Source, const char* StableID, bool bIsRoot)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<STransformComponent>(Entity);

        SPrefabInstanceComponent& Instance = Registry.Emplace<SPrefabInstanceComponent>(Entity);
        Instance.SourcePrefab = Source;
        Instance.StableID = FName(StableID);
        Instance.bIsRoot = bIsRoot;
        return Entity;
    }
}

// Detaching one instance must not unlink another prefab's instance that happens to sit under it.
TEST(PrefabInstances, DetachLeavesANestedInstanceOfAnotherPrefabLinked)
{
    CWorld* World = MakeWorld();
    ASSERT_NE(World, nullptr);

    CPrefab* Outer = MakePrefab();
    CPrefab* Inner = MakePrefab();
    ASSERT_NE(Outer, nullptr);
    ASSERT_NE(Inner, nullptr);

    ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

    const ECS::FEntity OuterRoot = AddInstanceNode(Registry, Outer, "outer-root", true);
    const ECS::FEntity OuterChild = AddInstanceNode(Registry, Outer, "outer-child", false);
    const ECS::FEntity InnerRoot = AddInstanceNode(Registry, Inner, "inner-root", true);
    const ECS::FEntity InnerChild = AddInstanceNode(Registry, Inner, "inner-child", false);

    ECS::Utils::ReparentEntity(Registry, OuterChild, OuterRoot);
    ECS::Utils::ReparentEntity(Registry, InnerRoot, OuterChild);
    ECS::Utils::ReparentEntity(Registry, InnerChild, InnerRoot);

    ASSERT_TRUE(CPrefab::DetachInstance(World, OuterRoot));

    EXPECT_FALSE(Registry.HasAny<SPrefabInstanceComponent>(OuterRoot)) << "the detached root is plain now";
    EXPECT_FALSE(Registry.HasAny<SPrefabInstanceComponent>(OuterChild)) << "its own children detach with it";

    ASSERT_TRUE(Registry.HasAny<SPrefabInstanceComponent>(InnerRoot))
        << "another prefab's instance keeps its own link";
    EXPECT_EQ(Registry.Get<SPrefabInstanceComponent>(InnerRoot).SourcePrefab.Get(), Inner);
    ASSERT_TRUE(Registry.HasAny<SPrefabInstanceComponent>(InnerChild));
    EXPECT_EQ(Registry.Get<SPrefabInstanceComponent>(InnerChild).SourcePrefab.Get(), Inner);
}

// A prefab that ships a node the instance lost must spawn it again rather than leave a dead id mapped.
TEST(PrefabInstances, RefreshRespawnsANodeDestroyedWithItsDeletedParent)
{
    CWorld* World = MakeWorld();
    CPrefab* Prefab = MakePrefab();
    ASSERT_NE(World, nullptr);
    ASSERT_NE(Prefab, nullptr);

    // The prefab keeps the grandchild but drops the middle node the instance still has.
    const ECS::FEntity PrefabRoot = AddNode(Prefab->Registry, "root");
    const ECS::FEntity PrefabLeaf = AddNode(Prefab->Registry, "leaf");
    ECS::Utils::ReparentEntity(Prefab->Registry, PrefabLeaf, PrefabRoot);

    ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

    const ECS::FEntity Root = AddInstanceNode(Registry, Prefab, "root", true);
    const ECS::FEntity Gone = AddInstanceNode(Registry, Prefab, "gone", false);
    const ECS::FEntity Leaf = AddInstanceNode(Registry, Prefab, "leaf", false);

    ECS::Utils::ReparentEntity(Registry, Gone, Root);
    ECS::Utils::ReparentEntity(Registry, Leaf, Gone);

    // The rescue pass only lifts a survivor that has a transform, so this one goes down with its parent.
    Registry.Remove<STransformComponent>(Leaf);

    Prefab->RefreshInstance(World, Root);

    EXPECT_FALSE(Registry.IsValid(Gone)) << "the node the prefab dropped is destroyed";

    ECS::FEntity Rebuilt = ECS::NullEntity;
    Registry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
    {
        if (Inst.StableID == FName("leaf"))
        {
            Rebuilt = E;
        }
    });

    ASSERT_NE(Rebuilt, ECS::NullEntity) << "the prefab still ships this node, so refresh must restore it";
    EXPECT_TRUE(Registry.IsValid(Rebuilt));
}
