#include <gtest/gtest.h>

#include "Audio/AudioContext.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectCore.h"
#include "World/World.h"
#include "World/Entity/Components/AudioVolumeComponent.h"
#include "World/Entity/Components/ProceduralAudioComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/AudioSystem.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    STransformComponent MakeTransform(const FVector3& Location, const FVector3& EulerDegrees = FVector3(0.0f), const FVector3& Scale = FVector3(1.0f))
    {
        return STransformComponent(FTransform(Location, EulerDegrees, Scale));
    }
}

TEST(AudioVolume, ABoxWeighsOneInsideAndZeroOutside)
{
    SAudioVolumeComponent Volume;
    Volume.Extent = FVector3(2.0f, 1.0f, 3.0f);
    const STransformComponent Transform = MakeTransform(FVector3(10.0f, 0.0f, -5.0f));

    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(10.0f, 0.0f, -5.0f)), 1.0f);
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(11.9f, 0.5f, -2.1f)), 1.0f);
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(12.1f, 0.0f, -5.0f)), 0.0f);
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(10.0f, 0.0f, -1.9f)), 0.0f);
}

TEST(AudioVolume, BlendDistanceRampsTheWeightWithDepth)
{
    SAudioVolumeComponent Volume;
    Volume.Extent = FVector3(10.0f);
    Volume.BlendDistance = 4.0f;
    const STransformComponent Transform = MakeTransform(FVector3(0.0f));

    EXPECT_NEAR(Volume.ListenerWeight(Transform, FVector3(9.0f, 0.0f, 0.0f)), 0.25f, 1e-4f);
    EXPECT_NEAR(Volume.ListenerWeight(Transform, FVector3(0.0f, 0.0f, -8.0f)), 0.5f, 1e-4f);
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(1.0f, 2.0f, 3.0f)), 1.0f);
}

TEST(AudioVolume, TheBoxFollowsItsEntitysRotationAndScale)
{
    SAudioVolumeComponent Volume;
    Volume.Extent = FVector3(4.0f, 1.0f, 1.0f);
    const STransformComponent Turned = MakeTransform(FVector3(0.0f), FVector3(0.0f, 90.0f, 0.0f), FVector3(2.0f, 1.0f, 1.0f));

    // The long axis now points along world Z and is doubled, reaching eight units.
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Turned, FVector3(0.0f, 0.0f, 7.5f)), 1.0f);
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Turned, FVector3(7.5f, 0.0f, 0.0f)), 0.0f);
}

TEST(AudioVolume, ASphereUsesItsLargestScaleAxis)
{
    SAudioVolumeComponent Volume;
    Volume.Shape = EAudioVolumeShape::Sphere;
    Volume.Radius = 3.0f;
    const STransformComponent Transform = MakeTransform(FVector3(1.0f, 2.0f, 3.0f), FVector3(0.0f), FVector3(1.0f, 2.0f, 1.0f));

    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(1.0f, 7.5f, 3.0f)), 1.0f);
    EXPECT_FLOAT_EQ(Volume.ListenerWeight(Transform, FVector3(1.0f, 8.5f, 3.0f)), 0.0f);
}

class AudioSystemChangeTracking : public ::testing::Test
{
protected:
    void SetUp() override
    {
        RegisterPendingTypes();
        Audio::Initialize();
        World = TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
        System = NewObject<CAudioSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        System->SetOwningWorld(World.Get());
        System->ConfigureSystem();
    }

    void TearDown() override
    {
        System->SetOwningWorld(nullptr);
        World.Reset();
        Audio::Shutdown();
    }

    template<typename TComponent>
    bool TickChanges(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        const uint32 Since = Registry.AdvanceChangeTick();
        System->OnUpdate(FWorldTestAccess::SystemContext(*World));
        return Registry.FindStorage(ECS::GetComponentTypeID<TComponent>())->HasChangedSince(Entity, Since);
    }

    TStrongObjectPtr<CWorld> World;
    CAudioSystem* System = nullptr;
};

TEST_F(AudioSystemChangeTracking, AStationaryIdleSourceIsNotMarkedChanged)
{
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<STransformComponent>(Entity, MakeTransform(FVector3(3.0f, 0.0f, 0.0f)));
    Registry.Emplace<SAudioSourceComponent>(Entity);
    Registry.TrackChanges<SAudioSourceComponent>();

    EXPECT_TRUE(TickChanges<SAudioSourceComponent>(Entity)) << "the first frame readies the source and records where it is";
    EXPECT_FALSE(TickChanges<SAudioSourceComponent>(Entity)) << "a source that neither moved nor plays has nothing to write";

    const FVector3 Moved(5.0f, 1.0f, 0.0f);
    Registry.Get<STransformComponent>(Entity).LocalTransform.SetLocation(Moved);
    EXPECT_TRUE(TickChanges<SAudioSourceComponent>(Entity)) << "a moved source refreshes the position a deferred Play spawns at";
    EXPECT_EQ(Registry.Get<SAudioSourceComponent>(Entity).LastPosition, Moved);
}

TEST_F(AudioSystemChangeTracking, AnIdleProceduralSourceIsNotMarkedChanged)
{
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<STransformComponent>(Entity, MakeTransform(FVector3(3.0f, 0.0f, 0.0f)));
    Registry.Emplace<SProceduralAudioComponent>(Entity);
    Registry.TrackChanges<SProceduralAudioComponent>();

    EXPECT_TRUE(TickChanges<SProceduralAudioComponent>(Entity)) << "the first frame readies the source";
    EXPECT_FALSE(TickChanges<SProceduralAudioComponent>(Entity)) << "a ready source that is not playing has nothing to write";
    EXPECT_TRUE(Registry.Get<SProceduralAudioComponent>(Entity).bReady);
}
