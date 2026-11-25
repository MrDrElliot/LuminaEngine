#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Matrix/Matrix.h"
#include "Core/Math/Vector/VectorTypes.h"

namespace Lumina
{
    class CAnimation;
    struct FSkeletonResource;
    struct FAnimWeightedSample;

    // A clip resampled against one skeleton at its own keyframes, so a GPU interpolates whole local poses with no track decoding or retargeting of its own.
    struct FAnimGpuClip
    {
        uint32 NumFrames = 0;
        uint32 NumBones  = 0;
        float  Duration  = 0.0f;

        // Frame-major with a frame's bones contiguous, each rotation four 16-bit signed normalized components in x, y, z, w order.
        TVector<FUIntVector2> Rotations;
        TVector<FVector4>     Translations;
    };

    namespace Anim
    {
        // One workgroup of bones, one thread a bone, so the GPU pose pass needs no bone loop.
        constexpr int32 kMaxGpuPoseBones = 128;

        // False for a clip the weighted-sum backend cannot play, an additive delta or one that scales bones.
        RUNTIME_API bool BakeGpuClip(const CAnimation& Clip, FSkeletonResource& Skeleton, FAnimGpuClip& Out);

        // One bone's component-space matrix from the samples the GPU pose pass sums, walking only its ancestors, for CPU readers of a GPU pose.
        RUNTIME_API FMatrix4 EvaluateWeightedBoneGlobal(FSkeletonResource& Skeleton, const TVector<FAnimWeightedSample>& Samples, int32 RootLockBone, int32 Bone);
    }
}
