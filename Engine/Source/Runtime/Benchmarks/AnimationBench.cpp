#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/AnimationGraphVM.h"
#include "Animation/AnimGpuClip.h"
#include "Animation/BoneLOD.h"
#include "Animation/Pose.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Animation/TaskSystem/AnimTaskRecord.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Memory/Memcpy.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/NativeFileSystem.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
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

// The scaling baseline the redesign is measured against, each character owning only the state a real one keeps.
TEST(AnimationBench, CrowdScaling)
{
    FBenchRig Shared;
    BuildBenchSkeleton(Shared.Skeleton);
    for (int32 i = 0; i < 4; ++i)
    {
        Shared.Clips.Add(MakeBenchClip(Shared.Skeleton, 1.2f + 0.1f * (float)i, (float)i));
    }
    Shared.Mask.Init(1.0f, kBenchBones);

    enum class EMode : uint8 { FullGraph, QuarterBones, SingleClip };
    const auto ModeName = [](EMode Mode)
    {
        switch (Mode)
        {
            case EMode::FullGraph:    return "full graph";
            case EMode::QuarterBones: return "full graph, 25% bones";
            default:                  return "single clip";
        }
    };

    struct FCharacter
    {
        FAnimInertializer Inert[4];
        TVector<FMatrix4> Matrices;
    };

    // Exact bytes a character owns once warm, since the process working set reuses whatever earlier rows freed.
    const auto OwnedBytes = [](const FCharacter& Character)
    {
        const auto PoseBytes = [](const FPose& Pose)
        {
            return Pose.GetNumBones() > 0 ? (size_t)FPose::StrideFor(Pose.GetNumBones()) * 10u * sizeof(float) : (size_t)0;
        };
        const auto ChannelBytes = [](const FInertChannelSet& Set)
        {
            return (size_t)Set.NumLanes() * FInertChannelSet::NumStreams * sizeof(float);
        };
        size_t Bytes = sizeof(FCharacter) + (size_t)Character.Matrices.Max() * sizeof(FMatrix4);
        for (const FAnimInertializer& Inert : Character.Inert)
        {
            Bytes += PoseBytes(Inert.PrevOutput) + PoseBytes(Inert.PrevPrevOutput);
            Bytes += ChannelBytes(Inert.Rot) + ChannelBytes(Inert.Trans) + ChannelBytes(Inert.Scale);
        }
        return Bytes;
    };

    std::printf("\n  %d bones, %u threads\n", kBenchBones, Jobs::GetNumThreadSlots());
    std::printf("  %-22s %8s %12s %12s %14s\n", "mode", "count", "ms/frame", "ns/char", "bytes/char");
    for (EMode Mode : { EMode::FullGraph, EMode::QuarterBones, EMode::SingleClip })
    {
        for (int32 Count : { 100, 1000, 10000, 50000, 100000 })
        {
            TVector<TUniquePtr<FCharacter>> Characters;
            Characters.Reserve(Count);
            for (int32 c = 0; c < Count; ++c)
            {
                Characters.Add(MakeUnique<FCharacter>());
            }

            const auto RunFrame = [&](int32 Frame)
            {
                Jobs::ParallelFor((uint32)Count, [&](uint32 c)
                {
                    thread_local FAnimTaskList List;
                    FCharacter& Character = *Characters[c];
                    const float Time = 0.016f * (float)Frame + 0.001f * (float)c;
                    if (Mode == EMode::SingleClip)
                    {
                        List.Reset();
                        List.Skeleton = &Shared.Skeleton;
                        FAnimTask Sample;
                        Sample.Type = EAnimTaskType::SampleClip;
                        Sample.Clip = Shared.Clips[c % 4];
                        Sample.Time = Time;
                        List.OutputTask = List.Add(Sample);
                    }
                    else
                    {
                        BuildLocomotionRecipe(Shared, List, Time, 0.35f);
                        for (FAnimTask& Task : List.Tasks)
                        {
                            if (Task.Inert != nullptr)
                            {
                                Task.Inert = &Character.Inert[Task.Inert - Shared.Inert];
                            }
                        }
                        List.ActiveBoneCount = Mode == EMode::QuarterBones ? kBenchBones / 4 : 0;
                    }
                    Anim::ExecuteTaskList(List, Character.Matrices, nullptr);
                }, 16);
            };

            for (int32 Warm = 0; Warm < 3; ++Warm)
            {
                RunFrame(-Warm);
            }
            const int32 Frames = Count >= 50000 ? 3 : 10;
            const uint64 Start = PlatformTime::Cycles();
            for (int32 f = 1; f <= Frames; ++f)
            {
                RunFrame(f);
            }
            const double FrameMs = NanosSince(Start) / Frames / 1e6;
            std::printf("  %-22s %8d %12.3f %12.1f %14zu\n", ModeName(Mode), Count, FrameMs, FrameMs * 1e6 / Count, OwnedBytes(*Characters[0]));
        }
    }
    std::printf("\n");
    SUCCEED();
}

namespace
{
    // Words of a CamelCase parameter name, less the ones every asset of its kind repeats.
    TVector<FString> NameWords(FStringView Name)
    {
        TVector<FString> Words;
        int32 WordStart = 0;
        for (int32 i = 1; i <= Name.Len(); ++i)
        {
            if (i == Name.Len() || (Name[i] >= 'A' && Name[i] <= 'Z'))
            {
                const FString Word(Name.Mid(WordStart, i - WordStart));
                if (Word != "Animation" && Word != "Blend" && Word != "Space")
                {
                    Words.Add(Word);
                }
                WordStart = i;
            }
        }
        return Words;
    }

    // The preloaded asset of the parameter's kind sharing the most words with its name, the shorter name on a tie.
    CObject* MatchObjectParameter(const SAnimGraphObjectParameter& Param, TSpan<const TStrongObjectPtr<CObject>> Assets)
    {
        const TVector<FString> Words = NameWords(Param.Name.ToString());
        CObject* Best = nullptr;
        int32 BestScore = 0;
        int32 BestLength = 0;
        for (const TStrongObjectPtr<CObject>& AssetRef : Assets)
        {
            CObject* Asset = AssetRef.Get();
            bool bKindMatches = Cast<CBlendSpace>(Asset) != nullptr;
            if (Param.Type == EAnimObjectParamType::Animation)
            {
                bKindMatches = Cast<CAnimation>(Asset) != nullptr;
            }
            if (!bKindMatches)
            {
                continue;
            }
            const FString AssetName = Asset->GetName().ToString();
            int32 Score = 0;
            for (const FString& Word : Words)
            {
                Score += AssetName.Contains(Word) ? 1 : 0;
            }
            if (Score > BestScore || (Score == BestScore && Score > 0 && AssetName.Len() < BestLength))
            {
                Best = Asset;
                BestScore = Score;
                BestLength = AssetName.Len();
            }
        }
        return Best;
    }

    // Locomotion parameters the crowd driver writes; a graph without one keeps its default.
    enum ELocomotionParam : uint8
    {
        Grounded, InAir, Jumped, HasInput, Speed, StandingState, JumpPlayRate, JumpTakeoffBlend, VerticalVelocity,
        LandImpactSpeed, MovingSmooth, RunningOrSprinting, Direction, PlayRate, SprintBlend, VelocityForward,
        VelocityBackward, VelocityLeft, VelocityRight, LeanRight, UpperBodyIdleAdditive, NumLocomotionParams
    };

