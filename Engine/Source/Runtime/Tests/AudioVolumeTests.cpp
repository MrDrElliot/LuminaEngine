#include <gtest/gtest.h>

#include "Core/Math/Transform.h"
#include "World/Entity/Components/AudioVolumeComponent.h"
#include "World/Entity/Components/TransformComponent.h"

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
