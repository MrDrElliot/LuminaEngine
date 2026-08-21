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
            In.Rot.resize(N);
            In.Trans.resize(N);
            In.Scale.resize(N);

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
                    In.Rot[i] = FInertChannel{ Axis, X0, V0 };
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
                    In.Trans[i] = FInertChannel{ Dir, X0, V0 };
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
                    In.Scale[i] = FInertChannel{ Dir, X0, V0 };
                }
            }
        }

        // Writes Target plus the decaying offset at time T, and Out may alias Target.
        void InertApply(const FAnimInertializer& In, const FPose& Target, FPose& Out, float T)
        {
            LUMINA_PROFILE_SCOPE();

            const int32 N  = Target.GetNumBones();
            const int32 NR = (int32)In.Rot.size();
            Out.SetNumBones(N);

            for (int32 i = 0; i < N; ++i)
            {
                if (i < NR)
                {
                    const float Xr = InertEval(In.Rot[i].X0,   In.Rot[i].V0,   In.Duration, T);
                    const float Xt = InertEval(In.Trans[i].X0, In.Trans[i].V0, In.Duration, T);
                    const float Xs = InertEval(In.Scale[i].X0, In.Scale[i].V0, In.Duration, T);

                    FQuat R = Target.Rotations[i];
                    if (Xr > 1e-6f)
                    {
                        R = Math::Normalize(Math::AngleAxis(Xr, In.Rot[i].Direction) * R);
                    }
                    Out.Rotations[i]    = R;
                    Out.Translations[i] = Target.Translations[i] + In.Trans[i].Direction * Xt;
                    Out.Scales[i]       = Target.Scales[i] + In.Scale[i].Direction * Xs;
                }
                else
                {
                    Out.Rotations[i]    = Target.Rotations[i];
                    Out.Translations[i] = Target.Translations[i];
                    Out.Scales[i]       = Target.Scales[i];
                }
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

            case EAnimTaskType::StateMachineOutput:
            {
                LUMINA_PROFILE_SECTION("Anim StateMachine");
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
                    LUMINA_PROFILE_SECTION("Anim SM History");
                    std::swap(Inert->PrevPrevOutput, Inert->PrevOutput);
                    Inert->PrevOutput   = Pool.Get(Dst);
                    Inert->HistoryCount = Math::Min(Inert->HistoryCount + 1, 2);
                }
                break;
            }

            case EAnimTaskType::BoneTransform:
            case EAnimTaskType::TwoBoneIK:
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
                if (Task.Type == EAnimTaskType::BoneTransform)
                {
                    AnimPose::ApplyBoneTransform(Pool.Get(Dst), Skeleton, (int32)Task.BoneA,
                                                 (AnimPose::EBoneSpace)Task.Space,
                                                 (AnimPose::EBoneApplyMode)Task.Mode,
                                                 Task.T, Task.R, Task.S, Task.Alpha);
                }
                else
                {
                    AnimPose::TwoBoneIK(Pool.Get(Dst), Skeleton,
                                        (int32)Task.BoneA, (int32)Task.BoneB, (int32)Task.BoneC,
                                        Task.T, Task.S, Task.Alpha);
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