    constexpr const char* kLocomotionParamNames[NumLocomotionParams] =
    {
        "bGrounded", "bInAir", "bJumped", "bHasInput", "Speed", "StandingState", "JumpPlayRate", "JumpTakeoffBlend", "VerticalVelocity",
        "LandImpactSpeed", "bMovingSmooth", "bRunningOrSprinting", "Direction", "PlayRate", "SprintBlend", "VelocityForwardAmount",
        "VelocityBackwardAmount", "VelocityLeftAmount", "VelocityRightAmount", "LeanRight", "UpperBodyIdleAdditiveAmount"
    };

    constexpr float kGoalOdds[]  = { 0.3f, 0.7f, 0.95f, 1.0f };
    constexpr float kGoalSpeed[] = { 0.0f, 1.75f, 3.75f, 6.0f };
    constexpr float kStandingIdle   = 0.0f;
    constexpr float kStandingMoving = 1.0f;
    constexpr float kStandingStop   = 2.0f;
    constexpr float kStandingTurn   = 4.0f;

    // A wandering character that picks a new goal every few seconds and jumps now and then, so the machines see a crowd's traffic.
    struct FCrowdWalker
    {
        uint32 Seed          = 1;
        float  Speed         = 0.0f;
        float  DesiredSpeed  = 0.0f;
        float  Heading       = 0.0f;
        float  UntilDecision = 0.0f;
        float  AirTime       = -1.0f;
        float  HoldTime      = 0.0f;
        float  HeldState     = kStandingIdle;
        float  LandSpeed     = 0.0f;
        float  Phase         = 0.0f;
        bool   bWasMoving    = false;

        float Random()
        {
            Seed = Seed * 1664525u + 1013904223u;
            return (float)(Seed >> 8) * (1.0f / 16777216.0f);
        }

        void Decide()
        {
            UntilDecision = 2.0f + 4.0f * Random();
            const float Pick = Random();
            int32 Goal = 0;
            while (Pick > kGoalOdds[Goal])
            {
                ++Goal;
            }
            DesiredSpeed = kGoalSpeed[Goal];
            Heading = 360.0f * Random() - 180.0f;
            if (AirTime < 0.0f && Random() < 0.1f)
            {
                AirTime = 0.0f;
            }
            if (DesiredSpeed == 0.0f && Speed < 0.1f && Random() < 0.2f)
            {
                HeldState = kStandingTurn;
                HoldTime  = 0.75f;
            }
        }

        float NextStandingState(bool bMovingSmooth)
        {
            if (HoldTime > 0.0f)
            {
                return HeldState;
            }
            if (bWasMoving && !bMovingSmooth)
            {
                HeldState = kStandingStop + (float)(Seed & 1u);
                HoldTime  = 0.6f;
                return HeldState;
            }
            return bMovingSmooth ? kStandingMoving : kStandingIdle;
        }

        static float AnimatedSpeedAt(float GroundSpeed)
        {
            if (GroundSpeed < 2.5f)
            {
                return 1.5f;
            }
            return GroundSpeed < 5.0f ? 3.5f : 6.0f;
        }

        void Tick(float Dt, const int32 (&Index)[NumLocomotionParams], TVector<float>& Params)
        {
            UntilDecision -= Dt;
            if (UntilDecision <= 0.0f)
            {
                Decide();
            }
            HoldTime -= Dt;
            Phase += Dt;
            Speed += Math::Clamp(DesiredSpeed - Speed, -4.0f * Dt, 4.0f * Dt);

            float VerticalSpeed = 0.0f;
            if (AirTime >= 0.0f)
            {
                AirTime += Dt;
                VerticalSpeed = 4.0f - 10.0f * AirTime;
                if (AirTime > 0.9f)
                {
                    LandSpeed = -VerticalSpeed;
                    AirTime   = -1.0f;
                }
            }

            const bool bGrounded     = AirTime < 0.0f;
            const bool bMovingSmooth = (DesiredSpeed > 0.0f && Speed > 0.05f) || Speed > 1.5f;
            const float Standing     = NextStandingState(bMovingSmooth);
            bWasMoving = bMovingSmooth;

            const float HeadingRad = Math::Radians(Heading);
            const float Values[NumLocomotionParams] =
            {
                bGrounded ? 1.0f : 0.0f, bGrounded ? 0.0f : 1.0f, VerticalSpeed > 0.0f ? 1.0f : 0.0f, DesiredSpeed > 0.0f ? 1.0f : 0.0f,
                Speed, Standing, 1.2f + 0.3f * Math::Min(Speed / 6.0f, 1.0f), Math::Clamp((Speed - 1.75f) / 2.0f, 0.0f, 1.0f), VerticalSpeed,
                LandSpeed, bMovingSmooth ? 1.0f : 0.0f, Speed > 3.0f ? 1.0f : 0.0f, Heading, Math::Clamp(Speed / AnimatedSpeedAt(Speed), 0.0f, 3.0f),
                Math::Clamp((Speed - 3.75f) / 2.25f, 0.0f, 1.0f), Math::Max(Math::Cos(HeadingRad), 0.0f), Math::Max(-Math::Cos(HeadingRad), 0.0f),
                Math::Max(-Math::Sin(HeadingRad), 0.0f), Math::Max(Math::Sin(HeadingRad), 0.0f), 0.2f * Math::Sin(Phase), 1.0f,
            };
            for (int32 p = 0; p < NumLocomotionParams; ++p)
            {
                if (Index[p] != Constants::kIndexNone)
                {
                    Params[Index[p]] = Values[p];
                }
            }
        }
    };

    // Bytes one character's update state owns, leaving out inertialization history that only the execute half fills.
    size_t LogicStateBytes(const FAnimGraphVMState& State, const FAnimTaskList& Tasks)
    {
        return sizeof(FAnimGraphVMState) + sizeof(FAnimTaskList)
            + (size_t)(State.ScalarRegisters.Max() + State.StateSlots.Max() + State.Parameters.Max() + State.CurveValues.Max()) * sizeof(float)
            + (size_t)State.ObjectParameters.Max() * sizeof(TStrongObjectPtr<CObject>)
            + (size_t)(State.Inertializers.Max() + State.NodeInertializers.Max()) * sizeof(FAnimInertializer)
            + (size_t)State.DeadBlends.Max() * sizeof(FAnimDeadBlend)
            + (size_t)State.PoseSnapshots.Max() * sizeof(FPose)
            + (size_t)State.SyncGroups.Max() * sizeof(FAnimSyncGroup)
            + (size_t)Tasks.Tasks.Max() * sizeof(FAnimTask);
    }
}

namespace
{
    constexpr float kGraphStep = 1.0f / 60.0f;

    struct FCrowdCharacter
    {
        FAnimGraphVMState         State;
        FAnimTaskList             Tasks;
        FAnimGraphRootMotion      RootMotion;
        TVector<FAnimNotifyEvent> Notifies;
        FCrowdWalker              Walker;
        FAnimTaskRecord           Record;
        TVector<FAnimWeightedSample> Weighted;
    };

    // Where an update's recorded tasks go, which is what separates the crowd tier's output cost from the logic itself.
    enum class EGraphOutput : uint8
    {
        PersistentTasks,
        ScratchTasks,
        Records,
        CrowdRecords,
        CrowdSamples,
    };

