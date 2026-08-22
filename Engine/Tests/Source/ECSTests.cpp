#include <gtest/gtest.h>
#include <entt/entt.hpp>
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

TEST(ECSTests, Parent_SingleChild)
{
    entt::registry Registry{};

    auto Parent = Registry.create();
    Registry.emplace<STransformComponent>(Parent);

    auto Child = Registry.create();
    Registry.emplace<STransformComponent>(Child);
    ECS::Utils::ReparentEntity(Registry, Child, Parent);

    const auto& ParentRel = Registry.get<FRelationshipComponent>(Parent);
    const auto& ChildRel  = Registry.get<FRelationshipComponent>(Child);

    EXPECT_EQ(ParentRel.Parent, entt::entity{entt::null});
    EXPECT_EQ(ChildRel.Parent, Parent);

    EXPECT_EQ(ParentRel.First, Child);
    EXPECT_EQ(ParentRel.Children, 1);

    EXPECT_EQ(ChildRel.Prev, entt::entity{entt::null});
    EXPECT_EQ(ChildRel.Next, entt::entity{entt::null});
}

TEST(ECSTests, Parent_MultipleChildren_Order)
{
    FEntityRegistry Registry{};

    auto Parent = Registry.create();
    Registry.emplace<STransformComponent>(Parent);

    auto ChildA = Registry.create();
    auto ChildB = Registry.create();
    auto ChildC = Registry.create();

    Registry.emplace<STransformComponent>(ChildA);
    Registry.emplace<STransformComponent>(ChildB);
    Registry.emplace<STransformComponent>(ChildC);

    ECS::Utils::ReparentEntity(Registry, ChildA, Parent);
    ECS::Utils::ReparentEntity(Registry, ChildB, Parent);
    ECS::Utils::ReparentEntity(Registry, ChildC, Parent);

    const auto& ParentRel = Registry.get<FRelationshipComponent>(Parent);

    EXPECT_EQ(ParentRel.Children, 3);
    EXPECT_EQ(ParentRel.First, ChildC);

    const auto& A = Registry.get<FRelationshipComponent>(ChildA);
    const auto& B = Registry.get<FRelationshipComponent>(ChildB);
    const auto& C = Registry.get<FRelationshipComponent>(ChildC);

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
    FEntityRegistry Registry{};

    auto ParentA = Registry.create();
    auto ParentB = Registry.create();
    Registry.emplace<STransformComponent>(ParentA);
    Registry.emplace<STransformComponent>(ParentB);

    auto Child = Registry.create();
    Registry.emplace<STransformComponent>(Child);

    ECS::Utils::ReparentEntity(Registry, Child, ParentA);
    ECS::Utils::ReparentEntity(Registry, Child, ParentB);

    const auto& A = Registry.get<FRelationshipComponent>(ParentA);
    const auto& B = Registry.get<FRelationshipComponent>(ParentB);
    const auto& C = Registry.get<FRelationshipComponent>(Child);

    EXPECT_EQ(C.Parent, ParentB);
    EXPECT_EQ(B.First, Child);
    EXPECT_EQ(B.Children, 1);

    EXPECT_EQ(A.Children, 0);
}

TEST(ECSTests, ResolveTransformChain_GrandchildFollowsRootMove)
{
    FEntityRegistry Registry{};

    auto A = Registry.create();
    auto B = Registry.create();
    auto C = Registry.create();

    Registry.emplace<STransformComponent>(A).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.emplace<STransformComponent>(B).LocalTransform.SetLocation(FVector3(5.f,  0.f, 0.f));
    Registry.emplace<STransformComponent>(C).LocalTransform.SetLocation(FVector3(2.f,  0.f, 0.f));

    // AddToParent only links, while ReparentEntity would bake the zeroed world matrix in.
    ECS::Utils::AddToParent(Registry, B, A);
    ECS::Utils::AddToParent(Registry, C, B);

    Registry.emplace<FNeedsTransformUpdate>(A);
    Registry.emplace<FNeedsTransformUpdate>(B);
    Registry.emplace<FNeedsTransformUpdate>(C);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    EXPECT_FLOAT_EQ(Registry.get<STransformComponent>(C).WorldTransform.GetLocation().x, 17.f);

    Registry.get<STransformComponent>(A).LocalTransform.SetLocation(FVector3(20.f, 0.f, 0.f));
    Registry.emplace_or_replace<FNeedsTransformUpdate>(A);

    // ResolveTransformChain must walk up to the dirty A rather than serving C's stale matrix.
    ECS::Utils::ResolveTransformChain(Registry, C);

    const STransformComponent& WorldC = Registry.get<STransformComponent>(C);
    const STransformComponent& WorldB = Registry.get<STransformComponent>(B);
    const STransformComponent& WorldA = Registry.get<STransformComponent>(A);

    EXPECT_FLOAT_EQ(WorldA.WorldTransform.GetLocation().x, 20.f);
    EXPECT_FLOAT_EQ(WorldB.WorldTransform.GetLocation().x, 25.f);
    EXPECT_FLOAT_EQ(WorldC.WorldTransform.GetLocation().x, 27.f);
    EXPECT_FLOAT_EQ(WorldC.WorldTransform.GetLocation().x - WorldB.WorldTransform.GetLocation().x, 2.f);
}

