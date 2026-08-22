#include "RuntimePCH.h"
#include "AnimTaskExecutor.h"

#include "Animation/RootMotion.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        // A task list runs on one thread, so live buffers are bounded by chain depth, not entity count.
        struct FPosePool
        {
            TVector<FPose> Buffers;
            TVector<uint8> Used;

            // Buffers currently checked out. Only walked while a debug capture is armed.
            int32 LiveCount() const
            {
                int32 Live = 0;
                for (uint8 U : Used)
                {
                    Live += U ? 1 : 0;
                }
                return Live;
            }

            int16 Acquire()
            {
                const int32 Num = (int32)Buffers.size();
                for (int32 i = 0; i < Num; ++i)
                {
                    if (!Used[i])
                    {
                        Used[i] = 1;
                        return (int16)i;
                    }
                }
                Buffers.emplace_back();
                Used.push_back(1);
                return (int16)Num;
            }

            void Release(int16 Index)
            {
                Used[Index] = 0;
            }

            FPose& Get(int16 Index)
            {
                return Buffers[Index];
            }
        };

        FPosePool& GetThreadPosePool()
        {
            thread_local FPosePool Pool;
            return Pool;
        }

        // Quintic offset-decay (Bollo 2018) with x(0)=X0, x'(0)=V0, x''(0)=0 and x(t1)=x'(t1)=x''(t1)=0.
        float InertEval(float X0, float V0, float T1, float T)
        {
            if (T1 <= 1e-5f || X0 <= 1e-7f)
            {
                return 0.0f;
            }
            if (V0 < 0.0f)
            {
                T1 = Math::Min(T1, -5.0f * X0 / V0); // clamp duration so the curve never overshoots below zero
                if (T1 <= 1e-5f)
                {
                    return 0.0f;
                }
            }
            if (T >= T1)
            {
                return 0.0f;
            }
            const float t1_2 = T1 * T1; const float t1_3 = t1_2 * T1; const float t1_4 = t1_3 * T1; const float t1_5 = t1_4 * T1;
            const float A = -(3.0f * V0 * T1 + 6.0f  * X0) / t1_5;
            const float B =  (8.0f * V0 * T1 + 15.0f * X0) / t1_4;
            const float C = -(6.0f * V0 * T1 + 10.0f * X0) / t1_3;
            const float t2 = T * T; const float t3 = t2 * T; const float t4 = t3 * T; const float t5 = t4 * T;
            const float X = A * t5 + B * t4 + C * t3 + V0 * T + X0;
            return X > 0.0f ? X : 0.0f;
        }

        // The same curve, 8 bones at a time, and every branch above becomes a select.
        void InertEvalChannels(FInertChannelSet& Set, float Duration, float T)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 N = Set.Num();
            if (N == 0)
            {
                return;
            }

            using namespace SIMD;

            const VFloat8 Zero      = VFloat8::Zero();
            const VFloat8 Eps       = VFloat8::Broadcast(1e-5f);
            const VFloat8 X0Eps     = VFloat8::Broadcast(1e-7f);
            const VFloat8 VDuration = VFloat8::Broadcast(Duration);
            const VFloat8 VT        = VFloat8::Broadcast(T);
            const VFloat8 VNegFive  = VFloat8::Broadcast(-5.0f);

            const float* RESTRICT X0Ptr = Set.X0.data();
            const float* RESTRICT V0Ptr = Set.V0.data();
            float* RESTRICT       Out   = Set.Eval.data();

            int32 i = 0;
            if (Duration > 1e-5f)
            {
                for (; i + 8 <= N; i += 8)
                {
                    const VFloat8 X0 = VFloat8::Load(X0Ptr + i);
                    const VFloat8 V0 = VFloat8::Load(V0Ptr + i);

                    // A closing offset would dip below zero, so its lane finishes early instead.
                    const VFloat8 Overshoot = VNegFive * X0 / Min(V0, -Eps);
                    const VFloat8 T1        = Select(CmpLt(V0, Zero), Min(VDuration, Overshoot), VDuration);

                    const VFloat8 T1_2 = T1 * T1;
                    const VFloat8 T1_3 = T1_2 * T1;
                    const VFloat8 T1_4 = T1_3 * T1;
                    const VFloat8 T1_5 = T1_4 * T1;

                    const VFloat8 A = -(VFloat8::Broadcast(3.0f) * V0 * T1 + VFloat8::Broadcast(6.0f)  * X0) / T1_5;
                    const VFloat8 B =  (VFloat8::Broadcast(8.0f) * V0 * T1 + VFloat8::Broadcast(15.0f) * X0) / T1_4;
                    const VFloat8 C = -(VFloat8::Broadcast(6.0f) * V0 * T1 + VFloat8::Broadcast(10.0f) * X0) / T1_3;

                    const VFloat8 T2 = VT * VT;
                    const VFloat8 T3 = T2 * VT;
                    const VFloat8 T4 = T3 * VT;
                    const VFloat8 T5 = T4 * VT;

                    VFloat8 X = MulAdd(A, T5, MulAdd(B, T4, MulAdd(C, T3, MulAdd(V0, VT, X0))));

                    // Dead lanes, matching the scalar early-outs one for one.
                    const VFloat8 Alive = And(CmpGt(X0, X0Eps), And(CmpGt(T1, Eps), CmpLt(VT, T1)));
                    X = Select(Alive, Max(X, Zero), Zero);
                    X.Store(Out + i);
                }
            }

            for (; i < N; ++i)
            {
                Out[i] = InertEval(X0Ptr[i], V0Ptr[i], Duration, T);
            }

            // Splatted once so the vector and translation passes share one flat layout.
            float* RESTRICT Out3 = Set.Eval3.data();
            for (int32 Bone = 0; Bone < N; ++Bone)
            {
                const float Value = Out[Bone];
                Out3[Bone * 3 + 0] = Value;
                Out3[Bone * 3 + 1] = Value;
                Out3[Bone * 3 + 2] = Value;
            }
        }

        // Out = Base + Dir * Scale over Count floats, which is the whole translation and scale apply.
        void MulAddArray(float* RESTRICT Out, const float* RESTRICT Base, const float* RESTRICT Dir,
                         const float* RESTRICT Scale, int32 Count)
        {
            using namespace SIMD;

            int32 i = 0;
            for (; i + 8 <= Count; i += 8)
            {
                MulAdd(VFloat8::Load(Dir + i), VFloat8::Load(Scale + i), VFloat8::Load(Base + i)).Store(Out + i);
            }
            for (; i < Count; ++i)
            {
                Out[i] = Base[i] + Dir[i] * Scale[i];
            }
        }

        // Signed angle (rad) of quaternion Q about unit Axis.
        FORCEINLINE float QuatAngleAbout(const FQuat& Q, const FVector3& Axis)
        {
            return 2.0f * Math::Atan2(Math::Dot(FVector3(Q.x, Q.y, Q.z), Axis), Q.w);
        }

        // Channels only, since the update pass sets the control fields, and the tail copies through.
        void InertCapture(FAnimInertializer& In, const FPose& Source, const FPose& SourcePrev,
                          const FPose& Target, float Dt, bool bHasVel, int32 NumActiveBones)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 N = (NumActiveBones >= 0 && NumActiveBones < Target.GetNumBones())
                ? NumActiveBones
                : Target.GetNumBones();
            In.Rot.Resize(N);
            In.Trans.Resize(N);
            In.Scale.Resize(N);

            const bool  bSrc  = Source.GetNumBones() == N;
            const bool  bVel  = bHasVel && Dt > 1e-6f && bSrc && SourcePrev.GetNumBones() == N;
            const float InvDt = bVel ? 1.0f / Dt : 0.0f;

            for (int32 i = 0; i < N; ++i)
            {
                // The rotation offset, Source relative to Target, expressed as an axis and angle.
                {
                    const FQuat Qs = bSrc ? Source.Rotations[i] : Target.Rotations[i];
                    FQuat Q0 = Math::Normalize(Qs * Math::Inverse(Target.Rotations[i]));
                    if (Q0.w < 0.0f)
                    {
                        Q0 = Q0 * -1.0f; // shortest arc
                    }
                    const FVector3 V(Q0.x, Q0.y, Q0.z);
                    const float Len  = Math::Length(V);
                    const float X0   = 2.0f * Math::Atan2(Len, Q0.w);
                    const FVector3 Axis = Len > 1e-5f ? V * (1.0f / Len) : FVector3(0.0f, 0.0f, 1.0f);
                    float V0 = 0.0f;
                    if (InvDt > 0.0f)
                    {
                        FQuat Qp = Math::Normalize(SourcePrev.Rotations[i] * Math::Inverse(Target.Rotations[i]));
                        if (Qp.w < 0.0f)
                        {
                            // Same shortest arc as X0; a full turn out here is fake velocity.
                            Qp = Qp * -1.0f;
                        }
                        V0 = (X0 - QuatAngleAbout(Qp, Axis)) * InvDt;
                    }
                    In.Rot.Dir[i * 3 + 0] = Axis.x;
                    In.Rot.Dir[i * 3 + 1] = Axis.y;
                    In.Rot.Dir[i * 3 + 2] = Axis.z;
                    In.Rot.X0[i] = X0;
                    In.Rot.V0[i] = V0;
                }
                // Translation is a vector offset with a fixed direction and a decaying length.
                {
                    const FVector3 Ps  = bSrc ? Source.Translations[i] : Target.Translations[i];
                    const FVector3 Off = Ps - Target.Translations[i];
                    const float    X0  = Math::Length(Off);
                    const FVector3 Dir = X0 > 1e-6f ? Off * (1.0f / X0) : FVector3(0.0f);
                    float V0 = 0.0f;
                    if (InvDt > 0.0f)
                    {
                        V0 = (X0 - Math::Dot(SourcePrev.Translations[i] - Target.Translations[i], Dir)) * InvDt;
                    }
                    In.Trans.Dir[i * 3 + 0] = Dir.x;
                    In.Trans.Dir[i * 3 + 1] = Dir.y;
                    In.Trans.Dir[i * 3 + 2] = Dir.z;
                    In.Trans.X0[i] = X0;
                    In.Trans.V0[i] = V0;
                }
                // Scale gets the same vector treatment as translation.
                {
                    const FVector3 Ss  = bSrc ? Source.Scales[i] : Target.Scales[i];
                    const FVector3 Off = Ss - Target.Scales[i];
                    const float    X0  = Math::Length(Off);
                    const FVector3 Dir = X0 > 1e-6f ? Off * (1.0f / X0) : FVector3(0.0f);
                    float V0 = 0.0f;
                    if (InvDt > 0.0f)
                    {
                        V0 = (X0 - Math::Dot(SourcePrev.Scales[i] - Target.Scales[i], Dir)) * InvDt;
                    }
                    In.Scale.Dir[i * 3 + 0] = Dir.x;
                    In.Scale.Dir[i * 3 + 1] = Dir.y;
                    In.Scale.Dir[i * 3 + 2] = Dir.z;
                    In.Scale.X0[i] = X0;
                    In.Scale.V0[i] = V0;
                }
            }
        }

        // Writes Target plus the decaying offset at time T, and Out may alias Target.
        void InertApply(FAnimInertializer& In, const FPose& Target, FPose& Out, float T)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 N  = Target.GetNumBones();
            const int32 NR = In.Rot.Num();
            Out.SetNumBones(N);

            InertEvalChannels(In.Rot,   In.Duration, T);
            InertEvalChannels(In.Trans, In.Duration, T);
            InertEvalChannels(In.Scale, In.Duration, T);

            // Both are flat xyz streams in the same order, so the offset applies in one vector pass.
            const int32 NumComponents = Math::Min(N, NR) * 3;
            MulAddArray(reinterpret_cast<float*>(Out.Translations.data()),
                        reinterpret_cast<const float*>(Target.Translations.data()),
                        In.Trans.Dir.data(), In.Trans.Eval3.data(), NumComponents);
            MulAddArray(reinterpret_cast<float*>(Out.Scales.data()),
                        reinterpret_cast<const float*>(Target.Scales.data()),
                        In.Scale.Dir.data(), In.Scale.Eval3.data(), NumComponents);

            // Rotation stays scalar; the axis-angle build and the shortest-arc handling are worth more
            // than the lanes, and a decayed bone skips the trig entirely.
            const float* RESTRICT RotEval = In.Rot.Eval.data();
            const float* RESTRICT RotDir  = In.Rot.Dir.data();

            for (int32 i = 0; i < N; ++i)
            {
                if (i >= NR)
                {
                    Out.Rotations[i]    = Target.Rotations[i];
                    Out.Translations[i] = Target.Translations[i];
                    Out.Scales[i]       = Target.Scales[i];
                    continue;
                }

                const float Xr = RotEval[i];
                if (Xr > 1e-6f)
                {
                    const FVector3 Axis(RotDir[i * 3 + 0], RotDir[i * 3 + 1], RotDir[i * 3 + 2]);
                    Out.Rotations[i] = Math::Normalize(Math::AngleAxis(Xr, Axis) * Target.Rotations[i]);
                }
                else if (&Out != &Target)
                {
                    Out.Rotations[i] = Target.Rotations[i];
                }
            }
        }

        // Seam velocities for dead blending, from the same 2-frame history inertialization uses.
        void DeadBlendCapture(FAnimDeadBlend& Dead, const FPose& Source, const FPose& SourcePrev,
                              const FPose& Target, float Dt, bool bHasVel, int32 NumActiveBones)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 N = (NumActiveBones >= 0 && NumActiveBones < Target.GetNumBones())
                ? NumActiveBones
                : Target.GetNumBones();

            const bool bSrc = Source.GetNumBones() >= N;
            Dead.Source = bSrc ? Source : Target;

            Dead.RotVel.assign(N, FVector3(0.0f));
            Dead.TransVel.assign(N, FVector3(0.0f));
            Dead.ScaleVel.assign(N, FVector3(0.0f));

            const bool bVel = bHasVel && Dt > 1e-6f && bSrc && SourcePrev.GetNumBones() >= N;
            if (!bVel)
            {
                return;
            }

            const float InvDt = 1.0f / Dt;
            for (int32 i = 0; i < N; ++i)
            {
                // Rotation velocity as a scaled axis, from the shortest arc between the last two frames.
                FQuat Delta = Math::Normalize(Source.Rotations[i] * Math::Inverse(SourcePrev.Rotations[i]));
                if (Delta.w < 0.0f)
                {
                    Delta = Delta * -1.0f;
                }
                const FVector3 V(Delta.x, Delta.y, Delta.z);
                const float Len = Math::Length(V);
                if (Len > 1e-5f)
                {
                    const float Angle = 2.0f * Math::Atan2(Len, Delta.w);
                    Dead.RotVel[i] = V * (Angle * InvDt / Len);
                }

                Dead.TransVel[i] = (Source.Translations[i] - SourcePrev.Translations[i]) * InvDt;
                Dead.ScaleVel[i] = (Source.Scales[i] - SourcePrev.Scales[i]) * InvDt;
            }
        }

        // Integral of an exponentially decaying velocity, which is how far the source coasts by time T.
        FORCEINLINE float DecayedTravel(float T, float HalfLife)
        {
            if (HalfLife <= 1e-5f)
            {
                return 0.0f;
            }
            const float Tau = HalfLife / 0.6931472f;
            return Tau * (1.0f - Math::Exp(-T / Tau));
        }

        // The extrapolated source cross-fades into Target, so a fast source keeps moving through the seam.
        void DeadBlendApply(const FAnimDeadBlend& Dead, const FPose& Target, FPose& Out, float T)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 NumBones = Target.GetNumBones();
            Out.SetNumBones(NumBones);

            const int32 NumChannels = (int32)Dead.RotVel.size();
            const int32 N = Math::Min(NumBones, Math::Min(NumChannels, Dead.Source.GetNumBones()));

            const float U = (Dead.Duration > 1e-5f) ? Math::Clamp(T / Dead.Duration, 0.0f, 1.0f) : 1.0f;

            // Smoothstep, so the hand-off starts and ends without a velocity step of its own.
            const float Alpha = U * U * (3.0f - 2.0f * U);
            const float Travel = DecayedTravel(T, Dead.HalfLife);

            for (int32 i = 0; i < N; ++i)
            {
                const FVector3 RotV = Dead.RotVel[i];
                const float Speed = Math::Length(RotV);

                FQuat SourceRot = Dead.Source.Rotations[i];
                if (Speed > 1e-5f)
                {
                    SourceRot = Math::Normalize(Math::AngleAxis(Speed * Travel, RotV * (1.0f / Speed)) * SourceRot);
                }

                const FVector3 SourceTrans = Dead.Source.Translations[i] + Dead.TransVel[i] * Travel;
                const FVector3 SourceScale = Dead.Source.Scales[i] + Dead.ScaleVel[i] * Travel;

                Out.Rotations[i]    = Math::Slerp(SourceRot, Target.Rotations[i], Alpha);
                Out.Translations[i] = SourceTrans + (Target.Translations[i] - SourceTrans) * Alpha;
                Out.Scales[i]       = SourceScale + (Target.Scales[i] - SourceScale) * Alpha;
            }

            for (int32 i = N; i < NumBones; ++i)
            {
                Out.Rotations[i]    = Target.Rotations[i];
                Out.Translations[i] = Target.Translations[i];
                Out.Scales[i]       = Target.Scales[i];
            }
        }
    }

    namespace
    {
        // Tools arm one component, so the system pays only a relaxed load per mesh in the common case.
        TAtomic<const void*> GCaptureOwner{ nullptr };
        FMutex               GCaptureMutex;
        FAnimTaskSnapshot    GCaptureSnapshot;
    }

    void Anim::ArmTaskCapture(const void* Owner)
    {
        GCaptureOwner.store(Owner, std::memory_order_relaxed);
    }

    void Anim::DisarmTaskCapture()
    {
        GCaptureOwner.store(nullptr, std::memory_order_relaxed);

        FScopeLock Lock(GCaptureMutex);
        GCaptureSnapshot.Reset();
    }

    bool Anim::IsTaskCaptureArmed(const void* Owner)
    {
        return Owner != nullptr && GCaptureOwner.load(std::memory_order_relaxed) == Owner;
    }

    void Anim::StoreTaskCapture(const FAnimTaskSnapshot& Snapshot)
    {
        FScopeLock Lock(GCaptureMutex);
        GCaptureSnapshot = Snapshot;
    }

    bool Anim::GetTaskCapture(FAnimTaskSnapshot& OutSnapshot)
    {
        FScopeLock Lock(GCaptureMutex);
        if (!GCaptureSnapshot.bValid)
        {
            return false;
        }
        OutSnapshot = GCaptureSnapshot;
        return true;
    }

    void Anim::ExecuteTaskList(FAnimTaskList& List, TVector<FMatrix4>& OutMatrices, FAnimTaskSnapshot* OutSnapshot)
    {
        LUMINA_PROFILE_SCOPE();

        FSkeletonResource* Skeleton = List.Skeleton;
        if (Skeleton == nullptr || Skeleton->GetNumBones() == 0)
        {
            List.Reset();
            return;
        }

        FPosePool& Pool = GetThreadPosePool();

        const int32 NumTasks = (int32)List.Tasks.size();

        // The tail rides along at bind-pose locals, and the final FK stays full-hierarchy.
        const int32 NumBones    = Skeleton->GetNumBones();
        const int32 ActiveBones = (List.ActiveBoneCount > 0 && List.ActiveBoneCount < NumBones)
            ? List.ActiveBoneCount
            : NumBones;

        // Record the recipe before executing it; the loops below then stamp in what actually ran.
        if (OutSnapshot != nullptr)
        {
            OutSnapshot->Reset();
            OutSnapshot->OutputTask      = List.OutputTask;
            OutSnapshot->NumBones        = NumBones;
            OutSnapshot->ActiveBoneCount = List.ActiveBoneCount;
            OutSnapshot->bLockRoot       = List.bLockRoot;
            OutSnapshot->Entries.resize(NumTasks);
            OutSnapshot->bValid          = true;

            for (int32 i = 0; i < NumTasks; ++i)
            {
                const FAnimTask& Task = List.Tasks[i];
                FAnimTaskDebugEntry& Entry = OutSnapshot->Entries[i];

                Entry.Type     = Task.Type;
                Entry.Stage    = Task.Stage;
                Entry.DepA     = Task.DepA;
                Entry.DepB     = Task.DepB;
                Entry.Alpha    = Task.Alpha;
                Entry.Time     = Task.Time;
                Entry.ClipName = Task.Clip != nullptr ? Task.Clip->GetName() : FName();

                if (Task.MaskWeights != nullptr)
                {
                    Entry.MaskTotalBones = (int32)Task.MaskWeights->size();
                    for (float Weight : *Task.MaskWeights)
                    {
                        Entry.MaskWeightedBones += Weight > 0.0f ? 1 : 0;
                    }
                }

                // Deps always precede their consumer, so one forward pass resolves the level.
                int16 Level = 0;
                if (Task.DepA >= 0 && Task.DepA < i)
                {
                    Level = Math::Max<int16>(Level, (int16)(OutSnapshot->Entries[Task.DepA].Level + 1));
                }
                if (Task.DepB >= 0 && Task.DepB < i)
                {
                    Level = Math::Max<int16>(Level, (int16)(OutSnapshot->Entries[Task.DepB].Level + 1));
                }
                Entry.Level = Level;
                OutSnapshot->NumLevels = Math::Max(OutSnapshot->NumLevels, (int32)Level + 1);
            }
        }

        // Empty/invalid recipe resolves to the bind pose (same fallback the old inline VM had).
        if (List.OutputTask < 0 || List.OutputTask >= NumTasks)
        {
            const int16 Buf = Pool.Acquire();
            FPose& Bind = Pool.Get(Buf);
            Bind.ResetToBindPose(Skeleton);
            AnimPose::ToSkinningMatrices(Bind, Skeleton, OutMatrices);
            Pool.Release(Buf);
            List.Reset();
            return;
        }

        // Dependencies always precede consumers, so one reverse pass suffices and the rest is skipped.
        thread_local TVector<uint8> Needed;
        thread_local TVector<uint8> UseCount;
        thread_local TVector<int16> ResultBuf;

        Needed.assign(NumTasks, 0);
        UseCount.assign(NumTasks, 0);
        ResultBuf.assign(NumTasks, FAnimTask::NoTask);

        Needed[List.OutputTask] = 1;
        for (int32 i = List.OutputTask; i >= 0; --i)
        {
            if (!Needed[i])
            {
                continue;
            }
            const FAnimTask& Task = List.Tasks[i];
            if (Task.DepA >= 0 && Task.DepA < i)
            {
                Needed[Task.DepA] = 1;
                ++UseCount[Task.DepA];
            }
            if (Task.DepB >= 0 && Task.DepB < i)
            {
                Needed[Task.DepB] = 1;
                ++UseCount[Task.DepB];
            }
        }

        if (OutSnapshot != nullptr)
        {
            for (int32 i = 0; i < NumTasks; ++i)
            {
                OutSnapshot->Entries[i].bReachable = Needed[i] != 0;
                OutSnapshot->ReachableCount += Needed[i] ? 1 : 0;
            }
        }
        int16 ExecOrderCounter = 0;

        for (int32 i = 0; i <= List.OutputTask; ++i)
        {
            if (!Needed[i])
            {
                continue;
            }

            const FAnimTask& Task = List.Tasks[i];

            const int16 BufA = (Task.DepA >= 0 && Task.DepA < i) ? ResultBuf[Task.DepA] : FAnimTask::NoTask;
            const int16 BufB = (Task.DepB >= 0 && Task.DepB < i) ? ResultBuf[Task.DepB] : FAnimTask::NoTask;

            // Stealing writes in place with zero copy, otherwise a fresh buffer leaves the shared result alone.
            const bool bStealA = BufA != FAnimTask::NoTask && UseCount[Task.DepA] == 1;
            int16 Dst = FAnimTask::NoTask;

            switch (Task.Type)
            {
            case EAnimTaskType::ReferencePose:
            {
                LUMINA_PROFILE_SECTION("Anim RefPose");
                Dst = Pool.Acquire();
                Pool.Get(Dst).ResetToBindPose(Skeleton);
                break;
            }

            case EAnimTaskType::SampleClip:
            {
                Dst = Pool.Acquire();
                FPose& Out = Pool.Get(Dst);
                if (Task.Clip != nullptr)
                {
                    Task.Clip->SampleLocalPose(Task.Time, Skeleton, Out, ActiveBones);
                }
                else
                {
                    Out.ResetToBindPose(Skeleton);
                }
                break;
            }

            case EAnimTaskType::Blend:
            case EAnimTaskType::BlendMasked:
            {
                LUMINA_PROFILE_SECTION("Anim Blend");
                if (BufA == FAnimTask::NoTask || BufB == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }
                Dst = bStealA ? BufA : Pool.Acquire();
                const FPose& A = Pool.Get(BufA);
                const FPose& B = Pool.Get(BufB);
                if (Task.Type == EAnimTaskType::BlendMasked && Task.MaskWeights != nullptr)
                {
                    AnimPose::BlendMasked(A, B, Task.Alpha, *Task.MaskWeights, Pool.Get(Dst), ActiveBones);
                }
                else
                {
                    AnimPose::Blend(A, B, Task.Alpha, Pool.Get(Dst), ActiveBones);
                }
                break;
            }

            case EAnimTaskType::MakeAdditive:
            {
                if (BufA == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }
                Dst = bStealA ? BufA : Pool.Acquire();

                const bool bMeshSpace = (EPoseAdditiveSpace)Task.AdditiveSpace == EPoseAdditiveSpace::MeshSpace;
                if (BufB != FAnimTask::NoTask)
                {
                    if (bMeshSpace)
                    {
                        AnimPose::MakeAdditiveMeshSpace(Pool.Get(BufA), Pool.Get(BufB), Skeleton, Pool.Get(Dst), ActiveBones);
                    }
                    else
                    {
                        AnimPose::MakeAdditiveFromBase(Pool.Get(BufA), Pool.Get(BufB), Pool.Get(Dst), ActiveBones);
                    }
                }
                else if (bMeshSpace)
                {
                    AnimPose::MakeAdditiveMeshSpace(Pool.Get(BufA), Skeleton, Pool.Get(Dst), ActiveBones);
                }
                else
                {
                    AnimPose::MakeAdditive(Pool.Get(BufA), Skeleton, Pool.Get(Dst), ActiveBones);
                }
                break;
            }

            case EAnimTaskType::ApplyAdditive:
            {
                if (BufA == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }

                // No delta reaching the layer means nothing to add, not a lost base pose.
                if (BufB == FAnimTask::NoTask)
                {
                    Dst = bStealA ? BufA : Pool.Acquire();
                    if (Dst != BufA)
                    {
                        Pool.Get(Dst) = Pool.Get(BufA);
                    }
                    break;
                }

                Dst = bStealA ? BufA : Pool.Acquire();
                AnimPose::ApplyAdditivePose(Pool.Get(BufA), Pool.Get(BufB), Task.Alpha, Skeleton, Pool.Get(Dst), ActiveBones);
                break;
            }

            case EAnimTaskType::Inertialize:
            {
                LUMINA_PROFILE_SECTION("Anim Inertialize");
                if (BufA == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }

                Dst = bStealA ? BufA : Pool.Acquire();
                FAnimInertializer* Inert = Task.Inert;

                if (Inert != nullptr && Task.bCapture)
                {
                    InertCapture(*Inert, Inert->PrevOutput, Inert->PrevPrevOutput, Pool.Get(BufA),
                                 Task.DeltaTime, Inert->HistoryCount >= 2, ActiveBones);
                }

                if (Inert != nullptr && Task.bApply)
                {
                    InertApply(*Inert, Pool.Get(BufA), Pool.Get(Dst), Task.Time);
                }
                else if (Dst != BufA)
                {
                    Pool.Get(Dst) = Pool.Get(BufA);
                }

                // 2-frame output history for the next seam's velocity estimate.
                if (Inert != nullptr)
                {
                    LUMINA_PROFILE_SECTION("Anim Inert History");
                    std::swap(Inert->PrevPrevOutput, Inert->PrevOutput);
                    Inert->PrevOutput   = Pool.Get(Dst);
                    Inert->HistoryCount = Math::Min(Inert->HistoryCount + 1, 2);
                }
                break;
            }

            case EAnimTaskType::SavePoseSnapshot:
            {
                LUMINA_PROFILE_SECTION("Anim SavePoseSnapshot");
                if (BufA == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }

                Dst = bStealA ? BufA : Pool.Acquire();
                if (Dst != BufA)
                {
                    Pool.Get(Dst) = Pool.Get(BufA);
                }

                if (Task.Snapshot != nullptr && Task.bCapture)
                {
                    *Task.Snapshot = Pool.Get(BufA);
                }
                break;
            }

            case EAnimTaskType::LoadPoseSnapshot:
            {
                LUMINA_PROFILE_SECTION("Anim LoadPoseSnapshot");
                Dst = Pool.Acquire();

                // A slot nothing has saved into yet reads as the bind pose rather than as garbage.
                if (Task.Snapshot != nullptr && Task.Snapshot->GetNumBones() == Skeleton->GetNumBones())
                {
                    Pool.Get(Dst) = *Task.Snapshot;
                }
                else
                {
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                }
                break;
            }

            case EAnimTaskType::DeadBlend:
            {
                LUMINA_PROFILE_SECTION("Anim DeadBlend");
                if (BufA == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }

                Dst = bStealA ? BufA : Pool.Acquire();
                FAnimDeadBlend* Dead = Task.Dead;

                if (Dead != nullptr && Task.bCapture)
                {
                    DeadBlendCapture(*Dead, Dead->PrevOutput, Dead->PrevPrevOutput, Pool.Get(BufA),
                                     Task.DeltaTime, Dead->HistoryCount >= 2, ActiveBones);
                }

                if (Dead != nullptr && Task.bApply)
                {
                    DeadBlendApply(*Dead, Pool.Get(BufA), Pool.Get(Dst), Task.Time);
                }
                else if (Dst != BufA)
                {
                    Pool.Get(Dst) = Pool.Get(BufA);
                }

                if (Dead != nullptr)
                {
                    LUMINA_PROFILE_SECTION("Anim DeadBlend History");
                    std::swap(Dead->PrevPrevOutput, Dead->PrevOutput);
                    Dead->PrevOutput   = Pool.Get(Dst);
                    Dead->HistoryCount = Math::Min(Dead->HistoryCount + 1, 2);
                }
                break;
            }

            case EAnimTaskType::BoneTransform:
            case EAnimTaskType::TwoBoneIK:
            case EAnimTaskType::FABRIK:
            case EAnimTaskType::LookAt:
            case EAnimTaskType::FootIK:
            case EAnimTaskType::TranslateBone:
            {
                LUMINA_PROFILE_SECTION("Anim BoneOp");
                if (BufA == FAnimTask::NoTask)
                {
                    Dst = Pool.Acquire();
                    Pool.Get(Dst).ResetToBindPose(Skeleton);
                    break;
                }
                Dst = bStealA ? BufA : Pool.Acquire();
                if (Dst != BufA)
                {
                    Pool.Get(Dst) = Pool.Get(BufA);
                }
                switch (Task.Type)
                {
                case EAnimTaskType::BoneTransform:
                    AnimPose::ApplyBoneTransform(Pool.Get(Dst), Skeleton, (int32)Task.BoneA,
                                                 (AnimPose::EBoneSpace)Task.Space,
                                                 (AnimPose::EBoneApplyMode)Task.Mode,
                                                 Task.T, Task.R, Task.S, Task.Alpha);
                    break;

                case EAnimTaskType::FABRIK:
                    AnimPose::FABRIK(Pool.Get(Dst), Skeleton, (int32)Task.BoneA, (int32)Task.BoneB,
                                     Task.T, (int32)Task.BoneC, Task.Alpha);
                    break;

                case EAnimTaskType::LookAt:
                    AnimPose::LookAt(Pool.Get(Dst), Skeleton, (int32)Task.BoneA, Task.T, Task.S, Task.Time, Task.Alpha);
                    break;

                case EAnimTaskType::FootIK:
                    AnimPose::FootIK(Pool.Get(Dst), Skeleton, (int32)Task.BoneA, (int32)Task.BoneB, (int32)Task.BoneC,
                                     Task.T, FVector3(Task.R.x, Task.R.y, Task.R.z), Task.S, Task.Time, Task.Alpha);
                    break;

                case EAnimTaskType::TranslateBone:
                    AnimPose::TranslateBoneComponentSpace(Pool.Get(Dst), Skeleton, (int32)Task.BoneA, Task.T, Task.Alpha);
                    break;

                default:
                    AnimPose::TwoBoneIK(Pool.Get(Dst), Skeleton,
                                        (int32)Task.BoneA, (int32)Task.BoneB, (int32)Task.BoneC,
                                        Task.T, Task.S, Task.Alpha);
                    break;
                }
                break;
            }
            }

            ResultBuf[i] = Dst;

            // Retire consumed dependencies; a stolen buffer is now owned by this task.
            if (Task.DepA >= 0 && Task.DepA < i && --UseCount[Task.DepA] == 0 && BufA != Dst && BufA != FAnimTask::NoTask)
            {
                Pool.Release(BufA);
            }
            if (Task.DepB >= 0 && Task.DepB < i && --UseCount[Task.DepB] == 0 && BufB != Dst && BufB != FAnimTask::NoTask)
            {
                Pool.Release(BufB);
            }

            // Stamped after the retire, so LiveBuffers reads as steady-state rather than a transient peak.
            if (OutSnapshot != nullptr)
            {
                FAnimTaskDebugEntry& Entry = OutSnapshot->Entries[i];
                Entry.ExecOrder    = ExecOrderCounter++;
                Entry.BufferIndex  = Dst;
                Entry.bStoleBuffer = Dst != FAnimTask::NoTask && Dst == BufA;
                Entry.LiveBuffers  = (int16)Pool.LiveCount();
                OutSnapshot->PeakLiveBuffers = Math::Max(OutSnapshot->PeakLiveBuffers, (int32)Entry.LiveBuffers);
            }
        }

        const int16 FinalBuf = ResultBuf[List.OutputTask];
        FPose& Final = Pool.Get(FinalBuf);

        if (List.bLockRoot && List.RootBoneIndex != INDEX_NONE)
        {
            RootMotion::PinRootToBindPose(Final, Skeleton, List.RootBoneIndex);
        }

        AnimPose::ToSkinningMatrices(Final, Skeleton, OutMatrices);
        Pool.Release(FinalBuf);
        List.Reset();
    }
}
