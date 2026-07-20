#pragma once

#include "Animation/Pose.h"
#include "Containers/Array.h"

namespace Lumina
{
    class CAnimation;
    struct FSkeletonResource;

    // Pose operation deferred out of the graph update. The update pass records these (cheap logic,
    // no pose math); the executor runs only the chains reachable from the output task.
    enum class EAnimTaskType : uint8
    {
        ReferencePose,
        SampleClip,
        Blend,
        BlendMasked,
        MakeAdditive,
        ApplyAdditive,
        StateMachineOutput,
        BoneTransform,
        TwoBoneIK,
    };

    // Which side of the physics step a task must run on. Everything is Any today; the split exists
    // so physics-coupled tasks (ragdoll write/read) can interleave with the simulation later.
    enum class EAnimTaskStage : uint8
    {
        Any,
        PrePhysics,
        PostPhysics,
    };

    // One channel's inertialization record (Bollo 2018): the offset decays along Direction from magnitude
    // X0 (with initial velocity V0) to zero over the transition. Rotation: Direction = axis, X0 = angle
    // (rad); translation/scale: Direction = unit offset, X0 = length.
    struct FInertChannel
    {
        FVector3 Direction = FVector3(0.0f);
        float    X0 = 0.0f;
        float    V0 = 0.0f;
    };

    // Per-state-machine inertialization. Control fields (bActive/Elapsed/Duration) are advanced by the
    // graph update pass; channel capture/apply and the 2-frame pose history run execute-side in the
    // StateMachineOutput task, since they need evaluated poses.
    struct FAnimInertializer
    {
        bool  bActive  = false;
        float Elapsed  = 0.0f;
        float Duration = 0.0f;
        TVector<FInertChannel> Rot;
        TVector<FInertChannel> Trans;
        TVector<FInertChannel> Scale;
        FPose PrevOutput;
        FPose PrevPrevOutput;
        int32 HistoryCount = 0; // 0/1/2 - velocity is only estimated once 2 frames of history exist
    };

    // A recorded pose operation. Dependencies are indices of earlier tasks in the same list, so
    // registration order is already a valid execution order. Flat payload rather than a per-type
    // class: the recipe comes from a fixed opcode set and stays POD/reusable with zero allocation.
    struct FAnimTask
    {
        static constexpr int16 NoTask = -1;

        EAnimTaskType  Type  = EAnimTaskType::ReferencePose;
        EAnimTaskStage Stage = EAnimTaskStage::Any;

        int16 DepA = NoTask;
        int16 DepB = NoTask;

        // SampleClip: asset + time (asset kept alive for the frame by the owning graph/component).
        CAnimation* Clip = nullptr;
        float Time = 0.0f;

        // Blends / additive / bone ops.
        float Alpha = 1.0f;

        // BlendMasked: dense per-bone weights on the graph asset; null falls back to a plain blend.
        const TVector<float>* MaskWeights = nullptr;

        // StateMachineOutput: per-instance smoothing state + this frame's capture/apply decisions
        // (made update-side; Time carries the pre-advance Elapsed the apply evaluates at).
        FAnimInertializer* Inert = nullptr;
        float DeltaTime = 0.0f;
        bool bCapture = false;
        bool bApply = false;

        // BoneTransform: offset TRS + target bone + space/mode. TwoBoneIK reuses T as the component-space
        // target, S as the pole, and BoneA/B/C as root/mid/end.
        FVector3 T = FVector3(0.0f);
        FQuat    R = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        FVector3 S = FVector3(1.0f);
        uint16 BoneA = 0;
        uint16 BoneB = 0;
        uint16 BoneC = 0;
        uint8 Space = 0; // AnimPose::EBoneSpace
        uint8 Mode  = 0; // AnimPose::EBoneApplyMode
    };

    // Per-entity recipe for one frame: the update pass fills it, the executor consumes it into
    // skinning matrices and clears it. Capacity persists across frames.
    struct FAnimTaskList
    {
        TVector<FAnimTask> Tasks;
        int16 OutputTask = FAnimTask::NoTask;

        // Final-pose fixups + the skeleton every pose is authored against (valid for the frame).
        FSkeletonResource* Skeleton = nullptr;
        bool bLockRoot = false;
        int32 RootBoneIndex = INDEX_NONE;

        // Skeleton LOD: bones sampled/blended this frame (0 = all). Bones past the cut ride along
        // at bind-pose locals; FK still runs the full hierarchy so skinning and attachments stay valid.
        int32 ActiveBoneCount = 0;

        bool HasWork() const { return OutputTask != FAnimTask::NoTask; }

        int16 Add(const FAnimTask& Task)
        {
            Tasks.push_back(Task);
            return (int16)(Tasks.size() - 1);
        }

        void Reset()
        {
            Tasks.clear();
            OutputTask      = FAnimTask::NoTask;
            Skeleton        = nullptr;
            bLockRoot       = false;
            RootBoneIndex   = INDEX_NONE;
            ActiveBoneCount = 0;
        }
    };
}
