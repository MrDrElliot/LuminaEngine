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
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

using namespace Lumina;

namespace
{
    CStruct* DerivedLayout()
    {
        RegisterPendingTypes();
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
    EXPECT_TRUE(Paths.IsEmpty()) << "an untouched instance overrides nothing";

    Instance.Inherited = 7;
    Paths.Reset();
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Prefab, Paths);
    ASSERT_EQ(Paths.Num(), 1) << "an inherited leaf must be reported once, not once per level";
    EXPECT_EQ(Paths[0], FName("Inherited"));

    Instance.Own = 3;
    Paths.Reset();
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Prefab, Paths);
    EXPECT_EQ(Paths.Num(), 2) << "own and inherited leaves are both visible to the diff";
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
    Overridden.Emplace("Inherited");

    PrefabOverride::ApplyInheritedLeaves(Layout, &Instance, &Prefab, Overridden);
    EXPECT_EQ(Instance.Inherited, 99) << "an overridden inherited leaf keeps the instance value";
    EXPECT_EQ(Instance.Own, 2) << "a non-overridden leaf takes the prefab value";

    SPrefabLeafTestDerived Dest;
    PrefabOverride::ApplyOverriddenLeaves(Layout, &Dest, &Instance, Overridden);
    EXPECT_EQ(Dest.Inherited, 99) << "the mirror copies only the overridden leaves";
    EXPECT_EQ(Dest.Own, 0);
}

// The color lives in a packed word, and a diff that saw only the type would drop every recolor.
TEST(PrefabOverrideDiff, ACustomPrimitiveColorChangeIsAnOverride)
{
    RegisterPendingTypes();
    CStruct* Layout = SStaticMeshComponent::StaticStruct();
    ASSERT_NE(Layout, nullptr);

    SStaticMeshComponent Prefab;
    SStaticMeshComponent Instance;
    Prefab.CustomPrimitiveData.SetAsColor(FVector4(1.0f, 0.0f, 0.0f, 1.0f));
    Instance.CustomPrimitiveData.SetAsColor(FVector4(0.0f, 0.0f, 1.0f, 1.0f));

    TVector<FName> Paths;
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Prefab, Paths);
    ASSERT_FALSE(Paths.IsEmpty()) << "a recolor must read as an override";

    THashSet<FName> Overridden;
    for (const FName& Path : Paths)
    {
        Overridden.Add(Path);
    }
    SStaticMeshComponent Resolved;
    Resolved.CustomPrimitiveData.SetAsColor(FVector4(1.0f, 0.0f, 0.0f, 1.0f));
    PrefabOverride::ApplyOverriddenLeaves(Layout, &Resolved, &Instance, Overridden);
    EXPECT_EQ(Resolved.CustomPrimitiveData.Data.Packed, Instance.CustomPrimitiveData.Data.Packed) << "and the override must land";
}

