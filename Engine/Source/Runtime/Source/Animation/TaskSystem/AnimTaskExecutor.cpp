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
        // A slot serves its own storage unless a task adopts a destination that outlives the frame.
        struct FPosePool
        {
            TVector<TUniquePtr<FPose>> Storage;
            TVector<FPose*> Slots;
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
                const int32 Num = (int32)Slots.size();
                for (int32 i = 0; i < Num; ++i)
                {
                    if (!Used[i])
                    {
                        Used[i] = 1;
                        Slots[i] = Storage[i].Get();
                        return (int16)i;
                    }
                }
                Storage.push_back(MakeUnique<FPose>());
                Slots.push_back(Storage.back().Get());
                Used.push_back(1);
                return (int16)Num;
            }

            int16 Adopt(FPose& External)
            {
                const int16 Index = Acquire();
                Slots[Index] = &External;
                return Index;
            }

            void Release(int16 Index)
            {
                Used[Index] = 0;
            }

            FPose& Get(int16 Index)
            {
                return *Slots[Index];
            }
        };

        FPosePool& GetThreadPosePool()
        {
            thread_local FPosePool Pool;
            return Pool;
        }

        // Quintic offset-decay (Bollo 2018) with x(0)=X0, x'(0)=V0, x''(0)=0 and x(t1)=x'(t1)=x''(t1)=0.
        [[maybe_unused]] float InertEval(float X0, float V0, float T1, float T)
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

            const int32 N = Set.NumLanes();
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

            const float* RESTRICT X0Ptr = Set.X0();
            const float* RESTRICT V0Ptr = Set.V0();
            float* RESTRICT       Out   = Set.Eval();

            if (Duration <= 1e-5f)
            {
                Memory::Memset(Out, 0, (SIZE_T)N * sizeof(float));
                return;
            }

            {
                for (int32 i = 0; i + 8 <= N; i += 8)
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

            // SourcePrev is an empty pose until two frames of history exist, so bVel gates every read of it.
            const FPose& Prev = bVel ? SourcePrev : Target;

            const auto WriteVectorChannel = [](FInertChannelSet& Set, int32 i, const FVector3& Cur,
                                               const FVector3& Previous, const FVector3& Dest, float InvDelta)
            {
                const FVector3 Off = Cur - Dest;
                const float    X0  = Math::Length(Off);
                const FVector3 Dir = X0 > 1e-6f ? Off * (1.0f / X0) : FVector3(0.0f);

                Set.DirX()[i] = Dir.x;
                Set.DirY()[i] = Dir.y;
                Set.DirZ()[i] = Dir.z;
                Set.X0()[i]   = X0;
                Set.V0()[i]   = InvDelta > 0.0f ? (X0 - Math::Dot(Previous - Dest, Dir)) * InvDelta : 0.0f;
            };

            for (int32 i = 0; i < N; ++i)
            {
                const FQuat TargetRot = Target.GetRotation(i);

                // The rotation offset, Source relative to Target, expressed as an axis and angle.
                {
                    const FQuat Qs = bSrc ? Source.GetRotation(i) : TargetRot;
                    FQuat Q0 = Math::Normalize(Qs * Math::Inverse(TargetRot));
                    if (Q0.w < 0.0f)
                    {
                        Q0 = Q0 * -1.0f; // shortest arc
                    }
                    const FVector3 V(Q0.x, Q0.y, Q0.z);
                    const float Len  = Math::Length(V);
                    const float X0   = 2.0f * Math::Atan2(Len, Q0.w);
                    const FVector3 Axis = Len > 1e-5f ? V * (1.0f / Len) : FVector3(0.0f, 0.0f, 1.0f);
                    float V0 = 0.0f;
                    if (bVel)
                    {
                        FQuat Qp = Math::Normalize(Prev.GetRotation(i) * Math::Inverse(TargetRot));
                        if (Qp.w < 0.0f)
                        {
                            // Same shortest arc as X0; a full turn out here is fake velocity.
                            Qp = Qp * -1.0f;
                        }
                        V0 = (X0 - QuatAngleAbout(Qp, Axis)) * InvDt;
                    }
                    In.Rot.DirX()[i] = Axis.x;
                    In.Rot.DirY()[i] = Axis.y;
                    In.Rot.DirZ()[i] = Axis.z;
                    In.Rot.X0()[i] = X0;
                    In.Rot.V0()[i] = V0;
                }

                const FVector3 TargetTrans = Target.GetTranslation(i);
                const FVector3 TargetScale = Target.GetScale(i);

                WriteVectorChannel(In.Trans, i, bSrc ? Source.GetTranslation(i) : TargetTrans,
                                   Prev.GetTranslation(i), TargetTrans, InvDt);
                WriteVectorChannel(In.Scale, i, bSrc ? Source.GetScale(i) : TargetScale,
                                   Prev.GetScale(i), TargetScale, InvDt);
            }
        }

        // Writes Target plus the decaying offset at time T, and Out may alias Target.
        void InertApply(FAnimInertializer& In, const FPose& Target, FPose& Out, float T)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 N = Target.GetNumBones();
            Out.SetNumBones(N);
            Out.AdditiveSpace = Target.AdditiveSpace;

            InertEvalChannels(In.Rot,   In.Duration, T);
            InertEvalChannels(In.Trans, In.Duration, T);
            InertEvalChannels(In.Scale, In.Duration, T);

            // A channel set shorter than the pose leaves zeroed lanes, which pass Target straight through.
            const int32 Lanes = Math::Min(In.Trans.NumLanes(), Out.GetStride());

            MulAddArray(Out.Tx(), Target.Tx(), In.Trans.DirX(), In.Trans.Eval(), Lanes);
            MulAddArray(Out.Ty(), Target.Ty(), In.Trans.DirY(), In.Trans.Eval(), Lanes);
            MulAddArray(Out.Tz(), Target.Tz(), In.Trans.DirZ(), In.Trans.Eval(), Lanes);
            MulAddArray(Out.Sx(), Target.Sx(), In.Scale.DirX(), In.Scale.Eval(), Lanes);
            MulAddArray(Out.Sy(), Target.Sy(), In.Scale.DirY(), In.Scale.Eval(), Lanes);
            MulAddArray(Out.Sz(), Target.Sz(), In.Scale.DirZ(), In.Scale.Eval(), Lanes);

            for (int32 i = Lanes; i < N; ++i)
            {
                Out.SetTranslation(i, Target.GetTranslation(i));
                Out.SetScale(i, Target.GetScale(i));
            }

            // Rotation stays scalar, since a decayed bone skips the axis-angle build entirely.
            const int32 RotLanes = In.Rot.NumLanes();
            const float* RESTRICT RotEval = In.Rot.Eval();
            const float* RESTRICT RotDirX = In.Rot.DirX();
            const float* RESTRICT RotDirY = In.Rot.DirY();
            const float* RESTRICT RotDirZ = In.Rot.DirZ();

            for (int32 i = 0; i < N; ++i)
            {
                const float Xr = i < RotLanes ? RotEval[i] : 0.0f;
                if (Xr > 1e-6f)
                {
                    const FVector3 Axis(RotDirX[i], RotDirY[i], RotDirZ[i]);
                    Out.SetRotation(i, Math::Normalize(Math::AngleAxis(Xr, Axis) * Target.GetRotation(i)));
                }
                else if (&Out != &Target)
                {
                    Out.SetRotation(i, Target.GetRotation(i));
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
                FQuat Delta = Math::Normalize(Source.GetRotation(i) * Math::Inverse(SourcePrev.GetRotation(i)));
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

                Dead.TransVel[i] = (Source.GetTranslation(i) - SourcePrev.GetTranslation(i)) * InvDt;
                Dead.ScaleVel[i] = (Source.GetScale(i) - SourcePrev.GetScale(i)) * InvDt;
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
            Out.AdditiveSpace = Target.AdditiveSpace;

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

                FQuat SourceRot = Dead.Source.GetRotation(i);
                if (Speed > 1e-5f)
                {
                    SourceRot = Math::Normalize(Math::AngleAxis(Speed * Travel, RotV * (1.0f / Speed)) * SourceRot);
                }

                const FVector3 SourceTrans = Dead.Source.GetTranslation(i) + Dead.TransVel[i] * Travel;
                const FVector3 SourceScale = Dead.Source.GetScale(i) + Dead.ScaleVel[i] * Travel;
                const FVector3 TargetTrans = Target.GetTranslation(i);
                const FVector3 TargetScale = Target.GetScale(i);

                Out.SetRotation(i, Math::Slerp(SourceRot, Target.GetRotation(i), Alpha));
                Out.SetTranslation(i, SourceTrans + (TargetTrans - SourceTrans) * Alpha);
                Out.SetScale(i, SourceScale + (TargetScale - SourceScale) * Alpha);
            }

            for (int32 i = N; i < NumBones; ++i)
            {
                Out.CopyBoneFrom(Target, i);
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

        // Result landed in storage the next frame still reads, so no consumer may work in place over it.
        thread_local TVector<uint8> Retained;

        Needed.assign(NumTasks, 0);
        UseCount.assign(NumTasks, 0);
        ResultBuf.assign(NumTasks, FAnimTask::NoTask);
        Retained.assign(NumTasks, 0);

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
            const bool bStealA = BufA != FAnimTask::NoTask && UseCount[Task.DepA] == 1 && !Retained[Task.DepA];
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

                FAnimInertializer* Inert = Task.Inert;
                if (Inert == nullptr)
                {
                    Dst = bStealA ? BufA : Pool.Acquire();
                    if (Dst != BufA)
                    {
                        Pool.Get(Dst) = Pool.Get(BufA);
                    }
                    break;
                }

                if (Task.bCapture)
                {
                    InertCapture(*Inert, Inert->PrevOutput, Inert->PrevPrevOutput, Pool.Get(BufA),
                                 Task.DeltaTime, Inert->HistoryCount >= 2, ActiveBones);
                }

                // Nothing reads the frame-before-last past that capture, so its buffer takes this frame.
                Inert->PrevPrevOutput.Swap(Inert->PrevOutput);
                Dst = Pool.Adopt(Inert->PrevOutput);
                Retained[i] = 1;

                if (Task.bApply)
                {
                    InertApply(*Inert, Pool.Get(BufA), Inert->PrevOutput, Task.Time);
                }
                else if (bStealA)
                {
                    // Last consumer of the input, so its storage can be traded for the dead history buffer.
                    Inert->PrevOutput.Swap(Pool.Get(BufA));
                }
                else
                {
                    Inert->PrevOutput = Pool.Get(BufA);
                }

                Inert->HistoryCount = Math::Min(Inert->HistoryCount + 1, 2);
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

                FAnimDeadBlend* Dead = Task.Dead;
                if (Dead == nullptr)
                {
                    Dst = bStealA ? BufA : Pool.Acquire();
                    if (Dst != BufA)
                    {
                        Pool.Get(Dst) = Pool.Get(BufA);
                    }
                    break;
                }

                if (Task.bCapture)
                {
                    DeadBlendCapture(*Dead, Dead->PrevOutput, Dead->PrevPrevOutput, Pool.Get(BufA),
                                     Task.DeltaTime, Dead->HistoryCount >= 2, ActiveBones);
                }

                Dead->PrevPrevOutput.Swap(Dead->PrevOutput);
                Dst = Pool.Adopt(Dead->PrevOutput);
                Retained[i] = 1;

                if (Task.bApply)
                {
                    DeadBlendApply(*Dead, Pool.Get(BufA), Dead->PrevOutput, Task.Time);
                }
                else if (bStealA)
                {
                    Dead->PrevOutput.Swap(Pool.Get(BufA));
                }
                else
                {
                    Dead->PrevOutput = Pool.Get(BufA);
                }

                Dead->HistoryCount = Math::Min(Dead->HistoryCount + 1, 2);
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

        int16 FinalBuf = ResultBuf[List.OutputTask];

        if (List.bLockRoot && List.RootBoneIndex != INDEX_NONE)
        {
            // Pinning rewrites the root, which would edit the history the output was written into.
            if (Retained[List.OutputTask])
            {
                const int16 Pinned = Pool.Acquire();
                Pool.Get(Pinned) = Pool.Get(FinalBuf);
                Pool.Release(FinalBuf);
                FinalBuf = Pinned;
            }
            RootMotion::PinRootToBindPose(Pool.Get(FinalBuf), Skeleton, List.RootBoneIndex);
        }

        AnimPose::ToSkinningMatrices(Pool.Get(FinalBuf), Skeleton, OutMatrices);
        Pool.Release(FinalBuf);
        List.Reset();
    }
}
