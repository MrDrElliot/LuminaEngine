#pragma once

#include "Animation/Pose.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Renderer/SkeletonResource.h"

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
        Inertialize,
        DeadBlend,
        SavePoseSnapshot,
        LoadPoseSnapshot,
        BoneTransform,
        TwoBoneIK,
        FABRIK,
        LookAt,
        FootIK,
        TranslateBone,
    };

    // Which side of the physics step a task must run on. Everything is Any today; the split exists
    // so physics-coupled tasks (ragdoll write/read) can interleave with the simulation later.
    enum class EAnimTaskStage : uint8
    {
        AnyStage,
        PrePhysics,
        PostPhysics,
    };

    // One channel of inertialization (Bollo 2018), where the offset decays along Dir from magnitude X0
    // with initial velocity V0. Rotation reads Dir as an axis and X0 as radians; translation and scale
    // read Dir as a unit offset and X0 as a length.
    // Structure of arrays, with Dir interleaved exactly as the pose stores its vectors, so the decay
    // evaluates 8 bones at a time and the translation apply is one flat pass over both.
    struct FInertChannelSet
    {
        TVector<float> Dir;    // 3 per bone
        TVector<float> X0;     // 1 per bone
        TVector<float> V0;     // 1 per bone
        TVector<float> Eval;   // 1 per bone, this frame's decayed magnitude
        TVector<float> Eval3;  // 3 per bone, Eval splatted for the flat vector pass

        void Resize(int32 NumBones)
        {
            Dir.assign((SIZE_T)NumBones * 3, 0.0f);
            X0.assign((SIZE_T)NumBones, 0.0f);
            V0.assign((SIZE_T)NumBones, 0.0f);
            Eval.resize((SIZE_T)NumBones);
            Eval3.resize((SIZE_T)NumBones * 3);
        }

        int32 Num() const { return (int32)X0.size(); }
    };

    // One smoothing record, held per state machine and per Inertialization node. Control fields
    // (bActive/Elapsed/Duration) are advanced by the graph update pass; channel capture/apply and the
    // 2-frame pose history run execute-side in the Inertialize task, since they need evaluated poses.
    struct FAnimInertializer
    {
        bool  bActive  = false;
        float Elapsed  = 0.0f;
        float Duration = 0.0f;
        FInertChannelSet Rot;
        FInertChannelSet Trans;
        FInertChannelSet Scale;
        FPose PrevOutput;
        FPose PrevPrevOutput;
        int32 HistoryCount = 0; // 0/1/2 - velocity is only estimated once 2 frames of history exist

        // Curve seam, driven entirely by the graph update pass (curves never reach the executor):
        // the offset from the last shown values decays to zero over the same transition.
        TVector<float> CurveOffsets;
        TVector<float> PrevCurves;
        bool bHasCurveHistory = false;

        // Rising edge of a node's Request input; unused by a state machine, which starts from a transition.
        float PrevRequest = 0.0f;
    };

    // Dead blending (Boulic-style extrapolation, UE5-style node). Where inertialization decays an offset
    // onto the target, this extrapolates the source pose forward from its seam velocity while the decayed
    // source cross-fades into the target, so a fast-moving source does not read as a stuck offset.
    struct FAnimDeadBlend
    {
        bool  bActive  = false;
        float Elapsed  = 0.0f;
        float Duration = 0.0f;

        // Seconds for the extrapolated velocity to halve; small keeps the source honest, large coasts.
        float HalfLife = 0.1f;

        // Pose shown at the seam, extrapolated forward for the length of the blend.
        FPose Source;
        TVector<FVector3> RotVel;     // axis times radians per second
        TVector<FVector3> TransVel;
        TVector<FVector3> ScaleVel;

        FPose PrevOutput;
        FPose PrevPrevOutput;
        int32 HistoryCount = 0;

        TVector<float> CurveOffsets;
        TVector<float> PrevCurves;
        bool bHasCurveHistory = false;

        float PrevRequest = 0.0f;
    };

    // A recorded pose operation. Dependencies are indices of earlier tasks in the same list, so
    // registration order is already a valid execution order. Flat payload rather than a per-type
    // class: the recipe comes from a fixed opcode set and stays POD/reusable with zero allocation.
    struct FAnimTask
    {
        static constexpr int16 NoTask = -1;

        EAnimTaskType  Type  = EAnimTaskType::ReferencePose;
        EAnimTaskStage Stage = EAnimTaskStage::AnyStage;

        int16 DepA = NoTask;
        int16 DepB = NoTask;

        // SampleClip: asset + time (asset kept alive for the frame by the owning graph/component).
        CAnimation* Clip = nullptr;
        float Time = 0.0f;

        // Blends / additive / bone ops.
        float Alpha = 1.0f;

        // MakeAdditive: delta space (EPoseAdditiveSpace). DepB is the base pose, absent = bind pose.
        uint8 AdditiveSpace = (uint8)EPoseAdditiveSpace::LocalSpace;

        // BlendMasked: dense per-bone weights on the graph asset; null falls back to a plain blend.
        const TVector<float>* MaskWeights = nullptr;

        // Inertialize / DeadBlend: per-instance smoothing state + this frame's capture/apply decisions
        // (made update-side; Time carries the pre-advance Elapsed the apply evaluates at).
        FAnimInertializer* Inert = nullptr;
        FAnimDeadBlend* Dead = nullptr;

        // Save / Load Pose Snapshot: the per-instance buffer this slot owns.
        FPose* Snapshot = nullptr;
        float DeltaTime = 0.0f;
        bool bCapture = false;
        bool bApply = false;

        // BoneTransform: offset TRS + target bone + space/mode. TwoBoneIK reuses T as the component-space
        // target, S as the pole, and BoneA/B/C as root/mid/end. FABRIK reuses T as the target and BoneC as
        // the iteration count; LookAt reuses T as the target, S as the local forward axis and Time as the
        // clamp; FootIK reuses T as the offset, R.xyz as the ground normal, S as the foot up axis and Time
        // as the align alpha; TranslateBone reuses T as the component-space offset.
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

    // One task's recorded parameters plus what the executor actually did with it. Filled only while a
    // capture is armed (Anim::ArmTaskCapture), so tools can show the real execution rather than a
    // re-derived guess. Everything here is a fact recorded at the point the executor made the decision.
    struct FAnimTaskDebugEntry
    {
        EAnimTaskType  Type  = EAnimTaskType::ReferencePose;
        EAnimTaskStage Stage = EAnimTaskStage::AnyStage;

        int16 DepA = FAnimTask::NoTask;
        int16 DepB = FAnimTask::NoTask;

        float Alpha = 1.0f;
        float Time  = 0.0f;
        FName ClipName;

        // BlendMasked only: how many bones the mask actually weights, out of the skeleton's total.
        int32 MaskWeightedBones = 0;
        int32 MaskTotalBones    = 0;

        // Ran this frame. False = unreachable from the output task (an inactive state-machine
        // branch), so the executor skipped it entirely and it cost nothing.
        bool  bReachable = false;

        // Wrote its result in place into DepA's buffer (this task was DepA's last consumer) instead
        // of taking a fresh one from the pool.
        bool  bStoleBuffer = false;

        int16 ExecOrder   = -1; // position in the executed sequence; -1 when skipped
        int16 BufferIndex = -1; // pose-pool buffer holding this task's result
        int16 Level       = 0;  // dependency depth: tasks sharing a level have no dependency between them
        int16 LiveBuffers = 0;  // pool buffers in use immediately after this task ran
    };

    // One frame's execution record for a single mesh's task list.
    struct FAnimTaskSnapshot
    {
        TVector<FAnimTaskDebugEntry> Entries;

        int16 OutputTask      = FAnimTask::NoTask;
        int32 NumBones        = 0;
        int32 ActiveBoneCount = 0; // 0 = every bone (no skeleton-LOD cut this frame)
        bool  bLockRoot       = false;

        int32 ReachableCount  = 0;
        int32 PeakLiveBuffers = 0;
        int32 NumLevels       = 0;

        bool bValid = false;

        void Reset()
        {
            Entries.clear();
            OutputTask      = FAnimTask::NoTask;
            NumBones        = 0;
            ActiveBoneCount = 0;
            bLockRoot       = false;
            ReachableCount  = 0;
            PeakLiveBuffers = 0;
            NumLevels       = 0;
            bValid          = false;
        }
    };
}
