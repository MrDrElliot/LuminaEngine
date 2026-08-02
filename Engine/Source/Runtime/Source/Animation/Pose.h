#pragma once

#include "Containers/Array.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    struct FSkeletonResource;

    // Local-space skeletal pose (per-bone TRS relative to parent); the VM blends in this space,
    // FK + InvBind fold into skinning matrices once when the final pose is resolved.
    struct RUNTIME_API FPose
    {
        TVector<FVector3> Translations;
        TVector<FQuat> Rotations;
        TVector<FVector3> Scales;

        FORCEINLINE int32 GetNumBones() const { return (int32)Rotations.size(); }
        FORCEINLINE bool IsValid() const { return !Rotations.empty(); }

        void SetNumBones(int32 NumBones)
        {
            Translations.resize(NumBones);
            Rotations.resize(NumBones);
            Scales.resize(NumBones);
        }

        // Fills every bone with the skeleton's bind-pose local transform.
        void ResetToBindPose(const FSkeletonResource* Skeleton);
    };

    namespace AnimPose
    {
        // Direct TRS -> column-major matrix; same result as Translate * ToMatrix4(R) * Scale
        // without the two 4x4 multiplies. Matches Math::ToMatrix3's quat convention.
        FORCEINLINE FMatrix4 ComposeTRS(const FVector3& T, const FQuat& R, const FVector3& S)
        {
            const float XX = R.x * R.x; const float YY = R.y * R.y; const float ZZ = R.z * R.z;
            const float XY = R.x * R.y; const float XZ = R.x * R.z; const float YZ = R.y * R.z;
            const float WX = R.w * R.x; const float WY = R.w * R.y; const float WZ = R.w * R.z;

            FMatrix4 M;
            M[0] = FVector4((1.0f - 2.0f * (YY + ZZ)) * S.x, (2.0f * (XY + WZ)) * S.x, (2.0f * (XZ - WY)) * S.x, 0.0f);
            M[1] = FVector4((2.0f * (XY - WZ)) * S.y, (1.0f - 2.0f * (XX + ZZ)) * S.y, (2.0f * (YZ + WX)) * S.y, 0.0f);
            M[2] = FVector4((2.0f * (XZ + WY)) * S.z, (2.0f * (YZ - WX)) * S.z, (1.0f - 2.0f * (XX + YY)) * S.z, 0.0f);
            M[3] = FVector4(T.x, T.y, T.z, 1.0f);
            return M;
        }

        // Cheap TRS extract for rigid + per-axis-scale matrices (no skew/projective handling);
        // the shared decomposition for all bind-pose math so results stay bit-consistent.
        FORCEINLINE void DecomposeTRS(const FMatrix4& M, FVector3& OutT, FQuat& OutR, FVector3& OutS)
        {
            OutT = FVector3(M[3]);

            const FVector3 C0(M[0]);
            const FVector3 C1(M[1]);
            const FVector3 C2(M[2]);

            OutS = FVector3(Math::Length(C0), Math::Length(C1), Math::Length(C2));

            const float InvSx = OutS.x > 1e-8f ? 1.0f / OutS.x : 0.0f;
            const float InvSy = OutS.y > 1e-8f ? 1.0f / OutS.y : 0.0f;
            const float InvSz = OutS.z > 1e-8f ? 1.0f / OutS.z : 0.0f;

            FMatrix3 Rot;
            Rot[0] = C0 * InvSx;
            Rot[1] = C1 * InvSy;
            Rot[2] = C2 * InvSz;
            OutR = Math::ToQuat(Rot);
        }

        // NumActiveBones on the kernels below is the skeleton-LOD cut: bones past it pass through
        // from the first input (A / Base) untouched instead of being blended. Negative = all bones.

        // Out = Lerp(A, B, Alpha). A, B and Out may alias. Alpha is clamped to [0,1].
        RUNTIME_API void Blend(const FPose& A, const FPose& B, float Alpha, FPose& Out, int32 NumActiveBones = -1);

        // Per-bone masked blend: the blend alpha for bone i is Alpha * BoneWeights[i].
        // BoneWeights shorter than the bone count treats missing entries as 1.0.
        RUNTIME_API void BlendMasked(const FPose& A, const FPose& B, float Alpha, const TVector<float>& BoneWeights, FPose& Out, int32 NumActiveBones = -1);

        // OutDelta := Src relative to bind pose (T/S differences/ratios, R = Src * inverse(Bind)). Pair with ApplyAdditive.
        // Bones past the LOD cut get the identity delta.
        RUNTIME_API void MakeAdditive(const FPose& Src, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones = -1);

        // Out := Base + Alpha * Delta (TRS-wise): T adds, S lerps from 1, R slerps from identity then post-multiplies base.
        RUNTIME_API void ApplyAdditive(const FPose& Base, const FPose& Delta, float Alpha, FPose& Out, int32 NumActiveBones = -1);

        // Resolves a local-space pose into GPU skinning matrices (Global * InvBind).
        RUNTIME_API void ToSkinningMatrices(const FPose& Pose, const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutMatrices);

        enum class EBoneSpace : uint8
        {
            LocalBone,
            ComponentSpace,
        };

        enum class EBoneApplyMode : uint8
        {
            Add,
            Replace,
        };

        // In-place (T, R, S) on a single bone; Space picks the offset frame, Mode picks layer-vs-replace, Alpha scales it.
        RUNTIME_API void ApplyBoneTransform(FPose& Pose,
                                            const FSkeletonResource* Skeleton,
                                            int32 BoneIndex,
                                            EBoneSpace Space,
                                            EBoneApplyMode Mode,
                                            const FVector3& Translation,
                                            const FQuat& Rotation,
                                            const FVector3& Scale,
                                            float Alpha);

        // In-place analytical two-bone IK so EndIdx reaches Target (component space); Pole picks bend side, Alpha slerps.
        // Requires MidIdx's parent == RootIdx and EndIdx's parent == MidIdx.
        RUNTIME_API void TwoBoneIK(FPose& Pose,
                                   const FSkeletonResource* Skeleton,
                                   int32 RootIdx, int32 MidIdx, int32 EndIdx,
                                   const FVector3& Target,
                                   const FVector3& Pole,
                                   float Alpha);
    }
}
