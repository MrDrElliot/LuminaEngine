#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Versioning/CoreVersion.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

using namespace Lumina;

namespace
{
    ECS::FHierarchyNode LinkOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const ECS::FHierarchyNode* Node = Registry.GetHierarchy().Find(Entity);
        return Node != nullptr ? *Node : ECS::FHierarchyNode();
    }
}

TEST(ECSTests, Parent_SingleChild)
{
    ECS::FRegistry Registry{};

    auto Parent = Registry.Create();
    Registry.Emplace<STransformComponent>(Parent);

    auto Child = Registry.Create();
    Registry.Emplace<STransformComponent>(Child);
    ECS::Utils::ReparentEntity(Registry, Child, Parent);

    const ECS::FHierarchyNode ParentNode = LinkOf(Registry, Parent);
    const ECS::FHierarchyNode ChildNode = LinkOf(Registry, Child);

    EXPECT_EQ(ParentNode.Parent, ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(ChildNode.Parent, Parent);

    EXPECT_EQ(ParentNode.First, Child);
    EXPECT_EQ(ParentNode.ChildCount, 1u);

    EXPECT_EQ(ChildNode.Prev, ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(ChildNode.Next, ECS::FEntity{ECS::NullEntity});
}

TEST(ECSTests, Parent_MultipleChildren_Order)
{
    ECS::FRegistry Registry{};

    auto Parent = Registry.Create();
    Registry.Emplace<STransformComponent>(Parent);

    auto ChildA = Registry.Create();
    auto ChildB = Registry.Create();
    auto ChildC = Registry.Create();

    Registry.Emplace<STransformComponent>(ChildA);
    Registry.Emplace<STransformComponent>(ChildB);
    Registry.Emplace<STransformComponent>(ChildC);

    ECS::Utils::ReparentEntity(Registry, ChildA, Parent);
    ECS::Utils::ReparentEntity(Registry, ChildB, Parent);
    ECS::Utils::ReparentEntity(Registry, ChildC, Parent);

    const ECS::FHierarchyNode ParentNode = LinkOf(Registry, Parent);

    EXPECT_EQ(ParentNode.ChildCount, 3u);
    EXPECT_EQ(ParentNode.First, ChildA) << "children append, so the list keeps attach order";
    EXPECT_EQ(ParentNode.Last, ChildC);

    const ECS::FHierarchyNode A = LinkOf(Registry, ChildA);
    const ECS::FHierarchyNode B = LinkOf(Registry, ChildB);
    const ECS::FHierarchyNode C = LinkOf(Registry, ChildC);

    EXPECT_EQ(A.Parent, Parent);
    EXPECT_EQ(B.Parent, Parent);
    EXPECT_EQ(C.Parent, Parent);

    EXPECT_EQ(A.Next, ChildB);
    EXPECT_EQ(B.Prev, ChildA);
    EXPECT_EQ(B.Next, ChildC);
    EXPECT_EQ(C.Prev, ChildB);
    EXPECT_EQ(Registry.GetHierarchy().GetSiblingIndex(ChildC), 2u);
}

TEST(ECSTests, Parent_Reparent_MovesCorrectly)
{
    ECS::FRegistry Registry{};

    auto ParentA = Registry.Create();
    auto ParentB = Registry.Create();
    Registry.Emplace<STransformComponent>(ParentA);
    Registry.Emplace<STransformComponent>(ParentB);

    auto Child = Registry.Create();
    Registry.Emplace<STransformComponent>(Child);

    ECS::Utils::ReparentEntity(Registry, Child, ParentA);
    ECS::Utils::ReparentEntity(Registry, Child, ParentB);

    const ECS::FHierarchyNode A = LinkOf(Registry, ParentA);
    const ECS::FHierarchyNode B = LinkOf(Registry, ParentB);
    const ECS::FHierarchyNode C = LinkOf(Registry, Child);

    EXPECT_EQ(C.Parent, ParentB);
    EXPECT_EQ(B.First, Child);
    EXPECT_EQ(B.ChildCount, 1u);

    EXPECT_EQ(A.ChildCount, 0u);
}

TEST(ECSTests, ComputeWorldTransform_GrandchildFollowsRootMoveWithoutWriting)
{
    ECS::FRegistry Registry{};

    auto A = Registry.Create();
    auto B = Registry.Create();
    auto C = Registry.Create();

    Registry.Emplace<STransformComponent>(A).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.Emplace<STransformComponent>(B).LocalTransform.SetLocation(FVector3(5.f,  0.f, 0.f));
    Registry.Emplace<STransformComponent>(C).LocalTransform.SetLocation(FVector3(2.f,  0.f, 0.f));

    // AddToParent only links, while ReparentEntity would bake the zeroed world matrix in.
    Registry.AttachChild(B, A);
    Registry.AttachChild(C, B);

    Registry.Emplace<FNeedsTransformUpdate>(A);
    Registry.Emplace<FNeedsTransformUpdate>(B);
    Registry.Emplace<FNeedsTransformUpdate>(C);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(C).WorldTransform.GetLocation().x, 17.f);

    Registry.Get<STransformComponent>(A).LocalTransform.SetLocation(FVector3(20.f, 0.f, 0.f));
    Registry.EmplaceOrReplace<FNeedsTransformUpdate>(A);

    // The compute must walk up to the dirty A rather than serving C's stale matrix.
    EXPECT_FLOAT_EQ(ECS::Utils::ComputeWorldTransform(Registry, A).GetLocation().x, 20.f);
    EXPECT_FLOAT_EQ(ECS::Utils::ComputeWorldTransform(Registry, B).GetLocation().x, 25.f);
    EXPECT_FLOAT_EQ(ECS::Utils::ComputeWorldTransform(Registry, C).GetLocation().x, 27.f);

    // Only a propagation writes, so the cached value still shows the last one.
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(C).WorldTransform.GetLocation().x, 17.f);

    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(C).WorldTransform.GetLocation().x, 27.f);
}