TEST(ECSTests, ResolveTransformChain_SiblingSubtreeStaysConsistent)
{
    FEntityRegistry Registry{};

    auto A = Registry.create();
    auto B = Registry.create();
    auto C = Registry.create();
    auto D = Registry.create();

    Registry.emplace<STransformComponent>(A).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.emplace<STransformComponent>(B).LocalTransform.SetLocation(FVector3(5.f,  0.f, 0.f));
    Registry.emplace<STransformComponent>(C).LocalTransform.SetLocation(FVector3(2.f,  0.f, 0.f));
    Registry.emplace<STransformComponent>(D).LocalTransform.SetLocation(FVector3(0.f,  3.f, 0.f));

    ECS::Utils::AddToParent(Registry, B, A);
    ECS::Utils::AddToParent(Registry, C, B);
    ECS::Utils::AddToParent(Registry, D, A);

    Registry.emplace<FNeedsTransformUpdate>(A);
    Registry.emplace<FNeedsTransformUpdate>(B);
    Registry.emplace<FNeedsTransformUpdate>(C);
    Registry.emplace<FNeedsTransformUpdate>(D);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);

    Registry.get<STransformComponent>(A).LocalTransform.SetLocation(FVector3(20.f, 0.f, 0.f));
    Registry.emplace_or_replace<FNeedsTransformUpdate>(A);

    // Resolving via C must also refresh sibling D, which is not dirty itself.
    ECS::Utils::ResolveTransformChain(Registry, C);
    ECS::Utils::ResolveTransformChain(Registry, D);

    EXPECT_FLOAT_EQ(Registry.get<STransformComponent>(D).WorldTransform.GetLocation().x, 20.f);
    EXPECT_FLOAT_EQ(Registry.get<STransformComponent>(D).WorldTransform.GetLocation().y, 3.f);
}

TEST(ECSTests, Parent_Unparent)
{
    FEntityRegistry Registry{};

    auto Parent = Registry.create();
    Registry.emplace<STransformComponent>(Parent);

    auto Child = Registry.create();
    Registry.emplace<STransformComponent>(Child);

    ECS::Utils::ReparentEntity(Registry, Child, Parent);
    ECS::Utils::ReparentEntity(Registry, Child, entt::null);

    const auto& ParentRel = Registry.get<FRelationshipComponent>(Parent);
    const auto& ChildRel  = Registry.get<FRelationshipComponent>(Child);

    EXPECT_EQ(ChildRel.Parent, entt::entity{entt::null});
    EXPECT_EQ(ParentRel.Children, 0);
    EXPECT_EQ(ParentRel.First, entt::entity{entt::null});
}

// After a clean phase the on_construct hook must re-arm bAnyDirty for the lazy read.
TEST(ECSTests, LazyResolve_AfterCleanPhase_SeesUpdatedWorld)
{
    FEntityRegistry Registry{};

    entt::entity Parent = Registry.create();
    entt::entity Child  = Registry.create();
    Registry.emplace<STransformComponent>(Parent).LocalTransform.SetLocation(FVector3(10.f, 0.f, 0.f));
    Registry.emplace<STransformComponent>(Child).LocalTransform.SetLocation(FVector3(5.f, 0.f, 0.f));
    ECS::Utils::AddToParent(Registry, Child, Parent);

    Registry.get<STransformComponent>(Parent).Bind(Registry, Parent);
    Registry.get<STransformComponent>(Child).Bind(Registry, Child);

    // Clean phase resolves everything, arming the lock-free fast path.
    Registry.emplace<FNeedsTransformUpdate>(Parent);
    Registry.emplace<FNeedsTransformUpdate>(Child);
    ECS::Utils::ResolveAllDirtyTransforms(Registry);
    EXPECT_FLOAT_EQ(Registry.get<STransformComponent>(Child).GetWorldLocation().x, 15.f);

    // Gameplay move through the normal setter -> MarkTransformDirty -> hook re-arms bAnyDirty.
    Registry.get<STransformComponent>(Parent).SetLocalLocation(FVector3(20.f, 0.f, 0.f));

    // The child isn't dirty itself; the read must still walk up, see the dirty parent, and resolve.
    EXPECT_FLOAT_EQ(Registry.get<STransformComponent>(Child).GetWorldLocation().x, 25.f);
}