namespace
{
    CPrefab* MakePrefab()
    {
        RegisterPendingTypes();
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
        RegisterPendingTypes();
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

    ECS::FRegistry& Registry = World->GetEntityRegistry();

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

    ECS::FRegistry& Registry = World->GetEntityRegistry();

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

namespace
{
    // An outer instance of Prefab whose child holds a second, nested instance of that same prefab.
    struct FSelfNested
    {
        ECS::FEntity OuterRoot, OuterChild, InnerRoot, InnerChild;
    };

    FSelfNested PlaceSelfNested(ECS::FRegistry& Registry, CPrefab* Prefab)
    {
        FSelfNested Out;
        Out.OuterRoot  = AddInstanceNode(Registry, Prefab, "root", true);
        Out.OuterChild = AddInstanceNode(Registry, Prefab, "child", false);
        Out.InnerRoot  = AddInstanceNode(Registry, Prefab, "root", true);
        Out.InnerChild = AddInstanceNode(Registry, Prefab, "child", false);
        ECS::Utils::ReparentEntity(Registry, Out.OuterChild, Out.OuterRoot);
        ECS::Utils::ReparentEntity(Registry, Out.InnerRoot, Out.OuterChild);
        ECS::Utils::ReparentEntity(Registry, Out.InnerChild, Out.InnerRoot);
        return Out;
    }

    CPrefab* MakeRootChildPrefab()
    {
        CPrefab* Prefab = MakePrefab();
        const ECS::FEntity Root = AddNode(Prefab->Registry, "root");
        ECS::Utils::ReparentEntity(Prefab->Registry, AddNode(Prefab->Registry, "child"), Root);
        return Prefab;
    }
}

// The nested copy shares every StableID with its outer instance, and a refresh of the outer must leave it a placement of its own.
TEST(PrefabInstances, RefreshLeavesANestedCopyOfTheSamePrefabAlone)
{
    CWorld* World = MakeWorld();
    CPrefab* Prefab = MakeRootChildPrefab();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const FSelfNested Placed = PlaceSelfNested(Registry, Prefab);

    Prefab->RefreshInstance(World, Placed.OuterRoot);

    ASSERT_TRUE(Registry.IsValid(Placed.InnerRoot));
    ASSERT_TRUE(Registry.IsValid(Placed.InnerChild));
    EXPECT_TRUE(Registry.Get<SPrefabInstanceComponent>(Placed.InnerRoot).bIsRoot) << "the nested root stays a root";
    EXPECT_TRUE(Registry.Get<SPrefabInstanceComponent>(Placed.OuterRoot).bIsRoot);
    EXPECT_EQ(Registry.GetHierarchy().GetParent(Placed.InnerChild), Placed.InnerRoot) << "the nested child stays under its own root";
    EXPECT_EQ(Registry.GetHierarchy().GetParent(Placed.OuterChild), Placed.OuterRoot);
}

TEST(PrefabInstances, DetachLeavesANestedCopyOfTheSamePrefabLinked)
{
    CWorld* World = MakeWorld();
    CPrefab* Prefab = MakeRootChildPrefab();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const FSelfNested Placed = PlaceSelfNested(Registry, Prefab);

    ASSERT_TRUE(CPrefab::DetachInstance(World, Placed.OuterRoot));

    EXPECT_FALSE(Registry.HasAny<SPrefabInstanceComponent>(Placed.OuterChild));
    EXPECT_TRUE(Registry.HasAny<SPrefabInstanceComponent>(Placed.InnerRoot));
    EXPECT_TRUE(Registry.HasAny<SPrefabInstanceComponent>(Placed.InnerChild)) << "the nested copy's own child keeps its link";
}

// Two nested placements of one prefab carry the same ids, so a capture holding both must give each its own.
TEST(PrefabInstances, CaptureGivesEveryNestedNodeItsOwnStableID)
{
    CWorld* World = MakeWorld();
    CPrefab* Inner = MakeRootChildPrefab();
    CPrefab* Outer = MakePrefab();
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    const ECS::FEntity Root = Registry.Create();
    Registry.Emplace<STransformComponent>(Root);
    for (int32 Copy = 0; Copy < 2; ++Copy)
    {
        const ECS::FEntity InnerRoot = AddInstanceNode(Registry, Inner, "root", true);
        const ECS::FEntity InnerChild = AddInstanceNode(Registry, Inner, "child", false);
        ECS::Utils::ReparentEntity(Registry, InnerRoot, Root);
        ECS::Utils::ReparentEntity(Registry, InnerChild, InnerRoot);
    }

    Outer->CaptureFromWorld(World, Root, /*bAdoptSourceAsInstance*/ true);

    THashSet<FName> IDs;
    int32 Nodes = 0;
    Outer->Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity, const SPrefabComponent& Node)
    {
        ++Nodes;
        IDs.Add(Node.StableID);
    });
    EXPECT_EQ(Nodes, 5);
    EXPECT_EQ(IDs.Num(), Nodes) << "no two captured nodes may share an id";

    // Adopted as the new prefab's instance, the source refreshes onto itself without spawning or losing a node.
    Outer->RefreshInstance(World, Root);
    int32 Placed = 1;
    Registry.GetHierarchy().ForEachDescendant(Root, [&](ECS::FEntity) { ++Placed; });
    EXPECT_EQ(Placed, 5);
}

// An overridden handle names a world entity, and a world id that happens to equal a prefab id must not be taken for one.
TEST(PrefabInstances, RefreshKeepsAnOverriddenHandleThatCollidesWithAPrefabID)
{
    CWorld* World = MakeWorld();
    CPrefab* Prefab = MakePrefab();
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    const ECS::FEntity PrefabRoot = AddNode(Prefab->Registry, "root");
    const ECS::FEntity PrefabLink = AddNode(Prefab->Registry, "link");
    ECS::Utils::ReparentEntity(Prefab->Registry, PrefabLink, PrefabRoot);
    Prefab->Registry.Emplace<SPrefabLinkTestComponent>(PrefabLink).Target = PrefabRoot;

    // Created first, so its id is the same number as the prefab root's.
    const ECS::FEntity WorldEntity = Registry.Create();
    ASSERT_EQ(WorldEntity.GetIndex(), PrefabRoot.GetIndex());

    const ECS::FEntity Root = AddInstanceNode(Registry, Prefab, "root", true);
    const ECS::FEntity Link = AddInstanceNode(Registry, Prefab, "link", false);
    ECS::Utils::ReparentEntity(Registry, Link, Root);
    Registry.Emplace<SPrefabLinkTestComponent>(Link).Target = WorldEntity;

    SPrefabOverrideComponent& Ledger = Registry.Emplace<SPrefabOverrideComponent>(Root);
    Ledger.PropertyOverrides.Emplace(FName("link"), SPrefabLinkTestComponent::StaticStruct()->GetName(), FName("Target"));

    Prefab->Registry.Get<SPrefabLinkTestComponent>(PrefabLink).Value = 7;
    Prefab->RefreshInstance(World, Root);

    EXPECT_EQ(Registry.Get<SPrefabLinkTestComponent>(Link).Target, WorldEntity) << "the override keeps naming the world entity";
    EXPECT_EQ(Registry.Get<SPrefabLinkTestComponent>(Link).Value, 7) << "an inherited leaf still follows the prefab";
}