TEST(ECSTests, ChildrenLinkedBeforeTheDirtyStateStillResolve)
{
    ECS::FRegistry Registry{};

    auto A = Registry.Create();
    auto B = Registry.Create();
    Registry.Emplace<STransformComponent>(A).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.Emplace<STransformComponent>(B).LocalTransform.SetLocation(FVector3(5.f,  0.f, 0.f));

    // A level load links its children this way, before anything has created the transform dirty state.
    Registry.AttachChild(B, A);

    // Two resolves, since the first clears the dirty flag and a child it missed would then never resolve.
    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(B).WorldTransform.GetLocation().x, 15.f);
    EXPECT_FLOAT_EQ(ECS::Utils::ComputeWorldTransform(Registry, B).GetLocation().x, 15.f);
}

TEST(ECSTests, ComputeWorldTransform_SiblingOfAMovedChainIsCurrent)
{
    ECS::FRegistry Registry{};

    auto A = Registry.Create();
    auto B = Registry.Create();
    auto C = Registry.Create();
    auto D = Registry.Create();

    Registry.Emplace<STransformComponent>(A).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.Emplace<STransformComponent>(B).LocalTransform.SetLocation(FVector3(5.f,  0.f, 0.f));
    Registry.Emplace<STransformComponent>(C).LocalTransform.SetLocation(FVector3(2.f,  0.f, 0.f));
    Registry.Emplace<STransformComponent>(D).LocalTransform.SetLocation(FVector3(0.f,  3.f, 0.f));

    Registry.AttachChild(B, A);
    Registry.AttachChild(C, B);
    Registry.AttachChild(D, A);

    Registry.Emplace<FNeedsTransformUpdate>(A);
    Registry.Emplace<FNeedsTransformUpdate>(B);
    Registry.Emplace<FNeedsTransformUpdate>(C);
    Registry.Emplace<FNeedsTransformUpdate>(D);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    Registry.Get<STransformComponent>(A).LocalTransform.SetLocation(FVector3(20.f, 0.f, 0.f));
    Registry.EmplaceOrReplace<FNeedsTransformUpdate>(A);

    // D is not dirty itself, but its parent is, so its computed world must include the move.
    const FTransform WorldD = ECS::Utils::ComputeWorldTransform(Registry, D);
    EXPECT_FLOAT_EQ(WorldD.GetLocation().x, 20.f);
    EXPECT_FLOAT_EQ(WorldD.GetLocation().y, 3.f);

    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(D).WorldTransform.GetLocation().x, WorldD.GetLocation().x);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(D).WorldTransform.GetLocation().y, WorldD.GetLocation().y);
}

