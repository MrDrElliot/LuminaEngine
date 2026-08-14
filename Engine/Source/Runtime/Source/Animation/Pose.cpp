#include "RuntimePCH.h"
#include "Pose.h"

#include "BindPose.h"
#include "Renderer/MeshData.h"
#include "Core/Math/SIMD/SIMD.h"
#include "Memory/Memcpy.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    void FSkeletonResource::BuildBindPoseCache()
    {
        const int32 NumBones = GetNumBones();
        BindLocalTranslations.resize(NumBones);
        BindLocalRotations.resize(NumBones);
        BindLocalScales.resize(NumBones);

        for (int32 i = 0; i < NumBones; ++i)
        {
            AnimPose::DecomposeTRS(Bones[i].LocalTransform, BindLocalTranslations[i], BindLocalRotations[i], BindLocalScales[i]);
        }

        ++BindPoseGeneration;
    }

    namespace Detail
    {
        // Out = A - B over Count floats.
        static void SubArray(float* Out, const float* A, const float* B, int Count)
        {
            using namespace SIMD;
            int i = 0;
            for (; i + 8 <= Count; i += 8)
            {
                (VFloat8::Load(A + i) - VFloat8::Load(B + i)).Store(Out + i);
            }
            for (; i < Count; ++i)
            {
                Out[i] = A[i] - B[i];
            }
        }

        // Out = A + B * S.
        static void AddScaledArray(float* Out, const float* A, const float* B, float S, int Count)
        {
            using namespace SIMD;
            const VFloat8 Vs = VFloat8::Broadcast(S);
            int i = 0;
            for (; i + 8 <= Count; i += 8)
            {
                MulAdd(VFloat8::Load(B + i), Vs, VFloat8::Load(A + i)).Store(Out + i);
            }
            for (; i < Count; ++i)
            {
                Out[i] = A[i] + B[i] * S;
            }
        }

        // Out = A * Mix(1, B, S), i.e. A * (1 + S*(B - 1)).
        static void MulLerpOneArray(float* Out, const float* A, const float* B, float S, int Count)
        {
            using namespace SIMD;
            const VFloat8 Vs  = VFloat8::Broadcast(S);
            const VFloat8 One = VFloat8::Broadcast(1.0f);
            int i = 0;
            for (; i + 8 <= Count; i += 8)
            {
                const VFloat8 Factor = MulAdd(VFloat8::Load(B + i) - One, Vs, One);
                (VFloat8::Load(A + i) * Factor).Store(Out + i);
            }
            for (; i < Count; ++i)
            {
                Out[i] = A[i] * (1.0f + S * (B[i] - 1.0f));
            }
        }

        // Out = A / B, lanes with B <= Eps pass A through (ratio 1).
        static void DivSafeArray(float* Out, const float* A, const float* B, int Count)
        {
            using namespace SIMD;
            const VFloat8 Eps = VFloat8::Broadcast(1e-8f);
            int i = 0;
            for (; i + 8 <= Count; i += 8)
            {
                const VFloat8 Va   = VFloat8::Load(A + i);
                const VFloat8 Vb   = VFloat8::Load(B + i);
                const VFloat8 Mask = CmpGt(Vb, Eps);
                Select(Mask, Va / Vb, Va).Store(Out + i);
            }
            for (; i < Count; ++i)
            {
                Out[i] = B[i] > 1e-8f ? A[i] / B[i] : A[i];
            }
        }

        // Out = A * Conjugate(B), 4 quats at a time.
        static void MulConjQuatArray(FQuat* Out, const FQuat* A, const FQuat* B, int Count)
        {
            using namespace SIMD;
            int i = 0;
            for (; i + 4 <= Count; i += 4)
            {
                StoreQuat4(Out + i, Mul(LoadQuat4(A + i), Conjugate(LoadQuat4(B + i))));
            }
            for (; i < Count; ++i)
            {
                Out[i] = A[i] * Math::Conjugate(B[i]);
            }
        }
    }

    void FPose::ResetToBindPose(const FSkeletonResource* Skeleton)
    {
        const int32 NumBones = Skeleton ? Skeleton->GetNumBones() : 0;

        if (Skeleton && Skeleton->HasBindPoseCache())
        {
            Translations = Skeleton->BindLocalTranslations;
            Rotations    = Skeleton->BindLocalRotations;
            Scales       = Skeleton->BindLocalScales;
            return;
        }

        SetNumBones(NumBones);
        for (int32 i = 0; i < NumBones; ++i)
        {
            AnimPose::DecomposeTRS(Skeleton->GetBone(i).LocalTransform, Translations[i], Rotations[i], Scales[i]);
        }
    }

    namespace Detail
    {
        // Skeleton-LOD tail passthrough: [Active, NumBones) copied from Src. No-op when Out aliases
        // Src (the executor's buffer-steal path), which is the common case.
        static void CopyPoseTail(FPose& Out, const FPose& Src, int32 Active, int32 NumBones)
        {
            if (Active >= NumBones || &Out == &Src)
            {
                return;
            }
            const SIZE_T Tail = (SIZE_T)(NumBones - Active);
            Memory::Memcpy(Out.Translations.data() + Active, Src.Translations.data() + Active, Tail * sizeof(FVector3));
            Memory::Memcpy(Out.Rotations.data() + Active,    Src.Rotations.data() + Active,    Tail * sizeof(FQuat));
            Memory::Memcpy(Out.Scales.data() + Active,       Src.Scales.data() + Active,       Tail * sizeof(FVector3));
        }

        static FORCEINLINE int32 ResolveActiveBones(int32 NumActiveBones, int32 NumBones)
        {
            return (NumActiveBones >= 0 && NumActiveBones < NumBones) ? NumActiveBones : NumBones;
        }
    }

    void AnimPose::Blend(const FPose& A, const FPose& B, float Alpha, FPose& Out, int32 NumActiveBones)
    {
        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);

        const int32 NumBones = A.GetNumBones();
        Out.SetNumBones(NumBones);

        if (Alpha <= 0.0f)
        {
            if (&Out != &A)
            {
                Out = A;
            }
            return;
        }
        if (Alpha >= 1.0f)
        {
            if (&Out != &B)
            {
                Out = B;
            }
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        // Translation + scale lerp as flat float streams (8-wide); rotations slerp 4 quats at a time.
        const int32 NumComponents = Active * 3;
        SIMD::LerpArray(reinterpret_cast<float*>(Out.Translations.data()),
                        reinterpret_cast<const float*>(A.Translations.data()),
                        reinterpret_cast<const float*>(B.Translations.data()), NumComponents, Alpha);
        SIMD::LerpArray(reinterpret_cast<float*>(Out.Scales.data()),
                        reinterpret_cast<const float*>(A.Scales.data()),
                        reinterpret_cast<const float*>(B.Scales.data()), NumComponents, Alpha);

        SIMD::BlendQuatArray(Out.Rotations.data(), A.Rotations.data(), B.Rotations.data(), Active, Alpha);

        Detail::CopyPoseTail(Out, A, Active, NumBones);
    }

    void AnimPose::BlendMasked(const FPose& A, const FPose& B, float Alpha, const TVector<float>& BoneWeights, FPose& Out, int32 NumActiveBones)
    {
        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);

        const int32 NumBones = A.GetNumBones();
        const int32 NumWeights = (int32)BoneWeights.size();
        Out.SetNumBones(NumBones);

        if (Alpha <= 0.0f)
        {
            if (&Out != &A)
            {
                Out = A;
            }
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        // Expand per-bone alphas once; the SIMD kernels consume them per quat / per component.
        thread_local TVector<float> BoneAlphas;
        thread_local TVector<float> ComponentAlphas;
        if ((int32)BoneAlphas.size() < Active)
        {
            BoneAlphas.resize(Active);
            ComponentAlphas.resize(Active * 3);
        }

        for (int32 i = 0; i < Active; ++i)
        {
            const float Weight = i < NumWeights ? BoneWeights[i] : 1.0f;
            const float BoneAlpha = Math::Clamp(Alpha * Weight, 0.0f, 1.0f);
            BoneAlphas[i] = BoneAlpha;
            ComponentAlphas[i * 3 + 0] = BoneAlpha;
            ComponentAlphas[i * 3 + 1] = BoneAlpha;
            ComponentAlphas[i * 3 + 2] = BoneAlpha;
        }

        const int32 NumComponents = Active * 3;
        SIMD::LerpArrayVarAlpha(reinterpret_cast<float*>(Out.Translations.data()),
                                reinterpret_cast<const float*>(A.Translations.data()),
                                reinterpret_cast<const float*>(B.Translations.data()),
                                ComponentAlphas.data(), NumComponents);
        SIMD::LerpArrayVarAlpha(reinterpret_cast<float*>(Out.Scales.data()),
                                reinterpret_cast<const float*>(A.Scales.data()),
                                reinterpret_cast<const float*>(B.Scales.data()),
                                ComponentAlphas.data(), NumComponents);

        SIMD::BlendQuatArrayVarAlpha(Out.Rotations.data(), A.Rotations.data(), B.Rotations.data(), BoneAlphas.data(), Active);

        Detail::CopyPoseTail(Out, A, Active, NumBones);
    }

    namespace Detail
    {
        // Bones past the LOD cut carry the identity delta so ApplyAdditive's tail stays the base pose.
        static void FillIdentityDeltaTail(FPose& OutDelta, int32 Active, int32 NumBones)
        {
            for (int32 i = Active; i < NumBones; ++i)
            {
                OutDelta.Translations[i] = FVector3(0.0f);
                OutDelta.Rotations[i]    = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
                OutDelta.Scales[i]       = FVector3(1.0f);
            }
        }
    }

    void AnimPose::MakeAdditive(const FPose& Src, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Skeleton ? Skeleton->GetNumBones() : 0;
        OutDelta.SetNumBones(NumBones);

        if (NumBones == 0 || Src.GetNumBones() != NumBones)
        {
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        if (Skeleton->HasBindPoseCache())
        {
            const int32 NumComponents = Active * 3;

            // Delta := Src "relative to" Bind: T subtracts, R = Src * conj(Bind) (bind is unit),
            // S is the component-wise ratio with degenerate bind scales passing through.
            Detail::SubArray(reinterpret_cast<float*>(OutDelta.Translations.data()),
                             reinterpret_cast<const float*>(Src.Translations.data()),
                             reinterpret_cast<const float*>(Skeleton->BindLocalTranslations.data()), NumComponents);
            Detail::MulConjQuatArray(OutDelta.Rotations.data(), Src.Rotations.data(), Skeleton->BindLocalRotations.data(), Active);
            Detail::DivSafeArray(reinterpret_cast<float*>(OutDelta.Scales.data()),
                                 reinterpret_cast<const float*>(Src.Scales.data()),
                                 reinterpret_cast<const float*>(Skeleton->BindLocalScales.data()), NumComponents);
            Detail::FillIdentityDeltaTail(OutDelta, Active, NumBones);
            return;
        }

        for (int32 i = 0; i < Active; ++i)
        {
            FVector3 BindT, BindS;
            FQuat BindR;
            AnimPose::GetBindLocalTRS(Skeleton, i, BindT, BindR, BindS);

            OutDelta.Translations[i] = Src.Translations[i] - BindT;
            OutDelta.Rotations[i]    = Src.Rotations[i] * Math::Inverse(BindR);

            const FVector3 BindInv(
                BindS.x > 1e-8f ? 1.0f / BindS.x : 1.0f,
                BindS.y > 1e-8f ? 1.0f / BindS.y : 1.0f,
                BindS.z > 1e-8f ? 1.0f / BindS.z : 1.0f);
            OutDelta.Scales[i] = Src.Scales[i] * BindInv;
        }
        Detail::FillIdentityDeltaTail(OutDelta, Active, NumBones);
    }

    void AnimPose::ApplyAdditive(const FPose& Base, const FPose& Delta, float Alpha, FPose& Out, int32 NumActiveBones)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Base.GetNumBones();
        Out.SetNumBones(NumBones);

        if (NumBones == 0 || Delta.GetNumBones() != NumBones || Alpha <= 0.0f)
        {
            if (&Out != &Base)
            {
                Out = Base;
            }
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        // The SIMD slerp's sin approximation is only valid for alpha in [0,1]; overdriven
        // additives (alpha > 1) take the exact scalar path.
        if (Alpha <= 1.0f)
        {
            using namespace SIMD;

            const int32 NumComponents = Active * 3;
            Detail::AddScaledArray(reinterpret_cast<float*>(Out.Translations.data()),
                                   reinterpret_cast<const float*>(Base.Translations.data()),
                                   reinterpret_cast<const float*>(Delta.Translations.data()), Alpha, NumComponents);
            Detail::MulLerpOneArray(reinterpret_cast<float*>(Out.Scales.data()),
                                    reinterpret_cast<const float*>(Base.Scales.data()),
                                    reinterpret_cast<const float*>(Delta.Scales.data()), Alpha, NumComponents);

            // Rotation: slerp identity -> Delta by alpha, then layer onto Base.
            const VFloat4 VAlpha = VFloat4::Broadcast(Alpha);
            const FQuat* BaseR  = Base.Rotations.data();
            const FQuat* DeltaR = Delta.Rotations.data();
            FQuat* OutR         = Out.Rotations.data();

            int32 i = 0;
            for (; i + 4 <= Active; i += 4)
            {
                const VQuat4 Scaled = SlerpShortest(QuatIdentity4(), LoadQuat4(DeltaR + i), VAlpha);
                StoreQuat4(OutR + i, Mul(Scaled, LoadQuat4(BaseR + i)));
            }
            for (; i < Active; ++i)
            {
                FQuat ScaledDelta = DeltaR[i];
                if (ScaledDelta.w < 0.0f)
                {
                    ScaledDelta = -ScaledDelta;
                }
                ScaledDelta = Math::Slerp(FQuat::Identity(), ScaledDelta, Alpha);
                OutR[i] = ScaledDelta * BaseR[i];
            }
            Detail::CopyPoseTail(Out, Base, Active, NumBones);
            return;
        }

        const FQuat Identity = FQuat::Identity();
        for (int32 i = 0; i < Active; ++i)
        {
            Out.Translations[i] = Base.Translations[i] + Alpha * Delta.Translations[i];

            FQuat ScaledDelta = Delta.Rotations[i];
            if (Math::Dot(Identity, ScaledDelta) < 0.0f)
            {
                ScaledDelta = -ScaledDelta;
            }
            ScaledDelta = Math::Slerp(Identity, ScaledDelta, Alpha);
            Out.Rotations[i] = ScaledDelta * Base.Rotations[i];

            const FVector3 ScaledScale = Math::Mix(FVector3(1.0f), Delta.Scales[i], Alpha);
            Out.Scales[i] = Base.Scales[i] * ScaledScale;
        }
        Detail::CopyPoseTail(Out, Base, Active, NumBones);
    }

    namespace Detail
    {
        // Uses Pose's current TRS so chained BoneTransform nodes compose correctly in component space.
        static FMatrix4 ComputeParentGlobal(const FPose& Pose, const FSkeletonResource* Skeleton, int32 BoneIndex)
        {
            const int32 ParentIndex = Skeleton->GetBone(BoneIndex).ParentIndex;
            if (ParentIndex < 0)
            {
                return FMatrix4(1.0f);
            }

            int32 Chain[64];
            int32 ChainLen = 0;
            int32 Cursor = ParentIndex;
            while (Cursor >= 0 && ChainLen < (int32)(sizeof(Chain) / sizeof(Chain[0])))
            {
                Chain[ChainLen++] = Cursor;
                Cursor = Skeleton->GetBone(Cursor).ParentIndex;
            }

            FMatrix4 Global(1.0f);
            for (int32 i = ChainLen - 1; i >= 0; --i)
            {
                const int32 b = Chain[i];
                Global = Global * AnimPose::ComposeTRS(Pose.Translations[b], Pose.Rotations[b], Pose.Scales[b]);
            }
            return Global;
        }
    }

    void AnimPose::ApplyBoneTransform(FPose& Pose,
                                      const FSkeletonResource* Skeleton,
                                      int32 BoneIndex,
                                      EBoneSpace Space,
                                      EBoneApplyMode Mode,
                                      const FVector3& InT,
                                      const FQuat& InR,
                                      const FVector3& InS,
                                      float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        if (Skeleton == nullptr || BoneIndex < 0 || BoneIndex >= Skeleton->GetNumBones())
        {
            return;
        }
        if (Pose.GetNumBones() != Skeleton->GetNumBones())
        {
            return;
        }

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        if (Alpha <= 0.0f)
        {
            return;
        }

        FQuat Rotation = InR;
        const float QLenSq = Math::Dot(Rotation, Rotation);
        Rotation = (QLenSq > 1e-8f) ? Rotation * (1.0f / Math::Sqrt(QLenSq)) : FQuat(1.0f, 0.0f, 0.0f, 0.0f);

        FVector3& T = Pose.Translations[BoneIndex];
        FQuat& R = Pose.Rotations[BoneIndex];
        FVector3& S = Pose.Scales[BoneIndex];

        const FQuat Identity(1.0f, 0.0f, 0.0f, 0.0f);

        if (Space == EBoneSpace::LocalBone)
        {
            if (Mode == EBoneApplyMode::Replace)
            {
                T = Math::Mix(T, InT, Alpha);
                S = Math::Mix(S, InS, Alpha);

                FQuat Target = Rotation;
                if (Math::Dot(R, Target) < 0.0f)
                {
                    Target = -Target;
                }
                R = Math::Normalize(Math::Slerp(R, Target, Alpha));
                return;
            }

            T += InT * Alpha;
            S *= Math::Mix(FVector3(1.0f), InS, Alpha);

            FQuat Scaled = Rotation;
            if (Math::Dot(Identity, Scaled) < 0.0f)
            {
                Scaled = -Scaled;
            }
            Scaled = Math::Slerp(Identity, Scaled, Alpha);
            R = Math::Normalize(Scaled * R);
            return;
        }

        const FMatrix4 ParentGlobal    = Detail::ComputeParentGlobal(Pose, Skeleton, BoneIndex);
        const FMatrix4 InvParentGlobal = Math::Inverse(ParentGlobal);

        const FMatrix4 BoneLocal  = ComposeTRS(T, R, S);
        const FMatrix4 BoneGlobal = ParentGlobal * BoneLocal;

        FMatrix4 NewGlobal;
        if (Mode == EBoneApplyMode::Replace)
        {
            const FMatrix4 Target = ComposeTRS(InT, Rotation, InS);
            FVector3 GT, GS; FQuat GR;
            DecomposeTRS(BoneGlobal, GT, GR, GS);
            FVector3 TgtT, TgtS; FQuat TgtR;
            DecomposeTRS(Target, TgtT, TgtR, TgtS);

            FQuat BlendedR = TgtR;
            if (Math::Dot(GR, BlendedR) < 0.0f)
            {
                BlendedR = -BlendedR;
            }
            BlendedR = Math::Normalize(Math::Slerp(GR, BlendedR, Alpha));
            NewGlobal = ComposeTRS(Math::Mix(GT, TgtT, Alpha), BlendedR, Math::Mix(GS, TgtS, Alpha));
        }
        else
        {
            FQuat Scaled = Rotation;
            if (Math::Dot(Identity, Scaled) < 0.0f)
            {
                Scaled = -Scaled;
            }
            Scaled = Math::Slerp(Identity, Scaled, Alpha);

            const FMatrix4 Offset = ComposeTRS(InT * Alpha, Scaled, Math::Mix(FVector3(1.0f), InS, Alpha));
            NewGlobal = Offset * BoneGlobal;
        }

        const FMatrix4 NewLocal = InvParentGlobal * NewGlobal;
        DecomposeTRS(NewLocal, T, R, S);
        R = Math::Normalize(R);
    }

    namespace Detail
    {
        // Shortest-arc rotation that takes unit vector A onto unit vector B.
        // Handles the antipodal case without producing NaN.
        static FQuat QuatFromTo(const FVector3& A, const FVector3& B)
        {
            const float d = Math::Dot(A, B);
            if (d > 0.99999f)
            {
                return FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (d < -0.99999f)
            {
                const FVector3 Ortho = Math::Abs(A.x) < 0.9f ? FVector3(1, 0, 0) : FVector3(0, 1, 0);
                const FVector3 Axis = Math::Normalize(Math::Cross(A, Ortho));
                return Math::AngleAxis(Math::Pi<float>(), Axis);
            }
            const FVector3 Axis = Math::Cross(A, B);
            const float S = Math::Sqrt((1.0f + d) * 2.0f);
            return Math::Normalize(FQuat(S * 0.5f, Axis.x / S, Axis.y / S, Axis.z / S));
        }

        static FMatrix4 ComputeBoneGlobalLocal(const FPose& Pose, const FSkeletonResource* Skeleton, int32 BoneIndex)
        {
            int32 Chain[64];
            int32 ChainLen = 0;
            int32 Cursor = BoneIndex;
            while (Cursor >= 0 && ChainLen < (int32)(sizeof(Chain) / sizeof(Chain[0])))
            {
                Chain[ChainLen++] = Cursor;
                Cursor = Skeleton->GetBone(Cursor).ParentIndex;
            }

            FMatrix4 Global(1.0f);
            for (int32 i = ChainLen - 1; i >= 0; --i)
            {
                const int32 b = Chain[i];
                Global = Global * AnimPose::ComposeTRS(Pose.Translations[b], Pose.Rotations[b], Pose.Scales[b]);
            }
            return Global;
        }
    }

    void AnimPose::TwoBoneIK(FPose& Pose, const FSkeletonResource* Skeleton,
                             int32 RootIdx, int32 MidIdx, int32 EndIdx,
                             const FVector3& Target, const FVector3& Pole, float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        if (Skeleton == nullptr) return;
        const int32 NumBones = Skeleton->GetNumBones();
        if (Pose.GetNumBones() != NumBones) return;

        if (RootIdx < 0 || MidIdx < 0 || EndIdx < 0) return;
        if (RootIdx >= NumBones || MidIdx >= NumBones || EndIdx >= NumBones) return;
        if (Skeleton->GetBone(MidIdx).ParentIndex != RootIdx) return;
        if (Skeleton->GetBone(EndIdx).ParentIndex != MidIdx) return;

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        if (Alpha <= 0.0f) return;

        const int32 RootParent = Skeleton->GetBone(RootIdx).ParentIndex;
        const FMatrix4 RootParentG = RootParent >= 0
            ? Detail::ComputeBoneGlobalLocal(Pose, Skeleton, RootParent)
            : FMatrix4(1.0f);

        const FMatrix4 RootLocal = ComposeTRS(Pose.Translations[RootIdx], Pose.Rotations[RootIdx], Pose.Scales[RootIdx]);
        const FMatrix4 MidLocal  = ComposeTRS(Pose.Translations[MidIdx],  Pose.Rotations[MidIdx],  Pose.Scales[MidIdx]);
        const FMatrix4 EndLocal  = ComposeTRS(Pose.Translations[EndIdx],  Pose.Rotations[EndIdx],  Pose.Scales[EndIdx]);

        const FMatrix4 RootG = RootParentG * RootLocal;
        const FMatrix4 MidG  = RootG * MidLocal;
        const FMatrix4 EndG  = MidG * EndLocal;

        const FVector3 R = FVector3(RootG[3]);
        const FVector3 M = FVector3(MidG[3]);
        const FVector3 E = FVector3(EndG[3]);

        const float L1 = Math::Length(M - R);
        const float L2 = Math::Length(E - M);
        if (L1 < 1e-5f || L2 < 1e-5f) return;

        const float MaxReach = L1 + L2;

        FVector3 ToTargetVec = Target - R;
        const float TargetDist = Math::Length(ToTargetVec);
        if (TargetDist < 1e-5f) return;

        const float ClampedDist = Math::Clamp(TargetDist, Math::Abs(L1 - L2) + 1e-4f, MaxReach - 1e-4f);
        const FVector3 ToTarget = ToTargetVec / TargetDist;

        FVector3 PoleDir = Pole - R;
        const float PoleLen = Math::Length(PoleDir);
        if (PoleLen > 1e-5f)
        {
            PoleDir = PoleDir / PoleLen;
        }
        else
        {
            const FVector3 CurrentBend = M - (R + ToTarget * Math::Dot(M - R, ToTarget));
            PoleDir = Math::Length(CurrentBend) > 1e-5f ? Math::Normalize(CurrentBend) : FVector3(0, 1, 0);
        }

        FVector3 BendAxis = Math::Cross(ToTarget, PoleDir);
        const float BendAxisLen = Math::Length(BendAxis);
        BendAxis = BendAxisLen > 1e-5f ? BendAxis / BendAxisLen : FVector3(0, 0, 1);

        const float CosUpper = Math::Clamp((L1 * L1 + ClampedDist * ClampedDist - L2 * L2) / (2.0f * L1 * ClampedDist), -1.0f, 1.0f);
        const float UpperAngle = Math::Acos(CosUpper);

        const FQuat RotUpper = Math::AngleAxis(-UpperAngle, BendAxis);
        const FVector3 NewDirRoot = Math::Normalize(RotUpper * ToTarget);
        const FVector3 NewM = R + NewDirRoot * L1;
        const FVector3 NewE = R + ToTarget * ClampedDist;
        const FVector3 NewDirMid = Math::Normalize(NewE - NewM);

        const FVector3 OldDirRoot = Math::Normalize(M - R);

        FVector3 RootGT, RootGS; FQuat RootGR;
        DecomposeTRS(RootG, RootGT, RootGR, RootGS);
        FVector3 RPGT, RPGS;     FQuat RPGR;
        DecomposeTRS(RootParentG, RPGT, RPGR, RPGS);

        const FQuat DeltaRoot     = Detail::QuatFromTo(OldDirRoot, NewDirRoot);
        const FQuat NewRootGlobal = DeltaRoot * RootGR;
        FQuat       NewRootLocal  = Math::Conjugate(RPGR) * NewRootGlobal;

        FQuat& RootLocalRotRef = Pose.Rotations[RootIdx];
        if (Math::Dot(RootLocalRotRef, NewRootLocal) < 0.0f) NewRootLocal = -NewRootLocal;
        RootLocalRotRef = Math::Normalize(Math::Slerp(RootLocalRotRef, NewRootLocal, Alpha));

        const FMatrix4 NewRootLocalMat = ComposeTRS(Pose.Translations[RootIdx], RootLocalRotRef, Pose.Scales[RootIdx]);
        const FMatrix4 NewRootG        = RootParentG * NewRootLocalMat;
        const FMatrix4 NewMidGCurrent  = NewRootG * MidLocal;
        const FMatrix4 NewEndGCurrent  = NewMidGCurrent * EndLocal;

        const FVector3 NewMpos = FVector3(NewMidGCurrent[3]);
        const FVector3 CurrEnd = FVector3(NewEndGCurrent[3]);
        const FVector3 CurrDirMid = Math::Normalize(CurrEnd - NewMpos);

        FVector3 MidGT, MidGS; FQuat MidGR;
        DecomposeTRS(NewMidGCurrent, MidGT, MidGR, MidGS);
        FVector3 NRGT, NRGS;   FQuat NRGR;
        DecomposeTRS(NewRootG, NRGT, NRGR, NRGS);

        const FQuat DeltaMid     = Detail::QuatFromTo(CurrDirMid, NewDirMid);
        const FQuat NewMidGlobal = DeltaMid * MidGR;
        FQuat       NewMidLocal  = Math::Conjugate(NRGR) * NewMidGlobal;

        FQuat& MidLocalRotRef = Pose.Rotations[MidIdx];
        if (Math::Dot(MidLocalRotRef, NewMidLocal) < 0.0f) NewMidLocal = -NewMidLocal;
        MidLocalRotRef = Math::Normalize(Math::Slerp(MidLocalRotRef, NewMidLocal, Alpha));
    }

    void AnimPose::ToSkinningMatrices(const FPose& Pose, const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutMatrices)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Skeleton ? Skeleton->GetNumBones() : 0;
        OutMatrices.resize(NumBones);

        if (NumBones == 0 || Pose.GetNumBones() != NumBones)
        {
            return;
        }

        // Pass 1: local TRS -> global, fused FK; Bones[] is parents-before-children so a single linear pass works.
        for (int32 i = 0; i < NumBones; ++i)
        {
            const FMatrix4 Local = ComposeTRS(Pose.Translations[i], Pose.Rotations[i], Pose.Scales[i]);
            const int32 Parent = Skeleton->GetBone(i).ParentIndex;
            OutMatrices[i] = Parent != INDEX_NONE ? OutMatrices[Parent] * Local : Local;
        }

        // Pass 2: fold in InvBind to produce the GPU skinning matrix.
        for (int32 i = 0; i < NumBones; ++i)
        {
            OutMatrices[i] = OutMatrices[i] * Skeleton->GetBone(i).InvBindMatrix;
        }
    }
}
