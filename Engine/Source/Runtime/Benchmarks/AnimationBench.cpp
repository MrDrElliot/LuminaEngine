#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/BoneLOD.h"
#include "Animation/Pose.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/SkeletonResource.h"
#include "TaskSystem/TaskSystem.h"
#include <cstdio>

using namespace Lumina;

namespace
{
    // A humanoid-sized rig, built as a binary heap so a leaf, its parent and grandparent form a leg.
    constexpr int32 kBenchBones = 115;
    constexpr int32 kLeftThigh  = 24;
    constexpr int32 kLeftCalf   = 49;
    constexpr int32 kLeftFoot   = 99;
    constexpr int32 kRightThigh = 25;
    constexpr int32 kRightCalf  = 51;
    constexpr int32 kRightFoot  = 103;

    void BuildBenchSkeleton(FSkeletonResource& Skeleton)
    {
        for (int32 i = 0; i < kBenchBones; ++i)
        {
            FSkeletonResource::FBoneInfo Bone;
            Bone.Name           = FName(("Bone" + std::to_string(i)).c_str());
            Bone.ParentIndex    = i == 0 ? Constants::kIndexNone : (i - 1) / 2;
            Bone.InvBindMatrix  = FMatrix4::Identity();
            Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(0.03f * (float)(i % 3), 0.12f, 0.02f),
                                                       FQuat(FVector3(0.1f, 0.05f * (float)(i % 4), 0.0f)), FVector3(1.0f));
            Skeleton.BoneNameToIndex[Bone.Name] = i;
            Skeleton.Bones.Add(Bone);
        }
        Skeleton.BuildBindPoseCache();
    }

    // Every bone rotates and a quarter of them translate, at 30 keys a second like an imported clip.
    CAnimation* MakeBenchClip(const FSkeletonResource& Skeleton, float Duration, float Seed)
    {
        CAnimation* Clip = NewObject<CAnimation>();
        FAnimationResource& Resource = *Clip->GetAnimationResource();
        Resource.Duration = Duration;

        const uint32 NumKeys = (uint32)(Duration * 30.0f) + 1;
        for (int32 b = 0; b < kBenchBones; ++b)
        {
            FAnimationChannel Rotation;
            Rotation.TargetBone = Skeleton.Bones[b].Name;
            Rotation.TargetPath = FAnimationChannel::ETargetPath::Rotation;
            for (uint32 k = 0; k < NumKeys; ++k)
            {
                const float T = Duration * (float)k / (float)(NumKeys - 1);
                Rotation.Timestamps.Add(T);
                Rotation.Rotations.Emplace(FVector3(Math::Sin(T * 3.0f + Seed + b) * 0.4f, Math::Cos(T * 2.0f + b) * 0.3f, 0.1f * Seed));
            }
            Resource.Channels.Add(Rotation);

            if (b % 4 == 0)
            {
                FAnimationChannel Translation;
                Translation.TargetBone = Skeleton.Bones[b].Name;
                Translation.TargetPath = FAnimationChannel::ETargetPath::Translation;
                for (uint32 k = 0; k < NumKeys; ++k)
                {
                    const float T = Duration * (float)k / (float)(NumKeys - 1);
                    Translation.Timestamps.Add(T);
                    Translation.Translations.Emplace(0.03f, 0.12f + 0.02f * Math::Sin(T * 5.0f + Seed), 0.02f);
                }
                Resource.Channels.Add(Translation);
            }
        }

        AnimCompression::Build(Resource);
        return Clip;
    }

    struct FBenchRig
    {
        FSkeletonResource       Skeleton;
        TVector<CAnimation*>    Clips;
        TVector<float>          Mask;
        FAnimInertializer       Inert[4];
    };

    // The reachable part of a MyLuminaProject locomotion recipe, as anim.DumpGraphTasks shows it.
    void BuildLocomotionRecipe(FBenchRig& Rig, FAnimTaskList& List, float Time, float LocomotionBlend)
    {
        List.Reset();
        List.Skeleton = &Rig.Skeleton;

        const auto Sample = [&](int32 ClipIndex)
        {
            FAnimTask Task;
            Task.Type = EAnimTaskType::SampleClip;
            Task.Clip = Rig.Clips[ClipIndex];
            Task.Time = Time;
            return List.Add(Task);
        };
        const auto Inertialize = [&](int16 Input, int32 Slot)
        {
            FAnimTask Task;
            Task.Type  = EAnimTaskType::Inertialize;
            Task.DepA  = Input;
            Task.Inert = &Rig.Inert[Slot];
            return List.Add(Task);
        };

        FAnimTask Blend;
        Blend.Type  = EAnimTaskType::Blend;
        Blend.DepA  = Sample(0);
        Blend.DepB  = Sample(1);
        Blend.Alpha = LocomotionBlend;
        const int16 Locomotion = List.Add(Blend);

        int16 Chain = Inertialize(Locomotion, 0);
        Chain = Inertialize(Chain, 1);
        Chain = Inertialize(Chain, 2);

        FAnimTask MakeAdditive;
        MakeAdditive.Type = EAnimTaskType::MakeAdditive;
        MakeAdditive.DepA = Sample(2);
        MakeAdditive.DepB = Sample(3);
        const int16 Lean = List.Add(MakeAdditive);

        const int16 Base = Inertialize(Chain, 3);

        FAnimTask Apply;
        Apply.Type  = EAnimTaskType::ApplyAdditive;
        Apply.DepA  = Base;
        Apply.DepB  = Lean;
        Apply.Alpha = 0.5f;
        const int16 Leaned = List.Add(Apply);

        FAnimTask Masked;
        Masked.Type        = EAnimTaskType::BlendMasked;
        Masked.DepA        = Leaned;
        Masked.DepB        = Leaned;
        Masked.MaskWeights = &Rig.Mask;
        const int16 Layered = List.Add(Masked);

        FAnimTask Pelvis;
        Pelvis.Type  = EAnimTaskType::TranslateBone;
        Pelvis.DepA  = Layered;
        Pelvis.BoneA = 1;
        Pelvis.T     = FVector3(0.0f, -0.02f, 0.0f);
        const int16 Lowered = List.Add(Pelvis);

        const auto Foot = [&](int16 Input, int32 Thigh, int32 Calf, int32 FootBone)
        {
            FAnimTask Task;
            Task.Type  = EAnimTaskType::FootIK;
            Task.DepA  = Input;
            Task.BoneA = (uint16)Thigh;
            Task.BoneB = (uint16)Calf;
            Task.BoneC = (uint16)FootBone;
            Task.T     = FVector3(0.0f, 0.03f, 0.0f);
            Task.R     = FQuat(0.0f, 0.0f, 1.0f, 0.0f);
            Task.S     = FVector3(0.0f, 1.0f, 0.0f);
            Task.Time  = 1.0f;
            return List.Add(Task);
        };

        List.OutputTask = Foot(Foot(Lowered, kLeftThigh, kLeftCalf, kLeftFoot), kRightThigh, kRightCalf, kRightFoot);
    }

    double NanosSince(uint64 Start)
    {
        return PlatformTime::ToSeconds(PlatformTime::Cycles() - Start) * 1e9;
    }
}