TEST(ECSTests, ComputeWorldTransform_MatchesThePropagationAcrossADeepRotatedChain)
{
    ECS::FRegistry Registry{};

    constexpr int32 Depth = 12;
    TVector<ECS::FEntity> Chain;
    for (int32 i = 0; i < Depth; ++i)
    {
        const ECS::FEntity E = Registry.Create();
        FTransform Local;
        Local.SetLocation(FVector3(1.0f, 0.5f * (float)i, 0.0f));
        Local.SetRotation(FQuat(Math::Radians(FVector3(0.0f, 15.0f, 0.0f))));
        Registry.Emplace<STransformComponent>(E).LocalTransform = Local;
        if (!Chain.empty())
        {
            Registry.AttachChild(E, Chain.back());
        }
        Registry.Emplace<FNeedsTransformUpdate>(E);
        Chain.push_back(E);
    }
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    // A move halfway down leaves everything above it clean, which is the base the compute must start from.
    Registry.Get<STransformComponent>(Chain[Depth / 2]).LocalTransform.SetLocation(FVector3(3.0f, -2.0f, 1.0f));
    Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Chain[Depth / 2]);

    TVector<FVector3> Computed;
    for (ECS::FEntity E : Chain)
    {
        Computed.push_back(ECS::Utils::ComputeWorldTransform(Registry, E).GetLocation());
    }

    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    for (int32 i = 0; i < Depth; ++i)
    {
        const FVector3 Resolved = Registry.Get<STransformComponent>(Chain[i]).WorldTransform.GetLocation();
        EXPECT_NEAR(Computed[i].x, Resolved.x, 1e-4f) << "level " << i;
        EXPECT_NEAR(Computed[i].y, Resolved.y, 1e-4f) << "level " << i;
        EXPECT_NEAR(Computed[i].z, Resolved.z, 1e-4f) << "level " << i;
    }
}

TEST(ECSTests, Parent_Unparent)
{
    ECS::FRegistry Registry{};

    auto Parent = Registry.Create();
    Registry.Emplace<STransformComponent>(Parent);

    auto Child = Registry.Create();
    Registry.Emplace<STransformComponent>(Child);

    ECS::Utils::ReparentEntity(Registry, Child, Parent);
    ECS::Utils::ReparentEntity(Registry, Child, ECS::NullEntity);

    const ECS::FHierarchyNode ParentNode = LinkOf(Registry, Parent);
    const ECS::FHierarchyNode ChildNode = LinkOf(Registry, Child);

    EXPECT_EQ(ChildNode.Parent, ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(ParentNode.ChildCount, 0u);
    EXPECT_EQ(ParentNode.First, ECS::FEntity{ECS::NullEntity});
}

// After a clean phase the on_construct hook must re-arm bAnyDirty for the lazy read.
TEST(ECSTests, LazyResolve_AfterCleanPhase_SeesUpdatedWorld)
{
    ECS::FRegistry Registry{};

    ECS::FEntity Parent = Registry.Create();
    ECS::FEntity Child  = Registry.Create();
    Registry.Emplace<STransformComponent>(Parent).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.Emplace<STransformComponent>(Child).LocalTransform.SetLocation(FVector3(5.f, 0.f, 0.f));
    Registry.AttachChild(Child, Parent);

    Registry.Get<STransformComponent>(Parent).Bind(Registry, Parent);
    Registry.Get<STransformComponent>(Child).Bind(Registry, Child);

    // Clean phase resolves everything, arming the lock-free fast path.
    Registry.Emplace<FNeedsTransformUpdate>(Parent);
    Registry.Emplace<FNeedsTransformUpdate>(Child);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(Child).GetWorldLocation().x, 15.f);

    // Gameplay move through the normal setter -> MarkTransformDirty -> hook re-arms bAnyDirty.
    Registry.Get<STransformComponent>(Parent).SetLocalLocation(FVector3(20.f, 0.f, 0.f));

    // The child isn't dirty itself; the read must still walk up, see the dirty parent, and resolve.
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(Child).GetWorldLocation().x, 25.f);
}