    // A real compiled graph loaded read-only from its project, with its clip inputs matched and the driver's parameters resolved.
    struct FRealGraph
    {
        TVector<TStrongObjectPtr<CObject>> Preloaded;
        TStrongObjectPtr<CAnimationGraph>  Graph;
        FSkeletonResource*                 Skeleton = nullptr;
        TVector<CObject*>                  ObjectValues;
        int32                              ParamIndex[NumLocomotionParams];
        int32                              RootBone = Constants::kIndexNone;
        FString                            VirtualPath;
        bool                               bMounted = false;

        FRealGraph() = default;
        FRealGraph(const FRealGraph&) = delete;
        FRealGraph(FRealGraph&&) = delete;
        FRealGraph& operator=(const FRealGraph&) = delete;
        FRealGraph& operator=(FRealGraph&&) = delete;

        ~FRealGraph()
        {
            if (bMounted)
            {
                VFS::Unmount("/Game");
                VFS::Unmount("/Engine");
            }
        }

        // Empty means loaded, otherwise the reason it could not be.
        FString Load()
        {
            const char* GraphFile = std::getenv("LUMINA_ANIM_BENCH_GRAPH");
            if (GraphFile == nullptr)
            {
                return "set LUMINA_ANIM_BENCH_GRAPH to an animation graph .lasset inside a project's Game directory";
            }

            FString DiskPath(GraphFile);
            for (char& C : DiskPath)
            {
                if (C == '\\')
                {
                    C = '/';
                }
            }
            const int32 GameAt = DiskPath.Find("/Game/");
            if (GameAt == Constants::kIndexNone)
            {
                return "the graph is not inside a Game directory";
            }
            VFS::Mount<VFS::FNativeFileSystem>("/Game", DiskPath.Left(GameAt + 5));
            VFS::Mount<VFS::FNativeFileSystem>("/Engine", Paths::GetEngineDirectory());
            bMounted = true;
            RegisterPendingTypes();

            // Imports resolve by GUID through an asset registry the benchmark never fills, so the graph's assets load first.
            const auto Preload = [&](FStringView Path)
            {
                CPackage* Dependency = CPackage::LoadPackage(Path);
                if (Dependency != nullptr && Dependency->FullyLoad())
                {
                    if (CObject* Asset = Dependency->LoadObjectByName(FName(VFS::FileName(Path, true))))
                    {
                        Preloaded.Add(Asset);
                    }
                }
            };
            if (const char* PreloadList = std::getenv("LUMINA_ANIM_BENCH_PRELOAD"))
            {
                FStringView Remaining(PreloadList);
                while (!Remaining.IsEmpty())
                {
                    const int32 Separator = Remaining.Find(';');
                    FStringView Entry = Remaining;
                    Remaining = FStringView();
                    if (Separator != Constants::kIndexNone)
                    {
                        Remaining = Entry.Mid(Separator + 1);
                        Entry = Entry.Left(Separator);
                    }
                    const FString Path = Format("/Game/{}", Entry);
                    if (VFS::IsDirectory(Path))
                    {
                        VFS::RecursiveDirectoryIterator(Path, [&](const VFS::FFileInfo& File)
                        {
                            if (File.IsLAsset())
                            {
                                Preload(File.VirtualPath);
                            }
                        });
                    }
                    else
                    {
                        Preload(Path);
                    }
                }
            }

            VirtualPath = DiskPath.Mid(GameAt);
            CPackage* Package = CPackage::LoadPackage(VirtualPath);
            if (Package == nullptr)
            {
                return "the graph package did not load";
            }
            Graph = Cast<CAnimationGraph>(Package->LoadObjectByName(FName(VFS::FileName(VirtualPath, true))));
            if (!Graph.IsValid() || !Graph->IsCompiled() || !Graph->Skeleton.IsValid())
            {
                return "the graph is missing, uncompiled or has no skeleton";
            }
            Skeleton = Graph->Skeleton->GetSkeletonResource();
            if (Skeleton == nullptr)
            {
                return "the graph's skeleton has no resource";
            }

            for (const SAnimGraphObjectParameter& Param : Graph->ObjectParameters)
            {
                ObjectValues.Add(MatchObjectParameter(Param, Preloaded));
            }
            for (int32 p = 0; p < NumLocomotionParams; ++p)
            {
                ParamIndex[p] = Constants::kIndexNone;
                for (int32 i = 0; i < Graph->Parameters.Num(); ++i)
                {
                    if (Graph->Parameters[i].Name == FName(kLocomotionParamNames[p]))
                    {
                        ParamIndex[p] = i;
                    }
                }
            }
            RootBone = RootMotion::ResolveRootBoneIndex(Skeleton, FName());
            return FString();
        }

        TUniquePtr<FCrowdCharacter> MakeCharacter(int32 Index, bool bPersistentRegisters = true) const
        {
            TUniquePtr<FCrowdCharacter> Character = MakeUnique<FCrowdCharacter>();
            Character->State.bPersistentRegisters = bPersistentRegisters;
            FAnimationGraphVM::InitState(Graph.Get(), Character->State);
            for (int32 o = 0; o < ObjectValues.Num(); ++o)
            {
                Character->State.ObjectParameters[o] = ObjectValues[o];
            }
            Character->RootMotion.RootBoneIndex = RootBone;
            Character->Walker.Seed = 0x9E3779B9u * (uint32)(Index + 1);
            Character->Walker.UntilDecision = 6.0f * Character->Walker.Random();
            return Character;
        }

        void Drive(FCrowdCharacter& Character) const
        {
            Character.Walker.Tick(kGraphStep, ParamIndex, Character.State.Parameters);
        }

        void Update(FCrowdCharacter& Character, FAnimTaskList& Tasks) const
        {
            Character.Notifies.Reset();
            FAnimationGraphVM::BuildTasks(Graph.Get(), Skeleton, kGraphStep, Character.State, Tasks, Character.RootMotion, &Character.Notifies);
        }

        void UpdateRecord(FCrowdCharacter& Character, const FAnimGraphPlan& Plan, float Step = kGraphStep, FAnimTaskList* CapturedTasks = nullptr) const
        {
            Character.Notifies.Reset();
            FAnimationGraphVM::BuildRecord(Graph.Get(), Plan, Skeleton, Step, Character.State, Character.Record, CapturedTasks, Character.RootMotion, &Character.Notifies);
        }

        // The crowd tier's whole CPU side for one character, from graph update to the weighted samples the GPU reads.
        void UpdateCrowd(FCrowdCharacter& Character, FAnimTopologyCache& Cache, FAnimTaskList& Captured, float Step = kGraphStep) const
        {
            UpdateRecord(Character, Graph->CrowdPlan, Step, &Captured);
            const FAnimTopologyEntry* Entry = Cache.Find(Character.Record.TopologyKey);
            if (Entry == nullptr)
            {
                Entry = &Cache.FindOrAdd(Character.Record.TopologyKey, Captured, Character.State);
            }
            Anim::EvaluateSampleProgram(Entry->Program, Character.Record, Character.Weighted);
        }
    };

    // FNV-1a over raw bytes, so floats compare by bit pattern.
    struct FGoldenHash
    {
        uint64 Value = 0xCBF29CE484222325ull;

        template <typename T>
        void Add(const T& Data)
        {
            const uint8* Bytes = reinterpret_cast<const uint8*>(&Data);
            for (size_t i = 0; i < sizeof(T); ++i)
            {
                Value = (Value ^ Bytes[i]) * 0x100000001B3ull;
            }
        }

