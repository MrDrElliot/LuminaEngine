#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

using namespace Lumina;

TEST(ECSTests, Parent_SingleChild)
{
    ECS::FRegistry Registry{};

    auto Parent = Registry.Create();
    Registry.Emplace<STransformComponent>(Parent);

    auto Child = Registry.Create();
    Registry.Emplace<STransformComponent>(Child);
    ECS::Utils::ReparentEntity(Registry, Child, Parent);

    const auto& ParentRel = Registry.Get<FRelationshipComponent>(Parent);
    const auto& ChildRel  = Registry.Get<FRelationshipComponent>(Child);

    EXPECT_EQ(ParentRel.Parent, ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(ChildRel.Parent, Parent);

    EXPECT_EQ(ParentRel.First, Child);
    EXPECT_EQ(ParentRel.Children, 1);

    EXPECT_EQ(ChildRel.Prev, ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(ChildRel.Next, ECS::FEntity{ECS::NullEntity});
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

    const auto& ParentRel = Registry.Get<FRelationshipComponent>(Parent);

    EXPECT_EQ(ParentRel.Children, 3);
    EXPECT_EQ(ParentRel.First, ChildC);

    const auto& A = Registry.Get<FRelationshipComponent>(ChildA);
    const auto& B = Registry.Get<FRelationshipComponent>(ChildB);
    const auto& C = Registry.Get<FRelationshipComponent>(ChildC);

    EXPECT_EQ(A.Parent, Parent);
    EXPECT_EQ(B.Parent, Parent);
    EXPECT_EQ(C.Parent, Parent);

    EXPECT_EQ(C.Next, ChildB);
    EXPECT_EQ(B.Prev, ChildC);
    EXPECT_EQ(B.Next, ChildA);
    EXPECT_EQ(A.Prev, ChildB);
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

    const auto& A = Registry.Get<FRelationshipComponent>(ParentA);
    const auto& B = Registry.Get<FRelationshipComponent>(ParentB);
    const auto& C = Registry.Get<FRelationshipComponent>(Child);

    EXPECT_EQ(C.Parent, ParentB);
    EXPECT_EQ(B.First, Child);
    EXPECT_EQ(B.Children, 1);

    EXPECT_EQ(A.Children, 0);
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
    ECS::Utils::AddToParent(Registry, B, A);
    ECS::Utils::AddToParent(Registry, C, B);

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

    ECS::Utils::AddToParent(Registry, B, A);
    ECS::Utils::AddToParent(Registry, C, B);
    ECS::Utils::AddToParent(Registry, D, A);

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
            ECS::Utils::AddToParent(Registry, E, Chain.back());
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

    const auto& ParentRel = Registry.Get<FRelationshipComponent>(Parent);
    const auto& ChildRel  = Registry.Get<FRelationshipComponent>(Child);

    EXPECT_EQ(ChildRel.Parent, ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(ParentRel.Children, 0);
    EXPECT_EQ(ParentRel.First, ECS::FEntity{ECS::NullEntity});
}

// After a clean phase the on_construct hook must re-arm bAnyDirty for the lazy read.
TEST(ECSTests, LazyResolve_AfterCleanPhase_SeesUpdatedWorld)
{
    ECS::FRegistry Registry{};

    ECS::FEntity Parent = Registry.Create();
    ECS::FEntity Child  = Registry.Create();
    Registry.Emplace<STransformComponent>(Parent).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.Emplace<STransformComponent>(Child).LocalTransform.SetLocation(FVector3(5.f, 0.f, 0.f));
    ECS::Utils::AddToParent(Registry, Child, Parent);

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