// A swap takes the other registry's singletons too, which is why InitializeWorld carries the settings by hand.
TEST(ECSTests, RenderSettings_SurviveRegistrySwap)
{
    ECS::FRegistry Live{};
    ECS::FRegistry Pending{};

    Live.Ctx().Emplace<FSceneRenderSettings>().Flags = ERenderSceneDebugFlags::Meshlets;
    Pending.Create();

    const FSceneRenderSettings Carried = Live.Ctx().Get<FSceneRenderSettings>();

    Live.Swap(Pending);

    EXPECT_FALSE(Live.Ctx().Contains<FSceneRenderSettings>());

    Live.Ctx().Emplace<FSceneRenderSettings>(Carried);

    EXPECT_TRUE(Live.Ctx().Contains<FSceneRenderSettings>());
    EXPECT_EQ((uint8)Live.Ctx().Get<FSceneRenderSettings>().Flags, (uint8)ERenderSceneDebugFlags::Meshlets);
}

namespace
{
    // Stands in for the renderer pointer CreateRenderer publishes and DestroyRenderer clears.
    struct FFakeRendererHandle { int Unused = 0; };
}

// A renderer torn down and rebuilt has to find the edited values again rather than fresh defaults.
TEST(ECSTests, RenderSettings_OutliveTheirReader)
{
    ECS::FRegistry Registry{};

    Registry.Ctx().GetOrEmplace<FSceneRenderSettings>().bDrawBillboards = false;

    Registry.Ctx().Emplace<FFakeRendererHandle>();
    EXPECT_TRUE(Registry.Ctx().Erase<FFakeRendererHandle>());

    EXPECT_TRUE(Registry.Ctx().Contains<FSceneRenderSettings>());

    const bool bDrawBillboards = Registry.Ctx().GetOrEmplace<FSceneRenderSettings>().bDrawBillboards;
    EXPECT_FALSE(bDrawBillboards);
}

namespace
{
    struct FEpochProbe
    {
        int32 Value = 0;
    };

    const uint32* EpochOf(const ECS::FRegistry& Registry)
    {
        const ECS::FSparseSet* Pool = Registry.FindStorage(ECS::GetComponentTypeID<FEpochProbe>());
        return Pool != nullptr ? Pool->GetLayoutEpoch() : nullptr;
    }
}

TEST(ECSTests, LayoutEpoch_ChangesWhenAPackedPoolRelocates)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity First = Registry.Create();
    const FEpochProbe* Before = &Registry.Emplace<FEpochProbe>(First);
    const uint32* Epoch = EpochOf(Registry);
    ASSERT_NE(Epoch, nullptr);
    const uint32 Initial = *Epoch;

    for (int32 Index = 0; Index < 4096; ++Index)
    {
        Registry.Emplace<FEpochProbe>(Registry.Create());
    }

    // An in-place widen keeps every address, and only a move owes the view a revalidation.
    const FEpochProbe* After = Registry.TryGet<FEpochProbe>(First);
    if (After != Before)
    {
        EXPECT_NE(*Epoch, Initial);
    }
}