        void AddName(const FName& Name)
        {
            for (const char* C = *Name; C != nullptr && *C != 0; ++C)
            {
                Value = (Value ^ (uint8)*C) * 0x100000001B3ull;
            }
            Add((uint8)0);
        }

        // An address becomes its index in the array that owns it, so runs with different allocations still agree.
        template <typename T>
        void AddIndexIn(const T* Pointer, const TVector<T>& Owner)
        {
            int64 Index = -1;
            if (Pointer != nullptr && Owner.Num() > 0 && Pointer >= Owner.GetData() && Pointer < Owner.GetData() + Owner.Num())
            {
                Index = Pointer - Owner.GetData();
            }
            Add(Index);
        }
    };

    // Everything one update produced, with every pointer reduced to a stable identity.
    uint64 HashUpdate(const CAnimationGraph& Graph, const FCrowdCharacter& Character)
    {
        FGoldenHash Hash;
        const FAnimTaskList& Tasks = Character.Tasks;
        Hash.Add(Tasks.OutputTask);
        Hash.Add(Tasks.bLockRoot);
        Hash.Add(Tasks.RootBoneIndex);
        Hash.Add(Tasks.Tasks.Num());
        for (const FAnimTask& Task : Tasks.Tasks)
        {
            Hash.Add(Task.Type);
            Hash.Add(Task.DepA);
            Hash.Add(Task.DepB);
            Hash.AddName(Task.Clip != nullptr ? Task.Clip->GetName() : FName());
            Hash.Add(Task.Time);
            Hash.Add(Task.Alpha);
            Hash.Add(Task.AdditiveSpace);
            int64 Mask = -1;
            for (int32 m = 0; m < Graph.BoneMasks.Num(); ++m)
            {
                Mask = Task.MaskWeights == &Graph.BoneMasks[m].Weights ? m : Mask;
            }
            Hash.Add(Mask);
            Hash.AddIndexIn(Task.Inert, Character.State.Inertializers);
            Hash.AddIndexIn(Task.Inert, Character.State.NodeInertializers);
            Hash.AddIndexIn(Task.Dead, Character.State.DeadBlends);
            Hash.AddIndexIn(Task.Snapshot, Character.State.PoseSnapshots);
            Hash.Add(Task.DeltaTime);
            Hash.Add(Task.bCapture);
            Hash.Add(Task.bApply);
            Hash.Add(Task.T);
            Hash.Add(Task.R);
            Hash.Add(Task.S);
            Hash.Add(Task.BoneA);
            Hash.Add(Task.BoneB);
            Hash.Add(Task.BoneC);
            Hash.Add(Task.Space);
            Hash.Add(Task.Mode);
        }
        for (const float Slot : Character.State.StateSlots)
        {
            Hash.Add(Slot);
        }
        for (const float Curve : Character.State.CurveValues)
        {
            Hash.Add(Curve);
        }
        Hash.Add(Character.RootMotion.Delta.Translation);
        Hash.Add(Character.RootMotion.Delta.Rotation);
        Hash.Add(Character.RootMotion.Delta.bHasMotion);
        for (const FAnimNotifyEvent& Event : Character.Notifies)
        {
            Hash.AddName(Event.Name);
            Hash.Add(Event.Weight);
            Hash.Add(Event.Type);
        }
        return Hash.Value;
    }
}