// Capture compares handles in the instance's ids, so an inherited handle never reads as edited and a world one never as inherited.
TEST(PrefabInstances, OverrideCaptureComparesHandlesInTheInstancesIDs)
{
    CWorld* World = MakeWorld();
    CPrefab* Prefab = MakePrefab();
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    const ECS::FEntity PrefabRoot = AddNode(Prefab->Registry, "root");
    const ECS::FEntity PrefabLink = AddNode(Prefab->Registry, "link");
    ECS::Utils::ReparentEntity(Prefab->Registry, PrefabLink, PrefabRoot);
    Prefab->Registry.Emplace<SPrefabLinkTestComponent>(PrefabLink).Target = PrefabRoot;

    const ECS::FEntity WorldEntity = Registry.Create();
    ASSERT_EQ(WorldEntity.GetIndex(), PrefabRoot.GetIndex());

    const ECS::FEntity Root = AddInstanceNode(Registry, Prefab, "root", true);
    const ECS::FEntity Link = AddInstanceNode(Registry, Prefab, "link", false);
    ECS::Utils::ReparentEntity(Registry, Link, Root);
    Registry.Emplace<SPrefabLinkTestComponent>(Link).Target = Root;

    auto Paths = [&]()
    {
        TVector<FName> Out;
        for (const SPrefabPropertyOverride& Override : Registry.Get<SPrefabOverrideComponent>(Root).PropertyOverrides)
        {
            Out.Add(Override.PropertyPath);
        }
        return Out;
    };

    Registry.Get<SPrefabLinkTestComponent>(Link).Value = 5;
    CPrefab::RecaptureComponentOverrides(Registry, Link, SPrefabLinkTestComponent::StaticStruct());
    EXPECT_EQ(Paths(), TVector<FName>{ FName("Value") }) << "the inherited handle is not an edit";

    Registry.Get<SPrefabLinkTestComponent>(Link).Target = WorldEntity;
    CPrefab::RecaptureComponentOverrides(Registry, Link, SPrefabLinkTestComponent::StaticStruct());
    const TVector<FName> Recorded = Paths();
    EXPECT_TRUE(Algo::Contains(Recorded, FName("Target"))) << "a world handle sharing the prefab root's number is still an edit";
}

// A prefab the caller already holds spawns placed and parented, with no path for the library to resolve.
TEST(PrefabInstances, SpawnLoadedPrefabPlacesAndParentsTheRoot)
{
    CWorld* World = MakeWorld();
    CPrefab* Prefab = MakePrefab();
    ASSERT_NE(World, nullptr);
    ASSERT_NE(Prefab, nullptr);

    const ECS::FEntity PrefabRoot = AddNode(Prefab->Registry, "root");
    const ECS::FEntity PrefabLeaf = AddNode(Prefab->Registry, "leaf");
    ECS::Utils::ReparentEntity(Prefab->Registry, PrefabLeaf, PrefabRoot);

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Holder = Registry.Create();
    Registry.Emplace<STransformComponent>(Holder);

    const FTransform Placement(FVector3(3.0f, 4.0f, 5.0f));
    const ECS::FEntity Root = World->GetSpawner()->SpawnLoadedPrefabAt(Prefab, Placement, Holder);
    ASSERT_TRUE(Registry.IsValid(Root));

    const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Root);
    ASSERT_NE(Instance, nullptr);
    EXPECT_EQ(Instance->SourcePrefab.Get(), Prefab);
    EXPECT_TRUE(Instance->bIsRoot);
    EXPECT_EQ(Registry.GetHierarchy().GetParent(Root), Holder);

    const FVector3 Location = Registry.Get<STransformComponent>(Root).GetLocalLocation();
    EXPECT_FLOAT_EQ(Location.x, 3.0f);
    EXPECT_FLOAT_EQ(Location.y, 4.0f);
    EXPECT_FLOAT_EQ(Location.z, 5.0f);

    EXPECT_EQ(World->GetSpawner()->SpawnLoadedPrefab(nullptr), ECS::NullEntity);
}
