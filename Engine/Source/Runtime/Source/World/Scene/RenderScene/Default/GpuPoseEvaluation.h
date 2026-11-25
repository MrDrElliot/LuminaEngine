#pragma once

#include "Animation/TaskSystem/AnimTaskRecord.h"
#include "Containers/HashTable.h"
#include "SceneBuffer.h"

namespace Lumina
{
    class CAnimation;
    struct FSkeletonResource;

    // A GPU-crowd mesh the gather found, holding pointers the render thread resolves into baked clips and skeletons.
    struct FGpuPoseRequest
    {
        uint32             BoneSlice    = 0;
        uint32             FirstSample  = 0;
        uint32             NumSamples   = 0;
        int32              RootLockBone = Constants::kIndexNone;
        FSkeletonResource* Skeleton     = nullptr;
    };

    // Marks a sample of no clip, which plays the skeleton's bind pose.
    constexpr uint32 kGpuPoseBindClip = 0xFFFFFFFFu;

    // The GPU mirrors below match AnimEvaluatePose.slang.
    struct FGpuPoseJob
    {
        uint32 ArenaBase    = 0;
        uint32 Skeleton     = 0;
        uint32 FirstSample  = 0;
        uint32 NumSamples   = 0;
        int32  RootLockBone = Constants::kIndexNone;
        uint32 _Pad0        = 0;
        uint32 _Pad1        = 0;
        uint32 _Pad2        = 0;
    };
    static_assert(sizeof(FGpuPoseJob) == 32, "FGpuPoseJob must match AnimEvaluatePose.slang");

    struct FGpuPoseSample
    {
        uint32 Clip   = kGpuPoseBindClip;
        float  Time   = 0.0f;
        float  Weight = 0.0f;
        uint32 _Pad   = 0;
    };
    static_assert(sizeof(FGpuPoseSample) == 16, "FGpuPoseSample must match AnimEvaluatePose.slang");

    // A baked clip's first frame-bone entry in the pools, frame-major with a frame's bones contiguous.
    struct FGpuPoseClip
    {
        uint32 FrameBase = 0;
        uint32 NumFrames = 0;
        uint32 NumBones  = 0;
        float  Duration  = 0.0f;
    };
    static_assert(sizeof(FGpuPoseClip) == 16, "FGpuPoseClip must match AnimEvaluatePose.slang");

    struct FGpuPoseSkeleton
    {
        uint32 BoneBase = 0;
        uint32 NumBones = 0;
        uint32 MaxDepth = 0;
        uint32 _Pad     = 0;
    };
    static_assert(sizeof(FGpuPoseSkeleton) == 16, "FGpuPoseSkeleton must match AnimEvaluatePose.slang");

    // A pool the CPU appends to and the GPU mirrors, uploading only what it has not sent yet.
    template <typename T>
    struct TGpuPosePool
    {
        TVector<T>   Data;
        FSceneBuffer Buffer;
        int32        Uploaded = 0;

        explicit TGpuPosePool(const char* DebugName) : Buffer(DebugName, 1.5f, EBufferInit::Undefined, false) {}
    };

    // Clips baked per skeleton and the skeletons they play on, owned by the scene renderer and grown on the render thread as crowds reach new clips.
    struct FGpuPoseRegistry
    {
        THashMap<const FSkeletonResource*, uint32> SkeletonIndex;
        THashMap<const FSkeletonResource*, THashMap<const CAnimation*, uint32>> ClipIndexBySkeleton;

        TGpuPosePool<FGpuPoseClip>     Clips        { "GpuPose.Clips" };
        TGpuPosePool<FUIntVector2>     Rotations    { "GpuPose.Rotations" };
        TGpuPosePool<FVector4>         Translations { "GpuPose.Translations" };
        TGpuPosePool<FGpuPoseSkeleton> Skeletons    { "GpuPose.Skeletons" };
        TGpuPosePool<FIntVector2>      BoneInfo     { "GpuPose.BoneInfo" };
        TGpuPosePool<FVector4>         InvBind      { "GpuPose.InvBind" };
        TGpuPosePool<FVector4>         BindLocal    { "GpuPose.BindLocal" };

        // This frame's resolved jobs and samples, uploaded transiently.
        TVector<FGpuPoseJob>    Jobs;
        TVector<FGpuPoseSample> Samples;

        // Index of the skeleton's GPU record, built on first use.
        uint32 ResolveSkeleton(const FSkeletonResource& Skeleton);

        // Index of the clip baked against this skeleton, or kGpuPoseBindClip for a clip the GPU cannot play.
        uint32 ResolveClip(const CAnimation* Clip, FSkeletonResource& Skeleton);

        void Release();
    };
}
