#include <gtest/gtest.h>
#include "Core/Object/ObjectCore.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/PersistentEntities.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/PersistentComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    CWorld* NewTestWorld()
    {
        // The carry copies only storages it can name, and these register on first use.
        STransformComponent::StaticStruct();
        SPersistentComponent::StaticStruct();
        SCameraFollowComponent::StaticStruct();
        SEntityScriptComponent::StaticStruct();
        STagComponent::StaticStruct();
        return NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    ECS::FEntity MakeEntity(ECS::FRegistry& Registry, FVector3 Location)
    {
        const ECS::FEntity Entity = Registry.Create();
        FTransform Transform;
        Transform.SetLocation(Location);
        Registry.Emplace<STransformComponent>(Entity, Transform);
        return Entity;
    }

    ECS::FEntity MakePersistent(ECS::FRegistry& Registry, FVector3 Location)
    {
        const ECS::FEntity Entity = MakeEntity(Registry, Location);
        Registry.Emplace<SPersistentComponent>(Entity);
        ECS::Utils::SetEntityTag(Registry, Entity, FName("Hero"));
        return Entity;
    }

    TVector<ECS::FEntity> Heroes(CWorld* World)
    {
        TVector<ECS::FEntity> Found;
        World->GetEntitiesByTag(FName("Hero"), Found);
        return Found;
    }
}

TEST(PersistentEntities, TheSameScriptInstanceAndChildrenArriveInTheNextWorld)
{
    CWorld* From = NewTestWorld();
    CWorld* To = NewTestWorld();
    ECS::FRegistry& Old = From->GetEntityRegistry();

    const ECS::FEntity Hero = MakePersistent(Old, FVector3(1.0f, 2.0f, 3.0f));
    const ECS::FEntity Child = MakeEntity(Old, FVector3(0.0f));
    ECS::Utils::ReparentEntity(Old, Child, Hero, /*bPreserveWorld*/ false);
    const ECS::FEntity LevelOnly = MakeEntity(Old, FVector3(0.0f));
    Old.Emplace<SCameraFollowComponent>(Hero).Target = LevelOnly;
    CEntityScript* Script = EntityScripts::Attach(Old, Hero, CEntityScript::StaticClass());
    ASSERT_NE(Script, nullptr);

    FPersistentCarry Carry;
    Carry.Lift(From);
    EXPECT_TRUE(Old.Get<SEntityScriptComponent>(Hero).Scripts.IsEmpty()) << "the old world's teardown must find nothing to detach";
    Carry.Land(To);

    ECS::FRegistry& New = To->GetEntityRegistry();
    const TVector<ECS::FEntity> Arrived = Heroes(To);
    ASSERT_EQ(Arrived.Num(), 1);
    const ECS::FEntity Arrival = Arrived[0];
    EXPECT_EQ(New.NumEntities(), 2) << "only the persistent entity and its child travel";

    const SEntityScriptComponent& Scripts = New.Get<SEntityScriptComponent>(Arrival);
    ASSERT_EQ(Scripts.Scripts.Num(), 1);
    EXPECT_EQ(Scripts.Scripts[0].Get(), Script);
    EXPECT_EQ(Script->GetOwningEntity(), Arrival);
    EXPECT_EQ(Script->GetWorld(), To);

    const ECS::FEntity ArrivedChild = New.GetHierarchy().GetFirstChild(Arrival);
    EXPECT_NE(ArrivedChild, ECS::NullEntity);
    EXPECT_EQ(New.Get<STransformComponent>(Arrival).LocalTransform.GetLocation(), FVector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(New.Get<SCameraFollowComponent>(Arrival).Target, ECS::NullEntity) << "a reference into the old level cannot survive it";
}

TEST(PersistentEntities, AnArrivalReplacesThePlacedEntityWithItsTag)
{
    CWorld* From = NewTestWorld();
    CWorld* To = NewTestWorld();
    MakePersistent(From->GetEntityRegistry(), FVector3(1.0f, 2.0f, 3.0f));

    ECS::FRegistry& New = To->GetEntityRegistry();
    const ECS::FEntity Placed = MakePersistent(New, FVector3(5.0f, 0.0f, 0.0f));
    const ECS::FEntity Door = MakeEntity(New, FVector3(0.0f));
    New.Emplace<SCameraFollowComponent>(Door).Target = Placed;

    FPersistentCarry Carry;
    Carry.Lift(From);
    Carry.Land(To);

    const TVector<ECS::FEntity> Arrived = Heroes(To);
    ASSERT_EQ(Arrived.Num(), 1);
    EXPECT_NE(Arrived[0], Placed);
    EXPECT_FALSE(New.IsValid(Placed));
    EXPECT_EQ(New.Get<SCameraFollowComponent>(Door).Target, Arrived[0]) << "the level's own references follow the replacement";
    EXPECT_EQ(New.Get<STransformComponent>(Arrived[0]).LocalTransform.GetLocation(), FVector3(5.0f, 0.0f, 0.0f));
}

TEST(PersistentEntities, WithoutTakePlacementTheArrivalKeepsItsOwnTransform)
{
    CWorld* From = NewTestWorld();
    CWorld* To = NewTestWorld();
    const ECS::FEntity Hero = MakePersistent(From->GetEntityRegistry(), FVector3(1.0f, 2.0f, 3.0f));
    From->GetEntityRegistry().Get<SPersistentComponent>(Hero).bTakePlacement = false;
    MakePersistent(To->GetEntityRegistry(), FVector3(5.0f, 0.0f, 0.0f));

    FPersistentCarry Carry;
    Carry.Lift(From);
    Carry.Land(To);

    const TVector<ECS::FEntity> Arrived = Heroes(To);
    ASSERT_EQ(Arrived.Num(), 1);
    EXPECT_EQ(To->GetEntityRegistry().Get<STransformComponent>(Arrived[0]).LocalTransform.GetLocation(), FVector3(1.0f, 2.0f, 3.0f));
}
