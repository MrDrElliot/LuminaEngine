#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/AnimEvents.h"
#include "Animation/BindPose.h"
#include "Animation/RootMotion.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/SkeletonResource.h"
#include "World/World.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr float LoopDuration = 1.0f;

    void MakeRootSkeleton(FSkeletonResource& Skeleton)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name           = FName("Root");
        Bone.ParentIndex    = Constants::kIndexNone;
        Bone.InvBindMatrix  = FMatrix4::Identity();
        Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(0.0f), FQuat::Identity(), FVector3(1.0f));

        Skeleton.BoneNameToIndex[Bone.Name] = 0;
        Skeleton.Bones.Add(Bone);
        Skeleton.BuildBindPoseCache();
    }

    // Travels Travel and yaws YawDegrees once per loop, both linearly.
    CAnimation* MakeRootClip(const FVector3& Travel, float YawDegrees)
    {
        constexpr uint32 NumKeys = 31;

        CAnimation* Clip = NewObject<CAnimation>();
        FAnimationResource& Resource = *Clip->GetAnimationResource();
        Resource.Duration = LoopDuration;

        FAnimationChannel Translation;
        Translation.TargetBone = FName("Root");
        Translation.TargetPath = FAnimationChannel::ETargetPath::Translation;

        FAnimationChannel Rotation = Translation;
        Rotation.TargetPath = FAnimationChannel::ETargetPath::Rotation;

        for (uint32 i = 0; i < NumKeys; ++i)
        {
            const float Alpha = (float)i / (float)(NumKeys - 1);
            Translation.Timestamps.Add(Alpha * LoopDuration);
            Translation.Translations.Add(Travel * Alpha);
            Rotation.Timestamps.Add(Alpha * LoopDuration);
            Rotation.Rotations.Emplace(FVector3(0.0f, Math::Radians(YawDegrees) * Alpha, 0.0f));
        }
        Resource.Channels.Add(Translation);
        Resource.Channels.Add(Rotation);

        AnimCompression::Build(Resource);
        return Clip;
    }

    float AngleDegrees(const FQuat& Rotation)
    {
        return Math::Degrees(2.0f * std::atan2(Math::Length(FVector3(Rotation.x, Rotation.y, Rotation.z)), Math::Abs(Rotation.w)));
    }
}

// Advancing 2.3 loops from 0.2 lands on 0.5, so both whole loops must count on top of the 0.3 the clock shows.
TEST(AnimLoopWrap, RootMotionCreditsEveryWholeLoopInOneAdvance)
{
    FSkeletonResource Skeleton;
    MakeRootSkeleton(Skeleton);

    const CAnimation* Walk = MakeRootClip(FVector3(1.0f, 0.0f, 0.0f), 0.0f);
    const FRootMotionDelta Moved = RootMotion::ExtractRootDelta(Walk, &Skeleton, 0, 0.2f, 0.5f, true, LoopDuration, 2);
    EXPECT_NEAR(Moved.Translation.x, 2.3f, 1e-3f);
    EXPECT_NEAR(Moved.Translation.y, 0.0f, 1e-3f);
    EXPECT_NEAR(Moved.Translation.z, 0.0f, 1e-3f);

    const CAnimation* Turn = MakeRootClip(FVector3(0.0f), 40.0f);
    const FRootMotionDelta Turned = RootMotion::ExtractRootDelta(Turn, &Skeleton, 0, 0.2f, 0.5f, true, LoopDuration, 2);
    EXPECT_NEAR(AngleDegrees(Turned.Rotation), 2.3f * 40.0f, 0.1f);
}

// Advancing 2.6 loops from 0.8 wraps three times, the last wrap showing as the clock landing behind where it started.
TEST(AnimLoopWrap, RootMotionCountsWholeLoopsOnTopOfAVisibleWrap)
{
    FSkeletonResource Skeleton;
    MakeRootSkeleton(Skeleton);

    const CAnimation* Walk = MakeRootClip(FVector3(1.0f, 0.0f, 0.0f), 0.0f);
    const FRootMotionDelta Moved = RootMotion::ExtractRootDelta(Walk, &Skeleton, 0, 0.8f, 0.4f, true, LoopDuration, 2);
    EXPECT_NEAR(Moved.Translation.x, 2.6f, 1e-3f);

    const FRootMotionDelta Clamped = RootMotion::ExtractRootDelta(Walk, &Skeleton, 0, 0.2f, 0.5f, false, LoopDuration, 2);
    EXPECT_NEAR(Clamped.Translation.x, 0.3f, 1e-3f);
}