TEST(ECSTests, LayoutEpoch_ChangesOnRemoval)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    Registry.Emplace<FEpochProbe>(A).Value = 1;
    Registry.Emplace<FEpochProbe>(B).Value = 2;

    const uint32* Epoch = EpochOf(Registry);
    ASSERT_NE(Epoch, nullptr);
    const uint32 Before = *Epoch;

    Registry.Remove<FEpochProbe>(A);
    EXPECT_NE(*Epoch, Before);
    EXPECT_EQ(Registry.Get<FEpochProbe>(B).Value, 2);
}

TEST(ECSTests, LayoutEpoch_ReadsDeadAfterThePoolIsDestroyed)
{
    const uint32* Epoch = nullptr;
    {
        ECS::FRegistry Registry{};
        Registry.Emplace<FEpochProbe>(Registry.Create());
        Epoch = EpochOf(Registry);
        ASSERT_NE(Epoch, nullptr);
        EXPECT_NE(*Epoch, ECS::FSparseSet::DeadLayoutEpoch);
    }

    EXPECT_EQ(*Epoch, ECS::FSparseSet::DeadLayoutEpoch);
}

TEST(ECSTests, Hierarchy_PreorderKeepsEverySubtreeContiguous)
{
    ECS::FRegistry Registry{};

    const ECS::FEntity Root = Registry.Create();
    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    const ECS::FEntity A1 = Registry.Create();
    const ECS::FEntity A2 = Registry.Create();
    const ECS::FEntity Other = Registry.Create();
    const ECS::FEntity OtherChild = Registry.Create();

    Registry.AttachChild(A, Root);
    Registry.AttachChild(B, Root);
    Registry.AttachChild(A1, A);
    Registry.AttachChild(A2, A);
    Registry.AttachChild(OtherChild, Other);

    const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
    Hierarchy.EnsureOrder();

    const TVector<ECS::FEntity>& Order = Hierarchy.GetOrder();
    ASSERT_EQ(Order.size(), 7u);

    for (uint32 Slot = 0; Slot < (uint32)Order.size(); ++Slot)
    {
        const uint32 Parent = Hierarchy.GetParentSlots()[Slot];
        if (Parent != ECS::FHierarchy::NoSlot)
        {
            EXPECT_LT(Parent, Slot) << "a parent precedes its children";
            EXPECT_LT(Slot, Parent + Hierarchy.GetSubtreeSizes()[Parent]) << "a child sits inside its parent's range";
            EXPECT_EQ(Order[Parent], Hierarchy.GetParent(Order[Slot]));
        }
    }

    EXPECT_EQ(Hierarchy.GetSubtreeSizes()[Hierarchy.GetSlot(Root)], 5u);
    EXPECT_EQ(Hierarchy.GetSubtreeSizes()[Hierarchy.GetSlot(A)], 3u);
    EXPECT_EQ(Hierarchy.GetSlot(A) + 1, Hierarchy.GetSlot(A1)) << "siblings keep their list order";
    EXPECT_EQ(Hierarchy.GetSlot(A1) + 1, Hierarchy.GetSlot(A2));
}

TEST(ECSTests, Hierarchy_AttachRefusesACycle)
{
    ECS::FRegistry Registry{};

    const ECS::FEntity Parent = Registry.Create();
    const ECS::FEntity Child = Registry.Create();
    ASSERT_TRUE(Registry.AttachChild(Child, Parent));

    EXPECT_FALSE(Registry.AttachChild(Parent, Child));
    EXPECT_FALSE(Registry.AttachChild(Parent, Parent));
    EXPECT_EQ(Registry.GetHierarchy().GetParent(Parent), ECS::FEntity{ECS::NullEntity});
}