// The update half alone over a real compiled graph, read-only from its project, the CPU work that stays once poses move to the GPU.
TEST(AnimationBench, GraphLogicCrowd)
{
    FRealGraph Real;
    if (const FString Failure = Real.Load(); !Failure.IsEmpty())
    {
        GTEST_SKIP() << *Failure;
    }
    const CAnimationGraph* Graph = Real.Graph.Get();

    int32 MaxStates = 0;
    int32 Transitions = 0;
    for (const FAnimGraphStateMachine& Machine : Graph->StateMachines)
    {
        MaxStates = Math::Max(MaxStates, Machine.StatePoseRegisters.Num());
        Transitions += Machine.Transitions.Num();
    }
    int32 DrivenParams = 0;
    for (const int32 Index : Real.ParamIndex)
    {
        DrivenParams += Index != Constants::kIndexNone ? 1 : 0;
    }
    int32 UnmatchedObjects = 0;
    for (const CObject* Value : Real.ObjectValues)
    {
        UnmatchedObjects += Value == nullptr ? 1 : 0;
    }
    std::printf("\n  %s: %d bytes of bytecode, %d bones, %d parameters (%d driven), %d object parameters (%d unmatched), %d state machines (largest %d states), %d transitions\n",
                *Real.VirtualPath, Graph->Bytecode.Num(), Real.Skeleton->GetNumBones(), Graph->Parameters.Num(), DrivenParams, Graph->ObjectParameters.Num(), UnmatchedObjects,
                Graph->StateMachines.Num(), MaxStates, Transitions);
    std::printf("  plan: %d ops, %d constants, %d parameter loads, %d object loads lifted out of the stream\n", Graph->Plan.Ops.Num(), Graph->Plan.Constants.Num(),
                Graph->Plan.ParameterLoads.Num(), Graph->Plan.ObjectParameterLoads.Num() + Graph->Plan.ObjectConstantLoads.Num());
    std::printf("  crowd plan: %d ops, %d constants, %d parameter loads, %d object loads\n", Graph->CrowdPlan.Ops.Num(), Graph->CrowdPlan.Constants.Num(),
                Graph->CrowdPlan.ParameterLoads.Num(), Graph->CrowdPlan.ObjectParameterLoads.Num() + Graph->CrowdPlan.ObjectConstantLoads.Num());

    // One thread over characters already warm, so this is the cost of the logic itself without scheduling.
    {
        constexpr int32 kSerialCharacters = 1000;
        TVector<TUniquePtr<FCrowdCharacter>> Characters;
        for (int32 c = 0; c < kSerialCharacters; ++c)
        {
            Characters.Add(Real.MakeCharacter(c));
        }
        double SerialNs = 0.0;
        for (int32 Frame = 0; Frame < 240; ++Frame)
        {
            for (TUniquePtr<FCrowdCharacter>& Character : Characters)
            {
                Real.Drive(*Character);
            }
            const uint64 Start = PlatformTime::Cycles();
            for (TUniquePtr<FCrowdCharacter>& Character : Characters)
            {
                Real.Update(*Character, Character->Tasks);
            }
            if (Frame >= 120)
            {
                SerialNs += NanosSince(Start);
            }
        }
        std::printf("  one thread, %d characters: %.2f us per update\n", kSerialCharacters, SerialNs / 120.0 / kSerialCharacters / 1000.0);

        // The crowd plan on the same characters, with its tasks counted to show what the tier gives up.
        double CrowdNs = 0.0;
        int64 CrowdTasks = 0;
        int64 CrowdSamples = 0;
        for (int32 Frame = 0; Frame < 240; ++Frame)
        {
            for (TUniquePtr<FCrowdCharacter>& Character : Characters)
            {
                Real.Drive(*Character);
            }
            const uint64 Start = PlatformTime::Cycles();
            for (TUniquePtr<FCrowdCharacter>& Character : Characters)
            {
                Real.UpdateRecord(*Character, Graph->CrowdPlan);
            }
            if (Frame >= 120)
            {
                CrowdNs += NanosSince(Start);
            }
        }
        int64 TasksByType[(int32)EAnimTaskType::TranslateBone + 1] = {};
        for (TUniquePtr<FCrowdCharacter>& Character : Characters)
        {
            Character->Notifies.Reset();
            FAnimationGraphVM::BuildTasks(Graph, Graph->CrowdPlan, Real.Skeleton, kGraphStep, Character->State, Character->Tasks, Character->RootMotion, &Character->Notifies);
            CrowdTasks += Character->Tasks.Tasks.Num();
            for (const FAnimTask& Task : Character->Tasks.Tasks)
            {
                CrowdSamples += Task.Type == EAnimTaskType::SampleClip ? 1 : 0;
                ++TasksByType[(int32)Task.Type];
            }
        }
        std::printf("  one thread, crowd plan records: %.2f us per update, %.1f tasks and %.1f samples per character\n", CrowdNs / 120.0 / kSerialCharacters / 1000.0,
                    (double)CrowdTasks / kSerialCharacters, (double)CrowdSamples / kSerialCharacters);

        // What a crowd topology is made of decides what the GPU has to implement.
        static constexpr const char* TaskTypeNames[] =
        {
            "RefPose", "SampleClip", "Blend", "BlendMasked", "MakeAdditive", "ApplyAdditive", "Inertialize", "DeadBlend",
            "SaveSnapshot", "LoadSnapshot", "BoneTransform", "TwoBoneIK", "FABRIK", "LookAt", "FootIK", "TranslateBone",
        };
        std::printf("  crowd plan tasks per character:");
        for (int32 t = 0; t < (int32)std::size(TasksByType); ++t)
        {
            if (TasksByType[t] > 0)
            {
                std::printf(" %s %.2f", TaskTypeNames[t], (double)TasksByType[t] / kSerialCharacters);
            }
        }
        std::printf("\n");
    }

    std::printf("  %u threads\n  %8s %12s %12s %16s %16s %16s %12s %12s %14s %14s\n", Jobs::GetNumThreadSlots(), "count", "ms/frame", "ns/char", "scratch ms/frame",
                "record ms/frame", "crowd ms/frame", "tasks/char", "samples/char", "state bytes", "record bytes");
    for (int32 Count : { 100, 1000, 10000, 50000, 100000 })
    {
        TVector<TUniquePtr<FCrowdCharacter>> Characters;
        Characters.Reserve(Count);
        for (int32 c = 0; c < Count; ++c)
        {
            Characters.Add(Real.MakeCharacter(c, false));
        }

        // The driver runs untimed beforehand, so only the graph's own update is on the clock.
        FAnimTopologyCache Topologies;
        uint32 FrameIndex = 0;
        const auto RunFrame = [&](EGraphOutput Output, uint32 RateDivisor)
        {
            Jobs::ParallelFor((uint32)Count, [&](uint32 c) { Real.Drive(*Characters[c]); }, 64);

            // A reduced rate updates an even slice of the crowd each frame with the elapsed time of the whole cycle.
            const uint32 Slice = FrameIndex++ % RateDivisor;
            const float  Step  = kGraphStep * (float)RateDivisor;
            const uint32 SliceCount = ((uint32)Count - Slice + RateDivisor - 1) / RateDivisor;
            const uint64 Start = PlatformTime::Cycles();
            Jobs::ParallelFor(SliceCount, [&](uint32 k)
            {
                FCrowdCharacter& Character = *Characters[Slice + k * RateDivisor];
                if (Output == EGraphOutput::PersistentTasks)
                {
                    Real.Update(Character, Character.Tasks);
                    return;
                }
                if (Output == EGraphOutput::Records || Output == EGraphOutput::CrowdRecords)
                {
                    Real.UpdateRecord(Character, Output == EGraphOutput::Records ? Graph->Plan : Graph->CrowdPlan, Step);
                    return;
                }
                thread_local FAnimTaskList Scratch;
                if (Output == EGraphOutput::CrowdSamples)
                {
                    Real.UpdateCrowd(Character, Topologies, Scratch, Step);
                    return;
                }
                Real.Update(Character, Scratch);
            }, 16);
            return NanosSince(Start);
        };

        // Scratch keeps the recorded tasks in a per-thread list, which shows how much of the crowd cost is per-character output traffic.
        const auto TimeFrames = [&](EGraphOutput Output, int32 Warm, uint32 RateDivisor = 1)
        {
            for (int32 w = 0; w < Warm; ++w)
            {
                RunFrame(Output, RateDivisor);
            }
            const int32 Frames = Count >= 50000 ? 20 : 60;
            double TotalNs = 0.0;
            for (int32 f = 0; f < Frames; ++f)
            {
                TotalNs += RunFrame(Output, RateDivisor);
            }
            return TotalNs / Frames / 1e6;
        };
        const double FrameMs = TimeFrames(EGraphOutput::PersistentTasks, 120);

        int64 TaskTotal = 0;
        int64 SampleTotal = 0;
        for (const TUniquePtr<FCrowdCharacter>& Character : Characters)
        {
            TaskTotal += Character->Tasks.Tasks.Num();
            for (const FAnimTask& Task : Character->Tasks.Tasks)
            {
                SampleTotal += Task.Type == EAnimTaskType::SampleClip ? 1 : 0;
            }
        }
        const size_t StateBytes = LogicStateBytes(Characters[0]->State, Characters[0]->Tasks);
        const double ScratchMs = TimeFrames(EGraphOutput::ScratchTasks, 30);
        const double RecordMs  = TimeFrames(EGraphOutput::Records, 30);
        const double CrowdMs   = TimeFrames(EGraphOutput::CrowdRecords, 30);
        const double CrowdHalfMs    = TimeFrames(EGraphOutput::CrowdRecords, 30, 2);
        const double CrowdQuarterMs = TimeFrames(EGraphOutput::CrowdRecords, 30, 4);
        const double PipelineMs        = TimeFrames(EGraphOutput::CrowdSamples, 30);
        const double PipelineQuarterMs = TimeFrames(EGraphOutput::CrowdSamples, 30, 4);

        THashMap<uint64, int32> Keys;
        int64 WeightedTotal = 0;
        for (const TUniquePtr<FCrowdCharacter>& Character : Characters)
        {
            ++Keys.FindOrAdd(Character->Record.TopologyKey);
            WeightedTotal += Character->Weighted.Num();
        }
        const size_t RecordBytes = sizeof(FAnimTaskRecord) + (size_t)Characters[0]->Record.Payload.Max() * sizeof(float);
        std::printf("  %8d %12.3f %12.1f %16.3f %16.3f %16.3f %12.1f %12.1f %14zu %14zu   %d topologies\n", Count, FrameMs, FrameMs * 1e6 / Count, ScratchMs, RecordMs, CrowdMs,
                    (double)TaskTotal / Count, (double)SampleTotal / Count, StateBytes, RecordBytes, (int32)Keys.Num());
        std::printf("  %8s crowd plan at half rate %.3f ms, quarter rate %.3f ms\n", "", CrowdHalfMs, CrowdQuarterMs);
        std::printf("  %8s crowd pipeline to weighted samples %.3f ms, quarter rate %.3f ms, %.2f samples per character, %d cached topologies\n", "",
                    PipelineMs, PipelineQuarterMs, (double)WeightedTotal / Count, Topologies.Num());

        if (Count == 10000)
        {
            for (int32 m = 0; m < Graph->StateMachines.Num(); ++m)
            {
                const FAnimGraphStateMachine& Machine = Graph->StateMachines[m];
                TVector<int32> Occupancy;
                Occupancy.Init(0, Machine.StatePoseRegisters.Num());
                for (const TUniquePtr<FCrowdCharacter>& Character : Characters)
                {
                    const int32 Current = (int32)Character->State.StateSlots[Machine.CurrentStateSlot];
                    if (Current >= 0 && Current < Occupancy.Num())
                    {
                        ++Occupancy[Current];
                    }
                }
                std::printf("    machine %d occupancy:", m);
                for (int32 Occupants : Occupancy)
                {
                    std::printf(" %d", Occupants);
                }
                std::printf("\n");
            }
        }
    }
    std::printf("\n");
    SUCCEED();
}

