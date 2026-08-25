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
        BindGlobalMatrices.resize(NumBones);
        BoneParents.resize(NumBones);
        BoneInvBind.resize(NumBones);

        for (int32 i = 0; i < NumBones; ++i)
        {
            AnimPose::DecomposeTRS(Bones[i].LocalTransform, BindLocalTranslations[i], BindLocalRotations[i], BindLocalScales[i]);
            BindGlobalMatrices[i] = Math::Inverse(Bones[i].InvBindMatrix);
            BoneParents[i] = Bones[i].ParentIndex;
            BoneInvBind[i] = Bones[i].InvBindMatrix;
        }

        BindStreamStride = FPose::StrideFor(NumBones);
        BindLocalStreams.assign((SIZE_T)BindStreamStride * FPose::NumStreams, 0.0f);

        float* RESTRICT Streams = BindLocalStreams.data();
        for (int32 s = 0; s < FPose::NumStreams; ++s)
        {
            // The pad past the last bone holds an identity transform so no kernel ever sees a garbage lane.
            const float Identity = (s >= FPose::StreamSx && s <= FPose::StreamSz) || s == FPose::StreamRw ? 1.0f : 0.0f;
            float* RESTRICT Stream = Streams + (SIZE_T)s * BindStreamStride;
            for (int32 i = NumBones; i < BindStreamStride; ++i)
            {
                Stream[i] = Identity;
            }
        }

        for (int32 i = 0; i < NumBones; ++i)
        {
            const FVector3& T = BindLocalTranslations[i];
            const FQuat&    R = BindLocalRotations[i];
            const FVector3& S = BindLocalScales[i];

            Streams[(SIZE_T)FPose::StreamTx * BindStreamStride + i] = T.x;
            Streams[(SIZE_T)FPose::StreamTy * BindStreamStride + i] = T.y;
            Streams[(SIZE_T)FPose::StreamTz * BindStreamStride + i] = T.z;
            Streams[(SIZE_T)FPose::StreamSx * BindStreamStride + i] = S.x;
            Streams[(SIZE_T)FPose::StreamSy * BindStreamStride + i] = S.y;
            Streams[(SIZE_T)FPose::StreamSz * BindStreamStride + i] = S.z;
            Streams[(SIZE_T)FPose::StreamRx * BindStreamStride + i] = R.x;
            Streams[(SIZE_T)FPose::StreamRy * BindStreamStride + i] = R.y;
            Streams[(SIZE_T)FPose::StreamRz * BindStreamStride + i] = R.z;
            Streams[(SIZE_T)FPose::StreamRw * BindStreamStride + i] = R.w;
        }

        ++BindPoseGeneration;
    }

    FPose::FPose(const FPose& Other)
    {
        *this = Other;
    }

    FPose::FPose(FPose&& Other) noexcept
    {
        Swap(Other);
    }

    FPose& FPose::operator=(const FPose& Other)
    {
        if (this == &Other)
        {
            return *this;
        }

        AdditiveSpace = Other.AdditiveSpace;
        NumBones      = Other.NumBones;
        if (NumBones == 0)
        {
            return *this;
        }

        if (Other.Stride > Capacity)
        {
            void* Old = Data;
            Data = (float*)Memory::Malloc((SIZE_T)Other.Stride * NumStreams * sizeof(float), 32);
            Capacity = Other.Stride;
            if (Old != nullptr)
            {
                Memory::Free(Old);
            }
        }

        Stride = Other.Stride;
        Memory::Memcpy(Data, Other.Data, (SIZE_T)Stride * NumStreams * sizeof(float));
        return *this;
    }

    FPose& FPose::operator=(FPose&& Other) noexcept
    {
        if (this != &Other)
        {
            Swap(Other);
        }
        return *this;
    }

    FPose::~FPose()
    {
        if (Data != nullptr)
        {
            void* Old = Data;
            Data = nullptr;
            Memory::Free(Old);
        }
    }

    void FPose::Swap(FPose& Other)
    {
        std::swap(Data, Other.Data);
        std::swap(NumBones, Other.NumBones);
        std::swap(Stride, Other.Stride);
        std::swap(Capacity, Other.Capacity);
        std::swap(AdditiveSpace, Other.AdditiveSpace);
    }

    void FPose::FillIdentity(int32 First, int32 Last)
    {
        if (First >= Last || Data == nullptr)
        {
            return;
        }

        for (int32 s = 0; s < NumStreams; ++s)
        {
            const float Identity = (s >= StreamSx && s <= StreamSz) || s == StreamRw ? 1.0f : 0.0f;
            float* RESTRICT Ptr = Stream(s);
            for (int32 i = First; i < Last; ++i)
            {
                Ptr[i] = Identity;
            }
        }
    }

    void FPose::Relayout(int32 NewStride, int32 NewNumBones)
    {
        const int32 Keep  = Math::Min(NumBones, NewNumBones);
        const SIZE_T Bytes = (SIZE_T)Keep * sizeof(float);

        if (NewStride > Capacity)
        {
            float* NewData = (float*)Memory::Malloc((SIZE_T)NewStride * NumStreams * sizeof(float), 32);
            for (int32 s = 0; s < NumStreams && Keep > 0; ++s)
            {
                Memory::Memcpy(NewData + (SIZE_T)s * NewStride, Stream(s), Bytes);
            }

            void* Old = Data;
            Data     = NewData;
            Capacity = NewStride;
            if (Old != nullptr)
            {
                Memory::Free(Old);
            }
        }
        else if (Keep > 0 && NewStride != Stride)
        {
            // Streams slide the way the stride moved, so this order never overwrites an unread source.
            if (NewStride < Stride)
            {
                for (int32 s = 1; s < NumStreams; ++s)
                {
                    ::memmove(Data + (SIZE_T)s * NewStride, Data + (SIZE_T)s * Stride, Bytes);
                }
            }
            else
            {
                for (int32 s = NumStreams - 1; s >= 1; --s)
                {
                    ::memmove(Data + (SIZE_T)s * NewStride, Data + (SIZE_T)s * Stride, Bytes);
                }
            }
        }

        Stride   = NewStride;
        NumBones = NewNumBones;

        FillIdentity(Keep, Stride);
    }

    void FPose::SetNumBones(int32 InNumBones)
    {
        if (InNumBones <= 0)
        {
            NumBones = 0;
            return;
        }
        if (InNumBones == NumBones)
        {
            return;
        }

        const int32 NewStride = StrideFor(InNumBones);
        if (NewStride == Stride)
        {
            const int32 Keep = Math::Min(NumBones, InNumBones);
            NumBones = InNumBones;
            FillIdentity(Keep, Stride);
            return;
        }

        Relayout(NewStride, InNumBones);
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

        // Ten component streams with a stride, so a skeleton bind block and an FPose share one path.
        struct FPoseStreams
        {
            const float* Data = nullptr;
            int32 Stride = 0;

            FORCEINLINE const float* Stream(int32 Index) const { return Data + (SIZE_T)Index * Stride; }

            FORCEINLINE SIMD::FConstQuatStreams Rotations() const
            {
                return { Stream(FPose::StreamRx), Stream(FPose::StreamRy),
                         Stream(FPose::StreamRz), Stream(FPose::StreamRw) };
            }
        };

        static FORCEINLINE FPoseStreams ViewOf(const FPose& Pose)
        {
            return { Pose.Stream(0), Pose.GetStride() };
        }

        static FORCEINLINE FPoseStreams BindViewOf(const FSkeletonResource* Skeleton)
        {
            return { Skeleton->BindLocalStreams.data(), Skeleton->BindStreamStride };
        }
    }

    void FPose::ResetToBindPose(const FSkeletonResource* Skeleton)
    {
        const int32 SkeletonBones = Skeleton ? Skeleton->GetNumBones() : 0;
        AdditiveSpace = EPoseAdditiveSpace::None;
        SetNumBones(SkeletonBones);

        if (SkeletonBones == 0)
        {
            return;
        }

        if (Skeleton->BindStreamStride == Stride &&
            Skeleton->BindLocalStreams.size() == (SIZE_T)Stride * NumStreams)
        {
            Memory::Memcpy(Data, Skeleton->BindLocalStreams.data(), (SIZE_T)Stride * NumStreams * sizeof(float));
            return;
        }

        for (int32 i = 0; i < SkeletonBones; ++i)
        {
            FVector3 T, S;
            FQuat R;
            AnimPose::DecomposeTRS(Skeleton->GetBone(i).LocalTransform, T, R, S);
            SetBone(i, T, R, S);
        }
    }

    namespace Detail
    {
        // A no-op when Out aliases Src on the executor's buffer-steal path, which is the common case.
        static void CopyPoseTail(FPose& Out, const FPose& Src, int32 Active, int32 NumBones)
        {
            if (Active >= NumBones || &Out == &Src)
            {
                return;
            }
            const SIZE_T Tail = (SIZE_T)(NumBones - Active) * sizeof(float);
            for (int32 s = 0; s < FPose::NumStreams; ++s)
            {
                Memory::Memcpy(Out.Stream(s) + Active, Src.Stream(s) + Active, Tail);
            }
        }

        // Rounded up to the vector width so the kernels below never need a scalar remainder.
        static FORCEINLINE int32 ResolveActiveBones(int32 NumActiveBones, int32 NumBones)
        {
            if (NumActiveBones < 0 || NumActiveBones >= NumBones)
            {
                return NumBones;
            }
            return Math::Min(FPose::StrideFor(NumActiveBones), NumBones);
        }

        // Bones past the cut are restored afterwards, and the pad past the last bone holds identity.
        static FORCEINLINE int32 LaneCount(int32 Active)
        {
            return FPose::StrideFor(Active);
        }

        static FORCEINLINE FMatrix4 ComposeLocal(const FPose& Pose, int32 Bone)
        {
            return AnimPose::ComposeTRS(Pose.GetTranslation(Bone), Pose.GetRotation(Bone), Pose.GetScale(Bone));
        }

        // Slerps the bone toward Target along the shortest arc, writes it back, and hands it to the caller.
        static FORCEINLINE FQuat BlendBoneRotation(FPose& Pose, int32 Bone, FQuat Target, float Alpha)
        {
            const FQuat Current = Pose.GetRotation(Bone);
            if (Math::Dot(Current, Target) < 0.0f)
            {
                Target = -Target;
            }
            const FQuat Result = Math::Normalize(Math::Slerp(Current, Target, Alpha));
            Pose.SetRotation(Bone, Result);
            return Result;
        }
    }

    void AnimPose::Blend(const FPose& A, const FPose& B, float Alpha, FPose& Out, int32 NumActiveBones)
    {
        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);

        const int32 NumBones = A.GetNumBones();
        Out.SetNumBones(NumBones);

        if (Alpha <= 0.0f || B.GetNumBones() != NumBones)
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
        const int32 Lanes  = Detail::LaneCount(Active);

        for (int32 s = FPose::StreamTx; s <= FPose::StreamSz; ++s)
        {
            SIMD::LerpArray(Out.Stream(s), A.Stream(s), B.Stream(s), Lanes, Alpha);
        }
        SIMD::SlerpQuatStreams(Out.Rotations(), A.Rotations(), B.Rotations(), Lanes, Alpha);

        Detail::CopyPoseTail(Out, A, Active, NumBones);
        Out.AdditiveSpace = A.IsAdditive() ? A.AdditiveSpace : B.AdditiveSpace;
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

        if (B.GetNumBones() != NumBones)
        {
            if (&Out != &A)
            {
                Out = A;
            }
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);
        const int32 Lanes  = Detail::LaneCount(Active);

        // One alpha per bone feeds every stream directly, where the AoS layout needed an xyz splat.
        thread_local TVector<float> BoneAlphas;
        if ((int32)BoneAlphas.size() < Lanes)
        {
            BoneAlphas.resize(Lanes);
        }

        for (int32 i = 0; i < Active; ++i)
        {
            const float Weight = i < NumWeights ? BoneWeights[i] : 1.0f;
            BoneAlphas[i] = Math::Clamp(Alpha * Weight, 0.0f, 1.0f);
        }
        for (int32 i = Active; i < Lanes; ++i)
        {
            BoneAlphas[i] = 0.0f;
        }

        for (int32 s = FPose::StreamTx; s <= FPose::StreamSz; ++s)
        {
            SIMD::LerpArrayVarAlpha(Out.Stream(s), A.Stream(s), B.Stream(s), BoneAlphas.data(), Lanes);
        }
        SIMD::SlerpQuatStreamsVarAlpha(Out.Rotations(), A.Rotations(), B.Rotations(), BoneAlphas.data(), Lanes);

        Detail::CopyPoseTail(Out, A, Active, NumBones);
        Out.AdditiveSpace = A.IsAdditive() ? A.AdditiveSpace : B.AdditiveSpace;
    }

    namespace Detail
    {
        // Bones past the LOD cut carry the identity delta so ApplyAdditive's tail stays the base pose.
        static void FillIdentityDeltaTail(FPose& OutDelta, int32 Active, int32 NumBones)
        {
            for (int32 i = Active; i < NumBones; ++i)
            {
                OutDelta.SetBone(i, FVector3(0.0f), FQuat(1.0f, 0.0f, 0.0f, 0.0f), FVector3(1.0f));
            }
        }
    }

    namespace Detail
    {
        // T subtracts, R multiplies by the conjugate, S is the ratio with a degenerate base passing through.
        static void MakeLocalDelta(FPose& OutDelta, const FPose& Src, const FPoseStreams& Base,
                                   int32 Active, int32 NumBones)
        {
            const int32 Lanes = LaneCount(Active);

            for (int32 s = FPose::StreamTx; s <= FPose::StreamTz; ++s)
            {
                SubArray(OutDelta.Stream(s), Src.Stream(s), Base.Stream(s), Lanes);
            }
            for (int32 s = FPose::StreamSx; s <= FPose::StreamSz; ++s)
            {
                DivSafeArray(OutDelta.Stream(s), Src.Stream(s), Base.Stream(s), Lanes);
            }
            SIMD::MulConjQuatStreams(OutDelta.Rotations(), Src.Rotations(), Base.Rotations(), Lanes);

            FillIdentityDeltaTail(OutDelta, Active, NumBones);
            OutDelta.AdditiveSpace = EPoseAdditiveSpace::LocalSpace;
        }
    }

    void AnimPose::MakeAdditive(const FPose& Src, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Skeleton ? Skeleton->GetNumBones() : 0;
        OutDelta.SetNumBones(NumBones);
        OutDelta.AdditiveSpace = EPoseAdditiveSpace::LocalSpace;

        if (NumBones == 0 || Src.GetNumBones() != NumBones)
        {
            Detail::FillIdentityDeltaTail(OutDelta, 0, NumBones);
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        if (Skeleton->BindStreamStride == Src.GetStride() &&
            Skeleton->BindLocalStreams.size() == (SIZE_T)Skeleton->BindStreamStride * FPose::NumStreams)
        {
            Detail::MakeLocalDelta(OutDelta, Src, Detail::BindViewOf(Skeleton), Active, NumBones);
            return;
        }

        for (int32 i = 0; i < Active; ++i)
        {
            FVector3 BindT, BindS;
            FQuat BindR;
            AnimPose::GetBindLocalTRS(Skeleton, i, BindT, BindR, BindS);

            const FVector3 SrcS = Src.GetScale(i);
            const FVector3 BindInv(
                BindS.x > 1e-8f ? 1.0f / BindS.x : 1.0f,
                BindS.y > 1e-8f ? 1.0f / BindS.y : 1.0f,
                BindS.z > 1e-8f ? 1.0f / BindS.z : 1.0f);

            OutDelta.SetTranslation(i, Src.GetTranslation(i) - BindT);
            OutDelta.SetRotation(i, Src.GetRotation(i) * Math::Inverse(BindR));
            OutDelta.SetScale(i, SrcS * BindInv);
        }
        Detail::FillIdentityDeltaTail(OutDelta, Active, NumBones);
    }

    void AnimPose::MakeAdditiveFromBase(const FPose& Src, const FPose& Base, FPose& OutDelta, int32 NumActiveBones)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Src.GetNumBones();
        OutDelta.SetNumBones(NumBones);
        OutDelta.AdditiveSpace = EPoseAdditiveSpace::LocalSpace;

        if (NumBones == 0 || Base.GetNumBones() != NumBones)
        {
            Detail::FillIdentityDeltaTail(OutDelta, 0, NumBones);
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);
        Detail::MakeLocalDelta(OutDelta, Src, Detail::ViewOf(Base), Active, NumBones);
    }

    namespace Detail
    {
        // Bones[] is parents-before-children, so one linear pass resolves the whole chain.
        static void ComputeComponentRotations(const FPose& Local, const FSkeletonResource* Skeleton, FQuat* Out, int32 Active)
        {
            for (int32 i = 0; i < Active; ++i)
            {
                const int32 Parent = Skeleton->GetBone(i).ParentIndex;
                const FQuat Rotation = Local.GetRotation(i);
                Out[i] = Parent >= 0 ? Out[Parent] * Rotation : Rotation;
            }
        }

        // Slerps identity -> Delta by Alpha along the shortest arc.
        static FORCEINLINE FQuat ScaleDelta(const FQuat& Delta, float Alpha)
        {
            FQuat Scaled = Delta;
            if (Scaled.w < 0.0f)
            {
                Scaled = -Scaled;
            }
            return Math::Slerp(FQuat::Identity(), Scaled, Alpha);
        }
    }

    void AnimPose::MakeAdditiveMeshSpace(const FPose& Src, const FPose& Base, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Skeleton ? Skeleton->GetNumBones() : 0;
        OutDelta.SetNumBones(NumBones);
        OutDelta.AdditiveSpace = EPoseAdditiveSpace::MeshSpace;

        if (NumBones == 0 || Src.GetNumBones() != NumBones || Base.GetNumBones() != NumBones)
        {
            Detail::FillIdentityDeltaTail(OutDelta, 0, NumBones);
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        thread_local TVector<FQuat> SrcComponent;
        thread_local TVector<FQuat> BaseComponent;
        if ((int32)SrcComponent.size() < Active)
        {
            SrcComponent.resize(Active);
            BaseComponent.resize(Active);
        }

        Detail::ComputeComponentRotations(Src, Skeleton, SrcComponent.data(), Active);
        Detail::ComputeComponentRotations(Base, Skeleton, BaseComponent.data(), Active);

        for (int32 i = 0; i < Active; ++i)
        {
            const FVector3 SrcS  = Src.GetScale(i);
            const FVector3 BaseS = Base.GetScale(i);

            OutDelta.SetRotation(i, SrcComponent[i] * Math::Conjugate(BaseComponent[i]));
            OutDelta.SetTranslation(i, Src.GetTranslation(i) - Base.GetTranslation(i));
            OutDelta.SetScale(i, FVector3(
                BaseS.x > 1e-8f ? SrcS.x / BaseS.x : SrcS.x,
                BaseS.y > 1e-8f ? SrcS.y / BaseS.y : SrcS.y,
                BaseS.z > 1e-8f ? SrcS.z / BaseS.z : SrcS.z));
        }

        Detail::FillIdentityDeltaTail(OutDelta, Active, NumBones);
    }

    void AnimPose::MakeAdditiveMeshSpace(const FPose& Src, const FSkeletonResource* Skeleton, FPose& OutDelta, int32 NumActiveBones)
    {
        thread_local FPose BindPose;
        BindPose.ResetToBindPose(Skeleton);
        MakeAdditiveMeshSpace(Src, BindPose, Skeleton, OutDelta, NumActiveBones);
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
            Out.AdditiveSpace = Base.AdditiveSpace;
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        // The SIMD slerp is only valid up to alpha 1, so an overdriven additive takes the scalar path.
        if (Alpha <= 1.0f)
        {
            using namespace SIMD;

            const int32 Lanes = Detail::LaneCount(Active);

            for (int32 s = FPose::StreamTx; s <= FPose::StreamTz; ++s)
            {
                Detail::AddScaledArray(Out.Stream(s), Base.Stream(s), Delta.Stream(s), Alpha, Lanes);
            }
            for (int32 s = FPose::StreamSx; s <= FPose::StreamSz; ++s)
            {
                Detail::MulLerpOneArray(Out.Stream(s), Base.Stream(s), Delta.Stream(s), Alpha, Lanes);
            }

            // Slerps identity toward Delta by alpha, then layers the result onto Base.
            const VFloat8 VAlpha = VFloat8::Broadcast(Alpha);
            const FQuatStreams      OutR   = Out.Rotations();
            const FConstQuatStreams BaseR  = Base.Rotations();
            const FConstQuatStreams DeltaR = Delta.Rotations();

            for (int32 i = 0; i < Lanes; i += 8)
            {
                const VQuat8 Scaled = SlerpShortest(QuatIdentity8(), LoadAt(DeltaR, i), VAlpha);
                StoreAt(OutR, i, Mul(Scaled, LoadAt(BaseR, i)));
            }
            Detail::CopyPoseTail(Out, Base, Active, NumBones);
            Out.AdditiveSpace = Base.AdditiveSpace;
            return;
        }

        const FQuat Identity = FQuat::Identity();
        for (int32 i = 0; i < Active; ++i)
        {
            FQuat ScaledDelta = Delta.GetRotation(i);
            if (Math::Dot(Identity, ScaledDelta) < 0.0f)
            {
                ScaledDelta = -ScaledDelta;
            }
            ScaledDelta = Math::Slerp(Identity, ScaledDelta, Alpha);

            const FVector3 ScaledScale = Math::Mix(FVector3(1.0f), Delta.GetScale(i), Alpha);

            Out.SetTranslation(i, Base.GetTranslation(i) + Alpha * Delta.GetTranslation(i));
            Out.SetRotation(i, ScaledDelta * Base.GetRotation(i));
            Out.SetScale(i, Base.GetScale(i) * ScaledScale);
        }
        Detail::CopyPoseTail(Out, Base, Active, NumBones);
        Out.AdditiveSpace = Base.AdditiveSpace;
    }

    void AnimPose::ApplyAdditiveMeshSpace(const FPose& Base, const FPose& Delta, float Alpha, const FSkeletonResource* Skeleton, FPose& Out, int32 NumActiveBones)
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = Base.GetNumBones();
        Out.SetNumBones(NumBones);

        if (Skeleton == nullptr || NumBones == 0 || NumBones != Skeleton->GetNumBones()
            || Delta.GetNumBones() != NumBones || Alpha <= 0.0f)
        {
            if (&Out != &Base)
            {
                Out = Base;
            }
            Out.AdditiveSpace = Base.AdditiveSpace;
            return;
        }

        const int32 Active = Detail::ResolveActiveBones(NumActiveBones, NumBones);

        thread_local TVector<FQuat> BaseComponent;
        thread_local TVector<FQuat> NewComponent;
        if ((int32)BaseComponent.size() < Active)
        {
            BaseComponent.resize(Active);
            NewComponent.resize(Active);
        }

        // Base rotations are consumed into BaseComponent before Out is written, so Out may alias Base.
        for (int32 i = 0; i < Active; ++i)
        {
            const int32 Parent = Skeleton->GetBone(i).ParentIndex;
            const FQuat BaseRotation = Base.GetRotation(i);

            BaseComponent[i] = Parent >= 0 ? BaseComponent[Parent] * BaseRotation : BaseRotation;
            NewComponent[i]  = Detail::ScaleDelta(Delta.GetRotation(i), Alpha) * BaseComponent[i];

            const FQuat Local = Parent >= 0 ? Math::Conjugate(NewComponent[Parent]) * NewComponent[i] : NewComponent[i];

            Out.SetTranslation(i, Base.GetTranslation(i) + Alpha * Delta.GetTranslation(i));
            Out.SetRotation(i, Math::Normalize(Local));
            Out.SetScale(i, Base.GetScale(i) * Math::Mix(FVector3(1.0f), Delta.GetScale(i), Alpha));
        }

        Detail::CopyPoseTail(Out, Base, Active, NumBones);
        Out.AdditiveSpace = Base.AdditiveSpace;
    }

    void AnimPose::ApplyAdditivePose(const FPose& Base, const FPose& Delta, float Alpha, const FSkeletonResource* Skeleton, FPose& Out, int32 NumActiveBones)
    {
        if (Delta.AdditiveSpace == EPoseAdditiveSpace::MeshSpace && Skeleton != nullptr)
        {
            ApplyAdditiveMeshSpace(Base, Delta, Alpha, Skeleton, Out, NumActiveBones);
            return;
        }
        ApplyAdditive(Base, Delta, Alpha, Out, NumActiveBones);
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
                Global = Global * Detail::ComposeLocal(Pose, b);
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

        FVector3 T, S;
        FQuat R;
        Pose.GetBone(BoneIndex, T, R, S);

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
                Pose.SetBone(BoneIndex, T, R, S);
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
            Pose.SetBone(BoneIndex, T, R, S);
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
        Pose.SetBone(BoneIndex, T, Math::Normalize(R), S);
    }

    namespace Detail
    {
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
                Global = Global * Detail::ComposeLocal(Pose, b);
            }
            return Global;
        }
    }

    void AnimPose::FABRIK(FPose& Pose, const FSkeletonResource* Skeleton, int32 RootIdx, int32 TipIdx,
                          const FVector3& Target, int32 Iterations, float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        if (Skeleton == nullptr) return;
        const int32 NumBones = Skeleton->GetNumBones();
        if (Pose.GetNumBones() != NumBones) return;
        if (RootIdx < 0 || TipIdx < 0 || RootIdx >= NumBones || TipIdx >= NumBones || RootIdx == TipIdx) return;

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        if (Alpha <= 0.0f) return;

        // Tip first, since only the parent chain is walkable; reversed below to run root to tip.
        constexpr int32 MaxChain = 32;
        int32 Chain[MaxChain];
        int32 ChainLen = 0;
        for (int32 Cursor = TipIdx; Cursor >= 0 && ChainLen < MaxChain; Cursor = Skeleton->GetBone(Cursor).ParentIndex)
        {
            Chain[ChainLen++] = Cursor;
            if (Cursor == RootIdx)
            {
                break;
            }
        }

        // The walk ended somewhere other than the root, so the two bones are not on one chain.
        if (ChainLen < 2 || Chain[ChainLen - 1] != RootIdx)
        {
            return;
        }

        for (int32 i = 0; i < ChainLen / 2; ++i)
        {
            const int32 Swap = Chain[i];
            Chain[i] = Chain[ChainLen - 1 - i];
            Chain[ChainLen - 1 - i] = Swap;
        }

        const int32 RootParent = Skeleton->GetBone(RootIdx).ParentIndex;
        const FMatrix4 RootParentG = RootParent >= 0
            ? Detail::ComputeBoneGlobalLocal(Pose, Skeleton, RootParent)
            : FMatrix4(1.0f);

        FMatrix4 Globals[MaxChain];
        FVector3 Points[MaxChain];
        FMatrix4 Running = RootParentG;
        for (int32 i = 0; i < ChainLen; ++i)
        {
            const int32 Bone = Chain[i];
            Running = Running * Detail::ComposeLocal(Pose, Bone);
            Globals[i] = Running;
            Points[i] = FVector3(Running[3]);
        }

        float Lengths[MaxChain];
        float TotalLength = 0.0f;
        for (int32 i = 0; i < ChainLen - 1; ++i)
        {
            Lengths[i] = Math::Length(Points[i + 1] - Points[i]);
            TotalLength += Lengths[i];
        }
        if (TotalLength < 1e-5f) return;

        const FVector3 Origin = Points[0];
        FVector3 Solved[MaxChain];
        for (int32 i = 0; i < ChainLen; ++i)
        {
            Solved[i] = Points[i];
        }

        const FVector3 ToTarget = Target - Origin;
        const float TargetDist = Math::Length(ToTarget);

        if (TargetDist > TotalLength)
        {
            // Out of reach, so the chain straightens at the target instead of iterating toward nothing.
            const FVector3 Dir = ToTarget / TargetDist;
            for (int32 i = 1; i < ChainLen; ++i)
            {
                Solved[i] = Solved[i - 1] + Dir * Lengths[i - 1];
            }
        }
        else
        {
            const int32 NumIterations = Math::Clamp(Iterations, 1, 32);
            for (int32 Iter = 0; Iter < NumIterations; ++Iter)
            {
                if (Math::Length(Solved[ChainLen - 1] - Target) < 0.01f)
                {
                    break;
                }

                // Backward pass pins the tip to the target, forward pass pins the root back home.
                Solved[ChainLen - 1] = Target;
                for (int32 i = ChainLen - 2; i >= 0; --i)
                {
                    const FVector3 Dir = Math::Normalize(Solved[i] - Solved[i + 1]);
                    Solved[i] = Solved[i + 1] + Dir * Lengths[i];
                }

                Solved[0] = Origin;
                for (int32 i = 1; i < ChainLen; ++i)
                {
                    const FVector3 Dir = Math::Normalize(Solved[i] - Solved[i - 1]);
                    Solved[i] = Solved[i - 1] + Dir * Lengths[i - 1];
                }
            }
        }

        // Solved positions become rotations, parent first, so each bone sees its parent's new frame.
        FMatrix4 NewParentG = RootParentG;
        for (int32 i = 0; i < ChainLen - 1; ++i)
        {
            const int32 Bone = Chain[i];

            const FMatrix4 CurrentG = NewParentG * Detail::ComposeLocal(Pose, Bone);
            const FVector3 CurrentPos = FVector3(CurrentG[3]);

            const FMatrix4 ChildG = CurrentG * Detail::ComposeLocal(Pose, Chain[i + 1]);
            const FVector3 CurrentChild = FVector3(ChildG[3]);

            const FVector3 CurrentDir = CurrentChild - CurrentPos;
            const FVector3 DesiredDir = Solved[i + 1] - CurrentPos;
            if (Math::Length(CurrentDir) < 1e-5f || Math::Length(DesiredDir) < 1e-5f)
            {
                NewParentG = CurrentG;
                continue;
            }

            FVector3 GT, GS; FQuat GR;
            DecomposeTRS(CurrentG, GT, GR, GS);
            FVector3 PT, PS; FQuat PR;
            DecomposeTRS(NewParentG, PT, PR, PS);

            const FQuat Delta = Detail::QuatFromTo(Math::Normalize(CurrentDir), Math::Normalize(DesiredDir));
            FQuat NewLocal = Math::Conjugate(PR) * (Delta * GR);

            Detail::BlendBoneRotation(Pose, Bone, NewLocal, Alpha);

            NewParentG = NewParentG * Detail::ComposeLocal(Pose, Bone);
        }
    }

    void AnimPose::LookAt(FPose& Pose, const FSkeletonResource* Skeleton, int32 BoneIdx,
                          const FVector3& Target, const FVector3& LocalForward, float MaxAngleRadians, float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        if (Skeleton == nullptr) return;
        const int32 NumBones = Skeleton->GetNumBones();
        if (Pose.GetNumBones() != NumBones) return;
        if (BoneIdx < 0 || BoneIdx >= NumBones) return;

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        if (Alpha <= 0.0f) return;

        const float ForwardLen = Math::Length(LocalForward);
        if (ForwardLen < 1e-5f) return;
        const FVector3 Forward = LocalForward / ForwardLen;

        const int32 ParentIdx = Skeleton->GetBone(BoneIdx).ParentIndex;
        const FMatrix4 ParentG = ParentIdx >= 0
            ? Detail::ComputeBoneGlobalLocal(Pose, Skeleton, ParentIdx)
            : FMatrix4(1.0f);

        const FMatrix4 BoneG = ParentG * Detail::ComposeLocal(Pose, BoneIdx);

        FVector3 GT, GS; FQuat GR;
        DecomposeTRS(BoneG, GT, GR, GS);

        const FVector3 ToTarget = Target - GT;
        if (Math::Length(ToTarget) < 1e-5f) return;

        const FVector3 RestDir    = Math::Normalize(GR * Forward);
        const FVector3 DesiredDir = Math::Normalize(ToTarget);

        FQuat Swing = Detail::QuatFromTo(RestDir, DesiredDir);

        // Clamped against the rest direction, so a target behind the head does not snap the neck around.
        if (MaxAngleRadians > 0.0f)
        {
            const float CosHalf = Math::Clamp(Swing.w, -1.0f, 1.0f);
            const float Angle = 2.0f * Math::Acos(CosHalf);
            if (Angle > MaxAngleRadians)
            {
                const FVector3 Axis = FVector3(Swing.x, Swing.y, Swing.z);
                const float AxisLen = Math::Length(Axis);
                if (AxisLen > 1e-5f)
                {
                    Swing = Math::AngleAxis(MaxAngleRadians, Axis / AxisLen);
                }
            }
        }

        FVector3 PT, PS; FQuat PR;
        DecomposeTRS(ParentG, PT, PR, PS);

        Detail::BlendBoneRotation(Pose, BoneIdx, Math::Conjugate(PR) * (Swing * GR), Alpha);
    }

    void AnimPose::FootIK(FPose& Pose, const FSkeletonResource* Skeleton, int32 ThighIdx, int32 CalfIdx,
                          int32 FootIdx, const FVector3& Offset, const FVector3& GroundNormal,
                          const FVector3& FootUpAxis, float NormalAlpha, float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        if (Skeleton == nullptr) return;
        const int32 NumBones = Skeleton->GetNumBones();
        if (Pose.GetNumBones() != NumBones) return;
        if (ThighIdx < 0 || CalfIdx < 0 || FootIdx < 0) return;
        if (ThighIdx >= NumBones || CalfIdx >= NumBones || FootIdx >= NumBones) return;

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        if (Alpha <= 0.0f) return;

        // One walk to the thigh's parent, which the solve never touches; the leg composes down from it.
        const int32 ThighParent = Skeleton->GetBone(ThighIdx).ParentIndex;
        const FMatrix4 ThighParentG = ThighParent >= 0
            ? Detail::ComputeBoneGlobalLocal(Pose, Skeleton, ThighParent)
            : FMatrix4(1.0f);

        const FMatrix4 ThighG = ThighParentG * Detail::ComposeLocal(Pose, ThighIdx);
        const FMatrix4 CalfG  = ThighG * Detail::ComposeLocal(Pose, CalfIdx);
        const FMatrix4 FootG  = CalfG * Detail::ComposeLocal(Pose, FootIdx);

        // The pole keeps the knee where the animation already had it, so IK does not flip the bend.
        const FVector3 Hip  = FVector3(ThighG[3]);
        const FVector3 Knee = FVector3(CalfG[3]);
        const FVector3 Foot = FVector3(FootG[3]);

        const FVector3 Target = Foot + Offset;
        const FVector3 Pole   = Knee + (Knee - Hip);

        TwoBoneIK(Pose, Skeleton, ThighIdx, CalfIdx, FootIdx, Target, Pole, Alpha);

        const float NormalLen = Math::Length(GroundNormal);
        const float UpLen = Math::Length(FootUpAxis);
        const float AlignAlpha = Math::Clamp(NormalAlpha, 0.0f, 1.0f) * Alpha;
        if (NormalLen < 1e-5f || UpLen < 1e-5f || AlignAlpha <= 0.0f)
        {
            return;
        }

        // Recomposed rather than re-walked, since only the thigh and calf rotations moved.
        const FMatrix4 SolvedThighG = ThighParentG * Detail::ComposeLocal(Pose, ThighIdx);
        const FMatrix4 SolvedCalfG  = SolvedThighG * Detail::ComposeLocal(Pose, CalfIdx);
        const FMatrix4 SolvedFootG  = SolvedCalfG * Detail::ComposeLocal(Pose, FootIdx);

        FVector3 FT, FS; FQuat FR;
        DecomposeTRS(SolvedFootG, FT, FR, FS);

        FVector3 PT, PS; FQuat PR;
        DecomposeTRS(SolvedCalfG, PT, PR, PS);

        const FVector3 CurrentUp = Math::Normalize(FR * (FootUpAxis / UpLen));
        const FQuat Align = Detail::QuatFromTo(CurrentUp, GroundNormal / NormalLen);

        Detail::BlendBoneRotation(Pose, FootIdx, Math::Conjugate(PR) * (Align * FR), AlignAlpha);
    }

    void AnimPose::TranslateBoneComponentSpace(FPose& Pose, const FSkeletonResource* Skeleton, int32 BoneIdx,
                                               const FVector3& Offset, float Alpha)
    {
        if (Skeleton == nullptr) return;
        const int32 NumBones = Skeleton->GetNumBones();
        if (Pose.GetNumBones() != NumBones) return;
        if (BoneIdx < 0 || BoneIdx >= NumBones) return;

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        if (Alpha <= 0.0f) return;

        const int32 ParentIdx = Skeleton->GetBone(BoneIdx).ParentIndex;
        if (ParentIdx < 0)
        {
            Pose.SetTranslation(BoneIdx, Pose.GetTranslation(BoneIdx) + Offset * Alpha);
            return;
        }

        // Rotated into the parent's frame, so the offset means the same thing whatever the hips are doing.
        const FMatrix4 ParentG = Detail::ComputeBoneGlobalLocal(Pose, Skeleton, ParentIdx);
        FVector3 PT, PS; FQuat PR;
        DecomposeTRS(ParentG, PT, PR, PS);

        const FVector3 LocalOffset = Math::Conjugate(PR) * (Offset * Alpha);
        Pose.SetTranslation(BoneIdx, Pose.GetTranslation(BoneIdx) + LocalOffset);
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

        const FMatrix4 RootLocal = Detail::ComposeLocal(Pose, RootIdx);
        const FMatrix4 MidLocal  = Detail::ComposeLocal(Pose, MidIdx);
        const FMatrix4 EndLocal  = Detail::ComposeLocal(Pose, EndIdx);

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

        Detail::BlendBoneRotation(Pose, RootIdx, NewRootLocal, Alpha);

        const FMatrix4 NewRootLocalMat = Detail::ComposeLocal(Pose, RootIdx);
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

        Detail::BlendBoneRotation(Pose, MidIdx, NewMidLocal, Alpha);
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

        // Bones[] is parents-before-children, so a single linear FK pass works.
        if (!Skeleton->HasFlatBoneCache())
        {
            for (int32 i = 0; i < NumBones; ++i)
            {
                const FMatrix4 Local = Detail::ComposeLocal(Pose, i);
                const int32 Parent = Skeleton->GetBone(i).ParentIndex;
                OutMatrices[i] = Parent != INDEX_NONE ? OutMatrices[Parent] * Local : Local;
            }

            for (int32 i = 0; i < NumBones; ++i)
            {
                OutMatrices[i] = OutMatrices[i] * Skeleton->GetBone(i).InvBindMatrix;
            }
            return;
        }

        // Globals go to scratch so InvBind folds in during the same pass rather than a second sweep.
        thread_local TVector<FMatrix4> Globals;
        Globals.resize(NumBones);

        const int32* RESTRICT    Parents = Skeleton->BoneParents.data();
        const FMatrix4* RESTRICT InvBind = Skeleton->BoneInvBind.data();

        const float* RESTRICT Tx = Pose.Tx(); const float* RESTRICT Ty = Pose.Ty(); const float* RESTRICT Tz = Pose.Tz();
        const float* RESTRICT Sx = Pose.Sx(); const float* RESTRICT Sy = Pose.Sy(); const float* RESTRICT Sz = Pose.Sz();
        const float* RESTRICT Rx = Pose.Rx(); const float* RESTRICT Ry = Pose.Ry(); const float* RESTRICT Rz = Pose.Rz();
        const float* RESTRICT Rw = Pose.Rw();

        for (int32 i = 0; i < NumBones; ++i)
        {
            const FMatrix4 Local = ComposeTRS(FVector3(Tx[i], Ty[i], Tz[i]),
                                              FQuat(Rw[i], Rx[i], Ry[i], Rz[i]),
                                              FVector3(Sx[i], Sy[i], Sz[i]));
            const int32 Parent = Parents[i];
            Globals[i] = Parent != INDEX_NONE ? Globals[Parent] * Local : Local;
            OutMatrices[i] = Globals[i] * InvBind[i];
        }
    }
}