TEST(ECSTests, Hierarchy_DestroyOrphansChildrenUnlessCascading)
{
    {
        ECS::FRegistry Registry{};
        const ECS::FEntity Parent = Registry.Create();
        const ECS::FEntity Child = Registry.Create();
        Registry.AttachChild(Child, Parent);

        Registry.Destroy(Parent);
        EXPECT_TRUE(Registry.IsValid(Child));
        EXPECT_FALSE(Registry.GetHierarchy().IsLinked(Child));
        EXPECT_EQ(Registry.GetHierarchy().NumLinked(), 0u);
    }

    {
        ECS::FRegistry Registry{};
        Registry.SetDestroyDescendantsWithParent(true);

        const ECS::FEntity Root = Registry.Create();
        const ECS::FEntity Parent = Registry.Create();
        const ECS::FEntity Child = Registry.Create();
        const ECS::FEntity Sibling = Registry.Create();
        Registry.AttachChild(Parent, Root);
        Registry.AttachChild(Sibling, Root);
        Registry.AttachChild(Child, Parent);

        Registry.Destroy(Parent);
        EXPECT_FALSE(Registry.IsValid(Child));
        EXPECT_TRUE(Registry.IsValid(Sibling));
        EXPECT_EQ(Registry.GetHierarchy().GetChildCount(Root), 1u);
        EXPECT_EQ(Registry.GetHierarchy().GetFirstChild(Root), Sibling);
    }
}

TEST(ECSTests, Resolve_DirtyChildUnderDirtyParentComposesWithTheNewParent)
{
    ECS::FRegistry Registry{};

    const ECS::FEntity Root = Registry.Create();
    const ECS::FEntity Child = Registry.Create();
    const ECS::FEntity Grandchild = Registry.Create();
    Registry.Emplace<STransformComponent>(Root);
    Registry.Emplace<STransformComponent>(Child).LocalTransform.SetLocation(FVector3(1.f, 0.f, 0.f));
    Registry.Emplace<STransformComponent>(Grandchild).LocalTransform.SetLocation(FVector3(1.f, 0.f, 0.f));
    Registry.AttachChild(Child, Root);
    Registry.AttachChild(Grandchild, Child);

    Registry.Emplace<FNeedsTransformUpdate>(Root);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(Grandchild).WorldTransform.GetLocation().x, 2.f);

    // Both ends move, and the grandchild must compose with the root's new world rather than its cached one.
    Registry.Get<STransformComponent>(Grandchild).LocalTransform.SetLocation(FVector3(3.f, 0.f, 0.f));
    Registry.Emplace<FNeedsTransformUpdate>(Grandchild);
    Registry.Get<STransformComponent>(Root).LocalTransform.SetLocation(FVector3(100.f, 0.f, 0.f));
    Registry.Emplace<FNeedsTransformUpdate>(Root);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(Child).WorldTransform.GetLocation().x, 101.f);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(Grandchild).WorldTransform.GetLocation().x, 104.f);
    EXPECT_FALSE(Registry.Get<STransformComponent>(Grandchild).bWorldDirty);
}

TEST(ECSTests, Hierarchy_SurvivesASaveAndLoad)
{
    ECS::FRegistry Source{};
    const ECS::FEntity Root = Source.Create();
    const ECS::FEntity A = Source.Create();
    const ECS::FEntity B = Source.Create();
    const ECS::FEntity Leaf = Source.Create();
    for (const ECS::FEntity E : { Root, A, B, Leaf })
    {
        Source.Emplace<STransformComponent>(E);
    }
    Source.AttachChild(A, Root);
    Source.AttachChild(B, Root, 0);
    Source.AttachChild(Leaf, A);

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        ASSERT_TRUE(ECS::Utils::SerializeRegistry(Writer, Source));
    }

    ECS::FRegistry Loaded{};
    {
        FMemoryReader Reader(Bytes);
        ASSERT_TRUE(ECS::Utils::SerializeRegistry(Reader, Loaded));
    }

    const ECS::FHierarchy& Hierarchy = Loaded.GetHierarchy();
    EXPECT_EQ(Hierarchy.NumLinked(), 4u);
    EXPECT_EQ(Hierarchy.GetParent(A), Root);
    EXPECT_EQ(Hierarchy.GetParent(Leaf), A);
    EXPECT_EQ(Hierarchy.GetChildCount(Root), 2u);
    EXPECT_EQ(Hierarchy.GetFirstChild(Root), B) << "sibling order is part of the saved links";
    EXPECT_EQ(Hierarchy.GetNextSibling(B), A);
    EXPECT_FALSE(ECS::Utils::IsEntityTransformFlat(Loaded, Leaf));

    Hierarchy.EnsureOrder();
    EXPECT_EQ(Hierarchy.GetSubtreeSizes()[Hierarchy.GetSlot(Root)], 4u);
}