// Every update's output hashed per character and frame, written once and compared on later runs, so a rewrite of the update stays bit-exact.
TEST(AnimationBench, GraphLogicGolden)
{
    const char* GoldenFile = std::getenv("LUMINA_ANIM_BENCH_GOLDEN");
    if (GoldenFile == nullptr)
    {
        GTEST_SKIP() << "set LUMINA_ANIM_BENCH_GOLDEN to a file path; the first run writes it and later runs compare against it";
    }
    FRealGraph Real;
    if (const FString Failure = Real.Load(); !Failure.IsEmpty())
    {
        GTEST_SKIP() << *Failure;
    }

    constexpr int32 kCharacters = 256;
    constexpr int32 kFrames     = 900;
    TVector<TUniquePtr<FCrowdCharacter>> Characters;
    for (int32 c = 0; c < kCharacters; ++c)
    {
        Characters.Add(Real.MakeCharacter(c));
    }

    TVector<uint64> Hashes;
    Hashes.Reserve(kCharacters * kFrames);
    for (int32 Frame = 0; Frame < kFrames; ++Frame)
    {
        for (TUniquePtr<FCrowdCharacter>& Character : Characters)
        {
            Real.Drive(*Character);
            Real.Update(*Character, Character->Tasks);
            Hashes.Add(HashUpdate(*Real.Graph, *Character));
        }
    }

    const FString GoldenPath(GoldenFile);
    TVector<uint8> Existing;
    if (!FileHelper::LoadFileToArray(Existing, GoldenPath))
    {
        TVector<uint8> Bytes;
        Bytes.SetNum(Hashes.Num() * (int32)sizeof(uint64));
        Memory::Memcpy(Bytes.GetData(), Hashes.GetData(), (SIZE_T)Bytes.Num());
        ASSERT_TRUE(FileHelper::SaveArrayToFile(Bytes, GoldenPath));
        std::printf("\n  wrote %d update hashes to %s\n", Hashes.Num(), GoldenFile);
        return;
    }

    ASSERT_EQ((SIZE_T)Existing.Num(), (SIZE_T)Hashes.Num() * sizeof(uint64));
    const uint64* Expected = reinterpret_cast<const uint64*>(Existing.GetData());
    int32 Mismatches = 0;
    for (int32 i = 0; i < Hashes.Num(); ++i)
    {
        if (Hashes[i] != Expected[i])
        {
            if (Mismatches == 0)
            {
                std::printf("\n  first mismatch at frame %d, character %d\n", i / kCharacters, i % kCharacters);
            }
            ++Mismatches;
        }
    }
    std::printf("\n  %d of %d updates match the golden run\n", Hashes.Num() - Mismatches, Hashes.Num());
    EXPECT_EQ(Mismatches, 0);
}

// The crowd tier runs the same graph into a record, so a twin running the task list must see identical state, and its record must expand back to that task list.
TEST(AnimationBench, GraphLogicRecordTwin)
{
    FRealGraph Real;
    if (const FString Failure = Real.Load(); !Failure.IsEmpty())
    {
        GTEST_SKIP() << *Failure;
    }

    constexpr int32 kCharacters = 256;
    constexpr int32 kFrames     = 900;
    TVector<TUniquePtr<FCrowdCharacter>> Tasks;
    TVector<TUniquePtr<FCrowdCharacter>> Records;
    for (int32 c = 0; c < kCharacters; ++c)
    {
        Tasks.Add(Real.MakeCharacter(c));
        Records.Add(Real.MakeCharacter(c, false));
    }

    THashMap<uint64, FAnimTopology> Topologies;
    FAnimTaskList Expanded;
    int32 Mismatches = 0;
    int32 MaxPayload = 0;
    for (int32 Frame = 0; Frame < kFrames; ++Frame)
    {
        for (int32 c = 0; c < kCharacters; ++c)
        {
            FCrowdCharacter& A = *Tasks[c];
            FCrowdCharacter& B = *Records[c];
            Real.Drive(A);
            B.State.Parameters = A.State.Parameters;
            Real.Update(A, A.Tasks);
            Real.UpdateRecord(B, Real.Graph->Plan);
            MaxPayload = Math::Max(MaxPayload, B.Record.Payload.Num());

            // The first character to reach a key supplies its template, so a key two topologies share shows up as a later mismatch.
            const bool bKnown = Topologies.Contains(B.Record.TopologyKey);
            FAnimTopology& Topology = Topologies.FindOrAdd(B.Record.TopologyKey);
            if (!bKnown)
            {
                Anim::MakeTopology(A.Tasks, A.State, Topology);
            }

            Anim::ExpandTaskList(Topology, B.Record, B.State, Expanded);
            std::swap(B.Tasks, Expanded);
            const bool bSame = HashUpdate(*Real.Graph, A) == HashUpdate(*Real.Graph, B) && Topology.PayloadFloats == B.Record.Payload.Num();
            std::swap(B.Tasks, Expanded);
            if (!bSame)
            {
                if (Mismatches == 0)
                {
                    std::printf("\n  first mismatch at frame %d, character %d\n", Frame, c);
                    int32 SlotDiffs = 0;
                    for (int32 i = 0; i < A.State.StateSlots.Num(); ++i)
                    {
                        SlotDiffs += A.State.StateSlots[i] != B.State.StateSlots[i] ? 1 : 0;
                    }
                    std::printf("  TEMP slot diffs %d, tasks %d vs template %d, payload %d vs %d, known %d\n", SlotDiffs, A.Tasks.Tasks.Num(), Topology.Tasks.Num(),
                                B.Record.Payload.Num(), Topology.PayloadFloats, (int32)bKnown);
                    for (int32 t = 0; t < Math::Min(A.Tasks.Tasks.Num(), Topology.Tasks.Num()); ++t)
                    {
                        const FAnimTask& X = A.Tasks.Tasks[t];
                        const FAnimTask& Y = Expanded.Tasks[t];
                        if (X.Type != Y.Type || X.DepA != Y.DepA || X.DepB != Y.DepB || X.Clip != Y.Clip || X.Time != Y.Time || X.Alpha != Y.Alpha || X.MaskWeights != Y.MaskWeights
                            || X.bCapture != Y.bCapture || X.bApply != Y.bApply || X.BoneA != Y.BoneA || X.DeltaTime != Y.DeltaTime)
                        {
                            std::printf("  TEMP task %d differs: type %d/%d deps %d,%d/%d,%d clip %p/%p time %g/%g alpha %g/%g cap %d/%d apply %d/%d\n", t, (int32)X.Type, (int32)Y.Type,
                                        X.DepA, X.DepB, Y.DepA, Y.DepB, (const void*)X.Clip, (const void*)Y.Clip, X.Time, Y.Time, X.Alpha, Y.Alpha, (int32)X.bCapture, (int32)Y.bCapture,
                                        (int32)X.bApply, (int32)Y.bApply);
                        }
                    }
                }
                ++Mismatches;
            }
        }
    }
    std::printf("\n  %d topologies over %d updates, payload up to %d floats, %d mismatches\n", (int32)Topologies.Num(), kCharacters * kFrames, MaxPayload, Mismatches);
    EXPECT_EQ(Mismatches, 0);
}