// Single-threaded, so the numbers are per-character cost without the contention a full crowd adds.
TEST(AnimationBench, LocomotionRecipeAndKernels)
{
    FBenchRig Rig;
    BuildBenchSkeleton(Rig.Skeleton);
    for (int32 i = 0; i < 4; ++i)
    {
        Rig.Clips.Add(MakeBenchClip(Rig.Skeleton, 1.2f + 0.1f * (float)i, (float)i));
    }
    Rig.Mask.Init(0.0f, kBenchBones);
    for (int32 i = 0; i < 89; ++i)
    {
        Rig.Mask[i] = 1.0f;
    }

    constexpr int32 Iterations = 4000;
    FAnimTaskList List;
    TVector<FMatrix4> Matrices;

    const auto RunRecipe = [&](float LocomotionBlend)
    {
        for (int32 i = 0; i < 200; ++i)
        {
            BuildLocomotionRecipe(Rig, List, 0.01f * (float)i, LocomotionBlend);
            Anim::ExecuteTaskList(List, Matrices, nullptr);
        }
        const uint64 Start = PlatformTime::Cycles();
        for (int32 i = 0; i < Iterations; ++i)
        {
            BuildLocomotionRecipe(Rig, List, 0.0003f * (float)i, LocomotionBlend);
            Anim::ExecuteTaskList(List, Matrices, nullptr);
        }
        return NanosSince(Start) / Iterations / 1000.0;
    };

    const double RecipeIdleUs  = RunRecipe(0.0f);
    const double RecipeBlendUs = RunRecipe(0.35f);

    FPose A;
    FPose B;
    FPose Out;
    uint64 Start = PlatformTime::Cycles();
    for (int32 i = 0; i < Iterations; ++i)
    {
        Rig.Clips[0]->SampleLocalPose(0.0003f * (float)i, &Rig.Skeleton, A, -1);
    }
    const double SampleUs = NanosSince(Start) / Iterations / 1000.0;
    Rig.Clips[1]->SampleLocalPose(0.5f, &Rig.Skeleton, B, -1);

    Start = PlatformTime::Cycles();
    for (int32 i = 0; i < Iterations; ++i)
    {
        AnimPose::Blend(A, B, 0.35f, Out, -1);
    }
    const double BlendUs = NanosSince(Start) / Iterations / 1000.0;

    Start = PlatformTime::Cycles();
    for (int32 i = 0; i < Iterations; ++i)
    {
        AnimPose::BlendMasked(A, B, 0.35f, Rig.Mask, Out, -1);
    }
    const double MaskedUs = NanosSince(Start) / Iterations / 1000.0;

    Start = PlatformTime::Cycles();
    for (int32 i = 0; i < Iterations; ++i)
    {
        Out = A;
        AnimPose::FootIK(Out, &Rig.Skeleton, kLeftThigh, kLeftCalf, kLeftFoot, FVector3(0.0f, 0.03f, 0.0f),
                         FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), 1.0f, 1.0f);
    }
    const double FootUs = NanosSince(Start) / Iterations / 1000.0;

    Start = PlatformTime::Cycles();
    for (int32 i = 0; i < Iterations; ++i)
    {
        AnimPose::ToSkinningMatrices(A, &Rig.Skeleton, Matrices);
    }
    const double SkinUs = NanosSince(Start) / Iterations / 1000.0;

    Start = PlatformTime::Cycles();
    for (int32 i = 0; i < Iterations; ++i)
    {
        Out = A;
    }
    const double CopyUs = NanosSince(Start) / Iterations / 1000.0;

    std::printf("\n  %d bones, per character, one thread\n", kBenchBones);
    std::printf("  recipe idle (blend alpha 0)   %7.2f us\n", RecipeIdleUs);
    std::printf("  recipe blending (alpha 0.35)  %7.2f us\n", RecipeBlendUs);
    std::printf("  sample clip                   %7.2f us\n", SampleUs);
    std::printf("  blend                         %7.2f us\n", BlendUs);
    std::printf("  blend masked                  %7.2f us\n", MaskedUs);
    std::printf("  foot IK (with pose copy)      %7.2f us\n", FootUs);
    std::printf("  skinning matrices             %7.2f us\n", SkinUs);
    std::printf("  pose copy                     %7.2f us\n\n", CopyUs);
    SUCCEED();
}

