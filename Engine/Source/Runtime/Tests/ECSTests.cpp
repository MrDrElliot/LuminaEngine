#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"

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

TEST(ECSTests, ResolveTransformChain_GrandchildFollowsRootMove)
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

    // ResolveTransformChain must walk up to the dirty A rather than serving C's stale matrix.
    ECS::Utils::ResolveTransformChain(Registry, C);

    const STransformComponent& WorldC = Registry.Get<STransformComponent>(C);
    const STransformComponent& WorldB = Registry.Get<STransformComponent>(B);
    const STransformComponent& WorldA = Registry.Get<STransformComponent>(A);

    EXPECT_FLOAT_EQ(WorldA.WorldTransform.GetLocation().x, 20.f);
    EXPECT_FLOAT_EQ(WorldB.WorldTransform.GetLocation().x, 25.f);
    EXPECT_FLOAT_EQ(WorldC.WorldTransform.GetLocation().x, 27.f);
    EXPECT_FLOAT_EQ(WorldC.WorldTransform.GetLocation().x - WorldB.WorldTransform.GetLocation().x, 2.f);
}

TEST(ECSTests, ResolveTransformChain_SiblingSubtreeStaysConsistent)
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

    // Resolving via C must also refresh sibling D, which is not dirty itself.
    ECS::Utils::ResolveTransformChain(Registry, C);
    ECS::Utils::ResolveTransformChain(Registry, D);

    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(D).WorldTransform.GetLocation().x, 20.f);
    EXPECT_FLOAT_EQ(Registry.Get<STransformComponent>(D).WorldTransform.GetLocation().y, 3.f);
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