// The crowd plan's records flatten to weighted clip samples, the GPU kernel's input, so their weights must cover the pose exactly once.
TEST(AnimationBench, GraphLogicCrowdSamples)
{
    FRealGraph Real;
    if (const FString Failure = Real.Load(); !Failure.IsEmpty())
    {
        GTEST_SKIP() << *Failure;
    }
    const CAnimationGraph* Graph = Real.Graph.Get();

    constexpr int32 kCharacters = 256;
    constexpr int32 kFrames     = 900;
    TVector<TUniquePtr<FCrowdCharacter>> Tasks;
    TVector<TUniquePtr<FCrowdCharacter>> Records;
    for (int32 c = 0; c < kCharacters; ++c)
    {
        Tasks.Add(Real.MakeCharacter(c));
        Records.Add(Real.MakeCharacter(c, false));
    }

    struct FKnownTopology
    {
        FAnimTopology      Topology;
        FAnimSampleProgram Program;
    };
    THashMap<uint64, FKnownTopology> Known;
    FAnimTaskList Expanded;
    TVector<FAnimWeightedSample> Weighted;
    int32  Mismatches       = 0;
    int32  WeightErrors     = 0;
    int32  Inexpressible    = 0;
    int64  SampleTotal      = 0;
    int32  MaxSamples       = 0;
    int64  SampleCountHistogram[17] = {};
    int64  TransitionTotal  = 0;
    int64  CapturesStarted  = 0;
    double EvaluateNs       = 0.0;
    for (int32 Frame = 0; Frame < kFrames; ++Frame)
    {
        for (int32 c = 0; c < kCharacters; ++c)
        {
            FCrowdCharacter& A = *Tasks[c];
            FCrowdCharacter& B = *Records[c];
            Real.Drive(A);
            B.State.Parameters = A.State.Parameters;
            A.Notifies.Reset();
            FAnimationGraphVM::BuildTasks(Graph, Graph->CrowdPlan, Real.Skeleton, kGraphStep, A.State, A.Tasks, A.RootMotion, &A.Notifies);
            Real.UpdateRecord(B, Graph->CrowdPlan);

            const bool bKnown = Known.Contains(B.Record.TopologyKey);
            FKnownTopology& Entry = Known.FindOrAdd(B.Record.TopologyKey);
            if (!bKnown)
            {
                Anim::MakeTopology(A.Tasks, A.State, Entry.Topology);
                Anim::MakeSampleProgram(Entry.Topology, Entry.Program);
            }

            Anim::ExpandTaskList(Entry.Topology, B.Record, B.State, Expanded);
            std::swap(B.Tasks, Expanded);
            const bool bSame = HashUpdate(*Real.Graph, A) == HashUpdate(*Real.Graph, B);
            std::swap(B.Tasks, Expanded);
            Mismatches += bSame ? 0 : 1;

            if (!Entry.Program.bExpressible)
            {
                ++Inexpressible;
                continue;
            }
            const uint64 Start = PlatformTime::Cycles();
            Anim::EvaluateSampleProgram(Entry.Program, B.Record, Weighted);
            EvaluateNs += NanosSince(Start);

            float WeightSum = 0.0f;
            for (const FAnimWeightedSample& Sample : Weighted)
            {
                WeightSum += Sample.Weight;
            }
            WeightErrors += Math::Abs(WeightSum - 1.0f) < 1e-4f ? 0 : 1;
            SampleTotal += Weighted.Num();
            MaxSamples = Math::Max(MaxSamples, Weighted.Num());
            ++SampleCountHistogram[Math::Min(Weighted.Num(), 16)];
            TransitionTotal += Entry.Program.Transitions.Num();
            for (const FAnimSampleProgram::FTransition& Transition : Entry.Program.Transitions)
            {
                CapturesStarted += ((int32)B.Record.Payload[Transition.FlagsFloat] & 1) != 0 ? 1 : 0;
            }
        }
    }

    const double Updates = (double)kCharacters * kFrames;
    std::printf("\n  %d crowd topologies; %d updates not expressible as a weighted sum, %d mismatches, %d weight sums off by over 1e-4\n",
                (int32)Known.Num(), Inexpressible, Mismatches, WeightErrors);
    std::printf("  per character update: %.2f weighted samples (at most %d), %.2f transitions, %.4f transition starts, flattening %.0f ns\n",
                SampleTotal / Updates, MaxSamples, TransitionTotal / Updates, CapturesStarted / Updates, EvaluateNs / Updates);
    std::printf("  share of updates by weighted sample count:");
    for (int32 n = 1; n <= 16; ++n)
    {
        if (SampleCountHistogram[n] > 0)
        {
            std::printf(" %d:%.3f", n, SampleCountHistogram[n] / Updates);
        }
    }
    std::printf("\n");
    EXPECT_EQ(Mismatches, 0);
    EXPECT_EQ(WeightErrors, 0);
}

namespace
{
    // The GPU crowd kernel's math on the CPU, from baked clips to skinning matrices, so its output compares against the executor's.
    struct FGpuPoseEmulator
    {
        FSkeletonResource& Skeleton;
        THashMap<const CAnimation*, FAnimGpuClip> Baked;
        TVector<int32> DepthOrder;

        explicit FGpuPoseEmulator(FSkeletonResource& InSkeleton) : Skeleton(InSkeleton)
        {
            const int32 NumBones = Skeleton.GetNumBones();
            TVector<int32> Depth;
            Depth.Init(0, NumBones);
            for (int32 Pass = 0; Pass < NumBones; ++Pass)
            {
                for (int32 b = 0; b < NumBones; ++b)
                {
                    const int32 Parent = Skeleton.Bones[b].ParentIndex;
                    Depth[b] = Parent < 0 ? 0 : Depth[Parent] + 1;
                }
            }
            for (int32 b = 0; b < NumBones; ++b)
            {
                DepthOrder.Add(b);
            }
            Algo::Sort(DepthOrder, [&](int32 A, int32 B) { return Depth[A] < Depth[B]; });
        }

        static FQuat UnpackSnorm4(const FUIntVector2& P)
        {
            const auto Component = [](uint32 Bits) { return Math::Max((float)(int16)(uint16)Bits / 32767.0f, -1.0f); };
            return FQuat(Component(P.y >> 16), Component(P.x), Component(P.x >> 16), Component(P.y));
        }

