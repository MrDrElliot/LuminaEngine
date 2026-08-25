#pragma once

#include "Containers/Vector.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Core/Math/SIMD/SIMD.h"
#include "Core/Serialization/Archiver.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    struct FSkeletonResource;

    // Absolute pose, or a delta layered onto one; a mesh-space delta needs the hierarchy to apply.
    enum class EPoseAdditiveSpace : uint8
    {
        None,
        LocalSpace,
        MeshSpace,
    };

    // Local-space pose as ten component streams in one allocation, each padded to the vector width.
    struct RUNTIME_API FPose
    {
        static constexpr int32 LaneWidth  = 8;
        static constexpr int32 NumStreams = 10;

        // Translation and scale are adjacent so a component-wise blend covers both in one range.
        enum EStream : int32
        {
            StreamTx = 0, StreamTy, StreamTz,
            StreamSx,     StreamSy, StreamSz,
            StreamRx,     StreamRy, StreamRz, StreamRw,
        };

        static FORCEINLINE int32 StrideFor(int32 InNumBones)
        {
            return (InNumBones + (LaneWidth - 1)) & ~(LaneWidth - 1);
        }

        FPose() = default;
        FPose(const FPose& Other);
        FPose(FPose&& Other) noexcept;
        FPose& operator=(const FPose& Other);
        FPose& operator=(FPose&& Other) noexcept;
        ~FPose();

        FORCEINLINE int32 GetNumBones() const { return NumBones; }
        FORCEINLINE int32 GetStride() const { return Stride; }
        FORCEINLINE bool IsValid() const { return NumBones > 0; }
        FORCEINLINE bool IsAdditive() const { return AdditiveSpace != EPoseAdditiveSpace::None; }

        FORCEINLINE float* Stream(int32 Index) { return Data + (SIZE_T)Index * Stride; }
        FORCEINLINE const float* Stream(int32 Index) const { return Data + (SIZE_T)Index * Stride; }

        FORCEINLINE float* Tx() { return Stream(StreamTx); }
        FORCEINLINE float* Ty() { return Stream(StreamTy); }
        FORCEINLINE float* Tz() { return Stream(StreamTz); }
        FORCEINLINE float* Sx() { return Stream(StreamSx); }
        FORCEINLINE float* Sy() { return Stream(StreamSy); }
        FORCEINLINE float* Sz() { return Stream(StreamSz); }
        FORCEINLINE float* Rx() { return Stream(StreamRx); }
        FORCEINLINE float* Ry() { return Stream(StreamRy); }
        FORCEINLINE float* Rz() { return Stream(StreamRz); }
        FORCEINLINE float* Rw() { return Stream(StreamRw); }

        FORCEINLINE const float* Tx() const { return Stream(StreamTx); }
        FORCEINLINE const float* Ty() const { return Stream(StreamTy); }
        FORCEINLINE const float* Tz() const { return Stream(StreamTz); }
        FORCEINLINE const float* Sx() const { return Stream(StreamSx); }
        FORCEINLINE const float* Sy() const { return Stream(StreamSy); }
        FORCEINLINE const float* Sz() const { return Stream(StreamSz); }
        FORCEINLINE const float* Rx() const { return Stream(StreamRx); }
        FORCEINLINE const float* Ry() const { return Stream(StreamRy); }
        FORCEINLINE const float* Rz() const { return Stream(StreamRz); }
        FORCEINLINE const float* Rw() const { return Stream(StreamRw); }

        FORCEINLINE SIMD::FQuatStreams Rotations() { return { Rx(), Ry(), Rz(), Rw() }; }
        FORCEINLINE SIMD::FConstQuatStreams Rotations() const { return { Rx(), Ry(), Rz(), Rw() }; }

        FORCEINLINE FVector3 GetTranslation(int32 i) const { return FVector3(Tx()[i], Ty()[i], Tz()[i]); }
        FORCEINLINE FVector3 GetScale(int32 i) const { return FVector3(Sx()[i], Sy()[i], Sz()[i]); }
        FORCEINLINE FQuat GetRotation(int32 i) const { return FQuat(Rw()[i], Rx()[i], Ry()[i], Rz()[i]); }

        FORCEINLINE void SetTranslation(int32 i, const FVector3& V) { Tx()[i] = V.x; Ty()[i] = V.y; Tz()[i] = V.z; }
        FORCEINLINE void SetScale(int32 i, const FVector3& V) { Sx()[i] = V.x; Sy()[i] = V.y; Sz()[i] = V.z; }
        FORCEINLINE void SetRotation(int32 i, const FQuat& Q) { Rx()[i] = Q.x; Ry()[i] = Q.y; Rz()[i] = Q.z; Rw()[i] = Q.w; }

        FORCEINLINE void GetBone(int32 i, FVector3& OutT, FQuat& OutR, FVector3& OutS) const
        {
            OutT = GetTranslation(i);
            OutR = GetRotation(i);
            OutS = GetScale(i);
        }

        FORCEINLINE void SetBone(int32 i, const FVector3& T, const FQuat& R, const FVector3& S)
        {
            SetTranslation(i, T);
            SetRotation(i, R);
            SetScale(i, S);
        }

        FORCEINLINE void CopyBoneFrom(const FPose& Other, int32 Index)
        {
            for (int32 s = 0; s < NumStreams; ++s)
            {
                Stream(s)[Index] = Other.Stream(s)[Index];
            }
        }

        // Existing bones survive a resize; new bones and the pad past the last bone read identity.
        void SetNumBones(int32 InNumBones);

        // Exchanges storage with Other; the executor uses this where a copy would only be a hand-off.
        void Swap(FPose& Other);

        // Fills every bone with the skeleton's bind-pose local transform.
        void ResetToBindPose(const FSkeletonResource* Skeleton);

        // Every kernel that writes a pose stamps this, since pose buffers are pooled and reused.
        EPoseAdditiveSpace AdditiveSpace = EPoseAdditiveSpace::None;

    private:

        void Relayout(int32 NewStride, int32 NewNumBones);
        void FillIdentity(int32 First, int32 Last);

        float* Data     = nullptr;
        int32  NumBones = 0;
        int32  Stride   = 0;
        int32  Capacity = 0;
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

        // MakeAdditive against an arbitrary base pose instead of the bind pose.
        RUNTIME_API void MakeAdditiveFromBase(const FPose& Src, const FPose& Base, FPose& OutDelta, int32 NumActiveBones = -1);

        // Rotation delta between component-space rotations, so it survives the base's parent chain. T/S stay local.
        RUNTIME_API void MakeAdditiveMeshSpace(const FPose& Src, const FPose& Base, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones = -1);
        RUNTIME_API void MakeAdditiveMeshSpace(const FPose& Src, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones = -1);

        // Out := Base + Alpha * Delta (TRS-wise): T adds, S lerps from 1, R slerps from identity then post-multiplies base.
        RUNTIME_API void ApplyAdditive(const FPose& Base, const FPose& Delta, float Alpha, FPose& Out, int32 NumActiveBones = -1);

        // Counterpart of MakeAdditiveMeshSpace: rotates each component-space rotation, then converts back.
        RUNTIME_API void ApplyAdditiveMeshSpace(const FPose& Base, const FPose& Delta, float Alpha, const FSkeletonResource* Skeleton, FPose& Out, int32 NumActiveBones = -1);

        // Dispatches on Delta's own AdditiveSpace, so callers holding a pose from anywhere apply it right.
        RUNTIME_API void ApplyAdditivePose(const FPose& Base, const FPose& Delta, float Alpha, const FSkeletonResource* Skeleton, FPose& Out, int32 NumActiveBones = -1);

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
        // Iterative chain solver (FABRIK) reaching Target with the chain from RootIdx down to TipIdx.
        // TipIdx must descend from RootIdx. Positions are component space, as everywhere else here.
        RUNTIME_API void FABRIK(FPose& Pose, const FSkeletonResource* Skeleton, int32 RootIdx, int32 TipIdx,
                                const FVector3& Target, int32 Iterations, float Alpha);

        // Turns one bone so its LocalForward axis points at Target, clamped to MaxAngleRadians of its
        // rest direction. 0 or less leaves it unclamped.
        RUNTIME_API void LookAt(FPose& Pose, const FSkeletonResource* Skeleton, int32 BoneIdx,
                                const FVector3& Target, const FVector3& LocalForward, float MaxAngleRadians, float Alpha);

        // Two bone IK onto the foot displaced by Offset, then the foot rotated onto GroundNormal.
        // Offset and GroundNormal are component space, so the caller converts its trace results once.
        RUNTIME_API void FootIK(FPose& Pose, const FSkeletonResource* Skeleton, int32 ThighIdx, int32 CalfIdx,
                                int32 FootIdx, const FVector3& Offset, const FVector3& GroundNormal,
                                const FVector3& FootUpAxis, float NormalAlpha, float Alpha);

        // Displaces one bone by Offset in component space, keeping its rotation. What lowers a pelvis
        // by the amount the feet needed, which no baked transform can express.
        RUNTIME_API void TranslateBoneComponentSpace(FPose& Pose, const FSkeletonResource* Skeleton, int32 BoneIdx,
                                                     const FVector3& Offset, float Alpha);

        RUNTIME_API void TwoBoneIK(FPose& Pose,
                                   const FSkeletonResource* Skeleton,
                                   int32 RootIdx, int32 MidIdx, int32 EndIdx,
                                   const FVector3& Target,
                                   const FVector3& Pole,
                                   float Alpha);
    }
}
