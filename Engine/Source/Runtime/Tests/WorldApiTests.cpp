#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "HierarchicalTags/GameplayMessageBus.h"
#include "World/Subsystems/MessageSubsystem.h"
#include "World/World.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    CWorld* NewWorld()
    {
        STransformComponent::StaticStruct();
        return NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }
}

TEST(WorldApi, SetEntityTransformAddsATransformWhenMissing)
{
    CWorld* World = NewWorld();
    const ECS::FEntity Entity = World->CreateEntity();
    ASSERT_FALSE(World->HasComponent<STransformComponent>(Entity));

    const FTransform Placed(FVector3(1.0f, 2.0f, 3.0f), FVector3(0.0f), FVector3(2.0f));
    World->SetEntityTransform(Entity, Placed);

    ASSERT_TRUE(World->HasComponent<STransformComponent>(Entity));
    EXPECT_EQ(World->GetEntityTransform(Entity).GetLocation(), Placed.GetLocation());
    EXPECT_EQ(World->GetEntityScale(Entity), FVector3(2.0f));
}

TEST(WorldApi, SetEntityTransformKeepsTheExistingComponent)
{
    CWorld* World = NewWorld();
    const ECS::FEntity Entity = World->ConstructEntity("Mover");
    const STransformComponent* Before = World->TryGetComponent<STransformComponent>(Entity);

    World->SetEntityTransform(Entity, FTransform(FVector3(5.0f, 0.0f, 0.0f)));
    World->SetEntityScale(Entity, FVector3(3.0f));

    EXPECT_EQ(World->TryGetComponent<STransformComponent>(Entity), Before);
    EXPECT_EQ(World->GetEntityLocation(Entity), FVector3(5.0f, 0.0f, 0.0f));
    EXPECT_EQ(World->GetEntityScale(Entity), FVector3(3.0f));
}

TEST(WorldApi, ConstWorldReadsThroughTheSameAccessors)
{
    CWorld* World = NewWorld();
    const ECS::FEntity Entity = World->ConstructEntity("Reader");
    const CWorld& ReadOnly = *World;

    const STransformComponent& Transform = ReadOnly.GetComponent<STransformComponent>(Entity);
    const STransformComponent* Found = ReadOnly.TryGetComponent<STransformComponent>(Entity);
    static_assert(std::is_same_v<decltype(ReadOnly.GetEntityRegistry()), const ECS::FRegistry&>);

    EXPECT_EQ(&Transform, Found);
    EXPECT_FLOAT_EQ(ReadOnly.GetTimeDilation(), 1.0f);
}

TEST(WorldApi, TransformsOfAMissingEntityAreDefaults)
{
    CWorld* World = NewWorld();
    const ECS::FEntity Gone = World->CreateEntity();
    World->DestroyEntity(Gone);

    World->SetEntityTransform(Gone, FTransform(FVector3(9.0f)));
    EXPECT_EQ(World->GetEntityScale(Gone), FVector3(1.0f));
    EXPECT_EQ(World->GetEntityTransform(Gone).GetLocation(), FVector3(0.0f));
}

TEST(WorldApi, EngineSubsystemsExistEvenForAnUninitializedWorld)
{
    CWorld* World = NewWorld();
    ASSERT_EQ(World->GetSubsystem<CPhysicsSubsystem>(), nullptr) << "a bare world starts with no subsystems";

    CPhysicsSubsystem* Physics = World->GetPhysics();
    ASSERT_NE(Physics, nullptr);
    EXPECT_EQ(Physics->GetWorld(), World);
    EXPECT_EQ(World->GetPhysics(), Physics) << "a second read finds the same one rather than creating another";
    EXPECT_EQ(World->GetSubsystem<CPhysicsSubsystem>(), Physics);
    EXPECT_NE(World->GetTweens(), nullptr);
    EXPECT_NE(World->GetMessageBus(), nullptr);
}

TEST(WorldApi, MessageBusLeavesTheRegistryAtTeardown)
{
    CWorld* World = NewWorld();
    const ECS::FComponentSignal& EntityDestroyed = World->OnEntityDestroy();
    ASSERT_TRUE(EntityDestroyed.IsEmpty()) << "a bare world has no entity destroyed listeners";

    CMessageSubsystem* Messages = World->GetMessageBus();
    ASSERT_FALSE(EntityDestroyed.IsEmpty());

    // A C# event field can hold the subsystem past the world, so teardown is the last time the registry is alive.
    Messages->OnTeardown();
    EXPECT_TRUE(EntityDestroyed.IsEmpty());

    uint32 Payload = 7;
    Messages->Send((uint32)EGameplayMessageRoute::Broadcast, ECS::NullEntity, 1, (uint32)EGameplayMessagePayload::Bytes, 0, &Payload, sizeof(Payload), nullptr, true);
    Messages->UnsubscribeAll(World->CreateEntity());
    EXPECT_FALSE(Messages->Unsubscribe(1));
}
