#pragma once

#include "Animation/AnimEvents.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Serialization/Archiver.h"
#include "Renderer/SkeletonResource.h"
#include "AnimationGraphVM.generated.h"

namespace Lumina
{
    class CAnimationGraph;
    struct FSkeletonResource;
    struct FAnimMontagePlayer;

    // What AdvanceClock does when the playback clock reaches a clip's duration.
    REFLECT()
    enum class EClipLoopMode : uint8
    {
        Loop,
        PlayOnce,
    };

    // Frame the BoneTransform op interprets its (T, R, S) offsets in.
    REFLECT()
    enum class EBoneTransformSpace : uint8
    {
        LocalBone,
        ComponentSpace,
    };

    // Whether the BoneTransform op layers the (T, R, S) onto the bone's existing
    // transform or replaces it.
    REFLECT()
    enum class EBoneTransformMode : uint8
    {
        Add,
        Replace,
    };

    // Frame the MakeAdditive op measures its delta in.
    REFLECT()
    enum class EAdditiveSpace : uint8
    {
        LocalSpace,
        MeshSpace,
    };

    enum class EAnimOp : uint8
    {
        Halt = 0,
        LoadConst,       // imm:float, dst:sReg
        LoadParam,       // paramIdx:uint16, dst:sReg
        ScalarOp,        // op:uint8, a:sReg, b:sReg, dst:sReg
        AdvanceClock,    // stateIdx:uint16, speed:sReg, clipIdx:uint16, loopMode:sReg, dstClock:sReg, dstFinished:sReg, syncGroup:uint16
        SampleAnim,      // clipIdx:uint16, time:sReg, dst:pReg
        SampleBlendSpace,// bsIdx:uint16, x:sReg, y:sReg, phase:sReg, dst:pReg
        RefPose,         // dst:pReg
        Blend,           // a:pReg, b:pReg, alpha:sReg, dst:pReg
        BlendMasked,     // a:pReg, b:pReg, alpha:sReg, maskIdx:uint16, dst:pReg
        MakeAdditive,    // src:pReg, dst:pReg
        ApplyAdditive,   // base:pReg, delta:pReg, alpha:sReg, dst:pReg
        EvalStateMachine,// smIdx:uint16, dst:pReg
        BoneTransform,   // src:pReg, alpha:sReg, boneIdx:uint16, space:sReg, mode:sReg, T:vec3, R:quat, S:vec3, dst:pReg
        TwoBoneIK,       // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, rootIdx:uint16, midIdx:uint16, endIdx:uint16, pole:vec3, dst:pReg
        Output,          // src:pReg
        GetCurve,        // src:pReg, curveIdx:uint16, dst:sReg
        SetCurve,        // src:pReg, curveIdx:uint16, value:sReg, dst:pReg
        EvalSlot,        // slotIdx:uint16, src:pReg, dst:pReg

        //~ Object dataflow. Appended, so every opcode above keeps its value and existing bytecode stays valid.
        LoadObjectParam,        // paramIdx:uint16, dst:oReg
        LoadObjectConst,        // constIdx:uint16, dst:oReg
        SampleAnimDyn,          // clip:oReg, time:sReg, dst:pReg
        AdvanceClockDyn,        // stateIdx:uint16, speed:sReg, clip:oReg, loopMode:sReg, dstClock:sReg, dstFinished:sReg, syncGroup:uint16
        SampleBlendSpaceDyn,    // bs:oReg, x:sReg, y:sReg, speed:sReg, phase:uint16, dst:pReg

        //~ MakeAdditive with an explicit base pose and space. Appended, so old programs stay valid.
        MakeAdditiveEx,         // src:pReg, base:pReg (kAnimNoPoseRegister = bind pose), space:uint8, dst:pReg

        //~ Seam smoothing a graph can ask for anywhere, not just at a state machine edge.
        Inertialize,            // src:pReg, request:sReg, duration:sReg, inertIdx:uint16, dst:pReg
        DeadBlend,              // src:pReg, request:sReg, duration:sReg, halfLife:sReg, deadIdx:uint16, dst:pReg

        //~ Exponential smoothing of a scalar, state carried in two slots (value + seeded flag).
        SmoothScalar,           // value:sReg, halfLife:sReg, valueSlot:uint16, seededSlot:uint16, dst:sReg

        //~ Named pose buffers that outlive the frame, for blending out of a pose captured earlier.
        SavePoseSnapshot,       // src:pReg, request:sReg, snapIdx:uint16, dst:pReg
        LoadPoseSnapshot,       // snapIdx:uint16, dst:pReg