// What a bone LOD cut saves per character, and what evaluating in a bone order other than the asset's costs.
TEST(AnimationBench, BoneLODCutScaling)
{
    FBenchRig Rig;
    BuildBenchSkeleton(Rig.Skeleton);
    for (int32 i = 0; i < 4; ++i)
    {
        Rig.Clips.Add(MakeBenchClip(Rig.Skeleton, 1.2f + 0.1f * (float)i, (float)i));
    }
    Rig.Mask.Init(1.0f, kBenchBones);

    // Depth first, where the asset is breadth first, so clip tracks and pose bones disagree the way a sorted layout makes them.
    TVector<int32> PoseToAsset;
    TVector<int32> Stack = { 0 };
    while (!Stack.IsEmpty())
    {
        const int32 Bone = Stack.Last();
        Stack.Pop();
        PoseToAsset.Add(Bone);
        for (int32 Child : { 2 * Bone + 2, 2 * Bone + 1 })
        {
            if (Child < kBenchBones)
            {
                Stack.Add(Child);
            }
        }
    }
    FSkeletonResource PoseSkeleton;
    PoseSkeleton.Bones = Rig.Skeleton.Bones;
    PoseSkeleton.BoneNameToIndex = Rig.Skeleton.BoneNameToIndex;
    const TVector<int32> AssetToPose = BoneLOD::ReorderSkeleton(PoseSkeleton, PoseToAsset);

    constexpr int32 Iterations = 4000;
    FAnimTaskList List;
    TVector<FMatrix4> Matrices;

    const auto Run = [&](int32 ActiveBones, bool bLayout)
    {
        const auto Once = [&](float Time)
        {
            BuildLocomotionRecipe(Rig, List, Time, 0.35f);
            List.ActiveBoneCount = ActiveBones;
            if (bLayout)
            {
                List.Skeleton    = &PoseSkeleton;
                List.PoseToAsset = PoseToAsset.GetData();
                List.AssetToPose = AssetToPose.GetData();
            }
            Anim::ExecuteTaskList(List, Matrices, nullptr);
        };
        for (int32 i = 0; i < 200; ++i)
        {
            Once(0.01f * (float)i);
        }
        const uint64 Start = PlatformTime::Cycles();
        for (int32 i = 0; i < Iterations; ++i)
        {
            Once(0.0003f * (float)i);
        }
        return NanosSince(Start) / Iterations / 1000.0;
    };

    std::printf("\n  %d bones, blending locomotion recipe, one thread\n", kBenchBones);
    std::printf("  every bone, asset order        %7.2f us\n", Run(0, false));
    std::printf("  every bone, pose order         %7.2f us\n", Run(0, true));
    for (int32 Percent : { 80, 50, 25 })
    {
        const int32 Active = kBenchBones * Percent / 100;
        std::printf("  %3d%% of bones (%3d), pose order %7.2f us\n", Percent, Active, Run(Active, true));
    }
    std::printf("\n");
    SUCCEED();
}