TEST(AnimLoopWrap, NotifyFiresOncePerLoopCrossedInOneAdvance)
{
    CAnimation* Clip = MakeRootClip(FVector3(0.0f), 0.0f);
    FAnimationNotify& Notify = Clip->GetAnimationResource()->Notifies.EmplaceGetRef();
    Notify.NotifyName = "Step";
    Notify.Time       = 0.5f;

    // From 0.2 by 3.1 loops passes 0.5 three times, none of them inside the 0.2 to 0.3 the clock shows.
    TVector<FAnimNotifyEvent> Skipped;
    AnimEvents::CollectTriggeredNotifies(Clip, 0.2f, 0.3f, true, 1.0f, Skipped, 3);
    EXPECT_EQ(Skipped.Num(), 3);

    // From 0.4 by 2.2 loops passes 0.5 three times, once inside the 0.4 to 0.6 the clock shows.
    TVector<FAnimNotifyEvent> Crossed;
    AnimEvents::CollectTriggeredNotifies(Clip, 0.4f, 0.6f, true, 1.0f, Crossed, 2);
    ASSERT_EQ(Crossed.Num(), 3);
    EXPECT_EQ(Crossed[0].Name, FName("Step"));

    // Exactly two loops leaves the clock where it started.
    TVector<FAnimNotifyEvent> Lapped;
    AnimEvents::CollectTriggeredNotifies(Clip, 0.2f, 0.2f, true, 1.0f, Lapped, 2);
    EXPECT_EQ(Lapped.Num(), 2);
}

// A system update advancing 2.5 loops in one step must credit every loop crossed, not just the one the wrapped clock shows.
TEST(AnimLoopWrap, AnimationSystemCreditsEveryWholeLoopInOneUpdate)
{
    TUniquePtr<FSkeletonResource> Resource = MakeUnique<FSkeletonResource>();
    MakeRootSkeleton(*Resource);

    CSkeleton* Skeleton = NewObject<CSkeleton>();
    Skeleton->SetSkeletonResource(std::move(Resource));

    CSkeletalMesh* Mesh = NewObject<CSkeletalMesh>();
    Mesh->Skeleton = Skeleton;

    CAnimation* Clip = MakeRootClip(FVector3(1.0f, 0.0f, 0.0f), 0.0f);
    Clip->bEnableRootMotion = true;
    FAnimationNotify& Notify = Clip->GetAnimationResource()->Notifies.EmplaceGetRef();
    Notify.NotifyName = "Step";
    Notify.Time       = 0.5f;

    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, FName("AnimLoopWrapTestWorld"), FGuid::New(), OF_Transient);
    TStrongObjectPtr<CWorld> WorldOwner = World;
    World->GetEntityRegistry().Ctx().Emplace<CWorld*>(World);

    // By name, since the system class is not exported to the test binary.
    FWorldTestAccess::KeepOnlySystems(*World, { "CAnimationSystem" });

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Entity = Registry.Create();
    SSkeletalMeshComponent& MeshComponent = Registry.Emplace<SSkeletalMeshComponent>(Entity);
    MeshComponent.SkeletalMesh = Mesh;
    MeshComponent.VisibilityBasedAnimTick = EAnimUpdateMode::AlwaysTickPose;
    SSimpleAnimationComponent& Anim = Registry.Emplace<SSimpleAnimationComponent>(Entity);
    Anim.PlayAnimation(Clip, true, 1.0f);

    // A one second looping clip advanced 2.5 seconds wraps twice and lands at 0.5.
    FWorldTestAccess::TickStage(*World, EUpdateStage::PrePhysics, 2.5);

    EXPECT_NEAR(Anim.CurrentTime, 0.5f, 1e-3f);
    ASSERT_EQ(Anim.NotifyEvents.Num(), 3) << "2.5 loops crosses the 0.5 notify three times, not once";
    EXPECT_NEAR(Anim.PendingRootMotion.Translation.x, 2.5f, 1e-3f) << "2.5 loops of 1 unit each must move 2.5 units, not 0.5";
}