        //~ Solvers whose targets are computed at runtime, so every operand is a register.
        FABRIK,                 // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, rootIdx:uint16, tipIdx:uint16, iterations:uint16, dst:pReg
        LookAt,                 // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, boneIdx:uint16, forward:vec3, clamp:float, dst:pReg
        FootIK,                 // src:pReg, alpha:sReg, ox/oy/oz:sReg, nx/ny/nz:sReg, alignAlpha:sReg, thigh/calf/foot:uint16, up:vec3, dst:pReg
        TranslateBone,          // src:pReg, alpha:sReg, x:sReg, y:sReg, z:sReg, boneIdx:uint16, dst:pReg
    };

    // MakeAdditiveEx base operand meaning "no base pose supplied".
    inline constexpr uint16 kAnimNoPoseRegister = 0xFFFFu;

    // Append-only: the enum value is baked into compiled bytecode.
    REFLECT()
    enum class EAnimScalarOp : uint8
    {
        Add,
        Sub,
        Mul,
        Div,
        Min,
        Max,
        // Unary (ignore operand B).
        Clamp01,
        OneMinus,    // 1 - A
        Abs,         // |A|
        Sin,         // sin(A)
        Cos,         // cos(A)
        // Binary.
        Mod,         // fmod(A, B), 0 when B == 0
        Pow,         // A ^ B
        Atan2,       // atan2(A, B)
        Less,        // A < B ? 1 : 0
        Greater,     // A > B ? 1 : 0
        // Unary (ignore operand B).
        Floor,
        Ceil,
        Frac,        // A - floor(A)
        Sqrt,        // sqrt(max(A, 0))
        Negate,      // -A
        Sign,        // -1 / 0 / 1
    };

    // AdvanceClock operand value for "not in a sync group".
    inline constexpr uint16 kAnimNoSyncGroup = 0xFFFFu;
    
    inline constexpr uint16 kAnimBytecodeVersion = 4;
    
    struct FAnimSyncGroup
    {
        float Phase        = 0.0f; // shared normalized playhead, wraps 0..1
        float PrevPhase    = 0.0f; // phase before this update's advance
        float Duration     = 0.0f; // blended seconds driving the phase speed
        float NextDuration = 0.0f; // accumulated from this update's blend provenance
        bool  bAdvanced    = false;
    };
    
    struct FAnimGraphVMState
    {
        TVector<float> ScalarRegisters;
        TVector<float> StateSlots;     // persistent playback clocks
        TVector<float> Parameters;

        // Current object parameter values, refilled each update from the entity's blackboard.
        TVector<TObjectPtr<CObject>> ObjectParameters;

        TVector<FAnimInertializer> Inertializers;     // per state machine; transition smoothing state
        TVector<FAnimInertializer> NodeInertializers; // per Inertialization node
        TVector<FAnimDeadBlend>    DeadBlends;        // per Dead Blending node
        TVector<FPose>             PoseSnapshots;     // per named snapshot slot
        TVector<FAnimSyncGroup> SyncGroups;       // shared phase per sync group

        // Curve values the output pose carried this update, indexed by CAnimationGraph::CurveNames.
        TVector<float> CurveValues;

        // Graph this state was sized against; the VM re-initializes the state
        // when the component's graph asset changes underneath it.
        const void* SourceGraph = nullptr;

        bool bInitialized = false;
    };

    struct FAnimGraphRootMotion
    {
        ERootMotionLockMode Mode = ERootMotionLockMode::FromAsset;
        int32 RootBoneIndex = INDEX_NONE;

        // Out: this frame's blended entity-space delta (FromAsset only).
        FRootMotionDelta Delta;
    };

    class RUNTIME_API FAnimationGraphVM
    {
    public:

        // Sizes register files / state slots / parameters from the graph; call when the graph asset changes.
        static void InitState(const CAnimationGraph* Graph, FAnimGraphVMState& State);

        static void BuildTasks(const CAnimationGraph* Graph,
                               FSkeletonResource* Skeleton,
                               float DeltaTime,
                               FAnimGraphVMState& State,
                               FAnimTaskList& OutTasks,
                               FAnimGraphRootMotion& RootMotionInOut,
                               TVector<FAnimNotifyEvent>* OutEvents = nullptr,
                               const FAnimMontagePlayer* Montages = nullptr);
    };
}