        void SampleBone(const FAnimGpuClip& Clip, float Time, int32 Bone, FQuat& Q, FVector3& T) const
        {
            uint32 F0 = 0;
            uint32 F1 = 0;
            float Alpha = 0.0f;
            if (Clip.NumFrames > 1 && Clip.Duration > 0.0f)
            {
                const float Position = Math::Clamp(Time / Clip.Duration, 0.0f, 1.0f) * (float)(Clip.NumFrames - 1);
                F0 = Math::Min((uint32)Math::Floor(Position), Clip.NumFrames - 1);
                F1 = Math::Min(F0 + 1, Clip.NumFrames - 1);
                Alpha = Position - Math::Floor(Position);
            }
            const int32 I0 = (int32)(F0 * Clip.NumBones) + Bone;
            const int32 I1 = (int32)(F1 * Clip.NumBones) + Bone;
            const FQuat QA = UnpackSnorm4(Clip.Rotations[I0]);
            FQuat QB = UnpackSnorm4(Clip.Rotations[I1]);
            if (Math::Dot(QA, QB) < 0.0f)
            {
                QB = -QB;
            }
            Q = Math::Normalize(QA * (1.0f - Alpha) + QB * Alpha);
            T = Math::Mix(FVector3(Clip.Translations[I0]), FVector3(Clip.Translations[I1]), Alpha);
        }

        void Evaluate(TSpan<const FAnimWeightedSample> Samples, int32 RootLockBone, TVector<FMatrix4>& OutSkinning)
        {
            const int32 NumBones = Skeleton.GetNumBones();
            // Every clip is baked before any address is taken, since adding to the map can move its entries.
            for (const FAnimWeightedSample& Sample : Samples)
            {
                if (Sample.Clip != nullptr && !Baked.Contains(Sample.Clip))
                {
                    Anim::BakeGpuClip(*Sample.Clip, Skeleton, Baked.FindOrAdd(Sample.Clip));
                }
            }
            TVector<const FAnimGpuClip*> Clips;
            for (const FAnimWeightedSample& Sample : Samples)
            {
                const FAnimGpuClip* Clip = Sample.Clip != nullptr ? Baked.Find(Sample.Clip) : nullptr;
                Clips.Add(Clip != nullptr && Clip->NumFrames > 0 ? Clip : nullptr);
            }

            TVector<FMatrix4> World;
            World.SetNum(NumBones);
            OutSkinning.SetNum(NumBones);
            for (const int32 b : DepthOrder)
            {
                FQuat Q(0.0f, 0.0f, 0.0f, 0.0f);
                FVector3 T(0.0f);
                FQuat Reference = Skeleton.BindLocalRotations[b];
                for (int32 s = 0; s < Samples.Num(); ++s)
                {
                    FQuat QS = Skeleton.BindLocalRotations[b];
                    FVector3 TS = Skeleton.BindLocalTranslations[b];
                    if (Clips[s] != nullptr && b != RootLockBone)
                    {
                        SampleBone(*Clips[s], Samples[s].Time, b, QS, TS);
                    }
                    Reference = s == 0 ? QS : Reference;
                    if (Math::Dot(Reference, QS) < 0.0f)
                    {
                        QS = -QS;
                    }
                    Q = Q + QS * Samples[s].Weight;
                    T = T + TS * Samples[s].Weight;
                }
                const FMatrix4 Local = AnimPose::ComposeTRS(T, Math::Normalize(Q), FVector3(1.0f));
                const int32 Parent = Skeleton.Bones[b].ParentIndex;
                World[b] = Parent < 0 ? Local : World[Parent] * Local;
                OutSkinning[b] = World[b] * Skeleton.Bones[b].InvBindMatrix;
            }
        }
    };
}

// How far the crowd tier's pose strays from the CPU executor on the same updates, for the translation each skinning matrix applies.
TEST(AnimationBench, GraphLogicCrowdPoseError)
{
    FRealGraph Real;
    if (const FString Failure = Real.Load(); !Failure.IsEmpty())
    {
        GTEST_SKIP() << *Failure;
    }
    const CAnimationGraph* Graph = Real.Graph.Get();

    constexpr int32 kCharacters = 64;
    constexpr int32 kFrames     = 600;
    TVector<TUniquePtr<FCrowdCharacter>> Tasks;
    TVector<TUniquePtr<FCrowdCharacter>> Records;
    for (int32 c = 0; c < kCharacters; ++c)
    {
        Tasks.Add(Real.MakeCharacter(c));
        Records.Add(Real.MakeCharacter(c, false));
    }

    FAnimTopologyCache Topologies;
    FAnimTaskList Captured;
    FGpuPoseEmulator Emulator(*Real.Skeleton);
    TVector<FMatrix4> Reference;
    TVector<FMatrix4> Emulated;
    TVector<float> FrameErrors;
    TVector<float> SteadyErrors;
    TVector<float> TransitionErrors;
    for (int32 Frame = 0; Frame < kFrames; ++Frame)
    {
        for (int32 c = 0; c < kCharacters; ++c)
        {
            FCrowdCharacter& A = *Tasks[c];
            FCrowdCharacter& B = *Records[c];
            Real.Drive(A);
            B.State.Parameters = A.State.Parameters;

            A.Notifies.Reset();
            FAnimationGraphVM::BuildTasks(Graph, Graph->CrowdPlan, Real.Skeleton, kGraphStep, A.State, A.Tasks, A.RootMotion, &A.Notifies);

            // Read before the executor consumes the list, whether any seam is mid-blend, which the GPU tier does not smooth yet.
            bool bTransitioning = false;
            for (const FAnimTask& Task : A.Tasks.Tasks)
            {
                bTransitioning |= Task.Type == EAnimTaskType::Inertialize && Task.bApply;
            }
            Anim::ExecuteTaskList(A.Tasks, Reference, nullptr);

            Real.UpdateRecord(B, Graph->CrowdPlan, kGraphStep, &Captured);
            const FAnimTopologyEntry* Entry = Topologies.Find(B.Record.TopologyKey);
            if (Entry == nullptr)
            {
                Entry = &Topologies.FindOrAdd(B.Record.TopologyKey, Captured, B.State);
            }
            if (!Entry->Program.bExpressible)
            {
                continue;
            }
            Anim::EvaluateSampleProgram(Entry->Program, B.Record, B.Weighted);
            const int32 RootLock = Captured.bLockRoot ? Captured.RootBoneIndex : Constants::kIndexNone;
            Emulator.Evaluate(B.Weighted, RootLock, Emulated);

            float Worst = 0.0f;
            for (int32 b = 0; b < Reference.Num() && b < Emulated.Num(); ++b)
            {
                Worst = Math::Max(Worst, Math::Length(FVector3(Reference[b][3]) - FVector3(Emulated[b][3])));
            }
            FrameErrors.Add(Worst);
            if (bTransitioning)
            {
                TransitionErrors.Add(Worst);
            }
            else
            {
                SteadyErrors.Add(Worst);
            }
        }
    }

    const auto Report = [](const char* Label, TVector<float>& Errors)
    {
        Algo::Sort(Errors);
        const auto Percentile = [&](double P) { return Errors.IsEmpty() ? 0.0f : Errors[Math::Min((int32)(P * Errors.Num()), Errors.Num() - 1)]; };
        std::printf("  %-16s %7d updates: median %.5f, p90 %.5f, p99 %.5f, max %.5f\n", Label, Errors.Num(), Percentile(0.5), Percentile(0.9), Percentile(0.99), Percentile(1.0));
    };
    std::printf("\n  crowd pose against the CPU executor, worst bone translation per update (model units)\n");
    Report("all", FrameErrors);
    Report("steady", SteadyErrors);
    Report("mid-transition", TransitionErrors);
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
