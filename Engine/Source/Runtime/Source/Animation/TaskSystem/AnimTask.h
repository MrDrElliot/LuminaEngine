#pragma once

#include "Animation/Pose.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CAnimation;
    struct FSkeletonResource;

    // The update pass records these without pose math, and the executor runs only chains reachable from the output task.
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

    // Everything is Any today, so physics-coupled tasks can interleave with the simulation later.
    enum class EAnimTaskStage : uint8
    {
        AnyStage,
        PrePhysics,
        PostPhysics,
    };

    // Six streams in one allocation, laid out like FPose so the decay and the apply both stride one block.
    struct FInertChannelSet
    {
        static constexpr int32 NumStreams = 6;

        enum EStream : int32
        {
            StreamDirX = 0, StreamDirY, StreamDirZ,
            StreamX0,       StreamV0,
            StreamEval,     // this frame's decayed magnitude
        };

        void Resize(int32 NumBones)
        {
            // Padded like a pose so the decay runs whole vectors; a zeroed pad lane decays to nothing.
            Stride = FPose::StrideFor(NumBones);
            Count  = NumBones;
            Data.Init(0.0f, (SIZE_T)NumStreams * Stride);
        }

        FORCEINLINE float* Stream(int32 Index) { return Data.GetData() + (SIZE_T)Index * Stride; }
        FORCEINLINE const float* Stream(int32 Index) const { return Data.GetData() + (SIZE_T)Index * Stride; }

        // Constness rides through Stream's own overload pair, so a const task still hands out const float*.
        template <typename Self> FORCEINLINE auto DirX(this Self&& S) { return S.Stream(StreamDirX); }
        template <typename Self> FORCEINLINE auto DirY(this Self&& S) { return S.Stream(StreamDirY); }
        template <typename Self> FORCEINLINE auto DirZ(this Self&& S) { return S.Stream(StreamDirZ); }
        template <typename Self> FORCEINLINE auto X0(this Self&& S)   { return S.Stream(StreamX0); }
        template <typename Self> FORCEINLINE auto V0(this Self&& S)   { return S.Stream(StreamV0); }
        template <typename Self> FORCEINLINE auto Eval(this Self&& S) { return S.Stream(StreamEval); }

        int32 Num() const { return Count; }
        int32 NumLanes() const { return Stride; }

    private:

        TVector<float> Data;
        int32 Count  = 0;
        int32 Stride = 0;
    };

    // Share of a seam's offset left at U, the fraction of the blend elapsed, for an offset that starts at rest.
    inline float InertializationDecay(float U)
    {
        return 1.0f - U * U * U * (U * (U * 6.0f - 15.0f) + 10.0f);
    }

    // Control fields advance in the graph update pass, while capture, apply and pose history run in the Inertialize task.
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

        // Curves never reach the executor, so this seam is driven entirely by the graph update pass.
        TVector<float> CurveOffsets;
        TVector<float> PrevCurves;
        bool bHasCurveHistory = false;

        // Rising edge of a node's Request input; unused by a state machine, which starts from a transition.
        float PrevRequest = 0.0f;

        // Graph update that last ran this record, so one that sat in a skipped state can tell it missed time.
        uint32 LastUpdate = 0;
    };

    // Extrapolates the source pose forward from its seam velocity so a fast-moving source does not read as a stuck offset.
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
        uint32 LastUpdate = 0;
    };

    // Dependencies are indices of earlier tasks, so registration order is already a valid execution order.
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

        // Inertialize and DeadBlend decisions are made update-side, and Time carries the pre-advance Elapsed.
        FAnimInertializer* Inert = nullptr;
        FAnimDeadBlend* Dead = nullptr;

        // Save / Load Pose Snapshot: the per-instance buffer this slot owns.
        FPose* Snapshot = nullptr;
        float DeltaTime = 0.0f;
        bool bCapture = false;
        bool bApply = false;

        // IK, LookAt and FootIK tasks reuse T, R, S, Time and BoneA/B/C with per-opcode meanings.
        FVector3 T = FVector3(0.0f);
        FQuat    R = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        FVector3 S = FVector3(1.0f);
        uint16 BoneA = 0;
        uint16 BoneB = 0;
        uint16 BoneC = 0;
        uint8 Space = 0; // AnimPose::EBoneSpace
        uint8 Mode  = 0; // AnimPose::EBoneApplyMode
    };

    // The update pass fills this and the executor consumes and clears it, keeping capacity across frames.
    struct FAnimTaskList
    {
        TVector<FAnimTask> Tasks;
        int16 OutputTask = FAnimTask::NoTask;

        // Final-pose fixups + the skeleton every pose is authored against (valid for the frame).
        FSkeletonResource* Skeleton = nullptr;
        bool bLockRoot = false;
        int32 RootBoneIndex = Constants::kIndexNone;

        // Bones past the cut stay at bind-pose locals, and FK still runs the full hierarchy so attachments stay valid.
        int32 ActiveBoneCount = 0;

        // Set when poses evaluate in a bone LOD order other than the asset's; task bones and RootBoneIndex stay asset indices.
        const int32* PoseToAsset = nullptr;
        const int32* AssetToPose = nullptr;

        bool HasWork() const { return OutputTask != FAnimTask::NoTask; }

        int16 Add(const FAnimTask& Task)
        {
            Tasks.Add(Task);
            return (int16)(Tasks.Num() - 1);
        }

        void Reset()
        {
            Tasks.Reset();
            OutputTask      = FAnimTask::NoTask;
            Skeleton        = nullptr;
            bLockRoot       = false;
            RootBoneIndex   = Constants::kIndexNone;
            ActiveBoneCount = 0;
            PoseToAsset     = nullptr;
            AssetToPose     = nullptr;
        }
    };

    // Filled only while a capture is armed (Anim::ArmTaskCapture), so tools show the real execution.
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

        // False means unreachable from the output task, so the executor skipped it.
        bool  bReachable = false;

        // Wrote in place into DepA's buffer because this task was its last consumer.
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
            Entries.Reset();
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