// The same recipe across every worker, one rig per character, so contention shows up as per-character time.
TEST(AnimationBench, LocomotionCrowdParallel)
{
    constexpr int32 Characters = 2000;
    constexpr int32 Frames     = 30;

    for (int32 NumSkeletons : { 1, 2 })
    {
        TVector<TUniquePtr<FSkeletonResource>> Skeletons;
        for (int32 s = 0; s < NumSkeletons; ++s)
        {
            Skeletons.Add(MakeUnique<FSkeletonResource>());
            BuildBenchSkeleton(*Skeletons.Last());
        }

        FBenchRig Shared;
        BuildBenchSkeleton(Shared.Skeleton);
        for (int32 i = 0; i < 4; ++i)
        {
            Shared.Clips.Add(MakeBenchClip(Shared.Skeleton, 1.2f + 0.1f * (float)i, (float)i));
        }
        Shared.Mask.Init(1.0f, kBenchBones);

        TVector<TUniquePtr<FBenchRig>> Rigs;
        for (int32 c = 0; c < Characters; ++c)
        {
            Rigs.Add(MakeUnique<FBenchRig>());
            Rigs.Last()->Clips = Shared.Clips;
            Rigs.Last()->Mask  = Shared.Mask;
        }

        TVector<TVector<FMatrix4>> Outputs(Characters);
        const auto RunFrame = [&](int32 Frame)
        {
            Jobs::ParallelFor((uint32)Characters, [&](uint32 c)
            {
                thread_local FAnimTaskList List;
                FBenchRig& Rig = *Rigs[c];
                BuildLocomotionRecipe(Rig, List, 0.016f * (float)Frame + 0.001f * (float)c, 0.35f);
                List.Skeleton = Skeletons[c % NumSkeletons].Get();
                Anim::ExecuteTaskList(List, Outputs[c], nullptr);
            }, 16);
        };

        RunFrame(0);
        const uint64 Start = PlatformTime::Cycles();
        for (int32 f = 1; f <= Frames; ++f)
        {
            RunFrame(f);
        }
        const double FrameUs = NanosSince(Start) / Frames / 1000.0;
        std::printf("\n  %d characters on %u threads, %d skeleton(s): %8.1f us per frame, %6.2f us wall per character\n",
                    Characters, Jobs::GetNumThreadSlots(), NumSkeletons, FrameUs, FrameUs / Characters);
    }
    SUCCEED();
}