TEST(ECSTests, Hierarchy_LoadsTheLegacyRelationshipLayout)
{
    const ECS::FEntity Root(0, 0), A(1, 0), B(2, 0);

    // Children listed after their parent's First, as the removed component saved them, with B ahead of A.
    struct FLegacyEntity { ECS::FEntity Self, First, Prev, Next, Parent; size_t Children; };
    const FLegacyEntity Legacy[] = {
        { A,    ECS::NullEntity, B,               ECS::NullEntity, Root, 0 },
        { Root, B,               ECS::NullEntity, ECS::NullEntity, ECS::NullEntity, 2 },
        { B,    ECS::NullEntity, ECS::NullEntity, A,               Root, 0 },
    };

    TVector<uint8> Bytes;
    {
        FMemoryWriter MemoryWriter(Bytes);
        FArchive& Writer = MemoryWriter;
        int32 Count = 3;
        Writer << Count;
        for (FLegacyEntity Entry : Legacy)
        {
            const int64 SizePos = Writer.Tell();
            int64 Size = 0;
            Writer << Size;
            const int64 Start = Writer.Tell();

            bool bHasRelationship = true;
            size_t NumComponents = 0;
            Writer << Entry.Self << bHasRelationship << Entry.Children << Entry.First << Entry.Prev << Entry.Next << Entry.Parent << NumComponents;

            const int64 End = Writer.Tell();
            Size = End - Start;
            Writer.Seek(SizePos);
            Writer << Size;
            Writer.Seek(End);
        }
    }

    ECS::FRegistry Loaded{};
    {
        FMemoryReader Reader(Bytes);
        Reader.SetFileVersion((int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE);
        ASSERT_TRUE(ECS::Utils::SerializeRegistry(Reader, Loaded));
    }

    const ECS::FHierarchy& Hierarchy = Loaded.GetHierarchy();
    EXPECT_EQ(Hierarchy.GetParent(A), Root);
    EXPECT_EQ(Hierarchy.GetParent(B), Root);
    EXPECT_EQ(Hierarchy.GetChildCount(Root), 2u);
    EXPECT_EQ(Hierarchy.GetFirstChild(Root), B) << "the saved sibling chain decides the order, not the file order";
    EXPECT_EQ(Hierarchy.GetNextSibling(B), A);
}

TEST(ECSTests, MaxLiveIndexSkipsTombstones)
{
    ECS::FRegistry Registry{};
    TVector<ECS::FEntity> Entities;
    for (int32 i = 0; i < 21; ++i)
    {
        Entities.push_back(Registry.Create());
        Registry.Emplace<STransformComponent>(Entities.back());
    }

    const ECS::FSparseSet* Pool = Registry.FindStorage<STransformComponent>().GetSet();
    ASSERT_NE(Pool, nullptr);
    EXPECT_EQ(Pool->MaxLiveIndex(), Entities.back().GetIndex());

    // The removed slot stays in the dense array as a tombstone whose index bits link the free list.
    Registry.Remove<STransformComponent>(Entities.back());
    Registry.Remove<STransformComponent>(Entities[7]);
    EXPECT_EQ(Pool->MaxLiveIndex(), Entities[19].GetIndex());

    for (const ECS::FEntity Entity : Entities)
    {
        Registry.Remove<STransformComponent>(Entity);
    }
    EXPECT_EQ(Pool->MaxLiveIndex(), 0u);
}
